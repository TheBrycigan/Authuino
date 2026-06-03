# Setup

The ISO 7816-3 driver (the `SmartCard` class) is the
[ESP-ISO7816](https://github.com/TheBrycigan/ESP-ISO7816) library, consumed
here as a **git submodule** at `libraries/ESP-ISO7816`, pinned to a specific
commit.

```
.gitmodules:  libraries/ESP-ISO7816 → https://github.com/TheBrycigan/ESP-ISO7816.git
```

## The ESP-ISO7816 library is already published

The driver lives in its own repo,
[ESP-ISO7816](https://github.com/TheBrycigan/ESP-ISO7816), and this firmware
pins it as a submodule — so there is no separate publishing step. Just
initialise the submodule (below) and Git fetches the exact pinned commit.

To see which commit is pinned: `git ls-files -s libraries/ESP-ISO7816` (the
`160000` gitlink). To adopt a newer library commit later, see *Pulling
library updates* below.

## Initialise the submodule in this firmware

```bash
git submodule update --init --recursive
```

This fetches every submodule under `libraries/` at its pinned commit:
ESP-ISO7816, LVGL, Arduino_GFX, quirc's `upstream/`, and esp32-camera.

## Third-party libraries (git submodules)

All libraries the firmware needs are pinned under `libraries/`:

| Submodule | Upstream | Pin |
| --- | --- | --- |
| `libraries/ESP-ISO7816` | TheBrycigan/ESP-ISO7816 | branch `main` |
| `libraries/lvgl` | lvgl/lvgl | `v8.4.0` |
| `libraries/Arduino_GFX` | moononournation/Arduino_GFX | `v1.6.5` |
| `libraries/quirc/upstream` | dlbeer/quirc | `v1.2` |
| `libraries/esp32-camera` | espressif/esp32-camera | `v2.1.6` |

Two need a word of explanation:

- **LVGL** must be configured with an `lv_conf.h`, which LVGL looks for one
  level **above** the `lvgl/` folder — i.e. at `libraries/lv_conf.h`. Copy
  `libraries/lvgl/lv_conf_template.h` there, rename it, flip its leading
  `#if 0` to `#if 1`, and set `LV_COLOR_DEPTH 16`. Without it the build fails
  with `lv_conf.h: No such file or directory`. (It's configuration, so it is
  not vendored here.)
- **esp32-camera** is pinned only as a provenance reference. `esp_camera.h`
  actually comes from the ESP32 Arduino core, which bundles this same
  component; `arduino-cli` does not treat `libraries/esp32-camera` as a
  library (it has no `library.properties`), so it neither overrides the core
  nor interferes with the build.

## Build

Arduino CLI (`--libraries libraries` puts all the submodule libraries on the
search path). Make sure `libraries/lv_conf.h` exists first (see above):

```bash
arduino-cli compile --fqbn esp32:esp32:esp32s3 --libraries libraries Authuino
```

Arduino IDE: point the IDE's sketchbook `libraries/` at this `libraries/`
folder (or symlink/copy its contents there), add `lv_conf.h` as above, then
open `Authuino/Authuino.ino`.

### Required Arduino IDE Tools settings (USB CCID)

```
USB Mode          = USB-OTG (TinyUSB)
USB CDC On Boot   = Disabled
USB Firmware MSC  = Disabled
USB DFU On Boot   = Disabled
```

(See the header comments in `Authuino/usb_ccid.*` for the rationale.)

## Pulling library updates into the firmware

Each submodule pins a commit (keeps builds reproducible). **ESP-ISO7816**
tracks branch `main`, so bump it with `--remote`:

```bash
git submodule update --remote libraries/ESP-ISO7816   # latest of branch main
git add libraries/ESP-ISO7816
git commit -m "Bump ESP-ISO7816"
git push
```

The third-party libraries (**LVGL**, **Arduino_GFX**, **quirc**,
**esp32-camera**) are pinned to **release tags**, so bump them by checking out
a newer tag inside the submodule:

```bash
git -C libraries/lvgl fetch --tags
git -C libraries/lvgl checkout v8.4.0        # or a newer v8.x tag
git add libraries/lvgl
git commit -m "Bump LVGL"
```

While developing ESP-ISO7816 locally, `libraries/ESP-ISO7816/` is a live
checkout — edits there are used by the build immediately; commit & push them
inside the submodule, then bump as above.
