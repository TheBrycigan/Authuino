# ESP-ISO7816

A clean, dependency-light **ISO 7816-3 smart-card driver** for the ESP32-S3
(and other ESP32 targets) driving an **NCN8024** — or compatible — card
interface IC.

It bit-bangs the half-duplex card I/O line and generates the card clock with
the ESP32 LEDC peripheral, so it needs no external UART or dedicated
smart-card controller. Both transmission protocols are implemented:

- **T=0** — full procedure-byte protocol (NULL `0x60`, ACK-all = `INS`,
  ACK-one = `INS ^ 0xFF`, direct SW1), plus `GET RESPONSE` chaining for
  `SW=61xx`.
- **T=1** — block protocol with LRC/CRC, IFSD negotiation (bumped to 254 at
  power-on so big responses fit one block), and WTX handling for cards that
  ask for more time during crypto.

The ATR is parsed for guard time (TC1), protocol (TD1), work-waiting time
(TC2) and the T=1 parameters (TA3/TB3/TC3). The ETU is **auto-detected from
TS-byte edge timing**, so it stays correct even when the interface IC divides
CLKIN before passing it to the card.

This library was factored out of the [Authuino](https://github.com/TheBrycigan/Authuino)
offline-2FA project so it can be reused and versioned on its own. The public
type is `SmartCard`, declared in `ESP_ISO7816.h`.

## Hardware

| NCN8024 signal | Purpose                              |
| -------------- | ------------------------------------ |
| `IO`           | half-duplex card I/O (pulled high)   |
| `RSTIN`        | drive HIGH to release card from reset|
| `CMDVCCn`      | drive LOW to power the card (active-low) |
| `OFFn` / `PRES`| HIGH = a card is present             |
| `CLKIN`        | card clock (this driver feeds 1–5 MHz)|

Any ESP32 board works — pins are constructor arguments. The defaults used in
the example match the Authuino ESP32-S3 board (`IO`=9, `RSTIN`=10,
`CMDVCCn`=11, `OFFn`=43, `CLKIN`=44).

## Install

### Arduino IDE

Clone into your sketchbook `libraries/` folder:

```bash
git clone https://github.com/TheBrycigan/ESP-ISO7816.git \
    ~/Arduino/libraries/ESP-ISO7816
```

### As a git submodule (what the Authuino firmware does)

```bash
git submodule add https://github.com/TheBrycigan/ESP-ISO7816.git \
    libraries/ESP-ISO7816
arduino-cli compile --fqbn esp32:esp32:esp32s3 --libraries libraries <sketch>
```

### PlatformIO

```ini
lib_deps = https://github.com/TheBrycigan/ESP-ISO7816.git
```

Requires the **ESP32 Arduino core** (`esp32-hal-ledc`, the Arduino LEDC API).

## Usage

```cpp
#include <ESP_ISO7816.h>

SmartCard card(/*IO*/9, /*RSTIN*/10, /*CMDVCCn*/11, /*OFFn*/43, /*CLKIN*/44);

void setup() {
  Serial.begin(115200);
  card.begin();                       // configure GPIOs; does not power the card
}

void loop() {
  if (!card.present()) return;

  uint8_t atr[SC_MAX_ATR_BYTES];
  int atrLen = card.powerOn(atr, sizeof(atr), 4000000); // 4 MHz
  if (atrLen < 2) { card.powerOff(); return; }

  // SELECT by AID, GET RESPONSE chaining handled automatically.
  uint8_t cmd[] = { 0x00, 0xA4, 0x04, 0x00, /*Lc*/ ... };
  uint8_t resp[256];
  size_t  respLen = sizeof(resp);
  int sw = card.transmitChained(cmd, sizeof(cmd), resp, &respLen);
  // sw >= 0  -> SW1<<8 | SW2 ; sw < 0 -> SmartCard::ERR_*

  card.powerOff();
}
```

See [`examples/ReadATR`](examples/ReadATR/ReadATR.ino) for a complete sketch.

## API

| Method | Description |
| ------ | ----------- |
| `SmartCard(io, rst, vcc, pres, clk)` | Construct with the NCN8024 pin numbers. |
| `begin()` | Configure GPIOs. Safe before a card is inserted; does **not** power the card. |
| `present()` | `true` if a card is in the slot (PRES line). |
| `powerOn(buf, size, clkHz=4MHz)` | Power up, clock the card, read the ATR. Returns ATR length (≥2) or a negative `ERR_*`. |
| `powerOff()` | Power down the card and stop the clock. |
| `transmit(apdu, len, resp, &respLen)` | One APDU exchange (auto-detects case 1–4). Returns `SW1<<8\|SW2` or `ERR_*`. |
| `transmitChained(...)` | Like `transmit()`, but follows `SW=61xx` with `GET RESPONSE` (T=0). |
| `isActive()` / `protocol()` / `etuMicros()` / `extraGuardEtu()` / `clockHz()` | State / diagnostics. |

Error codes (negative returns): `ERR_NOT_ACTIVE`, `ERR_TIMEOUT`,
`ERR_CARD_GONE`, `ERR_BAD_APDU`, `ERR_NO_CLOCK`.

Diagnostic progress is printed to `Serial` (e.g. clock start, ETU correction,
T=1 negotiation). Open the serial monitor at 115200 baud to see it.

## License

GPL-3.0, same as the Authuino project. See [LICENSE](LICENSE).
