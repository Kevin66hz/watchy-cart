# Third-party material and credits

## Files included in this repository

| File | Origin | License |
|---|---|---|
| `Seven_Segment10pt7b.h`, `icons.h` | 7_SEG watch face in the [Watchy](https://github.com/sqfmi/Watchy) library by SQFMI | MIT (Watchy library) |
| `DSEG7_Classic_Bold_25.h` | Same watch face. Bitmap of the [DSEG](https://www.keshikan.net/fonts-e.html) font by keshikan, converted with oleddisplay.squix.ch | SIL Open Font License 1.1 |

## Libraries used at build time (not included)

- Watchy library (SQFMI) and its dependencies
- Arduino core for ESP32 (Espressif)

## References

- **FM1Kit** by Drew Robinson (MIT) — https://fm-1.drewr.dev/fm1kit/ , source in https://github.com/ocean/FM-SWITCH
  The FM-1 update procedure, the 7-bit packing, and the package checks in `watchy-cart.ino` are a C++ reimplementation that follows FM1Kit's behaviour (`FM1Updater`, `SevenBit`, `FwscPackage`, `FirmwareContainer`).
- **FM-SWITCH** by Drew Robinson — https://fm-1.drewr.dev/fm-switch/
  Informed how the firmware catalogue and download sources are handled.

Thanks to their author, and to everyone writing firmware for the FM-1.

M-VAVE and FM-1 are names of their owner. This project is not associated with M-VAVE.
