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
    └── AuthuinoISO7816/       git submodule — the ISO 7816-3 driver
```

The low-level **ISO 7816-3 T=0/T=1 smart-card driver** (the `SmartCard`
class) was split out into its own repository,
[**AuthuinoISO7816**](https://github.com/thebrycigan/AuthuinoISO7816), and is
consumed here as a git submodule at `libraries/AuthuinoISO7816`. The firmware's
`sc_interface` layer builds the OATH/PIV/CCID logic on top of it.

## Building

See [**SETUP.md**](SETUP.md) for first-time setup (publishing the library,
initialising the submodule) and the full build command. In short:

```bash
git submodule update --init --recursive
arduino-cli compile --fqbn esp32:esp32:esp32s3 --libraries libraries Authuino
```

## License

GPL-3.0. See [LICENSE](LICENSE).
