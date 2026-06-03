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

Because ESP-ISO7816 is published, this fetches the library into
`libraries/ESP-ISO7816/` at the pinned commit.

## Build

Arduino CLI (`--libraries libraries` puts ESP-ISO7816 on the search path):

```bash
arduino-cli compile --fqbn esp32:esp32:esp32s3 --libraries libraries Authuino
```

Arduino IDE: point the IDE at this `libraries/` folder, or symlink/copy
`libraries/ESP-ISO7816` into your sketchbook `libraries/` directory, then open
`Authuino/Authuino.ino`.

### Required Arduino IDE Tools settings (USB CCID)

```
USB Mode          = USB-OTG (TinyUSB)
USB CDC On Boot   = Disabled
USB Firmware MSC  = Disabled
USB DFU On Boot   = Disabled
```

(See the header comments in `Authuino/usb_ccid.*` for the rationale.)

## Pulling library updates into the firmware

The submodule pins a commit (keeps builds reproducible). To adopt newer
ESP-ISO7816 commits:

```bash
git submodule update --remote libraries/ESP-ISO7816   # latest of the tracked branch (main)
git add libraries/ESP-ISO7816
git commit -m "Bump ESP-ISO7816"
git push
```

While developing the library locally, `libraries/ESP-ISO7816/` is a live
checkout — edits there are used by the build immediately; commit & push them
inside the submodule (to the ESP-ISO7816 repo), then bump as above.
