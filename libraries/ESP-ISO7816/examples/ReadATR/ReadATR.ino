/*
 * ReadATR.ino — minimal ESP-ISO7816 example.
 *
 * Powers on an inserted smart card, prints its ATR, and sends a single
 * SELECT (by AID) APDU so you can confirm the T=0/T=1 exchange works end
 * to end. Wire an NCN8024 (or compatible) card interface IC to the pins
 * below; adjust them for your board.
 *
 * The AID selected here is the YubiKey/Apex OATH applet — change it to
 * whatever applet your card exposes.
 */

#include <ESP_ISO7816.h>

// NCN8024 pin mapping (defaults match the Authuino ESP32-S3 board).
#define PIN_IO      9    // half-duplex card I/O
#define PIN_RSTIN  10    // RSTIN  (HIGH releases reset)
#define PIN_CMDVCC 11    // CMDVCCn (active LOW powers the card)
#define PIN_OFFn   43    // OFFn / PRES (HIGH = card inserted)
#define PIN_CLK    44    // CLKIN (LEDC square wave)

#define CARD_CLK_HZ 4000000UL   // 4 MHz — within ISO 7816-3 1..5 MHz

SmartCard card(PIN_IO, PIN_RSTIN, PIN_CMDVCC, PIN_OFFn, PIN_CLK);

// OATH applet AID (Yubico / Feitian Apex).
static const uint8_t OATH_AID[] = {
  0xA0, 0x00, 0x00, 0x07, 0x47, 0x00, 0x61, 0xFC, 0x54, 0xD5, 0x01
};

static void printHex(const char* label, const uint8_t* buf, size_t len) {
  Serial.print(label);
  for (size_t i = 0; i < len; i++) Serial.printf(" %02X", buf[i]);
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n[ReadATR] ESP-ISO7816 example");
  card.begin();
}

void loop() {
  if (!card.present()) {
    Serial.println("[ReadATR] insert a card...");
    delay(1000);
    return;
  }

  uint8_t atr[SC_MAX_ATR_BYTES];
  int atrLen = card.powerOn(atr, sizeof(atr), CARD_CLK_HZ);
  if (atrLen < 2) {
    Serial.printf("[ReadATR] powerOn failed (%d)\n", atrLen);
    card.powerOff();
    delay(1000);
    return;
  }

  printHex("[ReadATR] ATR:", atr, atrLen);
  Serial.printf("[ReadATR] protocol = T=%d, ETU = %u us, clock = %lu Hz\n",
                (int)card.protocol(), (unsigned)card.etuMicros(),
                (unsigned long)card.clockHz());

  // SELECT (by AID): 00 A4 04 00 <Lc> <AID>
  uint8_t cmd[5 + sizeof(OATH_AID)];
  cmd[0] = 0x00; cmd[1] = 0xA4; cmd[2] = 0x04; cmd[3] = 0x00;
  cmd[4] = (uint8_t)sizeof(OATH_AID);
  memcpy(&cmd[5], OATH_AID, sizeof(OATH_AID));

  uint8_t resp[256];
  size_t  respLen = sizeof(resp);
  int sw = card.transmitChained(cmd, sizeof(cmd), resp, &respLen);
  if (sw < 0) {
    Serial.printf("[ReadATR] SELECT error %d\n", sw);
  } else {
    Serial.printf("[ReadATR] SELECT SW=%04X, %u data byte(s)\n",
                  (uint16_t)sw, (unsigned)respLen);
    if (respLen) printHex("[ReadATR] data:", resp, respLen);
  }

  card.powerOff();
  Serial.println("[ReadATR] done — remove and reinsert to repeat\n");
  delay(3000);
}
