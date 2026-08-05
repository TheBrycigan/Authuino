# Authuino

Offline 2FA code generator — a TOTP smart-card reader for the Waveshare
ESP32-S3-Touch-LCD-3.5. It reads OATH/TOTP credentials from a smart card,
shows the rolling codes on an LVGL touch UI, and can also expose the card to
a host PC as a USB CCID reader.

## Repository layout

```
authuino/
├── Authuino/                  Arduino sketch (the firmware)
│   ├── Authuino.ino           setup / loop / orchestration
│   ├── sc_interface.*         OATH/PIV APDU layer, VALIDATE, CCID bridge
│   ├── usb_ccid.*             USB CCID class driver (host-facing reader)
│   ├── nvs_aid.*              persistent AID list
│   ├── nvs_meta.* nvs_settings.*   NVS-backed metadata / settings
│   ├── rtc_time.*             PCF85063 RTC wrapper
│   ├── ui.*                   LVGL screens
│   └── audio.* accel.* cam_qr.*    peripherals
└── libraries/                 third-party deps, each pinned as a git submodule
    ├── ESP-ISO7816/           → TheBrycigan/ESP-ISO7816        (smart-card driver)
    ├── lvgl/                  → lvgl/lvgl @ v8.4.0             (GUI toolkit)
    ├── Arduino_GFX/           → moononournation/Arduino_GFX @ v1.6.6  (ST7796 display)
    ├── quirc/                 Arduino wrapper; upstream/ → dlbeer/quirc @ v1.2  (QR decode)
    └── esp32-camera/          → espressif/esp32-camera @ v2.1.6       (pinned reference)
```

The low-level **ISO 7816-3 T=0/T=1 smart-card driver** (the `SmartCard`
class, `ESP_ISO7816.h`) lives in its own repository,
[**ESP-ISO7816**](https://github.com/TheBrycigan/ESP-ISO7816), and is consumed
here as a **git submodule** at `libraries/ESP-ISO7816`. The firmware's
`sc_interface` layer builds the OATH/PIV/CCID logic on top of it.

All third-party libraries are likewise pinned as **git submodules** under
`libraries/`, so `git clone --recurse-submodules` yields a reproducible,
self-contained build tree:

| Library | Submodule → upstream | Pin | Used by |
| --- | --- | --- | --- |
| ESP-ISO7816 | `libraries/ESP-ISO7816` → TheBrycigan/ESP-ISO7816 | branch `main` | `sc_interface` (smart-card driver) |
| LVGL | `libraries/lvgl` → lvgl/lvgl | `v8.4.0` | `ui.*`, `Authuino.ino` (touch UI) |
| GFX Library for Arduino | `libraries/Arduino_GFX` → moononournation/Arduino_GFX | `v1.6.6` | `Authuino.ino` (ST7796 display) |
| quirc | `libraries/quirc/upstream` → dlbeer/quirc | `v1.2` | `cam_qr.cpp` (QR decode) |
| esp32-camera | `libraries/esp32-camera` → espressif/esp32-camera | `v2.1.6` | `cam_qr.cpp` (see note) |

> - **LVGL** is pinned to the v8 line (the firmware uses the v8 driver API) and
>   still needs an `lv_conf.h` placed in `libraries/` (next to `lvgl/`) to
>   compile — that's configuration, not vendored here.
> - **quirc** isn't an Arduino library upstream, so `libraries/quirc` adds a
>   thin wrapper (`library.properties` + `src/` shims) that compiles
>   `upstream/lib/*.c`.
> - **esp32-camera** is pinned for provenance; the build still uses the copy
>   bundled in the ESP32 Arduino core, which provides `esp_camera.h`.

## Cloning

```bash
git clone --recurse-submodules https://github.com/TheBrycigan/Authuino.git
# or, after a plain clone:
git submodule update --init --recursive
```

> Each submodule is pinned to a specific commit/tag for reproducible builds.
> `--recurse-submodules` (or `git submodule update --init --recursive`) fetches
> them all into `libraries/`.

## Toolchain

The firmware builds against the **ESP32 Arduino core** (the `esp32` boards
package by Espressif Systems) — not a submodule, so its version is recorded
here:

| Component | Version |
| --- | --- |
| ESP32 Arduino core (`esp32` by Espressif Systems) | `3.2.1` |
| Board / FQBN | ESP32S3 Dev Module (`esp32:esp32:esp32s3`) |

```bash
arduino-cli core install esp32:esp32@3.2.1 \
  --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
```

(Arduino IDE: Boards Manager → "esp32 by Espressif Systems" → 3.2.1.)

## Building

```bash
arduino-cli compile --fqbn esp32:esp32:esp32s3 --libraries libraries Authuino
```

See [**SETUP.md**](SETUP.md) for the required Arduino IDE USB settings, the
`lv_conf.h` requirement, and how to bump the submodules.

## License

GPL-3.0. See [LICENSE](LICENSE).
