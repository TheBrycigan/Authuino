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
    └── ESP-ISO7816/           git submodule → TheBrycigan/ESP-ISO7816
```

The low-level **ISO 7816-3 T=0/T=1 smart-card driver** (the `SmartCard`
class, `ESP_ISO7816.h`) lives in its own repository,
[**ESP-ISO7816**](https://github.com/TheBrycigan/ESP-ISO7816), and is consumed
here as a **git submodule** at `libraries/ESP-ISO7816`. The firmware's
`sc_interface` layer builds the OATH/PIV/CCID logic on top of it.

## Cloning

```bash
git clone --recurse-submodules https://github.com/TheBrycigan/Authuino.git
# or, after a plain clone:
git submodule update --init --recursive
```

> The submodule is pinned to a specific commit of ESP-ISO7816. If that repo
> isn't published yet, `submodule update` can't fetch it — see
> [SETUP.md](SETUP.md) for the one-time publish step.

## Building

```bash
arduino-cli compile --fqbn esp32:esp32:esp32s3 --libraries libraries Authuino
```

See [**SETUP.md**](SETUP.md) for the required Arduino IDE USB settings, the
publish step, and how to pull library updates into the firmware.

## License

GPL-3.0. See [LICENSE](LICENSE).
