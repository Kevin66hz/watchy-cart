// watchy-cart.ino — Watchy v3 (ESP32-S3FN8) 右2ボタン（UP / DOWN）運用ファーム v0.36
//
//   UP   短押し : 上へ          長押し : 1 つ戻る（時計ではロック解除）
//   DOWN 短押し : 下へ          長押し : 決定
//   UP と DOWN を同時に短く押すと、どの画面からでも時計に戻る。
//   UP と DOWN を同時に 3 秒押すと USB mode に入る（ショートカット）。
//   固まっているときは 10 秒押し続けると強制再起動し、そのまま USB mode で立ち上がる。
//   画面の階層：時計 → ホーム（上半分がカートリッジ、下に設定のカテゴリ）
//                  ├ カートリッジ → 一覧 → 確認 → FM-1 へ書き込み
//                  ├ WiFi / Net  → WiFi router / WiFi direct / Fetch latest
//                  ├ System      → FM-1 probe
//                  └ USB mode    （寝ずに待機。IDE からそのまま書き込める）
//   各モード（probe / WiFi / USB mode）の終了だけは DOWN。終了後に再起動するので、
//   UP（GPIO0）を押したまま再起動すると書き込み待ち（ROM ブートローダー）に入ってしまうため。
//   一覧の「FM-1 probe」: USB ホストになって FM-1 が見えるか 20 秒試し、結果を画面に出す。
//   一覧の「WiFi upload」: ブラウザから .fwsc を出し入れする。ルーターが登録済みならそこへつなぎ（時刻も合わせる）、
//     未登録・つながらないときは Watchy 自身が Wi-Fi の親機（watchy-cart、パスワードなし）になる。
//   一覧の「Fetch latest」: ルーター経由で、登録した取得元（各作者の GitHub リリースなど）から .fwsc を取り直す。
//     取得元は WiFi upload のページで登録する。The Firmware Hub からは取らない（規約がパッケージを作者側から取るよう求めているため）。
//   一覧の最後「USB mode」を選ぶと寝ずに待機し、IDE からそのまま書き込める。
//   DOWN を押しながら電源投入／リセットでも USB mode。
//
// v3 の事実（ライブラリ config.h / Watchy.cpp より）
//   ボタンはアクティブ LOW（押すと 0）。UP=GPIO0（ブートストラップ）, DOWN=GPIO8, BACK=6, MENU=7
//   I2C は SDA=12 / SCL=11。handleButtonPress() は virtual なので公式 init() をそのまま使う。
//
// Arduino IDE: Board ESP32S3 Dev Module / Flash 8MB
//              Partition はスケッチフォルダの partitions.csv が自動で使われる（メニューの選択は無視される）
//              USB Mode: Hardware CDC and JTAG / USB CDC On Boot: Enabled

#include <Watchy.h>
#include <Fonts/FreeMonoBold24pt7b.h>
#include "usb/usb_host.h"   // Arduino ESP32 コア同梱の USB ホストライブラリ
#include "soc/usb_serial_jtag_reg.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/usb_wrap_reg.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <LittleFS.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include "mbedtls/sha256.h"
#include "Seven_Segment10pt7b.h"      // 以下 3 つは Watchy 公式の 7_SEG フェイスから（スケッチフォルダに同梱）
#include "DSEG7_Classic_Bold_25.h"
#include "icons.h"
#include "HWCDC.h"   // HWCDCSerial：IDE の「USB CDC On Boot」設定に関係なく USB シリアルへ出す

#define FW_VERSION "v0.36"   // 時計画面の左上に表示。書き込むたびに上げる

static const uint32_t LONG_PRESS_MS      = 700;
static const uint8_t  FULL_REFRESH_EVERY = 20;

// ---- カートリッジ（内蔵フラッシュの LittleFS に置いた .fwsc ファイル）----
// ボタンを押すたびにスリープから起き直すので、一覧は RTC メモリに覚えておく（電源投入時と WiFi upload 後に読み直す）。
#define MAX_CARTS 8
struct CartInfo { char file[32]; char product[14]; uint32_t size; };
RTC_DATA_ATTR CartInfo s_carts[MAX_CARTS];
RTC_DATA_ATTR uint8_t  s_cartCount = 0;
// ホームの下半分に並ぶカテゴリと、その中の項目
static const int   CAT_COUNT = 2;
static const int   HOME_ROWS = CAT_COUNT + 2;   // カートリッジ、カテゴリ…、USB mode
static const char* CAT_NAMES[CAT_COUNT] = {"WiFi / Net", "System"};
static const int   CAT_SIZES[CAT_COUNT] = {3, 1};
static const char* CAT_ITEMS[CAT_COUNT][3] = {
  {"WiFi router", "WiFi direct", "Fetch latest"},
  {"FM-1 probe", NULL, NULL},
};

// .fwsc の先頭 960 バイト（0x30 バイト × 20 ブロック）の各ブロック末尾 1 バイトが製品名を表す（例 FM-1_903）。
// i 番目の値は「文字 + i + 1」、0x7D は「文字なし」（FM1Kit の FwscPackage と同じ読み方）
#define FWSC_HEADER_LEN 960
static void fwscProduct(const uint8_t* hdr, char* out, int outSize) {
  int k = 0;
  for (int blk = 0; blk < 20 && k < outSize - 1; blk++) {
    uint8_t m = hdr[blk * 0x30 + 0x2F];
    if (m == 0x7D) continue;
    uint8_t c = (uint8_t)(m - blk - 1);
    out[k++] = (c >= 0x21 && c <= 0x7E) ? (char)c : '?';
  }
  out[k] = 0;
}
static bool isFwscName(const char* n) {
  size_t L = strlen(n);
  return L > 5 && strcasecmp(n + L - 5, ".fwsc") == 0;
}
static bool readFwscProduct(File& f, char* out, int outSize) {
  uint8_t hdr[FWSC_HEADER_LEN];
  out[0] = 0;
  if (f.size() <= FWSC_HEADER_LEN || f.read(hdr, sizeof(hdr)) != (int)sizeof(hdr)) return false;
  fwscProduct(hdr, out, outSize);
  return true;
}
// M-VAVE Open 系（製品名が FM-1_8000000〜8000999）か。これを入れた FM-1 は更新の前に事前の手続き（arm）が要り、
// Watchy からは別のファームに替えられない（PC のインストーラーが要る）。一覧では名前の頭に「!」を付けて知らせる
static bool isOpenProduct(const char* product) {
  if (strncmp(product, "FM-1_", 5) != 0) return false;
  long ver = atol(product + 5);
  return ver >= 8000000 && ver <= 8000999;
}
// 置いてあるカートリッジを読み直して、名前順に並べる
static void scanCarts() {
  s_cartCount = 0;
  if (!LittleFS.begin(true)) return;   // 初回は自動でフォーマットされる
  File root = LittleFS.open("/");
  for (File f = root.openNextFile(); f && s_cartCount < MAX_CARTS; f = root.openNextFile()) {
    if (f.isDirectory() || !isFwscName(f.name())) continue;
    CartInfo& c = s_carts[s_cartCount];
    strlcpy(c.file, f.name(), sizeof(c.file));
    c.size = f.size();
    readFwscProduct(f, c.product, sizeof(c.product));
    s_cartCount++;
  }
  LittleFS.end();
  for (int i = 0; i < s_cartCount; i++)
    for (int j = i + 1; j < s_cartCount; j++)
      if (strcasecmp(s_carts[j].file, s_carts[i].file) < 0) { CartInfo t = s_carts[i]; s_carts[i] = s_carts[j]; s_carts[j] = t; }
}
// 画面用の名前：拡張子を外して 17 文字まで。Open 系は頭に「!」を付ける（その分 1 文字短くなる）
static void cartLabel(int i, char* out, int outSize) {
  int pre = isOpenProduct(s_carts[i].product) ? 1 : 0;
  if (pre) out[0] = '!';
  strlcpy(out + pre, s_carts[i].file, outSize - pre);
  size_t L = strlen(s_carts[i].file);
  if (L > 5 && L - 5 < (size_t)(outSize - pre)) out[pre + L - 5] = 0;
}

// ---- ルーター（自宅 Wi-Fi）の登録。本体の設定領域（NVS）に保存する ----
RTC_DATA_ATTR bool s_wifiSaved = false;   // 登録があるか（電源投入時に読み直す）
RTC_DATA_ATTR bool s_wifiOk = false;      // 直近の接続が成功したか（時計画面の Wi-Fi アイコン用）
static bool loadWifi(String& ssid, String& pass) {
  Preferences pr;
  pr.begin("cart", true);
  ssid = pr.getString("ssid", "");
  pass = pr.getString("pass", "");
  bool ok = pr.getBool("ok", false);
  pr.end();
  s_wifiSaved = ssid.length() > 0;
  s_wifiOk = s_wifiSaved && ok;
  return s_wifiSaved;
}
// ルーターに実際につながったか／つながらなかったかを覚える（時計の Wi-Fi アイコン用）
static void setWifiOk(bool ok) {
  if (ok == s_wifiOk) return;
  Preferences pr;
  pr.begin("cart", false);
  pr.putBool("ok", ok);
  pr.end();
  s_wifiOk = ok;
}
static void saveWifi(const String& ssid, const String& pass) {
  Preferences pr;
  pr.begin("cart", false);
  pr.putString("ssid", ssid);
  pr.putString("pass", pass);
  pr.putBool("ok", false);   // 登録を変えたら、次につながるまで「未確認」に戻す
  pr.end();
  s_wifiSaved = ssid.length() > 0;
  s_wifiOk = false;
}
static String htmlEsc(const String& in) {
  String o;
  for (unsigned i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '&') o += "&amp;"; else if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;"; else if (c == '\'') o += "&#39;"; else o += c;
  }
  return o;
}
static bool s_staMode = false;   // いまルーター経由でつながっているか（ページの表示用）
// Watchy から見えている Wi-Fi（2.4GHz）の名前。登録ページの候補と、つながらないときの判定に使う
#define MAX_SEEN 12
static String s_seen[MAX_SEEN];
static int    s_seenCount = -1;   // -1 = まだ調べていない
static void scanSeen() {
  WiFi.mode(WIFI_STA);
  int n = WiFi.scanNetworks();
  s_seenCount = 0;
  for (int i = 0; i < n && s_seenCount < MAX_SEEN; i++) {
    String name = WiFi.SSID(i);
    bool dup = name.length() == 0;
    for (int k = 0; k < s_seenCount && !dup; k++) dup = (s_seen[k] == name);
    if (!dup) s_seen[s_seenCount++] = name;
  }
  WiFi.scanDelete();
}
static bool isSeen(const String& ssid) {
  for (int i = 0; i < s_seenCount; i++) if (s_seen[i] == ssid) return true;
  return false;
}
// ルーターにつながらなかったときの理由（Wi-Fi の切断イベントから拾う。画面に出す）
static volatile int s_wifiReason = 0;
static void wifiEventCb(arduino_event_t* e) {
  if (e->event_id == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) s_wifiReason = e->event_info.wifi_sta_disconnected.reason;
}

// ---- 取得元の登録（名前と URL。保存領域の /sources.txt に「名前<TAB>URL」を 1 行ずつ）----
#define MAX_SOURCES 8
static int loadSources(String names[], String urls[]) {
  int n = 0;
  File f = LittleFS.open("/sources.txt", "r");
  if (!f) return 0;
  while (f.available() && n < MAX_SOURCES) {
    String line = f.readStringUntil('\n');
    line.trim();
    int tab = line.indexOf('\t');
    if (tab <= 0) continue;
    names[n] = line.substring(0, tab);
    urls[n] = line.substring(tab + 1);
    n++;
  }
  f.close();
  return n;
}
static bool saveSources(String names[], String urls[], int n) {
  File f = LittleFS.open("/sources.txt", "w");
  if (!f) return false;
  for (int i = 0; i < n; i++) { f.print(names[i]); f.print('\t'); f.print(urls[i]); f.print('\n'); }
  f.close();
  return true;
}

// ---- WiFi upload 用（ブラウザからの受け取り）----
static WebServer* s_web = NULL;
static File   s_upFile;
static bool   s_upOk = false;
static String s_upMsg;
static char   s_upPath[40];

// 受け取ったファイル名を安全な形にする（英数字と . - _ だけ、本体 24 文字まで、拡張子 .fwsc 必須）
static bool cartPathFromName(const String& in, char* out) {
  int slash = in.lastIndexOf('/');
  int bs = in.lastIndexOf('\\');
  if (bs > slash) slash = bs;
  String b = in.substring(slash + 1);
  if (!isFwscName(b.c_str())) return false;
  int n = b.length() - 5;
  if (n > 24) n = 24;
  int k = 0;
  out[k++] = '/';
  for (int i = 0; i < n; i++) {
    char ch = b[i];
    out[k++] = (isalnum((unsigned char)ch) || ch == '.' || ch == '-' || ch == '_') ? ch : '_';
  }
  strcpy(out + k, ".fwsc");
  return true;
}
static int fsCartCount() {
  int n = 0;
  File root = LittleFS.open("/");
  for (File f = root.openNextFile(); f; f = root.openNextFile()) if (!f.isDirectory() && isFwscName(f.name())) n++;
  return n;
}
static void webUploadChunk() {
  HTTPUpload& up = s_web->upload();
  if (up.status == UPLOAD_FILE_START) {
    s_upOk = false; s_upMsg = "";
    if (!cartPathFromName(up.filename, s_upPath)) s_upMsg = "拡張子が .fwsc のファイルだけ置けます";
    else if (!LittleFS.exists(s_upPath) && fsCartCount() >= MAX_CARTS) s_upMsg = String("これ以上置けません（上限 ") + String(MAX_CARTS) + " 個）。先にどれか削除してください";
    else {
      s_upFile = LittleFS.open(s_upPath, "w");
      s_upOk = (bool)s_upFile;
      if (!s_upOk) s_upMsg = "ファイルを作れませんでした";
    }
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (s_upOk && s_upFile.write(up.buf, up.currentSize) != up.currentSize) {
      s_upOk = false; s_upMsg = "空き容量が足りません";
      s_upFile.close(); LittleFS.remove(s_upPath);
    }
  } else if (up.status == UPLOAD_FILE_END) {
    if (s_upOk) {
      s_upFile.close();
      // 最低限の確認：FM-1 用の .fwsc かどうか（中身の検査は書き込みの直前に別途行う）
      char product[14] = "";
      File f = LittleFS.open(s_upPath, "r");
      bool ok = f && readFwscProduct(f, product, sizeof(product)) && strncmp(product, "FM-1", 4) == 0;
      uint32_t size = f ? f.size() : 0;
      if (f) f.close();
      if (ok) s_upMsg = String("保存しました：") + (s_upPath + 1) + "（" + product + "、" + String(size / 1024) + " KB）"
                       + (isOpenProduct(product) ? "<br>！ M-VAVE Open 系です。入れたあとは Watchy から別のファームに替えられません（PC のインストーラーが要ります）" : "");
      else { LittleFS.remove(s_upPath); s_upOk = false; s_upMsg = "FM-1 用の .fwsc ではないので保存しませんでした"; }
    }
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    if (s_upOk) { s_upFile.close(); LittleFS.remove(s_upPath); }
    s_upOk = false; s_upMsg = "転送が中断されました";
  }
}
static void webSendPage() {
  String h = F("<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
               "<title>watchy-cart</title><style>body{font:16px/1.5 monospace;margin:16px}td{padding:4px 10px 4px 0}"
               "button,input{font:inherit}</style><h3>watchy-cart " FW_VERSION "</h3>");
  if (s_upMsg.length()) { h += String("<p><b>") + s_upMsg + "</b></p>"; s_upMsg = ""; }
  h += F("<table>");
  int n = 0;
  File root = LittleFS.open("/");
  for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    if (f.isDirectory() || !isFwscName(f.name())) continue;
    char product[14];
    readFwscProduct(f, product, sizeof(product));
    h += String("<tr><td>") + (isOpenProduct(product) ? "！ " : "") + f.name() + "<td>" + product + "<td>" + String(f.size() / 1024) + " KB"
       + "<td><form method=post action=/delete><input type=hidden name=f value='" + f.name() + "'><button>削除</button></form>";
    n++;
  }
  h += F("</table><p>！＝M-VAVE Open 系。入れたあとは Watchy から別のファームに替えられません</p>");
  if (n == 0) h += F("<p>（まだ何も置いていません）</p>");
  h += String("<p>") + String(n) + " / " + String(MAX_CARTS) + " 個、空き " + String((LittleFS.totalBytes() - LittleFS.usedBytes()) / 1024) + " KB</p>";
  h += F("<form method=post action=/upload enctype=multipart/form-data><input type=file name=f> <button>置く</button></form>");
  // 取得元
  String sn[MAX_SOURCES], su[MAX_SOURCES];
  int sc = loadSources(sn, su);
  h += F("<h4>取得元（作者の配布先）</h4><table>");
  for (int i = 0; i < sc; i++) {
    h += String("<tr><td>") + htmlEsc(sn[i]) + "<td style='max-width:14em;overflow:hidden;text-overflow:ellipsis;white-space:nowrap'>" + htmlEsc(su[i])
       + "<td><form method=post action=/src-get><input type=hidden name=n value='" + htmlEsc(sn[i]) + "'><button>取得</button></form>"
       + "<td><form method=post action=/src-del><input type=hidden name=n value='" + htmlEsc(sn[i]) + "'><button>削除</button></form>";
  }
  h += F("</table><form method=post action=/src-add>名前 <input name=n size=12> URL <input name=u size=30><br><button>登録</button></form>"
         "<p>URL は .fwsc ファイルそのものを指す https のアドレス。GitHub なら "
         "https://github.com/作者/リポジトリ/releases/latest/download/ファイル名.fwsc の形にすると、いつも最新版が取れます。"
         "「取得」はルーター経由でつながっているときだけ動きます（数十秒かかります）。</p>");
  String ssid, pass;
  loadWifi(ssid, pass);
  h += F("<h4>ルーター接続</h4>");
  if (s_seenCount >= 0) {
    h += F("<p>Watchy から見えている Wi-Fi（2.4GHz のみ）：");
    for (int i = 0; i < s_seenCount; i++) h += (i ? String("、") : String("")) + htmlEsc(s_seen[i]);
    if (s_seenCount == 0) h += F("（なし）");
    h += F("</p>");
    if (ssid.length() && !isSeen(ssid)) h += String("<p><b>登録中の「") + htmlEsc(ssid) + "」は、この中にありません。5GHz 専用の名前か、入力違いです。上の一覧から選び直してください。</b></p>";
  }
  h += F("<p>");
  h += s_staMode ? F("いまルーター経由でつながっています。") : F("いま Watchy 直結（親機モード）でつながっています。");
  h += String("保存済みのパスワードは ") + pass.length() + " 文字です。";
  h += F("登録すると、メニューの WiFi router でルーターへつなぎます。名前を空欄で保存すると登録を消します。2.4GHz の Wi-Fi だけ使えます。</p>"
         "<form method=post action=/wifi>名前(SSID) <input name=s list=nets value='");
  h += htmlEsc(ssid);
  h += F("'><datalist id=nets>");
  for (int i = 0; i < s_seenCount; i++) h += String("<option value='") + htmlEsc(s_seen[i]) + "'>";
  h += F("</datalist><br>パスワード <input name=p type=password><br><button>保存</button></form>"
         "<p id=clk>時計：合わせています…</p>"
         "<script>fetch('/time?t='+Math.floor(Date.now()/1000)+'&o='+(-new Date().getTimezoneOffset()*60),{method:'POST'})"
         ".then(r=>{document.getElementById('clk').textContent=r.ok?'時計：この端末の時刻に合わせました':'時計：合わせられませんでした'})"
         ".catch(()=>{document.getElementById('clk').textContent='時計：合わせられませんでした'})</script>"
         "<p>終わったら Watchy の DOWN を長押し。</p>");
  s_web->send(200, "text/html; charset=utf-8", h);
}
static void webRedirectHome() {
  s_web->sendHeader("Location", "/");
  s_web->send(303, "text/plain", "");
}
// ページを開いた端末（スマホや PC）の時刻で Watchy の時計を合わせる。t = 世界標準時の秒、o = その端末の時差（秒）
static void webTime() {
  long long t = atoll(s_web->arg("t").c_str());
  long o = atol(s_web->arg("o").c_str());
  if (t > 1700000000LL && o >= -43200 && o <= 50400) {
    tmElements_t tm;
    breakTime((time_t)(t + o), tm);   // Watchy の時計は現地時刻で持つ（公式の時刻合わせと同じ）
    Watchy::RTC.set(tm);
    s_web->send(200, "text/plain", "ok");
  } else {
    s_web->send(400, "text/plain", "bad time");
  }
}
static void webWifi() {
  String ssid = s_web->arg("s");
  ssid.trim();
  saveWifi(ssid, s_web->arg("p"));
  s_upMsg = ssid.length() ? String("ルーターを登録しました：") + htmlEsc(ssid) : String("ルーターの登録を消しました");
  webRedirectHome();
}
static void webDelete() {
  char path[40];
  if (cartPathFromName(s_web->arg("f"), path) && LittleFS.remove(path)) s_upMsg = String("削除しました：") + (path + 1);
  else s_upMsg = "削除できませんでした";
  webRedirectHome();
}

enum Screen : uint8_t { SCR_CLOCK = 0, SCR_HOME, SCR_CARTS, SCR_SUB, SCR_CONFIRM };
RTC_DATA_ATTR Screen  screen = SCR_CLOCK;
RTC_DATA_ATTR int8_t  cursor = 0;       // 一覧・カテゴリ内での位置
RTC_DATA_ATTR int8_t  homeCursor = 0;   // ホームでの位置（0 = カートリッジ、1〜 = カテゴリ）
RTC_DATA_ATTR int8_t  subCat = 0;       // 開いているカテゴリ
RTC_DATA_ATTR uint8_t partialCount = 0;

// ---- ボタン（アクティブ LOW）----
// 入力はプルアップ付きにする。プルアップなしだと、押していない側のボタンが「押されている」と読めることがある
// （v0.25 で同時押しの判定を入れてから、押していない UP を読むようになり、誤って時計へ戻ることがあった疑い）。
static bool isDown(int pin) { return digitalRead(pin) == LOW; }
// 両方が続けて 4 回（約 40ms）押されていると読めたときだけ、同時押しと見なす
static bool bothDown() {
  for (int i = 0; i < 4; i++) {
    if (!(isDown(UP_BTN_PIN) && isDown(DOWN_BTN_PIN))) return false;
    delay(10);
  }
  return true;
}

static bool measureLongPress(int pin) {
  delay(30);
  uint32_t t0 = millis();
  while (isDown(pin)) {
    if (millis() - t0 >= LONG_PRESS_MS) return true;
    delay(5);
  }
  return false;
}

static void waitRelease() {
  uint32_t t0 = millis();
  while ((isDown(UP_BTN_PIN) || isDown(DOWN_BTN_PIN)) && millis() - t0 < 3000) delay(10);
  delay(30);
}

// ---- FM-1 probe 用：USB ホストのイベント受け ----
static volatile bool    s_probeNewDev = false;
static volatile uint8_t s_probeAddr   = 0;
static volatile bool    s_devGone     = false;   // 開いていた機器が外れた
static void probeClientCb(const usb_host_client_event_msg_t* msg, void*) {
  if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
    s_probeAddr   = msg->new_dev.address;
    s_probeNewDev = true;
  } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
    s_devGone = true;
  }
}

// ---- probe の記録（再起動後も残る。USB mode でシリアルに何か送ると出力される）----
#define PROBE_LOG_MAGIC 0x50524F42
static RTC_NOINIT_ATTR uint32_t s_logMagic;
static RTC_NOINIT_ATTR uint32_t s_logLen;
static RTC_NOINIT_ATTR char     s_log[3000];
static volatile int s_logLines = 0;
// 起動の記録：電源投入・再起動のたびに数え、直近の再起動の理由を覚える（USB mode で表示。落ちて再起動を繰り返していないかを見る）
static RTC_NOINIT_ATTR uint32_t s_bootMagic;
static RTC_NOINIT_ATTR uint32_t s_bootCount;
static RTC_NOINIT_ATTR uint32_t s_lastResetReason;
static RTC_NOINIT_ATTR uint32_t s_bootToUsb;   // 強制再起動のあと USB mode で立ち上がる合図（PROBE_LOG_MAGIC のとき有効）
static RTC_NOINIT_ATTR uint32_t s_probeMode;   // 0 = A（ピンに触らない） / 1 = B（v0.9 と同じくピンを触る）。実行ごとに交互

static void probeLogAppend(const char* fmt, ...) {
  if (s_logLen >= sizeof(s_log) - 1) return;
  va_list ap; va_start(ap, fmt);
  int n = vsnprintf(s_log + s_logLen, sizeof(s_log) - s_logLen, fmt, ap);
  va_end(ap);
  if (n > 0) s_logLen = min((uint32_t)(s_logLen + n), (uint32_t)(sizeof(s_log) - 1));
}
// USB ホストライブラリが出すエラーログをここで受け取る（ホスト動作中はシリアルに出せないため）
static int probeLogVprintf(const char* fmt, va_list ap) {
  if (s_logLines < 8 && s_logLen < sizeof(s_log) - 1) {   // 全文を残すのは最初の 8 件。以降は件数だけ
    int n = vsnprintf(s_log + s_logLen, sizeof(s_log) - s_logLen, fmt, ap);
    if (n > 0) s_logLen = min((uint32_t)(s_logLen + n), (uint32_t)(sizeof(s_log) - 1));
  }
  s_logLines++;
  return 0;
}
// USB ホストコントローラ（DWC2）のレジスタ番地。ESP32-S3 では 0x60080000 から（リンカ定義 USB_DWC と同じ）
#define PROBE_DWC_BASE     0x60080000UL
#define PROBE_GOTGCTL_REG  (PROBE_DWC_BASE + 0x000)
#define PROBE_HPRT_REG     (PROBE_DWC_BASE + 0x440)

// USB まわりのレジスタを記録する。HPRT がホストから見たポートの状態
// （bit0=接続あり, bit2=有効, bit10-11=線の状態 D+/D-, bit12=ポート電源）
static void probeDumpRegs(const char* tag) {
  probeLogAppend("[%s] USJ=%08lx RTC=%08lx WRAP=%08lx OTG=%08lx HPRT=%08lx\n", tag,
                 (unsigned long)REG_READ(USB_SERIAL_JTAG_CONF0_REG), (unsigned long)REG_READ(RTC_CNTL_USB_CONF_REG),
                 (unsigned long)REG_READ(USB_WRAP_OTG_CONF_REG), (unsigned long)REG_READ(PROBE_GOTGCTL_REG),
                 (unsigned long)REG_READ(PROBE_HPRT_REG));
}

// HPRT（ホストから見たポートの状態）を 1ms ごとに監視し、値が変わった時刻だけ記録する。
// バスリセットの前後で FM-1 がどう振る舞うか（D+ を下げるか、いつ戻すか）を見るため。
struct HprtEv { uint16_t t; uint32_t v; };
static HprtEv       s_trace[56];
static volatile int s_traceN = 0;
static volatile bool s_traceRun = false;
static void hprtTraceTask(void*) {
  uint32_t t0 = millis(), last = 0xFFFFFFFF;
  while (s_traceRun && millis() - t0 < 3000) {
    uint32_t v = REG_READ(PROBE_HPRT_REG);
    if (v != last && s_traceN < 56) { s_trace[s_traceN].t = (uint16_t)(millis() - t0); s_trace[s_traceN].v = v; s_traceN++; last = v; }
    vTaskDelay(1);
  }
  s_traceRun = false;
  vTaskDelete(NULL);
}

// 列挙の途中経過：デバイスディスクリプタを読めた時点で呼ばれる
static volatile bool     s_enumCbFired = false;
static volatile uint16_t s_enumVid = 0, s_enumPid = 0;
static bool probeEnumFilter(const usb_device_desc_t* d, uint8_t* cfg) {
  s_enumVid = d->idVendor; s_enumPid = d->idProduct; s_enumCbFired = true;
  return true;
}

// ---- USB-MIDI の送受信（FM-1 への問い合わせ用）----
// USB-MIDI は 4 バイト単位のパケットで運ぶ：[種別][MIDI 3 バイトまで]。
// SysEx は「続き(0x4)」と「終わり(0x5/0x6/0x7)」の種別で分割される。
static uint8_t       s_rxSysex[96];
static volatile int  s_rxLen  = 0;
static volatile bool s_rxDone = false;
static bool          s_rxIn   = false;
static volatile bool s_inRun  = false;
static volatile bool s_outBusy = false;
static volatile bool s_inPending = false;   // 受信待ちの転送を出したままか
static volatile int  s_inErrCount = 0, s_inLastStatus = 0;     // 受信の失敗回数と、直近の失敗の種類（0 = なし）
static volatile int  s_outErrCount = 0, s_outLastStatus = 0;   // 送信の同上

static void midiInCb(usb_transfer_t* t) {
  if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
    for (int i = 0; i + 3 < t->actual_num_bytes; i += 4) {
      uint8_t cin = t->data_buffer[i] & 0x0F;
      int n = (cin == 0x4 || cin == 0x7) ? 3 : (cin == 0x6) ? 2 : (cin == 0x5) ? 1 : 0;
      for (int k = 0; k < n && !s_rxDone; k++) {
        uint8_t b = t->data_buffer[i + 1 + k];
        if (b == 0xF0) { s_rxLen = 0; s_rxIn = true; }
        if (s_rxIn && s_rxLen < (int)sizeof(s_rxSysex)) s_rxSysex[s_rxLen++] = b;
        if (b == 0xF7 && s_rxIn) { s_rxIn = false; s_rxDone = true; }
      }
    }
  }
  // 失敗で終わったときは受信口が止まっている（halt）ので、ここでは出し直さない。fm1Service() が口を戻してから出し直す
  if (t->status != USB_TRANSFER_STATUS_COMPLETED) { s_inErrCount++; s_inLastStatus = (int)t->status; }
  bool again = s_inRun && t->status == USB_TRANSFER_STATUS_COMPLETED;
  if (!(again && usb_host_transfer_submit(t) == ESP_OK)) s_inPending = false;
}
static void midiOutCb(usb_transfer_t* t) {
  if (t->status != USB_TRANSFER_STATUS_COMPLETED) { s_outErrCount++; s_outLastStatus = (int)t->status; }
  s_outBusy = false;
}

// SysEx 1 通（F0 ... F7）を USB-MIDI パケットにして送る（転送バッファに入る長さまで）
static bool midiSendSysex(usb_transfer_t* out, const uint8_t* msg, int len) {
  int n = 0;
  for (int i = 0; i < len && n + 4 <= (int)out->data_buffer_size;) {
    int rem = len - i;
    uint8_t* q = out->data_buffer + n;
    q[1] = q[2] = q[3] = 0;
    if (rem > 3)       { q[0] = 0x04; q[1] = msg[i]; q[2] = msg[i + 1]; q[3] = msg[i + 2]; i += 3; }
    else if (rem == 3) { q[0] = 0x07; q[1] = msg[i]; q[2] = msg[i + 1]; q[3] = msg[i + 2]; i += 3; }
    else if (rem == 2) { q[0] = 0x06; q[1] = msg[i]; q[2] = msg[i + 1]; i += 2; }
    else               { q[0] = 0x05; q[1] = msg[i]; i += 1; }
    n += 4;
  }
  out->num_bytes = n;
  s_outBusy = true;
  return usb_host_transfer_submit(out) == ESP_OK;
}

// FM-1 の更新プロトコルの 7 ビット詰め（FM1Kit の SevenBit と同じ：全体を 1 本のビット列として下位から 7 ビットずつ切る）
static int sevenBitPack(const uint8_t* in, int len, uint8_t* out) {
  uint32_t acc = 0; int bits = 0, n = 0;
  for (int i = 0; i < len; i++) {
    acc |= (uint32_t)in[i] << bits; bits += 8;
    while (bits >= 7) { out[n++] = acc & 0x7F; acc >>= 7; bits -= 7; }
  }
  if (bits > 0) out[n++] = acc & 0x7F;
  return n;
}
static int sevenBitUnpack(const uint8_t* in, int len, uint8_t* out) {
  uint32_t acc = 0; int bits = 0, n = 0;
  for (int i = 0; i < len; i++) {
    acc |= (uint32_t)(in[i] & 0x7F) << bits; bits += 7;
    while (bits >= 8) { out[n++] = acc & 0xFF; acc >>= 8; bits -= 8; }
  }
  return n;
}
// 受信した SysEx が「名乗り」の返事なら、その文字列を取り出す（例 FM-1_015）。違えば false
static bool parseIdentityReply(const uint8_t* sysex, int len, char* text, int textSize) {
  if (len < 10 || sysex[0] != 0xF0 || sysex[len - 1] != 0xF7) return false;
  uint8_t body[96];
  int n = sevenBitUnpack(sysex + 1, len - 2, body);
  if (n < 8 || body[0] != 0x00 || body[1] != 0x59 || body[2] != 0x11) return false;
  int k = 0;
  for (int i = 6; i < n - 1 && body[i] != 0 && k < textSize - 1; i++) {
    if (body[i] < 0x21 || body[i] > 0x7E) return false;
    text[k++] = (char)body[i];
  }
  text[k] = 0;
  return k > 0;
}

// ---- FM-1 との接続（USB ホスト）：つなぐ・開く・閉じる・つなぎ直す ----
// 書き込みでは FM-1 が途中で 2 回再起動する（更新用ローダーへ → 新しいファームへ）ので、つなぎ直しが要る。
static usb_host_client_handle_t s_client = NULL;
static usb_device_handle_t      s_dev    = NULL;
static usb_transfer_t*          s_xin    = NULL;
static usb_transfer_t*          s_xout   = NULL;
static int      s_ifNum = -1, s_epIn = -1, s_epOut = -1;
static bool     s_claimed = false;
static uint16_t s_vid = 0, s_pid = 0;

// 送受信の口が失敗で止まっていたら戻し、受信待ちが切れていたら出し直す（イベントを回すたびに呼ぶ）
static uint32_t s_lastInTry = 0;
static int      s_svcLogs = 0;
static void fm1Service() {
  if (!s_dev || !s_xin || s_devGone || !s_inRun) return;
  if (s_outLastStatus != 0 && !s_outBusy) {
    esp_err_t e1 = usb_host_endpoint_flush(s_dev, s_epOut);
    esp_err_t e2 = usb_host_endpoint_clear(s_dev, s_epOut);
    if (s_svcLogs < 8) { s_svcLogs++; probeLogAppend("%lu out err st:%d -> flush 0x%x clear 0x%x\n", (unsigned long)millis(), (int)s_outLastStatus, (unsigned)e1, (unsigned)e2); }
    s_outLastStatus = 0;
  }
  if (!s_inPending && millis() - s_lastInTry >= 50) {
    s_lastInTry = millis();
    int st = s_inLastStatus;
    esp_err_t e1 = ESP_OK, e2 = ESP_OK;
    if (st != 0) {
      e1 = usb_host_endpoint_flush(s_dev, s_epIn);
      e2 = usb_host_endpoint_clear(s_dev, s_epIn);
      s_inLastStatus = 0;
    }
    s_xin->num_bytes = s_xin->data_buffer_size;
    esp_err_t e3 = usb_host_transfer_submit(s_xin);
    s_inPending = (e3 == ESP_OK);
    if (s_svcLogs < 8) { s_svcLogs++; probeLogAppend("%lu in err st:%d -> flush 0x%x clear 0x%x resubmit 0x%x\n", (unsigned long)millis(), st, (unsigned)e1, (unsigned)e2, (unsigned)e3); }
  }
}

// イベントを回しながら待つ。cond が true になるか ms 経過で戻る
static bool fm1Pump(volatile bool& cond, uint32_t ms) {
  uint32_t t = millis();
  while (!cond && millis() - t < ms) {
    fm1Service();
    uint32_t flags = 0;
    usb_host_lib_handle_events(pdMS_TO_TICKS(5), &flags);
    if (s_client) usb_host_client_handle_events(s_client, pdMS_TO_TICKS(5));
  }
  return (bool)cond;
}
static void fm1PumpFor(uint32_t ms) { volatile bool never = false; fm1Pump(never, ms); }

// ホスト側で D+ を高く保つ／やめる。
// 保つ：FM-1 はバスリセット直後の約 1ms だけ D+ を離すので、保たないとリセットに失敗する（v0.13〜0.14 で確認）。
// やめる：保っている間は「外れた」ことが分からないので、外れるのを待つ間と、本物の接続を待つ間はやめる。
static void dpHold(bool on) {
  if (on) {
    CLEAR_PERI_REG_MASK(USB_WRAP_OTG_CONF_REG, USB_WRAP_DP_PULLDOWN);
    SET_PERI_REG_MASK(USB_WRAP_OTG_CONF_REG, USB_WRAP_PAD_PULL_OVERRIDE | USB_WRAP_DP_PULLUP);
  } else {
    CLEAR_PERI_REG_MASK(USB_WRAP_OTG_CONF_REG, USB_WRAP_DP_PULLUP);
    SET_PERI_REG_MASK(USB_WRAP_OTG_CONF_REG, USB_WRAP_PAD_PULL_OVERRIDE | USB_WRAP_DP_PULLDOWN);
  }
}
static bool portConnected() { return REG_READ(PROBE_HPRT_REG) & 1; }

static esp_err_t fm1HostStart() {
  esp_log_set_vprintf(probeLogVprintf);
  s_probeNewDev = false;
  usb_host_config_t hc = {};
  hc.intr_flags = ESP_INTR_FLAG_LEVEL1;
  hc.enum_filter_cb = probeEnumFilter;
  esp_err_t e = usb_host_install(&hc);
  if (e != ESP_OK) return e;
  dpHold(true);   // v0.14〜0.15 で動いた条件：ホスト開始の直後から D+ を保つ
  usb_host_client_config_t cc = {};
  cc.is_synchronous = false;
  cc.max_num_event_msg = 5;
  cc.async.client_event_callback = probeClientCb;
  cc.async.callback_arg = NULL;
  return usb_host_client_register(&cc, &s_client);
}

// 開いている機器を閉じる（外れた後でも、つながったままでも）
static void fm1Close() {
  if (!s_dev) return;
  s_inRun = false;
  uint32_t t = millis();
  while (s_outBusy && millis() - t < 300) fm1PumpFor(10);
  if (s_inPending) {
    usb_host_endpoint_halt(s_dev, s_epIn);
    usb_host_endpoint_flush(s_dev, s_epIn);
    t = millis();
    while (s_inPending && millis() - t < 500) fm1PumpFor(10);
  }
  esp_err_t e1 = s_claimed ? usb_host_interface_release(s_client, s_dev, s_ifNum) : ESP_OK;
  if (s_xin)  usb_host_transfer_free(s_xin);
  if (s_xout) usb_host_transfer_free(s_xout);
  esp_err_t e2 = usb_host_device_close(s_client, s_dev);
  probeLogAppend("close: pending %d release 0x%x close 0x%x\n", (int)s_inPending, (unsigned)e1, (unsigned)e2);
  s_dev = NULL; s_xin = NULL; s_xout = NULL; s_claimed = false; s_inPending = false; s_outBusy = false;
  fm1PumpFor(50);
}

static bool fm1Open(char* err, int errSize);

// USB ホストを丸ごと止める（機器を閉じる → 登録解除 → 全機器の解放 → 停止）。
// D+ を保っている間は「外れた」ことが分からないので、つなぎ直しはホストごと作り直して行う。
static int s_stopLogs = 0;
static void fm1HostStop() {
  fm1Close();
  esp_err_t e1 = ESP_OK, e2 = ESP_OK, e3 = ESP_OK;
  if (s_client) { e1 = usb_host_client_deregister(s_client); s_client = NULL; }
  e2 = usb_host_device_free_all();
  if (e2 == ESP_ERR_NOT_FINISHED) {
    uint32_t t = millis();
    while (millis() - t < 1500) {
      uint32_t flags = 0;
      usb_host_lib_handle_events(pdMS_TO_TICKS(10), &flags);
      if (flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) { e2 = ESP_OK; break; }
    }
  }
  // 外れた機器の後始末はイベント処理の中で進むので、終わるまで回しながら停止を試す（v0.19 では即停止して 0x103 で失敗した）
  uint32_t t3 = millis();
  do {
    uint32_t flags = 0;
    usb_host_lib_handle_events(pdMS_TO_TICKS(20), &flags);
    e3 = usb_host_uninstall();
  } while (e3 != ESP_OK && millis() - t3 < 3000);
  if ((e1 != ESP_OK || e2 != ESP_OK || e3 != ESP_OK) && s_stopLogs < 4) {
    s_stopLogs++;
    probeLogAppend("host stop: dereg 0x%x free 0x%x uninst 0x%x\n", (unsigned)e1, (unsigned)e2, (unsigned)e3);
  }
}

// FM-1 につながるまで試す：ホスト開始 → 認識待ち（2 秒）→ 開く。だめならホストを止めてやり直し。
// 成功したらホストは動かしたまま戻る。
static bool fm1Connect(uint32_t timeoutMs, char* err, int errSize) {
  uint32_t t0 = millis();
  int tries = 0;
  do {
    tries++;
    esp_err_t e = fm1HostStart();
    if (e != ESP_OK) {
      snprintf(err, errSize, "host err 0x%x", (unsigned)e);
      if (tries <= 2) probeLogAppend("connect try %d: host start 0x%x\n", tries, (unsigned)e);
    } else if (!fm1Pump(s_probeNewDev, 2000)) {
      snprintf(err, errSize, "no device seen");
      if (tries <= 2) probeLogAppend("connect try %d: no device, enum:%d HPRT=%05lx\n", tries, (int)s_enumCbFired, (unsigned long)REG_READ(PROBE_HPRT_REG));
    } else if (fm1Open(err, errSize)) {
      probeLogAppend("connect ok: try %d, %lums\n", tries, (unsigned long)(millis() - t0));
      return true;
    } else if (tries <= 2) {
      probeLogAppend("connect try %d: %s\n", tries, err);
    }
    fm1HostStop();
    delay(100);
  } while (millis() - t0 < timeoutMs);
  probeLogAppend("connect FAIL: tries %d (%s)\n", tries, err);
  return false;
}

// 認識された機器を開き、MIDI の送信(OUT)・受信(IN)口を確保して受信を始める。失敗したら err に理由
static bool fm1Open(char* err, int errSize) {
  s_devGone = false;
  esp_err_t e = usb_host_device_open(s_client, s_probeAddr, &s_dev);
  if (e != ESP_OK) { s_dev = NULL; snprintf(err, errSize, "open err 0x%x", (unsigned)e); return false; }
  const usb_device_desc_t* dd = NULL;
  const usb_config_desc_t* cd = NULL;
  usb_host_get_device_descriptor(s_dev, &dd);
  usb_host_get_active_config_descriptor(s_dev, &cd);
  s_vid = dd ? dd->idVendor : 0; s_pid = dd ? dd->idProduct : 0;
  int ifAlt = 0, mpsIn = 64;
  s_ifNum = -1; s_epIn = -1; s_epOut = -1;
  if (cd) {
    const uint8_t* p = (const uint8_t*)cd;
    int total = cd->wTotalLength;
    bool inMidi = false;
    for (int i = 0; i + 1 < total && p[i] >= 2; i += p[i]) {
      if (p[i + 1] == 0x04 && i + 6 < total) {                    // interface
        inMidi = (p[i + 5] == 0x01 && p[i + 6] == 0x03);          // Audio / MIDI Streaming
        if (inMidi && s_ifNum < 0) { s_ifNum = p[i + 2]; ifAlt = p[i + 3]; }
      } else if (p[i + 1] == 0x05 && inMidi && i + 5 < total) {   // endpoint
        uint8_t addr = p[i + 2];
        if (addr & 0x80) { if (s_epIn < 0) { s_epIn = addr; mpsIn = p[i + 4] | (p[i + 5] << 8); } }
        else             { if (s_epOut < 0) s_epOut = addr; }
      }
    }
  }
  probeLogAppend("open %04x:%04x midi if:%d out:%02x in:%02x mps:%d\n", s_vid, s_pid, s_ifNum, s_epOut & 0xFF, s_epIn & 0xFF, mpsIn);
  if (s_ifNum < 0 || s_epOut < 0 || s_epIn < 0) { snprintf(err, errSize, "no MIDI endpoint"); fm1Close(); return false; }
  if ((e = usb_host_interface_claim(s_client, s_dev, s_ifNum, ifAlt)) != ESP_OK) { snprintf(err, errSize, "claim err 0x%x", (unsigned)e); fm1Close(); return false; }
  s_claimed = true;
  if (usb_host_transfer_alloc(mpsIn, 0, &s_xin) != ESP_OK || usb_host_transfer_alloc(1024, 0, &s_xout) != ESP_OK) { snprintf(err, errSize, "alloc err"); fm1Close(); return false; }
  s_xin->device_handle = s_dev;  s_xin->bEndpointAddress = s_epIn;   s_xin->callback = midiInCb;   s_xin->num_bytes = mpsIn;
  s_xout->device_handle = s_dev; s_xout->bEndpointAddress = s_epOut; s_xout->callback = midiOutCb;
  s_rxLen = 0; s_rxDone = false; s_rxIn = false; s_inRun = true; s_outBusy = false;
  s_inLastStatus = 0; s_outLastStatus = 0;
  e = usb_host_transfer_submit(s_xin);
  s_inPending = (e == ESP_OK);
  if (e != ESP_OK) { snprintf(err, errSize, "in err 0x%x", (unsigned)e); fm1Close(); return false; }
  return true;
}

// 「あなたは誰？」と問い合わせて名乗りを受け取る（読み取りだけ。FM-1 の状態は変えない）
static bool s_quietHandshake = false;   // true の間は問い合わせの経過を記録しない（1 秒ごとの確認で記録があふれないように）
static bool fm1Identity(char* ident, int identSize) {
  static const uint8_t handshake[] = {0xF0, 0x00, 0x32, 0x45, 0x00, 0x00, 0x00, 0x40, 0x7F, 0xF7};
  ident[0] = 0;
  for (int attempt = 0; attempt < 3 && !ident[0] && !s_devGone; attempt++) {
    s_rxDone = false;
    bool ok = midiSendSysex(s_xout, handshake, sizeof(handshake));
    uint32_t t = millis();
    while (!ident[0] && millis() - t < 700) {
      if (!fm1Pump(s_rxDone, 700 - (millis() - t))) break;
      if (!parseIdentityReply(s_rxSysex, s_rxLen, ident, identSize)) ident[0] = 0;
      s_rxDone = false;   // 次の受信を許可
    }
    if (!s_quietHandshake) probeLogAppend("handshake #%d tx:%s -> %s\n", attempt + 1, ok ? "ok" : "FAIL", ident[0] ? ident : "(no reply)");
  }
  return ident[0] != 0;
}

// ---- 書き込み（FM-1 の更新手順。FM1Kit の FM1Updater と同じ流れ）----
// 1. 動作中のファームに「更新開始」を送ると、FM-1 がパッケージの中身を少しずつ読みに来る（下見。合格すると更新用ローダーで再起動）
// 2. ローダーにもう一度「更新開始」を送ると、また読みに来て、今度はフラッシュへ書く
// どちらも FM-1 が「番地 A から L バイトください」と頼み、こちらはその部分を返すだけ。
static File     s_img;          // 書き込む .fwsc
static uint32_t s_imgLen = 0;   // FM-1 から見た長さ（先頭 20 個の目印バイトを除く）
static uint32_t s_appStart = 0, s_appEnd = 0;   // ファーム本体のある範囲（進み具合の表示用）
static uint8_t  s_txBody[15 + 512 + 8];
static uint8_t  s_txSysex[640];

// FM-1 から見た番地でファイルを読む。先頭 940 バイト分は 47 バイトごとに目印 1 バイトが挟まっているので飛ばす
static bool imgRead(uint32_t addr, uint8_t* out, int len) {
  while (len > 0 && addr < 940) {
    if (!s_img.seek(addr + addr / 47)) return false;
    int c = s_img.read();
    if (c < 0) return false;
    *out++ = (uint8_t)c; addr++; len--;
  }
  if (len == 0) return true;
  if (!s_img.seek(addr + 20)) return false;
  return (int)s_img.read(out, len) == len;
}
static uint16_t crc16Update(uint16_t v, const uint8_t* p, int n) {
  for (int i = 0; i < n; i++) {
    v ^= (uint16_t)p[i] << 8;
    for (int k = 0; k < 8; k++) v = (v & 0x8000) ? (uint16_t)((v << 1) ^ 0x1021) : (uint16_t)(v << 1);
  }
  return v;
}
static void fwDecipher(uint8_t* p, int n) {
  uint16_t key = 0xFFFF;
  for (int i = 0; i < n; i++) {
    p[i] ^= (uint8_t)key;
    key = (key & 0x8000) ? (uint16_t)((key << 1) ^ 0x1021) : (uint16_t)(key << 1);
  }
}
static uint32_t leU(const uint8_t* p, int n) { uint32_t v = 0; for (int i = 0; i < n; i++) v |= (uint32_t)p[i] << (8 * i); return v; }

// パッケージの検査（FM1Kit の FirmwareContainer と同じ）：見出しと目次のチェックサム、対象チップ、各部品のチェックサム。
// 壊れたファイルや別機種用を、FM-1 に何か送る前に弾く。
static bool fwscCheck(char* err, int errSize) {
  uint8_t h[64];
  if (!imgRead(0, h, 64)) { snprintf(err, errSize, "file read FAIL"); return false; }
  fwDecipher(h, 64);
  int count = (int)leU(h + 8, 2);
  if (leU(h, 2) != crc16Update(0, h + 2, 62) || leU(h + 4, 4) != s_imgLen || count < 1 || count > 12) { snprintf(err, errSize, "bad header"); return false; }
  if (memcmp(h + 16, "AC791N", 7) != 0) { snprintf(err, errSize, "wrong chip"); return false; }
  uint32_t tableEnd = 64 + count * 80;
  static uint8_t table[12 * 80];
  if (tableEnd > s_imgLen || !imgRead(64, table, count * 80)) { snprintf(err, errSize, "bad table"); return false; }
  if (leU(h + 2, 2) != crc16Update(0, table, count * 80)) { snprintf(err, errSize, "bad table crc"); return false; }
  int flashes = 0, loaders = 0;
  s_appStart = 0; s_appEnd = 0;
  for (int i = 0; i < count; i++) {
    uint8_t* e = table + i * 80;
    fwDecipher(e, 80);
    uint32_t type = leU(e, 2), sum = leU(e + 4, 2), off = leU(e + 8, 4), size = leU(e + 12, 4), stored = leU(e + 16, 4), enc = leU(e + 24, 4);
    if (off < tableEnd || stored < size || off + size > s_imgLen || (enc != 0 && enc != stored)) { snprintf(err, errSize, "bad part %d", i); return false; }
    if (enc == 0) {
      uint16_t v = 0;
      static uint8_t buf[512];
      for (uint32_t done = 0; done < size;) {
        int n = (int)min((uint32_t)sizeof(buf), size - done);
        if (!imgRead(off + done, buf, n)) { snprintf(err, errSize, "file read FAIL"); return false; }
        v = crc16Update(v, buf, n);
        done += n;
      }
      if (v != sum) { snprintf(err, errSize, "part %d damaged", i); return false; }
    }
    if (type == 0) {
      flashes++;
      if (enc != 0 || size < 16384 || size > 1044480 || size % 4096 != 0) { snprintf(err, errSize, "bad fw image"); return false; }
      s_appStart = (size > 0x4000) ? off + 0x4000 : off;
      s_appEnd = off + size;
    } else if (type == 100) {
      loaders++;
      if (enc != 0 || size < 1 || size >= 32768) { snprintf(err, errSize, "bad loader"); return false; }
    }
  }
  if (flashes == 0) { snprintf(err, errSize, "installer pkg"); return false; }   // ローダーだけのパッケージ（m-bootloader など）は対象外
  if (flashes != 1 || loaders != 1) { snprintf(err, errSize, "bad parts"); return false; }
  return true;
}

// 送信：前の送信が終わるのを待ってから SysEx を 1 通送る
static bool fm1SendSysex(const uint8_t* msg, int len) {
  uint32_t t = millis();
  while (s_outBusy && millis() - t < 1000) fm1PumpFor(2);
  if (s_outBusy || !s_xout || s_devGone) return false;
  return midiSendSysex(s_xout, msg, len);
}
static bool fm1SendUpgrade() {
  static const uint8_t cmd[] = {0xF0, 0x22, 0x24, 0x35, 0x7F, 0xF7};   // 更新開始
  return fm1SendSysex(cmd, sizeof(cmd));
}
// 読み取り依頼への返事：00 59 30 | 長さ+8 (3) | 種別 | 番地 (4) | 長さ (3) | データ | チェックサム を 7 ビット詰めして送る
static bool fm1SendReadReply(uint8_t flashType, uint32_t addr, const uint8_t* data, int len) {
  uint8_t* b = s_txBody;
  b[0] = 0x00; b[1] = 0x59; b[2] = 0x30;
  uint32_t bl = (uint32_t)len + 8;
  b[3] = bl & 0xFF; b[4] = (bl >> 8) & 0xFF; b[5] = (bl >> 16) & 0xFF;
  b[6] = flashType;
  for (int i = 0; i < 4; i++) b[7 + i] = (addr >> (8 * i)) & 0xFF;
  for (int i = 0; i < 3; i++) b[11 + i] = ((uint32_t)len >> (8 * i)) & 0xFF;
  if (data != b + 14) memcpy(b + 14, data, len);
  uint8_t sum = 0;
  for (int i = 6; i < 14 + len; i++) sum += b[i];
  b[14 + len] = (uint8_t)~sum;
  s_txSysex[0] = 0xF0;
  int n = sevenBitPack(b, 15 + len, s_txSysex + 1);
  s_txSysex[1 + n] = 0xF7;
  return fm1SendSysex(s_txSysex, n + 2);
}
// 受信した SysEx が読み取り依頼なら、種別・番地・長さを取り出す
static bool parseReadRequest(uint8_t* flashType, uint32_t* addr, uint32_t* len) {
  int L = s_rxLen;
  if (L < 4 || s_rxSysex[0] != 0xF0 || s_rxSysex[L - 1] != 0xF7) return false;
  uint8_t b[96];
  int n = sevenBitUnpack(s_rxSysex + 1, L - 2, b);
  if (n != 15 || b[0] != 0x00 || b[1] != 0x59 || b[2] != 0x30) return false;
  uint8_t sum = 0;
  for (int i = 6; i < 14; i++) sum += b[i];
  if ((uint8_t)~sum != b[14]) return false;
  *flashType = b[6]; *addr = leU(b + 7, 4); *len = leU(b + 11, 3);
  return true;
}
#define FM1_STAGE_FINISH 0xE0000000UL
#define FM1_WRITE_FINISH 0xF0000000UL
enum ServeResult { SV_FINISHED = 0, SV_STALLED, SV_LOST, SV_BAD };

// ---- ネットから .fwsc を取る ----
// 証明書を確かめた https で一時ファイルへ落とし、FM-1 用パッケージとして検査に通ったものだけを置き換える。
// 取れたファイルの SHA-256 を msg に入れるので、作者が公表している値と見比べられる。
static bool fetchCart(const String& name, const String& url, String& msg) {
  char path[40];
  if (!cartPathFromName(name + ".fwsc", path)) { msg = "名前が使えません"; return false; }
  if (!url.startsWith("https://")) { msg = "URL は https:// で始まるものだけ使えます"; return false; }
  if (WiFi.status() != WL_CONNECTED) { msg = "ルーターにつながっていません"; return false; }
  if (!LittleFS.exists(path) && fsCartCount() >= MAX_CARTS) { msg = "これ以上置けません。先にどれか削除してください"; return false; }

  NetworkClientSecure client;
  client.useBuiltinCACertBundle();   // 内蔵の認証局一覧でサーバー証明書を確かめる
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);   // GitHub のリリースは別ホストへ転送される
  http.setUserAgent("watchy-cart/" FW_VERSION);
  http.setConnectTimeout(15000);
  http.setTimeout(15000);
  if (!http.begin(client, url)) { msg = "URL を開けません"; return false; }
  int code = http.GET();
  if (code != 200) { msg = String("取得できません（HTTP ") + code + "）"; http.end(); return false; }
  int total = http.getSize();   // 不明なら -1
  uint32_t freeBytes = LittleFS.totalBytes() - LittleFS.usedBytes();
  if (total > 2097152 || (total > 0 && (uint32_t)total + 8192 > freeBytes)) { msg = "大きすぎるか、空き容量が足りません"; http.end(); return false; }

  LittleFS.remove("/dl.tmp");
  File out = LittleFS.open("/dl.tmp", "w");
  if (!out) { msg = "一時ファイルを作れません"; http.end(); return false; }
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  NetworkClient* in = http.getStreamPtr();
  static uint8_t buf[1024];
  uint32_t got = 0, lastData = millis();
  bool ok = true;
  while ((total < 0 || got < (uint32_t)total) && (http.connected() || in->available())) {
    int n = in->available();
    if (n <= 0) {
      if (millis() - lastData > 15000) { ok = false; msg = "途中で止まりました"; break; }
      delay(5);
      continue;
    }
    n = in->readBytes(buf, min(n, (int)sizeof(buf)));
    if (n <= 0) continue;
    if (got + n > 2097152 || out.write(buf, n) != (size_t)n) { ok = false; msg = "書き込めません（空き容量不足？）"; break; }
    mbedtls_sha256_update(&sha, buf, n);
    got += n;
    lastData = millis();
  }
  uint8_t digest[32];
  mbedtls_sha256_finish(&sha, digest);
  mbedtls_sha256_free(&sha);
  out.close();
  http.end();
  if (ok && total > 0 && got != (uint32_t)total) { ok = false; msg = "途中で切れました"; }

  // 検査：FM-1 用の製品名か、見出し・目次・各部品のチェックサムが合うか
  char product[14] = "", err[24] = "";
  if (ok) {
    s_img = LittleFS.open("/dl.tmp", "r");
    ok = s_img && s_img.size() > FWSC_HEADER_LEN && readFwscProduct(s_img, product, sizeof(product)) && strncmp(product, "FM-1_", 5) == 0;
    if (ok) { s_imgLen = s_img.size() - 20; ok = fwscCheck(err, sizeof(err)); }
    if (s_img) s_img.close();
    if (!ok) msg = String("FM-1 用の .fwsc として検査に通りません") + (err[0] ? String("（") + err + "）" : String(""));
  }
  if (!ok) { LittleFS.remove("/dl.tmp"); return false; }
  LittleFS.remove(path);
  if (!LittleFS.rename("/dl.tmp", path)) { msg = "置き換えに失敗しました"; LittleFS.remove("/dl.tmp"); return false; }
  char hex[65];
  for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
  msg = String("取得しました：") + (path + 1) + "（" + product + "、" + String(got / 1024) + " KB）SHA-256 " + hex;
  return true;
}
static void webSrcAdd() {
  String name = s_web->arg("n"), url = s_web->arg("u");
  name.trim(); url.trim();
  char path[40];
  String sn[MAX_SOURCES], su[MAX_SOURCES];
  int sc = loadSources(sn, su);
  if (!cartPathFromName(name + ".fwsc", path) || !url.startsWith("https://")) s_upMsg = "名前と、https:// で始まる URL を入れてください";
  else {
    String clean = String(path + 1);
    clean.remove(clean.length() - 5);   // 保存名と同じ形（英数字と . - _、24 文字まで）にそろえる
    int at = -1;
    for (int i = 0; i < sc; i++) if (sn[i] == clean) at = i;
    if (at < 0 && sc >= MAX_SOURCES) s_upMsg = String("取得元は ") + MAX_SOURCES + " 個までです";
    else {
      if (at < 0) at = sc++;
      sn[at] = clean; su[at] = url;
      s_upMsg = saveSources(sn, su, sc) ? String("取得元を登録しました：") + clean : String("保存に失敗しました");
    }
  }
  webRedirectHome();
}
static void webSrcDel() {
  String name = s_web->arg("n");
  String sn[MAX_SOURCES], su[MAX_SOURCES];
  int sc = loadSources(sn, su), k = 0;
  for (int i = 0; i < sc; i++) if (sn[i] != name) { sn[k] = sn[i]; su[k] = su[i]; k++; }
  saveSources(sn, su, k);
  s_upMsg = String("取得元を消しました：") + htmlEsc(name) + "（置いてあるファイルは残ります）";
  webRedirectHome();
}
static void webSrcGet() {
  String name = s_web->arg("n");
  String sn[MAX_SOURCES], su[MAX_SOURCES];
  int sc = loadSources(sn, su);
  s_upMsg = "その取得元は登録されていません";
  for (int i = 0; i < sc; i++) if (sn[i] == name) { String m; fetchCart(sn[i], su[i], m); s_upMsg = m; }
  webRedirectHome();
}

class CartWatchy : public Watchy {
public:
  CartWatchy(const watchySettings& s) : Watchy(s) {}

  // ---- 画面 ----
  // Watchy 公式の標準フェイス（7_SEG）と同じ配置。天気の欄だけ、版番号とカートリッジ数に置き換えている
  void drawWatchFace() override {
    display.fillScreen(GxEPD_BLACK);
    display.setTextColor(GxEPD_WHITE);
    // 時刻
    display.setFont(&DSEG7_Classic_Bold_53);
    display.setCursor(5, 53 + 5);
    if (currentTime.Hour < 10) display.print("0");
    display.print(currentTime.Hour);
    display.print(":");
    if (currentTime.Minute < 10) display.print("0");
    display.println(currentTime.Minute);
    // 曜日・月・日・年
    display.setFont(&Seven_Segment10pt7b);
    int16_t x1, y1; uint16_t w, h;
    String dayOfWeek = dayStr(currentTime.Wday);
    display.getTextBounds(dayOfWeek, 5, 85, &x1, &y1, &w, &h);
    if (currentTime.Wday == 4) w = w - 5;
    display.setCursor(85 - w, 85);
    display.println(dayOfWeek);
    String month = monthShortStr(currentTime.Month);
    display.getTextBounds(month, 60, 110, &x1, &y1, &w, &h);
    display.setCursor(85 - w, 110);
    display.println(month);
    display.setFont(&DSEG7_Classic_Bold_25);
    display.setCursor(5, 120);
    if (currentTime.Day < 10) display.print("0");
    display.println(currentTime.Day);
    display.setCursor(5, 150);
    display.println(tmYearToCalendar(currentTime.Year));
    // 歩数
    if (currentTime.Hour == 0 && currentTime.Minute == 0) sensor.resetStepCounter();
    display.drawBitmap(10, 165, steps, 19, 23, GxEPD_WHITE);
    display.setCursor(35, 190);
    display.println(sensor.getCounter());
    // 電池（3 段階）と Wi-Fi 登録の有無
    display.drawBitmap(158, 73, battery, 37, 21, GxEPD_WHITE);
    display.fillRect(163, 78, 27, 11, GxEPD_BLACK);
    float vbat = getBatteryVoltage();
    int level = vbat > 4.0 ? 3 : vbat > 3.6 ? 2 : vbat > 3.2 ? 1 : 0;
    for (int i = 0; i < level; i++) display.fillRect(163 + i * 9, 78, 7, 11, GxEPD_WHITE);
    display.drawBitmap(116, 75, s_wifiOk ? wifi : wifioff, 26, 18, GxEPD_WHITE);
    // USB から給電されている間は充電マーク（公式フェイスと同じ位置。抜き差しでライブラリが起こして描き直す）
    if (USB_PLUGGED_IN) display.drawBitmap(140, 75, charge, 16, 18, GxEPD_WHITE);
    // 天気の欄の代わり：版番号とカートリッジ数
    display.setFont(&FreeMonoBold9pt7b);
    display.setCursor(104, 122); display.print("cart");
    display.setCursor(104, 142); display.print(FW_VERSION);
    // 直近の再起動が異常（パニック・ウォッチドッグ・電圧低下など）だったら、理由の番号を出す（1=電源投入 2=外部 3=自分で再起動 は正常）
    if (s_bootMagic == PROBE_LOG_MAGIC && s_lastResetReason > 3 && s_lastResetReason != 8) { display.print(" r"); display.print((int)s_lastResetReason); }
    display.setCursor(104, 166); display.print(s_cartCount); display.print(" carts");
    display.setCursor(104, 190); display.print("UP:menu");
  }

  // ---- ホーム：上半分がカートリッジ（線画）、下に設定のカテゴリ ----
  void drawHome() {
    display.fillScreen(GxEPD_WHITE);
    const bool sel = (homeCursor == 0);
    const uint16_t fg = sel ? GxEPD_WHITE : GxEPD_BLACK;
    if (sel) display.fillRect(0, 0, 200, 110, GxEPD_BLACK);
    // カートリッジの外形、つまみの溝、ラベル
    display.drawRoundRect(30, 6, 140, 98, 6, fg);
    for (int y = 12; y <= 20; y += 4) display.drawFastHLine(62, y, 76, fg);
    display.drawRect(40, 26, 120, 52, fg);
    display.setTextColor(fg);
    display.setFont(&FreeMonoBold24pt7b);
    display.setCursor(44, 64);
    display.print("FM-1");
    display.setFont(&FreeMonoBold9pt7b);
    char cnt[16];
    snprintf(cnt, sizeof(cnt), "%d carts", (int)s_cartCount);
    display.setCursor(100 - (int)strlen(cnt) * 11 / 2, 97);
    display.print(cnt);
    // カテゴリ
    // カテゴリ（右端に > ）と、最後に USB mode
    const int rowH = 21, top = 128;
    for (int i = 0; i <= CAT_COUNT; i++) {
      int y = top + i * rowH;
      if (homeCursor == i + 1) { display.fillRect(0, y - 15, 200, rowH, GxEPD_BLACK); display.setTextColor(GxEPD_WHITE); }
      else display.setTextColor(GxEPD_BLACK);
      display.setCursor(12, y);
      display.print(i < CAT_COUNT ? CAT_NAMES[i] : "USB mode");
      if (i < CAT_COUNT) { display.setCursor(178, y); display.print(">"); }
    }
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(8, 196);
    display.print("DN:open both:exit");
  }

  // 一覧の共通部分：見出しと下の案内
  void listFrame(const char* title) {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    display.setFont(&FreeMonoBold9pt7b);
    display.setCursor(8, 20);
    display.print(title);
    display.drawFastHLine(0, 26, 200, GxEPD_BLACK);
    display.setCursor(8, 192);
    display.print("DN:go UP:back");
  }
  void listRow(int r, bool selected, const char* text) {
    const int rowH = 20, top = 46;
    int y = top + r * rowH;
    if (selected) { display.fillRect(0, y - 16, 200, rowH, GxEPD_BLACK); display.setTextColor(GxEPD_WHITE); }
    else display.setTextColor(GxEPD_BLACK);
    display.setCursor(8, y);
    display.print(text);
    display.setTextColor(GxEPD_BLACK);
  }

  // ---- カートリッジの一覧（7 行。多いときはカーソルが見える位置まで送る）----
  void drawCarts() {
    char title[24];
    snprintf(title, sizeof(title), "CARTRIDGE (%d)", (int)s_cartCount);
    listFrame(title);
    if (s_cartCount == 0) {
      display.setCursor(8, 70);  display.print("no carts yet");
      display.setCursor(8, 95);  display.print("add: WiFi / Net");
      return;
    }
    const int vis = 7, rows = s_cartCount;
    int first = 0;
    if (rows > vis) {
      first = cursor - 3;
      if (first < 0) first = 0;
      if (first > rows - vis) first = rows - vis;
    }
    for (int r = 0; r < vis && first + r < rows; r++) {
      char name[18];
      cartLabel(first + r, name, sizeof(name));
      listRow(r, first + r == cursor, name);
    }
  }

  // ---- カテゴリの中身 ----
  void drawSub() {
    listFrame(CAT_NAMES[subCat]);
    for (int i = 0; i < CAT_SIZES[subCat]; i++) listRow(i, i == cursor, CAT_ITEMS[subCat][i]);
  }

  void drawConfirm() {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    display.setFont(&FreeMonoBold9pt7b);
    display.setCursor(8, 30);  display.print("Flash this?");
    display.drawRect(4, 44, 192, 74, GxEPD_BLACK);
    if (cursor < s_cartCount) {
      char name[18]; cartLabel(cursor, name, sizeof(name));
      display.setCursor(8, 64);  display.print(name);
      display.setCursor(8, 86);  display.print(s_carts[cursor].product);
      display.setCursor(8, 108); display.print(s_carts[cursor].size / 1024); display.print(" KB");
      if (isOpenProduct(s_carts[cursor].product)) { display.setCursor(8, 134); display.print("! one-way (Open)"); }
    }
    display.setCursor(8, 150); display.print("hold UP  : NO");
    display.setCursor(8, 172); display.print("hold DOWN: YES");
  }

  // 公式 showWatchFace() と同じ手順で描く
  void render(bool full = false) {
    display.setFullWindow();
    display.epd2.asyncPowerOn();
    switch (screen) {
      case SCR_CLOCK:   RTC.read(currentTime); drawWatchFace(); break;
      case SCR_HOME:    drawHome(); break;
      case SCR_CARTS:   drawCarts(); break;
      case SCR_SUB:     drawSub(); break;
      case SCR_CONFIRM: drawConfirm(); break;
    }
    bool partial = !full && (partialCount < FULL_REFRESH_EVERY);
    display.display(partial);
    partialCount = partial ? partialCount + 1 : 0;
    // 時計以外の画面では、毎分のタイマー起床で公式が時計を上書きしないようにする
    guiState = (screen == SCR_CLOCK) ? WATCHFACE_STATE : APP_STATE;
  }

  // ---- USB モード：寝ずに待機。IDE の自動リセットで書き込める ----
  void usbServiceMode() {
    display.setFullWindow();
    display.epd2.asyncPowerOn();
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    display.setFont(&FreeMonoBold9pt7b);
    display.setCursor(10, 90);  display.print("USB mode");
    display.setCursor(10, 115); display.print("upload from IDE");
    display.setCursor(10, 150); display.print("hold DOWN: exit");
    display.display(false);
    HWCDCSerial.begin(115200);
    pinMode(DOWN_BTN_PIN, INPUT_PULLUP);
    waitRelease();
    uint32_t lastDump = 0;
    while (true) {
      // 前回の probe の記録を 3 秒ごとに自動で出力する（送信操作は不要。受信しても出力）
      if (HWCDCSerial.available() || millis() - lastDump >= 3000) {
        lastDump = millis();
        while (HWCDCSerial.available()) HWCDCSerial.read();
        HWCDCSerial.printf("alive %lu  (watchy-cart " FW_VERSION ")\n", (unsigned long)(millis() / 1000));
        HWCDCSerial.printf("boots: %lu  last reset reason: %lu\n", (unsigned long)s_bootCount, (unsigned long)s_lastResetReason);
        HWCDCSerial.printf("carts: %d\n", (int)s_cartCount);
        for (int i = 0; i < s_cartCount; i++) HWCDCSerial.printf("  %s  %s  %lu bytes\n", s_carts[i].file, s_carts[i].product, (unsigned long)s_carts[i].size);
        HWCDCSerial.println("---- probe log ----");   // probe と書き込みの記録
        if (s_logMagic == PROBE_LOG_MAGIC && s_logLen < sizeof(s_log)) { s_log[s_logLen] = 0; HWCDCSerial.print(s_log); }
        else HWCDCSerial.println("(none)");
        HWCDCSerial.println("---- end ----");
      }
      if (isDown(DOWN_BTN_PIN) && measureLongPress(DOWN_BTN_PIN)) {
        waitRelease();
        screen = SCR_CLOCK; cursor = 0;
        render(true);
        deepSleep();
      }
      delay(20);
    }
  }

  // ---- Bootloader：ROM の書き込み待ち（ダウンロードモード）へ強制的に入る ----
  // スケッチが USB mode に入れない状態になっても、ここまで来られれば IDE から書き直せる。
  // この状態ではボタンも画面も動かない。書き込み後に自動で再起動しないことがあるので、そのときは電池を抜き差しする。
  void enterBootloader() {
    // 入ったら Watchy 側からは出られない（書き込みが終わるか、電池を抜くまで）。誤って入らないよう、もう一度確かめる
    showLines("BOOTLOADER?", "no exit except", "upload or battery", "", "DN:enter UP:cancel");
    pinMode(UP_BTN_PIN, INPUT_PULLUP);
    pinMode(DOWN_BTN_PIN, INPUT_PULLUP);
    waitRelease();
    bool go = false;
    uint32_t t = millis();
    while (millis() - t < 20000) {   // 20 秒何もしなければ取り消し
      if (isDown(DOWN_BTN_PIN) && measureLongPress(DOWN_BTN_PIN)) { go = true; break; }
      if (isDown(UP_BTN_PIN) && measureLongPress(UP_BTN_PIN)) break;
      delay(20);
    }
    waitRelease();
    if (!go) { render(true); return; }   // 元の一覧に戻る
    showLines("BOOTLOADER", "upload from IDE.", "if it stays here:", "replug battery", "");
    display.hibernate();
    CLEAR_PERI_REG_MASK(RTC_CNTL_USB_CONF_REG, RTC_CNTL_SW_HW_USB_PHY_SEL | RTC_CNTL_SW_USB_PHY_SEL);
    SET_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_DP_PULLUP | USB_SERIAL_JTAG_USB_PAD_ENABLE);
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);   // 次の再起動を書き込み待ちで始める
    esp_restart();
  }

  // ---- 文字だけの画面 ----
  void showLines(const char* l1, const char* l2, const char* l3, const char* l4, const char* foot, bool partial = false) {
    display.setFullWindow();
    display.epd2.asyncPowerOn();
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    display.setFont(&FreeMonoBold9pt7b);
    display.setCursor(8, 30);  display.print(l1);
    display.drawFastHLine(0, 38, 200, GxEPD_BLACK);
    display.setCursor(8, 70);  display.print(l2);
    display.setCursor(8, 95);  display.print(l3);
    display.setCursor(8, 120); display.print(l4);
    display.setCursor(8, 185); display.print(foot);
    display.display(partial);
  }

  // ---- WiFi upload：ブラウザから .fwsc を置く／消す、ルーターを登録する ----
  // ルーターが登録済みならそこへつなぐ（つながったら時刻も合わせる）。未登録・つながらないときは Watchy が親機になる。
  // 終わったら再起動して一覧を読み直す。電池を食うので 10 分で自動終了。
  void wifiUploadMode(bool tryRouter) {
    display.epd2.setBusyCallback(0);   // 画面書き換え中のスリープを無効に（スリープすると Wi-Fi が切れる）
    pinMode(UP_BTN_PIN, INPUT_PULLUP);
    pinMode(DOWN_BTN_PIN, INPUT_PULLUP);
    bool fsOk = LittleFS.begin(true);
    String ssid, pass;
    char l2[24] = "", l3[24] = "", l4[24] = "";
    s_staMode = false;
    bool netOk = false;
    if (tryRouter && !loadWifi(ssid, pass)) {
      showLines("WiFi router", "not set yet", "use WiFi direct", "to register", "");
      delay(2500);
    } else if (tryRouter) {
      snprintf(l2, sizeof(l2), "%.17s", ssid.c_str());
      showLines("WiFi: joining", l2, "", "", "hold DOWN: skip");
      waitRelease();
      s_wifiReason = 0;
      WiFi.onEvent(wifiEventCb);
      WiFi.mode(WIFI_STA);
      WiFi.begin(ssid.c_str(), pass.c_str());
      uint32_t t = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - t < 20000) {
        if (isDown(DOWN_BTN_PIN) && measureLongPress(DOWN_BTN_PIN)) break;   // 親機モードへ切り替える
        delay(50);
      }
      waitRelease();
      if (WiFi.status() == WL_CONNECTED) {
        s_staMode = true; netOk = true;
        setWifiOk(true);
        bool ntp = syncNTP(9 * 3600);   // 日本時間で時計を合わせる
        MDNS.begin("watchy-cart");
        snprintf(l3, sizeof(l3), "%s", WiFi.localIP().toString().c_str());
        snprintf(l4, sizeof(l4), ntp ? "clock synced" : "clock: no sync");
        showLines("WiFi: router", l2, l3, l4, "hold DOWN: exit");
      } else {
        // つながらなかった理由を見せる：その名前の電波が 2.4GHz で見えているかどうかで、原因が分かれる
        int reason = s_wifiReason;
        setWifiOk(false);
        WiFi.disconnect();
        scanSeen();
        if (!isSeen(ssid)) {
          snprintf(l4, sizeof(l4), "(%d others seen)", s_seenCount);
          showLines("ROUTER NOT FOUND", l2, "not on 2.4GHz or", "name is wrong", l4);
        } else {
          snprintf(l4, sizeof(l4), "code %d, pw %d ch", reason, (int)pass.length());
          showLines("JOIN REFUSED", l2, "found, but", "password wrong?", l4);
        }
        WiFi.disconnect(true);
        delay(7000);
        showLines("WiFi", "switching to", "direct mode..", "", "");
      }
    }
    if (!s_staMode) {
      if (s_seenCount < 0) { showLines("WiFi: direct", "looking for", "nearby WiFi..", "", ""); scanSeen(); }   // 登録ページの候補用
      WiFi.mode(WIFI_AP);
      netOk = WiFi.softAP("watchy-cart");   // パスワードなし
      showLines("WiFi: direct", "watchy-cart", "(no password)", "192.168.4.1", "hold DOWN: exit");
      s_staMode = false;
    }
    if (!fsOk || !netOk) showLines("WiFi upload", fsOk ? "storage ok" : "storage FAIL", netOk ? "wifi ok" : "wifi FAIL", "", "hold DOWN: exit");

    WebServer web(80);
    s_web = &web;
    s_upMsg = "";
    web.on("/", HTTP_GET, webSendPage);
    web.on("/upload", HTTP_POST, webRedirectHome, webUploadChunk);
    web.on("/delete", HTTP_POST, webDelete);
    web.on("/wifi", HTTP_POST, webWifi);
    web.on("/time", HTTP_POST, webTime);
    web.on("/src-add", HTTP_POST, webSrcAdd);
    web.on("/src-del", HTTP_POST, webSrcDel);
    web.on("/src-get", HTTP_POST, webSrcGet);
    web.onNotFound(webRedirectHome);
    if (fsOk && netOk) web.begin();

    waitRelease();
    uint32_t t0 = millis();
    while (millis() - t0 < 600000UL) {
      web.handleClient();
      if (isDown(DOWN_BTN_PIN) && measureLongPress(DOWN_BTN_PIN)) break;
      delay(2);
    }
    web.stop();
    s_web = NULL;
    if (s_staMode) { MDNS.end(); WiFi.disconnect(true); } else WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    LittleFS.end();
    waitRelease();   // DOWN を押したまま再起動すると USB mode に入ってしまうので、離すまで待つ
    ESP.restart();   // 一覧を読み直して時計画面から始まる
  }

  // ---- Fetch latest：ルーターへつなぎ、登録した取得元を順に取り直す。終わったら再起動して一覧を読み直す ----
  void fetchAllMode() {
    display.epd2.setBusyCallback(0);   // 画面書き換え中のスリープを無効に（スリープすると Wi-Fi が切れる）
    pinMode(UP_BTN_PIN, INPUT_PULLUP);
    pinMode(DOWN_BTN_PIN, INPUT_PULLUP);
    char l2[24] = "", l3[24] = "", l4[24] = "";
    String ssid, pass;
    String sn[MAX_SOURCES], su[MAX_SOURCES];
    int okCount = 0, sc = 0;
    if (!LittleFS.begin(true)) snprintf(l2, sizeof(l2), "storage FAIL");
    else if ((sc = loadSources(sn, su)) == 0) { snprintf(l2, sizeof(l2), "no sources set"); snprintf(l3, sizeof(l3), "add: WiFi direct"); }
    else if (!loadWifi(ssid, pass)) { snprintf(l2, sizeof(l2), "no router set"); snprintf(l3, sizeof(l3), "add: WiFi direct"); }
    else {
      snprintf(l2, sizeof(l2), "%.17s", ssid.c_str());
      showLines("Fetch: joining", l2, "", "", "");
      WiFi.mode(WIFI_STA);
      WiFi.begin(ssid.c_str(), pass.c_str());
      uint32_t t = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - t < 15000) delay(50);
      if (WiFi.status() != WL_CONNECTED) { snprintf(l2, sizeof(l2), "router: no link"); setWifiOk(false); }
      else {
        setWifiOk(true);
        syncNTP(9 * 3600);   // 時計合わせ（証明書の有効期限の確認にも現在時刻が要る）
        for (int i = 0; i < sc; i++) {
          snprintf(l2, sizeof(l2), "%d/%d %.12s", i + 1, sc, sn[i].c_str());
          snprintf(l3, sizeof(l3), "ok so far: %d", okCount);
          showLines("Fetch latest", l2, "downloading..", l3, "", i > 0);
          String m;
          bool ok = fetchCart(sn[i], su[i], m);
          if (ok) okCount++;
          else snprintf(l4, sizeof(l4), "fail: %.16s", sn[i].c_str());   // 最後に失敗したもの
        }
        snprintf(l2, sizeof(l2), "done: %d / %d ok", okCount, sc);
        l3[0] = 0;
      }
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
    }
    LittleFS.end();
    vibMotor(75, 4);
    showLines("Fetch latest", l2, l3, l4, "press DOWN: exit");
    waitRelease();
    uint32_t t1 = millis();
    while (!isDown(DOWN_BTN_PIN) && millis() - t1 < 120000) delay(20);
    waitRelease();
    ESP.restart();   // 一覧を読み直して時計画面から始まる
  }

  // ---- FM-1 probe：USB ホストになり、つながった機器の VID/PID と MIDI 対応を表示 ----
  // ホスト動作中は USB シリアルが使えないので、結果は画面に出す。終わったら再起動して元に戻す。
  void fm1Probe() {
    showLines("FM-1 probe", "connect FM-1 now", "waiting 20s...", "", "");
    // 公式ライブラリは画面の書き換え待ちの間 CPU を軽いスリープに入れる。その間 USB ホストが止まり、
    // FM-1 との通信が切れる（v0.18〜0.19 で、画面を書き換えた直後から送受信が全滅した原因）。USB を使う間は無効にする。
    display.epd2.setBusyCallback(0);
    char l2[24] = "", l3[24] = "", l4[24] = "";
    char id1[32] = "", id2[32] = "";

    s_logMagic = PROBE_LOG_MAGIC; s_logLen = 0; s_log[0] = 0; s_logLines = 0;
    s_enumCbFired = false; s_stopLogs = 0;
    probeLogAppend("probe " FW_VERSION "\n");
    pinMode(UP_BTN_PIN, INPUT_PULLUP);
    pinMode(DOWN_BTN_PIN, INPUT_PULLUP);

    if (!fm1Connect(20000, l3, sizeof(l3))) {
      snprintf(l2, sizeof(l2), "connect failed");
      snprintf(l4, sizeof(l4), "errlog:%d", (int)s_logLines);
    } else {
      snprintf(l2, sizeof(l2), "VID:PID %04X:%04X", s_vid, s_pid);
      if (!fm1Identity(id1, sizeof(id1))) {
        snprintf(l3, sizeof(l3), "no identity reply");
      } else {
        snprintf(l3, sizeof(l3), "%.22s", id1);
        // ---- 送受信が途切れる原因の切り分け（v0.18 で、最初の応答の直後に受信が止まった）----
        // 何もしない 1 秒・振動・画面の書き換えのそれぞれの後に問い合わせて、どこで失敗が出るかを記録する。
        char tmp[32];
        fm1PumpFor(1000);
        bool a = fm1Identity(tmp, sizeof(tmp));
        probeLogAppend("%lu A idle:%d inErr:%d outErr:%d\n", (unsigned long)millis(), (int)a, (int)s_inErrCount, (int)s_outErrCount);
        vibMotor(75, 2);
        fm1PumpFor(300);
        bool b2 = fm1Identity(tmp, sizeof(tmp));
        probeLogAppend("%lu B vib:%d inErr:%d outErr:%d\n", (unsigned long)millis(), (int)b2, (int)s_inErrCount, (int)s_outErrCount);
        showLines("Reconnect test", l3, "FM-1: power OFF", "then ON (60s)", "DOWN: skip");
        fm1PumpFor(300);
        bool c = fm1Identity(tmp, sizeof(tmp));
        probeLogAppend("%lu C draw:%d inErr:%d outErr:%d\n", (unsigned long)millis(), (int)c, (int)s_inErrCount, (int)s_outErrCount);

        // ---- つなぎ直しの試験 ----
        // FM-1 の電源を切って入れ直してもらう。1 秒ごとに問い合わせ、2 回続けて返事がなければ「外れた」と見なして、
        // ホストを作り直しながら、もう一度名乗りが取れるまで試す（書き込み中の再起動と同じ流れ）。
        waitRelease();
        bool skipped = false, lost = false;
        int polls = 0, misses = 0;
        uint32_t t = millis();
        while (millis() - t < 60000) {
          for (int i = 0; i < 50 && !skipped; i++) { fm1PumpFor(20); if (isDown(DOWN_BTN_PIN)) skipped = true; }
          if (skipped) break;
          polls++;
          s_quietHandshake = true;
          bool alive = fm1Identity(tmp, sizeof(tmp));
          s_quietHandshake = false;
          misses = alive ? 0 : misses + 1;
          if (misses >= 2) { lost = true; break; }
        }
        probeLogAppend("lost:%d after %lums (polls %d) gone:%d skipped:%d HPRT=%05lx\n", (int)lost, (unsigned long)(millis() - t),
                       polls, (int)s_devGone, (int)skipped, (unsigned long)REG_READ(PROBE_HPRT_REG));
        if (skipped) snprintf(l4, sizeof(l4), "test skipped");
        else if (!lost) snprintf(l4, sizeof(l4), "no loss seen");
        else {
          vibMotor(75, 2);   // 途絶を検知した合図（1 回振動。第 2 引数は「オン・オフの切替回数」なので、奇数にするとオンのまま残る）
          fm1HostStop();
          uint32_t t2 = millis();
          bool ok = false;
          while (!ok && millis() - t2 < 40000) {
            if (!fm1Connect(40000 - (millis() - t2), l4, sizeof(l4))) break;
            ok = fm1Identity(id2, sizeof(id2));
            if (!ok) { snprintf(l4, sizeof(l4), "reconn: no reply"); fm1HostStop(); }
          }
          if (ok) snprintf(l4, sizeof(l4), "reconnect OK");
          probeLogAppend("reconnect %s after %lums\n", ok ? "OK" : "FAIL", (unsigned long)(millis() - t2));
        }
      }
    }
    probeDumpRegs("end");
    probeLogAppend("errlog:%d\n", (int)s_logLines);
    probeLogAppend("result: %s | %s | %s | id2:%s\n", l2, l3, l4, id2);
    vibMotor(75, 4);   // 結果が出たことを振動で知らせる
    showLines("FM-1 probe", l2, l3, l4, "press DOWN: exit");
    waitRelease();
    uint32_t t1 = millis();
    while (!isDown(DOWN_BTN_PIN) && millis() - t1 < 120000) delay(20);   // 2分で自動終了
    waitRelease();
    // USB の配線切替はソフト再起動では元に戻らない（RTC 領域に残る）ので、明示的にシリアル側へ戻す。
    // これをしないと、再起動後に USB mode でも IDE から見えなくなる。
    CLEAR_PERI_REG_MASK(RTC_CNTL_USB_CONF_REG, RTC_CNTL_SW_HW_USB_PHY_SEL | RTC_CNTL_SW_USB_PHY_SEL);
    SET_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_DP_PULLUP | USB_SERIAL_JTAG_USB_PAD_ENABLE);
    ESP.restart();   // 時計画面から始まる
  }

  // ---- 書き込み：FM-1 の読み取り依頼に答え続ける ----
  // finishAddr を頼まれたら完了。idleMs 黙ったら停滞、外れたら途絶。進み具合は 10% ごとに画面へ。
  ServeResult serveReads(uint32_t finishAddr, uint32_t idleMs, const char* title, const char* name, uint32_t* requests, uint32_t* maxEnd, uint32_t* lastAddr) {
    static uint8_t data[512];
    uint32_t lastAct = millis(), lastDraw = 0;
    int errBase = s_inErrCount + s_outErrCount;
    int pass = 1, shownPct = -1, shownPass = 0;
    uint32_t passMax = 0;
    *requests = 0; *maxEnd = 0; *lastAddr = 0;
    while (true) {
      if (s_devGone) return SV_LOST;
      if (s_inErrCount + s_outErrCount - errBase >= 40) return SV_LOST;   // 返事なしの失敗が続いている（約 2 秒）
      if (millis() - lastAct > idleMs) return SV_STALLED;
      if (!fm1Pump(s_rxDone, 200)) continue;
      uint8_t ft; uint32_t addr, len;
      bool isReq = parseReadRequest(&ft, &addr, &len);
      s_rxDone = false;   // 次の受信を許可
      if (!isReq) continue;
      lastAct = millis();
      errBase = s_inErrCount + s_outErrCount;
      if (addr == FM1_STAGE_FINISH || addr == FM1_WRITE_FINISH) {
        if (addr != finishAddr || len != 8) return SV_BAD;
        static const uint8_t fin[8] = {'s', 'u', 'c', 'c', 'e', 's', 's', 0};
        fm1SendReadReply(ft, addr, fin, 8);
        // 返事を送りきるまで少し待ち、同じ依頼が繰り返されたら答え直す
        uint32_t t = millis();
        while (millis() - t < 600 && !s_devGone) {
          if (!fm1Pump(s_rxDone, 600 - (millis() - t))) break;
          uint8_t ft2; uint32_t a2, l2;
          if (parseReadRequest(&ft2, &a2, &l2) && a2 == addr) fm1SendReadReply(ft2, a2, fin, 8);
          s_rxDone = false;
        }
        return SV_FINISHED;
      }
      if (len > 512 || addr + len > s_imgLen) { *lastAddr = addr; return SV_BAD; }
      if (!imgRead(addr, data, (int)len)) { *lastAddr = addr; return SV_BAD; }
      if (!fm1SendReadReply(ft, addr, data, (int)len)) return SV_LOST;
      (*requests)++;
      *lastAddr = addr;
      if (addr + len > *maxEnd) *maxEnd = addr + len;
      // ローダーによってはパッケージを 2 周読む（1 周目で検査、2 周目で書く）。大きく戻ったら次の周と数える
      if (addr + 0x20000 < passMax) { pass++; passMax = 0; }
      if (addr + len > passMax) passMax = addr + len;
      int pct = (int)((uint64_t)passMax * 100 / s_imgLen);
      if ((pct / 10 != shownPct / 10 || pass != shownPass) && millis() - lastDraw > 1500) {
        char l3[24], l4[24];
        snprintf(l3, sizeof(l3), "pass %d  %d%%", pass, pct);
        snprintf(l4, sizeof(l4), "req %lu", (unsigned long)*requests);
        showLines(title, name, l3, l4, "do not unplug", true);
        shownPct = pct; shownPass = pass; lastDraw = millis();
        lastAct = millis();   // 画面の書き換え時間は停滞に数えない
      }
    }
  }

  // ホストを作り直しながら、wantLoader に合う相手（ローダー／通常のファーム）が名乗るまで待つ
  bool findDevice(bool wantLoader, uint32_t timeoutMs, char* ident, int identSize) {
    uint32_t t0 = millis();
    char err[24];
    ident[0] = 0;
    while (millis() - t0 < timeoutMs) {
      if (!fm1Connect(timeoutMs - (millis() - t0), err, sizeof(err))) return false;
      if (fm1Identity(ident, identSize) && (strncasecmp(ident, "ota-", 4) == 0) == wantLoader) return true;
      fm1HostStop();
      delay(1000);
    }
    return false;
  }

  // ---- 書き込み本体 ----
  void flashCart(int idx) {
    char name[18];
    cartLabel(idx, name, sizeof(name));
    showLines("FLASH", name, "checking file..", "", "");
    display.epd2.setBusyCallback(0);   // USB を使う間は、画面書き換え中のスリープを無効にする（fm1Probe の説明を参照）
    char l3[24] = "", l4[24] = "";
    char dev[32] = "", product[14] = "";
    bool done = false;

    s_logMagic = PROBE_LOG_MAGIC; s_logLen = 0; s_log[0] = 0; s_logLines = 0;
    s_enumCbFired = false; s_stopLogs = 0; s_svcLogs = 0;
    probeLogAppend("flash " FW_VERSION " %s\n", s_carts[idx].file);
    pinMode(UP_BTN_PIN, INPUT_PULLUP);
    pinMode(DOWN_BTN_PIN, INPUT_PULLUP);

    do {
      // --- ファイルの検査（FM-1 にはまだ何も送らない）---
      if (!LittleFS.begin(false)) { snprintf(l3, sizeof(l3), "storage FAIL"); break; }
      s_img = LittleFS.open(String("/") + s_carts[idx].file, "r");
      if (!s_img || s_img.size() <= FWSC_HEADER_LEN) { snprintf(l3, sizeof(l3), "file open FAIL"); break; }
      s_imgLen = s_img.size() - 20;
      if (!readFwscProduct(s_img, product, sizeof(product)) || strncmp(product, "FM-1_", 5) != 0) { snprintf(l3, sizeof(l3), "not FM-1 pkg"); break; }
      if (!fwscCheck(l3, sizeof(l3))) { snprintf(l4, sizeof(l4), "file rejected"); break; }
      probeLogAppend("file ok: %s len %lu app %lx-%lx\n", product, (unsigned long)s_imgLen, (unsigned long)s_appStart, (unsigned long)s_appEnd);

      // --- 接続と相手の確認 ---
      showLines("FLASH", name, "connect FM-1..", "", "");
      if (!fm1Connect(20000, l3, sizeof(l3))) break;
      if (!fm1Identity(dev, sizeof(dev))) { snprintf(l3, sizeof(l3), "no identity reply"); break; }
      bool loader = strncasecmp(dev, "ota-", 4) == 0;
      uint32_t reqs = 0, maxEnd = 0, lastAddr = 0;

      if (!loader) {
        // 断る場合（FM1Kit と同じ判断）
        const char* us = strrchr(dev, '_');
        long ver = us ? atol(us + 1) : -1;
        if (strncmp(dev, "FM-1_", 5) != 0) { snprintf(l3, sizeof(l3), "%.22s", dev); snprintf(l4, sizeof(l4), "unsupported fw"); break; }
        if (ver >= 8000000 && ver <= 8000999) { snprintf(l3, sizeof(l3), "open firmware:"); snprintf(l4, sizeof(l4), "use PC installer"); break; }   // 事前の手続き（arm）が要る
        if (strcmp(dev, product) == 0 && (strcmp(dev, "FM-1_015") == 0 || strcmp(dev, "FM-1_016") == 0)) { snprintf(l3, sizeof(l3), "%.22s", dev); snprintf(l4, sizeof(l4), "already installed"); break; }

        // --- 1/3 下見（動作中のファームがパッケージを確かめる。ここで断られても FM-1 は元のまま）---
        showLines("FLASH 1/3 check", name, dev, "", "do not unplug");
        if (!fm1SendUpgrade()) { snprintf(l3, sizeof(l3), "send FAIL"); break; }
        ServeResult r = serveReads(FM1_STAGE_FINISH, 8000, "FLASH 1/3 check", name, &reqs, &maxEnd, &lastAddr);
        probeLogAppend("staging: result %d reqs %lu max %lx last %lx inErr %d outErr %d\n", (int)r, (unsigned long)reqs, (unsigned long)maxEnd, (unsigned long)lastAddr, (int)s_inErrCount, (int)s_outErrCount);
        if (r != SV_FINISHED) {
          snprintf(l3, sizeof(l3), r == SV_STALLED ? "FM-1 refused pkg" : r == SV_LOST ? "lost in check" : "bad request");
          snprintf(l4, sizeof(l4), "req %lu @%lx", (unsigned long)reqs, (unsigned long)lastAddr);
          break;
        }
        // --- 2/3 ローダーで再起動してくるのを待つ ---
        showLines("FLASH 2/3 loader", name, "waiting..", "", "do not unplug");
        fm1HostStop();
        delay(2000);
        if (!findDevice(true, 30000, dev, sizeof(dev))) {
          probeLogAppend("loader not found (last id: %s)\n", dev);
          snprintf(l3, sizeof(l3), "loader not found");
          snprintf(l4, sizeof(l4), "%.22s", dev);
          break;
        }
      }
      probeLogAppend("loader: %s\n", dev);
      if (strncasecmp(dev, "ota-FM-1_", 9) != 0) { snprintf(l3, sizeof(l3), "%.22s", dev); snprintf(l4, sizeof(l4), "unknown loader"); break; }

      // --- 3/3 書き込み（ここで中断すると FM-1 はローダーで待つ。もう一度この書き込みを実行すれば続きができる）---
      showLines("FLASH 3/3 write", name, dev, "", "do not unplug");
      if (!fm1SendUpgrade()) { snprintf(l3, sizeof(l3), "send FAIL"); snprintf(l4, sizeof(l4), "retry: flash again"); break; }
      ServeResult r = serveReads(FM1_WRITE_FINISH, 60000, "FLASH 3/3 write", name, &reqs, &maxEnd, &lastAddr);
      probeLogAppend("writing: result %d reqs %lu max %lx last %lx inErr %d outErr %d\n", (int)r, (unsigned long)reqs, (unsigned long)maxEnd, (unsigned long)lastAddr, (int)s_inErrCount, (int)s_outErrCount);
      // 完了を頼まずに新しいファームで再起動するローダーもあるので、ほぼ読み終えていれば完了扱いで確かめに行く
      bool nearly = s_appEnd > s_appStart && maxEnd >= s_appStart + (uint32_t)((uint64_t)(s_appEnd - s_appStart) * 95 / 100);
      if (r == SV_BAD || (r != SV_FINISHED && !nearly)) {
        snprintf(l3, sizeof(l3), r == SV_STALLED ? "write stalled" : r == SV_LOST ? "lost in write" : "bad request");
        snprintf(l4, sizeof(l4), "retry: flash again");
        break;
      }
      // --- 新しいファームで戻ってくるのを待つ ---
      showLines("FLASH", name, "FM-1 restarting", "", "do not unplug");
      fm1HostStop();
      delay(2000);
      if (!findDevice(false, 40000, dev, sizeof(dev))) {
        probeLogAppend("no return (last id: %s)\n", dev);
        snprintf(l3, sizeof(l3), dev[0] ? "still in loader" : "FM-1 not back");
        snprintf(l4, sizeof(l4), "retry: flash again");
        break;
      }
      done = true;
      snprintf(l3, sizeof(l3), "now: %.17s", dev);
      snprintf(l4, sizeof(l4), strcmp(dev, product) == 0 ? "DONE" : "DONE (id differs)");
    } while (false);

    if (s_img) s_img.close();
    probeDumpRegs("end");
    probeLogAppend("errlog:%d\n", (int)s_logLines);
    probeLogAppend("result: %s | %s | %s | pkg:%s\n", done ? "OK" : "FAIL", l3, l4, product);
    vibMotor(75, done ? 4 : 8);   // 終わりの合図（成功 2 回、失敗 4 回）
    showLines(done ? "FLASH OK" : "FLASH FAILED", name, l3, l4, "press DOWN: exit");
    waitRelease();
    uint32_t t1 = millis();
    while (!isDown(DOWN_BTN_PIN) && millis() - t1 < 300000) delay(20);   // 5分で自動終了
    waitRelease();
    CLEAR_PERI_REG_MASK(RTC_CNTL_USB_CONF_REG, RTC_CNTL_SW_HW_USB_PHY_SEL | RTC_CNTL_SW_USB_PHY_SEL);
    SET_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_DP_PULLUP | USB_SERIAL_JTAG_USB_PAD_ENABLE);
    ESP.restart();
  }

  // ---- ボタン処理 ----
  void onButton(int btn, bool longPress) {
    const bool up = (btn == UP_BTN_PIN);
    const bool ok = !up;   // 長押しの役割：DOWN = 決定、UP = 戻る
    switch (screen) {
      case SCR_CLOCK:
        if (up && longPress) { screen = SCR_HOME; homeCursor = 0; render(true); }   // ロック解除
        break;
      case SCR_HOME:
        if (longPress) {
          if (ok) {
            cursor = 0;
            if (homeCursor == CAT_COUNT + 1) usbServiceMode();   // 戻ってこない
            if (homeCursor == 0) screen = SCR_CARTS;
            else { subCat = homeCursor - 1; screen = SCR_SUB; }
            render();
          }
          // UP 長押しは何もしない（時計に戻るのは UP+DOWN の同時押し）
        } else {
          const int n = HOME_ROWS;
          homeCursor = up ? (homeCursor + n - 1) % n : (homeCursor + 1) % n;
          render();
        }
        break;
      case SCR_CARTS:
        if (longPress) {
          if (!ok) { screen = SCR_HOME; render(); }
          else if (s_cartCount > 0) { screen = SCR_CONFIRM; render(); }
        } else if (s_cartCount > 0) {
          cursor = up ? (cursor + s_cartCount - 1) % s_cartCount : (cursor + 1) % s_cartCount;
          render();
        }
        break;
      case SCR_SUB:
        if (longPress) {
          if (!ok) { screen = SCR_HOME; render(); }
          else if (subCat == 0) {
            if (cursor == 0) wifiUploadMode(true);    // 戻ってこない（再起動する）
            if (cursor == 1) wifiUploadMode(false);   // 戻ってこない（再起動する）
            if (cursor == 2) fetchAllMode();          // 戻ってこない（再起動する）
          } else {
            if (cursor == 0) fm1Probe();              // 戻ってこない（再起動する）
          }
        } else {
          const int n = CAT_SIZES[subCat];
          cursor = up ? (cursor + n - 1) % n : (cursor + 1) % n;
          render();
        }
        break;
      case SCR_CONFIRM:
        if (longPress) {
          if (ok) {
            waitRelease();
            if (cursor < s_cartCount) flashCart(cursor);   // 戻ってこない（再起動する）
            screen = SCR_CARTS; render(true);
          } else {
            screen = SCR_CARTS; render();
          }
        }
        break;
    }
  }

  // UP+DOWN 同時押し：すぐ離せば時計に戻る。3 秒押し続けたら USB mode（戻ってこない）
  void bothPressed() {
    uint32_t t = millis();
    while (isDown(UP_BTN_PIN) && isDown(DOWN_BTN_PIN)) {
      if (millis() - t >= 3000) { vibMotor(75, 2); usbServiceMode(); }
      delay(10);
    }
    if (screen != SCR_CLOCK) { screen = SCR_CLOCK; cursor = 0; render(true); }
    waitRelease();
  }

  // 公式 init() がボタン起床時に呼ぶ。ここを差し替える
  void handleButtonPress() override {
    uint64_t status = esp_sleep_get_ext1_wakeup_status();
    int btn = -1;
    if (status & UP_BTN_MASK)        btn = UP_BTN_PIN;
    else if (status & DOWN_BTN_MASK) btn = DOWN_BTN_PIN;
    // MENU / BACK は壊れているので無視
    if (btn < 0) return;
    pinMode(UP_BTN_PIN, INPUT_PULLUP);
    pinMode(DOWN_BTN_PIN, INPUT_PULLUP);
    // UP と DOWN の同時押し → 時計に戻る（押し始めのずれを 150ms まで待つ）
    // スリープ中は時計用の回路（RTC）がピンを握っているので、普通の入力ピンに戻してから読む
    rtc_gpio_deinit((gpio_num_t)UP_BTN_PIN);
    rtc_gpio_deinit((gpio_num_t)DOWN_BTN_PIN);
    pinMode(UP_BTN_PIN, INPUT_PULLUP);
    pinMode(DOWN_BTN_PIN, INPUT_PULLUP);
    delay(2);
    for (int i = 0; i < 15 && isDown(btn); i++) {
      if (bothDown()) { bothPressed(); return; }
      delay(10);
    }
    bool longPress = measureLongPress(btn);
    // 長押しの計測中にもう片方が押された場合も同時押しとして扱う
    if (bothDown()) { bothPressed(); return; }
    onButton(btn, longPress);
    waitRelease();                 // 離すまで寝ない（二重検出防止）
  }
};

watchySettings settings{
  .cityID = "", .lat = "", .lon = "",
  .weatherAPIKey = "", .weatherURL = "", .weatherUnit = "metric", .weatherLang = "en",
  .weatherUpdateInterval = 30,
  .ntpServer = "pool.ntp.org",
  .gmtOffset = 9 * 3600,
  .vibrateOClock = false,
};

CartWatchy watchy(settings);

// UP と DOWN を同時に 10 秒押し続けたら強制再起動（処理が固まったとき用。100ms ごとにタイマーから確認）
static void forceResetCheck(void*) {
  static uint32_t heldSince = 0;
  if (digitalRead(UP_BTN_PIN) == LOW && digitalRead(DOWN_BTN_PIN) == LOW) {
    if (heldSince == 0) heldSince = millis();
    else if (millis() - heldSince >= 10000) {
      digitalWrite(VIB_MOTOR_PIN, LOW);
      s_bootToUsb = PROBE_LOG_MAGIC;   // 再起動したら USB mode へ
      CLEAR_PERI_REG_MASK(RTC_CNTL_USB_CONF_REG, RTC_CNTL_SW_HW_USB_PHY_SEL | RTC_CNTL_SW_USB_PHY_SEL);
      SET_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_DP_PULLUP | USB_SERIAL_JTAG_USB_PAD_ENABLE);
      esp_restart();
    }
  } else {
    heldSince = 0;
  }
}

void setup() {
  pinMode(UP_BTN_PIN, INPUT_PULLUP);     // 入力にしてから監視を始める（未設定のままだと「押されている」と読めてしまう）
  pinMode(DOWN_BTN_PIN, INPUT_PULLUP);
  static esp_timer_handle_t frTimer;
  esp_timer_create_args_t fr = {};
  fr.callback = forceResetCheck;
  fr.name = "force_reset";
  if (esp_timer_create(&fr, &frTimer) == ESP_OK) esp_timer_start_periodic(frTimer, 100000);

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

  // 電源投入・書き込み直後のリセット
  if (cause != ESP_SLEEP_WAKEUP_EXT1 && cause != ESP_SLEEP_WAKEUP_TIMER && cause != ESP_SLEEP_WAKEUP_EXT0) {
    screen = SCR_CLOCK; cursor = 0; homeCursor = 0; subCat = 0; partialCount = 0;
    // ホスト動作中に落ちて再起動した場合に備え、USB の配線を毎回シリアル側へ戻す（戻っていれば何も変わらない）
    CLEAR_PERI_REG_MASK(RTC_CNTL_USB_CONF_REG, RTC_CNTL_SW_HW_USB_PHY_SEL | RTC_CNTL_SW_USB_PHY_SEL);
    SET_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_DP_PULLUP | USB_SERIAL_JTAG_USB_PAD_ENABLE);
    if (s_bootMagic != PROBE_LOG_MAGIC) { s_bootMagic = PROBE_LOG_MAGIC; s_bootCount = 0; }
    s_bootCount++;
    s_lastResetReason = (uint32_t)esp_reset_reason();
    scanCarts();   // 置いてあるカートリッジを読み直す
    { String a, b; loadWifi(a, b); }   // ルーター登録の有無を読み直す（時計のアイコン用）
    // DOWN を押しながら起動 → USB mode（UP は GPIO0 なので ROM ブートローダーに入ってしまう）
    // 電源投入・外部リセットのときだけ。落ちて再起動した場合（パニック・電圧低下など）に DOWN を押していても入らない
    pinMode(DOWN_BTN_PIN, INPUT_PULLUP);
    esp_reset_reason_t why = esp_reset_reason();
    bool forced = (s_bootToUsb == PROBE_LOG_MAGIC);   // UP+DOWN 10 秒の強制再起動から来た
    s_bootToUsb = 0;
    if (forced || (isDown(DOWN_BTN_PIN) && (why == ESP_RST_POWERON || why == ESP_RST_EXT))) {
      Wire.begin(WATCHY_V3_SDA, WATCHY_V3_SCL);
      Watchy::RTC.init();
      Watchy::display.epd2.initWatchy();
      watchy.usbServiceMode();
    }
  }
  if (cause == ESP_SLEEP_WAKEUP_TIMER) partialCount++;
  // 時計を表示しているなら、毎分の書き換えを必ず有効にする。
  // この目印（guiState）は再起動しても残るので、メニューから各モードに入って再起動したあと、時計が止まったままになっていた。
  if (screen == SCR_CLOCK) guiState = WATCHFACE_STATE;

  // 公式の init() は、再起動のたびに「渡された日時」で時計を設定し直す。何も渡さないと時刻が消える
  // （WiFi のページで合わせた直後、終了時の再起動で消えていた）。いまの時刻が有効なら、それを渡して保つ。
  String keep = "";
  time_t now = time(nullptr);
  if (now > 1700000000) {
    struct tm t;
    gmtime_r(&now, &t);
    char buf[24];
    snprintf(buf, sizeof(buf), "%04d:%02d:%02d:%02d:%02d:%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    keep = buf;
  }
  watchy.init(keep);   // 公式：起床理由に応じて描画 → handleButtonPress() → deepSleep()
}

void loop() {}
