# watchy-cart

Turn a [Watchy](https://watchy.sqfmi.com/) v3 e-ink watch into a firmware "cartridge" for the M-VAVE FM-1.
Plug the watch into the FM-1 over USB, pick a firmware package (`.fwsc`) from the list, and the watch writes it. No computer needed at the moment of switching.

日本語は [README.ja.md](README.ja.md)。

## This is a test version

v0.36 is an early test build, published so that others can look at it and try it, not a finished tool.

- It has been written to **one FM-1 with one Watchy**, over the two routes in the table below. Everything else is untested.
- It can fail. A failed write can leave the FM-1 needing recovery from a computer, so have a computer-side installer ready before you try it.
- Behaviour, menus and storage layout may change between versions without notice.
- The clock is fixed to Japan time (UTC+9) when it syncs over the router.
- The on-watch screens are in English; the Wi-Fi page and the source comments are in Japanese.
- Reports are welcome in Issues: the route you tried (from which firmware to which), the watchy-cart version, and what the watch showed.

Use it at your own risk. This project is independent: it is not made by or associated with M-VAVE, SQFMI, or any firmware author.

## Status (v0.36)

| Route | Result |
|---|---|
| Modular One → official V15 | written successfully on real hardware |
| Modular One → Dub-1 | written successfully on real hardware |
| FM-1 running the official firmware → anything | **not working yet** (the watch cannot connect) |
| FM-1 running an M-VAVE Open based firmware (`FM-1_8000xxx`) | **refused by design** (see below) |

Not yet verified: resuming after an interruption in the last stage, and file checks on packages other than the official one.

### M-VAVE Open based firmware is one-way

Firmware built on M-VAVE Open identifies as `FM-1_8000000`–`FM-1_8000999` and needs an extra step ("arm") before it accepts an update. watchy-cart does not implement that step. If the FM-1 is running such a firmware, the watch shows `open firmware: / use PC installer` and writes nothing. To leave it, use a computer-side installer.
In other words: the watch can put such a firmware **in**, but cannot take it **out**.

## Firmware packages are not included

This repository contains no `.fwsc` files, and none should be added. Each firmware is the work of its author. You load the packages you have chosen onto the watch yourself, over Wi-Fi.

Limits: up to 8 packages, about 4.9 MB of storage in total. Only files ending in `.fwsc` that identify as FM-1 packages are kept.

## Loading packages over Wi-Fi

The watch serves a small web page (in Japanese) where you add and delete packages. There are two ways to reach it.

### A. WiFi direct (no router needed, use this first)

1. On the watch: home → **WiFi / Net** → **WiFi direct**. The screen shows `watchy-cart / (no password) / 192.168.4.1`.
2. On your phone or computer, join the Wi-Fi network **watchy-cart** (no password). It has no internet, so tell the phone to stay connected if it asks.
3. Open `http://192.168.4.1` in a browser.
4. Choose a `.fwsc` file and press **置く** (put). It appears in the table with its product name and size. **削除** deletes it.
5. Hold DOWN on the watch to finish. The watch restarts and the package is in the cartridge list.

### B. WiFi router (the watch joins your home Wi-Fi)

1. Register the router once: open the page via WiFi direct (above), pick your network under the router section, enter its password and save. Only 2.4 GHz networks work.
2. From then on: home → **WiFi / Net** → **WiFi router**. The watch joins the router, sets its clock, and shows its IP address.
3. From a device on the same network, open that IP address, or `http://watchy-cart.local`.
4. Add and delete packages as in A, then hold DOWN to finish.

If the watch cannot join the router, it falls back to direct mode.

### Fetch latest (optional)

On the same page, under **取得元** (sources), register a name and an `https://` URL that points at the `.fwsc` file itself, for example an author's GitHub release:
`https://github.com/AUTHOR/REPO/releases/latest/download/FILE.fwsc`

Then **取得** on the page, or **Fetch latest** on the watch, downloads it again through the router (it takes some tens of seconds; files up to 2 MB). The watch does not download from The Firmware Hub.

### Wi-Fi cautions

- **The router name and password are stored on the watch**, in its flash, unencrypted. They are not in this source code. Anyone who has the watch and reads its flash can get them. Before lending, selling or giving away the watch, erase them: open the page, save the router form with the name left empty. A full flash erase from a computer also removes them.
- Do not share a flash dump of your watch; it contains the password.
- **WiFi direct has no password and the page has no login.** While it is on, anyone within range can join `watchy-cart` and add or delete packages, or change the router setting. The page shows only the length of the saved password, never the password itself. Turn it on only when you need it and hold DOWN to end it afterwards.
- The page is plain `http`. In router mode, anyone on the same network can reach it while the mode is on.

### The "!" mark

Packages built on M-VAVE Open are marked with **！** on the page. Read "M-VAVE Open based firmware is one-way" above before writing one.

## Hardware

- Watchy v3 (ESP32-S3FN8, 8 MB flash, no PSRAM)
- A USB-C cable between the watch and the FM-1. The watch acts as USB host; it does not supply 5 V.

### Why only two buttons

The two left-hand buttons (Back / Menu) broke off the author's Watchy. So the stock firmware's four-button operation was rewritten to work entirely with the two right-hand buttons (UP / DOWN), using short and long presses. The left buttons are not used at all, so a Watchy with all four buttons works the same way.

## Controls

| | Short press | Long press |
|---|---|---|
| UP | up | back (on the clock: unlock → home) |
| DOWN | down | select |

- UP + DOWN briefly: back to the clock from anywhere.
- UP + DOWN for 3 s: USB mode. For 10 s: forced restart into USB mode.
- Hold DOWN while powering on: USB mode.
- Leaving probe / WiFi / USB mode is always DOWN. (UP is GPIO0; holding it through the restart would drop the watch into the ROM bootloader.)

Menu: clock → home → **cartridges** (list → confirm → write), **WiFi / Net** (router, direct, fetch latest), **System** (FM-1 probe), **USB mode**.

## Build

Arduino IDE, with the sketch folder named `watchy-cart`.

- Watchy library 1.4.14, Arduino ESP32 core 3.3.12
- Board: **ESP32S3 Dev Module**
- Flash Size: **8MB**
- USB Mode: **Hardware CDC and JTAG**
- USB CDC On Boot: **Enabled**
- PSRAM: **Disabled**
- Partition scheme: the `partitions.csv` in the sketch folder is picked up automatically (3 MB app, ~4.9 MB LittleFS).

To upload later builds, choose **USB mode** on the watch first; it stays awake so the IDE can reach it.

## How it works

1. The watch sends "start update" (`F0 22 24 35 7F F7`) to the running firmware. The FM-1 reads the package once as a check.
2. The FM-1 restarts into its loader. The watch sends "start update" again; the FM-1 reads the package and writes it.
3. The FM-1 restarts into the new firmware, and the watch confirms its identity.

Notes from real hardware:

- The FM-1 enumerates without 5 V, but the host has to keep D+ pulled up.
- While D+ is held, disconnection cannot be seen on the line; it is detected by replies stopping.
- E-ink refreshes put the CPU into light sleep, which must be disabled while USB or Wi-Fi is active.

## Credits

See [THIRD_PARTY.md](THIRD_PARTY.md).

## License

MIT. See [LICENSE](LICENSE). Bundled fonts and icons keep their own licenses, listed in THIRD_PARTY.md.
