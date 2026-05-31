# Setup

The ISO 7816-3 driver lives in its own repository,
[`thebrycigan/AuthuinoISO7816`](https://github.com/thebrycigan/AuthuinoISO7816),
and is pulled into this firmware as a git submodule at
`libraries/AuthuinoISO7816`.

This repo already records the submodule (`.gitmodules` + a gitlink pinned to
the library's initial commit). The library content just needs to be published
to its GitHub repo once, after which `git submodule update` resolves cleanly
for everyone.

## 1. Publish the AuthuinoISO7816 library (one time)

You were handed a git bundle, **`authuino-iso7816.bundle`**, containing the
complete library repo (branch `main`). The gitlink in this firmware points at
the exact commit inside that bundle, so publishing it verbatim makes the
submodule resolve immediately — no SHA juggling.

1. Create an **empty** repo on GitHub named `AuthuinoISO7816` under your
   account/org (no README, no license, no `.gitignore` — keep it empty so the
   first push is a clean fast-forward).

2. Push the bundle to it:

   ```bash
   git clone authuino-iso7816.bundle AuthuinoISO7816
   cd AuthuinoISO7816
   git remote set-url origin https://github.com/thebrycigan/AuthuinoISO7816.git
   git push -u origin main
   ```

   The import commit is authored as `TheBrycigan`. If you'd rather re-author
   it, do so **before** pushing (`git commit --amend --reset-author`) — but
   then the SHA changes and you must update the gitlink (see step 3 note).

## 2. Initialise the submodule in this firmware

From a clone of this (Authuino) repo:

```bash
git submodule update --init --recursive
```

Because the recorded gitlink SHA matches the bundle's commit, this checks the
library out cleanly. (If you run it *before* step 1, it fails — the remote has
no commits yet. Publish first.)

> **Note:** if you amended/re-authored the library commit in step 1 (changing
> its SHA), re-point the gitlink:
> ```bash
> git -C libraries/AuthuinoISO7816 fetch origin
> git -C libraries/AuthuinoISO7816 checkout main
> git add libraries/AuthuinoISO7816
> git commit -m "Bump AuthuinoISO7816 submodule"
> ```

## 3. Build

Arduino CLI (the `--libraries libraries` flag puts the submodule on the
library search path):

```bash
arduino-cli compile --fqbn esp32:esp32:esp32s3 --libraries libraries Authuino
```

Arduino IDE: either point the IDE at this `libraries/` folder, or symlink/copy
`libraries/AuthuinoISO7816` into your sketchbook `libraries/` directory, then
open `Authuino/Authuino.ino`.

### Required Arduino IDE Tools settings (USB CCID)

```
USB Mode          = USB-OTG (TinyUSB)
USB CDC On Boot   = Disabled
USB Firmware MSC  = Disabled
USB DFU On Boot   = Disabled
```

(See the header comments in `Authuino/usb_ccid.*` for the rationale.)

## Updating the library later

```bash
git -C libraries/AuthuinoISO7816 pull origin main
git add libraries/AuthuinoISO7816
git commit -m "Update AuthuinoISO7816 submodule"
```
