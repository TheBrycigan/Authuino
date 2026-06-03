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
└── libraries/
    └── ESP-ISO7816/           the ISO 7816-3 driver (the SmartCard class)
```

The low-level **ISO 7816-3 T=0/T=1 smart-card driver** (the `SmartCard`
class, `ESP_ISO7816.h`) was split out into its own library,
[**ESP-ISO7816**](https://github.com/TheBrycigan/ESP-ISO7816). The firmware's
`sc_interface` layer builds the OATH/PIV/CCID logic on top of it.

> **Status:** the library currently lives **vendored** at
> `libraries/ESP-ISO7816/` so the firmware builds with no extra steps. Once
> it's published to its own GitHub repo it becomes a git **submodule** at the
> same path — see [SETUP.md](SETUP.md). Either way the build command is the
> same.

## Building

```bash
arduino-cli compile --fqbn esp32:esp32:esp32s3 --libraries libraries Authuino
```

See [**SETUP.md**](SETUP.md) for the required Arduino IDE USB settings and for
publishing the library / switching to a submodule.

## License

GPL-3.0. See [LICENSE](LICENSE).
