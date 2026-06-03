# Setup

The ISO 7816-3 driver (the `SmartCard` class) is the
[ESP-ISO7816](https://github.com/TheBrycigan/ESP-ISO7816) library. The intended
end state is to consume it as a git **submodule** at `libraries/ESP-ISO7816`.

Until that repo is published, the library is **vendored** (checked in as plain
files) at `libraries/ESP-ISO7816/`, so the firmware builds with no extra steps.

## Build

Arduino CLI (the `--libraries libraries` flag puts ESP-ISO7816 on the library
search path):

```bash
arduino-cli compile --fqbn esp32:esp32:esp32s3 --libraries libraries Authuino
```

Arduino IDE: either point the IDE at this `libraries/` folder, or symlink/copy
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

## Publishing ESP-ISO7816 and switching to a submodule

Do this once, when `github.com/TheBrycigan/ESP-ISO7816` exists and is empty.
This pushes the vendored library to its own repo and replaces the vendored
folder with a submodule pinned to that commit.

```bash
# 1. Publish the vendored library to its own repo.
cd libraries/ESP-ISO7816
git init -b main
git add .
git commit -m "Initial import: ISO 7816-3 T=0/T=1 smart-card driver"
git remote add origin https://github.com/TheBrycigan/ESP-ISO7816.git
git push -u origin main
SHA=$(git rev-parse HEAD)
cd ../..

# 2. Replace the vendored folder with a submodule at that commit.
rm -rf libraries/ESP-ISO7816
git rm -r --cached libraries/ESP-ISO7816
git submodule add https://github.com/TheBrycigan/ESP-ISO7816.git libraries/ESP-ISO7816
git -C libraries/ESP-ISO7816 checkout "$SHA"
git add .gitmodules libraries/ESP-ISO7816
git commit -m "Consume ESP-ISO7816 as a submodule"
```

After this, collaborators initialise the library with:

```bash
git submodule update --init --recursive
```

## Updating the library later (once it's a submodule)

```bash
git -C libraries/ESP-ISO7816 pull origin main
git add libraries/ESP-ISO7816
git commit -m "Update ESP-ISO7816 submodule"
```
