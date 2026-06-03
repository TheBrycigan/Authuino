/*
 * ESP_ISO7816.h
 *
 * Clean ISO 7816-3 T=0 smart card driver for ESP32-S3 + NCN8024.
 *
 * Replaces SCLib_universal. Fixes known issues:
 *   - LEDC clock generation now uses a duty-resolution that actually
 *     produces a 50 % square wave at the requested frequency (the
 *     old code asked for resolution=1 which only allows duty=0/100 %).
 *   - Inter-byte guard time during transmit honours TC1 from the ATR
 *     (the old code used a fixed 5 etu, far too short for cards that
 *     advertise extra guard time, e.g. TC1 = 0x30 → 60 etu required).
 *   - ETU is computed from the configured clock frequency rather than
 *     measured from TS-byte timing, so it stays correct even if the
 *     polling loop has jitter.
 *   - Power-on sequence matches the NCN8024 expectations:
 *       VCC off + RST low  →  VCC on  →  CLK on  →  RST high  →  ATR
 *   - APDU exchange is a single transmit() call that handles the full
 *     procedure-byte protocol (NULL 0x60, ACK ALL = INS, ACK ONE =
 *     INS xor 0xFF, direct SW1).  transmitChained() additionally
 *     issues GET RESPONSE for SW=61xx.
 *
 * Hardware assumptions (NCN8024):
 *   - pinIO   : half-duplex card I/O line, pulled high by the chip
 *   - pinRST  : RSTIN — driven HIGH releases the card from reset
 *   - pinVCC  : CMDVCCn — driven LOW powers the card (active-low)
 *   - pinPRES : OFFn / PRES — HIGH means a card is present
 *   - pinCLK  : CLKIN — we drive a square wave (1–5 MHz typical)
 *
 * The Arduino-ESP32 v3.x LEDC API is used (ledcAttach / ledcWrite).
 */

#pragma once
#include <Arduino.h>

#define SC_MAX_ATR_BYTES   32
#define SC_MAX_RESP_CHUNK  258   // 256 data + SW1 + SW2

class SmartCard {
public:
  // Error codes returned by transmit() in place of an APDU SW.
  // Any value >= 0 is a 16-bit SW1<<8|SW2; any negative value is an error.
  static constexpr int ERR_NOT_ACTIVE = -1;
  static constexpr int ERR_TIMEOUT    = -2;
  static constexpr int ERR_CARD_GONE  = -3;
  static constexpr int ERR_BAD_APDU   = -4;
  static constexpr int ERR_NO_CLOCK   = -5;

  SmartCard(uint8_t pinIO, uint8_t pinRST, uint8_t pinVCC,
            uint8_t pinPRES, uint8_t pinCLK);

  // Set up GPIOs. Does NOT power the card. Safe to call before a card is
  // inserted.
  void begin();

  // True if a card is currently in the slot (PRES line).
  bool present() const;

  // Power on the card, drive the clock, and read the ATR.
  // clkHz must be >= 1 MHz per ISO 7816-3 (try 4 MHz for fastest reliable
  // operation; some cards prefer 3.57 MHz).
  // Returns ATR length (>= 2) on success, < 0 on failure.
  // After success, isActive() is true and transmit() may be called.
  int powerOn(uint8_t* atrBuf, size_t atrBufSize, uint32_t clkHz = 4000000);

  // Power down the card and stop the clock.
  void powerOff();

  bool     isActive()       const { return _active; }
  uint16_t etuMicros()      const { return _etuUs; }
  uint8_t  extraGuardEtu()  const { return _extraGuardEtu; }
  uint32_t clockHz()        const { return _clkHz; }

  // ---- APDU exchange (T=0, short APDUs) ----
  //
  // apdu encoding (case auto-detected from length):
  //   Case 1: 4 bytes        CLA INS P1 P2
  //   Case 2: 5 bytes        CLA INS P1 P2 Le
  //   Case 3: 5 + Lc bytes   CLA INS P1 P2 Lc data...
  //   Case 4: 6 + Lc bytes   CLA INS P1 P2 Lc data... Le
  //
  // *respLen on entry  : capacity of respBuf
  // *respLen on exit   : number of data bytes written (excluding SW1 SW2)
  //
  // Return: SW1<<8 | SW2 (0..0xFFFF), or one of the negative ERR_ codes.
  int transmit(const uint8_t* apdu, size_t apduLen,
               uint8_t* respBuf, size_t* respLen);

  // Same as transmit(), but if the card returns SW=61xx it automatically
  // issues "GET RESPONSE" and concatenates further chunks into respBuf.
  // For T=1 cards, this is identical to transmit() (T=1 doesn't use
  // GET RESPONSE; chained responses are handled inside transmit()).
  int transmitChained(const uint8_t* apdu, size_t apduLen,
                      uint8_t* respBuf, size_t* respLen);

  enum Protocol { PROTO_T0 = 0, PROTO_T1 = 1 };
  Protocol protocol() const { return _protocol; }

private:
  // Pins
  const uint8_t _pinIO, _pinRST, _pinVCC, _pinPRES, _pinCLK;

  // State
  bool      _active;
  uint16_t  _etuUs;          // one ETU in microseconds
  uint8_t   _extraGuardEtu;  // TC1 value from ATR (0 if not present)
  uint32_t  _wwtMicros;      // T=0 work waiting time, microseconds
  uint32_t  _clkHz;          // configured card clock frequency
  bool      _clkRunning;
  uint8_t   _ledcResolution;

  // Protocol selection (set by _parseAtr from TD1's low nibble; first
  // offered protocol wins, no PPS negotiation).
  Protocol  _protocol;

  // T=1 parameters (defaults from ISO 7816-3 §11; overridden by ATR's
  // TA3/TB3/TC3 if present).
  uint8_t   _t1Ifsc;         // card's max INF size (default 32, often 254)
  uint8_t   _t1Ifsd;         // OUR max INF size (default 32, negotiated up at session start)
  uint8_t   _t1Bwi;          // BWI from TB3 high nibble (default 4)
  uint8_t   _t1Cwi;          // CWI from TB3 low nibble  (default 13)
  bool      _t1EdcCrc;       // TC3 bit 0: false=LRC (default), true=CRC
  uint8_t   _t1Ns;           // our N(S), toggled after each I-block we send

  // Clock generation
  void _startClock(uint32_t hz);
  void _stopClock();

  // Bit-bang character-level primitives (used by both T=0 and T=1)
  bool _sendByteRaw(uint8_t b);
  // Returns 0 on success, -1 on timeout, -2 on card removal.
  int  _recvByteRaw(uint8_t* out, uint32_t timeoutMicros);

  // Specialised receive for the TS byte: measures the ETU from edge
  // timing and updates _etuUs accordingly. Required because the NCN8024
  // (and similar interface ICs) may divide CLKIN before sending it to
  // the card, so the card's actual ETU can differ from what we'd compute
  // from the requested clkHz alone.
  int  _receiveTSAuto(uint8_t* out, uint32_t timeoutMicros);

  bool   _txBytes(const uint8_t* buf, size_t len);
  size_t _rxBytes(uint8_t* buf, size_t maxLen, uint32_t firstTimeoutMicros);

  // Parse interface bytes from ATR: TC1 (extra guard), protocol (TD1),
  // TC2 (WI for T=0), TA3/TB3/TC3 (T=1 IFSC, BWI/CWI, EDC type).
  void _parseAtr(const uint8_t* atr, int len);

  // Per-protocol APDU exchange (called by the public transmit()).
  int _transmitT0(const uint8_t* apdu, size_t apduLen,
                  uint8_t* respBuf, size_t* respLen);
  int _transmitT1(const uint8_t* apdu, size_t apduLen,
                  uint8_t* respBuf, size_t* respLen);

  // T=1 only: send an IFS request S-block to bump our IFSD up from the
  // default 32 to a larger value, so the card can put bigger responses
  // in a single I-block. Called automatically by powerOn().
  bool _t1NegotiateIfsd(uint8_t newIfsd);
};
