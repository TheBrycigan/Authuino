/*
 * ESP_ISO7816.cpp
 * See ESP_ISO7816.h for design notes.
 */

#include "ESP_ISO7816.h"
#include <esp32-hal-ledc.h>

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------

// Default work waiting time when the card does not advertise TC2.
// ISO 7816-3: WWT = 960 * D * WI etu, with WI default = 10 → 9600 etu.
static constexpr uint32_t WWT_DEFAULT_ETU = 9600;

// Floor for the per-byte receive timeout, in microseconds, so that very
// fast clocks don't end up with sub-millisecond timeouts.
static constexpr uint32_t MIN_BYTE_TIMEOUT_US = 100000;  // 100 ms

// Floor for the ATR timeout (used for the FIRST byte only).
static constexpr uint32_t MIN_ATR_TIMEOUT_US = 50000;    // 50 ms

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
SmartCard::SmartCard(uint8_t pinIO, uint8_t pinRST, uint8_t pinVCC,
                     uint8_t pinPRES, uint8_t pinCLK)
  : _pinIO(pinIO), _pinRST(pinRST), _pinVCC(pinVCC),
    _pinPRES(pinPRES), _pinCLK(pinCLK),
    _active(false), _etuUs(0), _extraGuardEtu(0),
    _wwtMicros(0), _clkHz(0), _clkRunning(false), _ledcResolution(0) {}

void SmartCard::begin() {
  pinMode(_pinPRES, INPUT);
  pinMode(_pinVCC,  OUTPUT);
  pinMode(_pinRST,  OUTPUT);
  pinMode(_pinIO,   INPUT);          // released, NCN8024 pulls it high
  pinMode(_pinCLK,  OUTPUT);

  digitalWrite(_pinVCC, HIGH);       // CMDVCCn HIGH = card OFF
  digitalWrite(_pinRST, LOW);        // RSTIN LOW = card held in reset
  digitalWrite(_pinCLK, LOW);
}

bool SmartCard::present() const {
  return digitalRead(_pinPRES) == HIGH;
}

// ---------------------------------------------------------------------------
// LEDC clock generation
//
// ledcAttach(pin, freq, N) configures a timer for one PWM cycle of length
// (1 / freq), divided into 2^N steps.  Duty register sets how many steps
// the output is HIGH.  For a 50 % square wave we need duty = 2^(N-1),
// which requires N >= 1 — but practically N >= 2, because at N=1 the
// duty register can only be 0 or 1 (always-low or always-high).
//
// LEDC requires source_clock >= freq * 2^resolution * 1 (minimum integer
// divider).  The Arduino-ESP32 layer auto-picks the source clock (APB at
// 80 MHz, XTAL at 40 MHz, or RC_FAST at ~17.5 MHz) and may not always
// give us 80 MHz.  So we try resolutions 4 -> 3 -> 2 in turn and take
// the first that ledcAttach() accepts.
// ---------------------------------------------------------------------------
void SmartCard::_startClock(uint32_t hz) {
  _stopClock();

  // Try resolutions 4→3→2→1. Resolution 1 was proven to produce a
  // clean 4 MHz on this hardware via ledcWriteTone / ledcAttach tests.
  // At res=1 the duty register is 0 or 1 (always-low or always-high)
  // but ledcWrite(_pin, 1) gives a 50% duty square wave because the
  // LEDC counter alternates between 0 and 1 each half-cycle.
  for (uint8_t res = 4; res >= 1; res--) {
    if (ledcAttach(_pinCLK, hz, res)) {
      ledcWrite(_pinCLK, 1U << (res - 1));   // 50 % duty
      _clkRunning     = true;
      _clkHz          = hz;
      _ledcResolution = res;
      Serial.printf("[SC] CLK %u Hz started (LEDC res=%u)\n", hz, res);
      return;
    }
  }

  Serial.printf("[SC] CLK %u Hz FAILED — all LEDC resolutions rejected\n", hz);
  _clkRunning = false;
  _clkHz      = 0;
  pinMode(_pinCLK, OUTPUT);
  digitalWrite(_pinCLK, LOW);
}

void SmartCard::_stopClock() {
  if (_clkRunning) {
    ledcDetach(_pinCLK);
    _clkRunning = false;
    _clkHz      = 0;
  }
  pinMode(_pinCLK, OUTPUT);
  digitalWrite(_pinCLK, LOW);
}

// ---------------------------------------------------------------------------
// ATR parsing — walk interface bytes (TA/TB/TC/TD for each i) per ISO 7816-3
// §8.2.  TD(i)'s low nibble says which protocol the FOLLOWING TA(i+1)/TB(i+1)/
// TC(i+1) describe; the FIRST such low nibble (in TD1) is the protocol the
// terminal will use unless PPS is performed.
//
// We extract:
//   TC1            : extra guard time (etu)
//   TD1 low nibble : protocol indicator (0 = T=0, 1 = T=1)
//   TC2            : WI for T=0 work waiting time
//   TA3 / TB3 / TC3: IFSC / BWI/CWI / EDC type for T=1
// ---------------------------------------------------------------------------
void SmartCard::_parseAtr(const uint8_t* atr, int len) {
  if (len < 2) return;

  // Defaults
  _protocol      = PROTO_T0;
  _extraGuardEtu = 0;
  _wwtMicros     = (uint32_t)WWT_DEFAULT_ETU * _etuUs;
  _t1Ifsc        = 32;
  _t1Ifsd        = 32;
  _t1Bwi         = 4;
  _t1Cwi         = 13;
  _t1EdcCrc      = false;
  _t1Ns          = 0;

  uint8_t Y   = (atr[1] >> 4) & 0x0F;     // T0 high nibble = mask of TA1..TD1
  uint8_t i   = 1;                         // interface byte index
  int     idx = 2;                         // current position in ATR
  bool    firstTd = true;

  while (Y != 0 && idx < len) {
    if ((Y & 0x01) && idx < len) {
      uint8_t TA = atr[idx++];
      // TA1 = Fi/Di — ignored (no PPS, stay at default)
      if (i == 3 && _protocol == PROTO_T1) _t1Ifsc = TA;
    }
    if ((Y & 0x02) && idx < len) {
      uint8_t TB = atr[idx++];
      if (i == 3 && _protocol == PROTO_T1) {
        _t1Bwi = (TB >> 4) & 0x0F;
        _t1Cwi = TB & 0x0F;
      }
    }
    if ((Y & 0x04) && idx < len) {
      uint8_t TC = atr[idx++];
      if (i == 1) {
        _extraGuardEtu = (TC == 0xFF) ? 0 : TC;
      } else if (i == 2 && _protocol == PROTO_T0 && TC > 0) {
        _wwtMicros = (uint32_t)960 * TC * _etuUs;
      } else if (i == 3 && _protocol == PROTO_T1) {
        _t1EdcCrc = (TC & 0x01) != 0;
      }
    }
    if ((Y & 0x08) && idx < len) {
      uint8_t TD = atr[idx++];
      if (firstTd) {
        _protocol = (TD & 0x0F) ? PROTO_T1 : PROTO_T0;
        firstTd   = false;
      }
      Y = (TD >> 4) & 0x0F;
      i++;
    } else {
      Y = 0;
    }
  }
}

// ---------------------------------------------------------------------------
// powerOn / powerOff
// ---------------------------------------------------------------------------
int SmartCard::powerOn(uint8_t* atrBuf, size_t atrBufSize, uint32_t clkHz) {
  if (!atrBuf || atrBufSize < 2) return ERR_BAD_APDU;
  if (!present()) return ERR_CARD_GONE;

  if (_active) powerOff();

  // ETU at default Fi=372, Di=1: ETU_us = 372 * 1e6 / clkHz
  _etuUs = (uint16_t)((372000000UL + clkHz / 2) / clkHz);
  if (_etuUs == 0) _etuUs = 1;
  _extraGuardEtu = 0;
  _wwtMicros     = (uint32_t)WWT_DEFAULT_ETU * _etuUs;

  // ----- NCN8024 activation sequence -----
  digitalWrite(_pinRST, LOW);
  digitalWrite(_pinVCC, HIGH);            // ensure off first
  pinMode(_pinIO, INPUT);
  delay(2);

  digitalWrite(_pinVCC, LOW);             // VCC on (active LOW on NCN8024)
  delay(5);                                // wait for VCC ramp

  _startClock(clkHz);
  if (!_clkRunning) {
    digitalWrite(_pinVCC, HIGH);
    return ERR_NO_CLOCK;
  }
  delay(2);                                // let card settle on the clock

  digitalWrite(_pinRST, HIGH);             // release reset → card sends ATR

  // Card has up to ~40 000 cycles to start replying (10 ms @ 4 MHz).
  uint32_t atrTimeoutUs = 40000UL * 1000000UL / clkHz;
  if (atrTimeoutUs < MIN_ATR_TIMEOUT_US) atrTimeoutUs = MIN_ATR_TIMEOUT_US;

  // Read TS byte with edge-based ETU auto-detection. After this _etuUs
  // reflects the card's actual ETU, regardless of any clock divider
  // sitting between the MCU and the card (e.g. the NCN8024's CLKDIV
  // pins).
  uint16_t computedEtu = _etuUs;
  int tsr = _receiveTSAuto(&atrBuf[0], atrTimeoutUs);
  if (tsr != 0) {
    powerOff();
    return ERR_TIMEOUT;
  }
  if (_etuUs != computedEtu) {
    Serial.printf("[SC] ETU corrected: %u us (was %u us based on requested clkHz)\n",
                  (unsigned)_etuUs, (unsigned)computedEtu);
  }
  _wwtMicros = (uint32_t)WWT_DEFAULT_ETU * _etuUs;

  // Read remaining ATR bytes with the corrected ETU.
  size_t got = 1;
  if (atrBufSize > 1) {
    got += _rxBytes(atrBuf + 1, atrBufSize - 1, atrTimeoutUs);
  }
  if (got < 2) {
    powerOff();
    return ERR_TIMEOUT;
  }

  _parseAtr(atrBuf, (int)got);
  _active = true;

  // Bump IFSD from default 32 to 254 so the card can fit larger responses
  // (CALCULATE ALL with several credentials, etc.) in a single T=1 I-block,
  // avoiding both M-bit chaining and applet-specific 6105 quirks. If the
  // card refuses, we silently stay at 32 — small responses still work.
  if (_protocol == PROTO_T1) {
    if (_t1NegotiateIfsd(254)) {
      Serial.println("[SC] T=1 IFSD negotiated to 254");
    } else {
      Serial.println("[SC] T=1 IFSD negotiation failed; staying at 32");
    }
  }

  return (int)got;
}

void SmartCard::powerOff() {
  digitalWrite(_pinRST, LOW);
  delayMicroseconds(200);
  _stopClock();
  digitalWrite(_pinVCC, HIGH);
  pinMode(_pinIO, INPUT);
  _active = false;
}

// ---------------------------------------------------------------------------
// Bit-bang T=0 — single byte send
//
// Frame layout (11 etu total):
//   etu 0       : start bit  (low)
//   etu 1..8    : data bits  (LSB first)
//   etu 9       : even-parity bit
//   etu 10      : driven high (clean stop edge before releasing the line)
//
// After etu 10 we set the pin back to INPUT and let NCN8024's pull-up hold
// the line.  Inter-byte spacing is added by _txBytes().
// ---------------------------------------------------------------------------
bool SmartCard::_sendByteRaw(uint8_t b) {
  // Even parity: parity bit = (count of 1s in b) mod 2
  uint8_t p = b;
  p ^= p >> 4; p ^= p >> 2; p ^= p >> 1;
  uint8_t parity = p & 1;

  uint16_t etuUs = _etuUs;
  pinMode(_pinIO, OUTPUT);
  uint32_t t0 = micros();

  digitalWrite(_pinIO, LOW);                                   // start
  uint32_t target = t0 + etuUs;
  while ((int32_t)(micros() - target) < 0) { /* spin */ }

  for (uint8_t i = 0; i < 8; i++) {                             // data
    digitalWrite(_pinIO, (b >> i) & 1);
    target = t0 + (uint32_t)(i + 2) * etuUs;
    while ((int32_t)(micros() - target) < 0) { }
  }

  digitalWrite(_pinIO, parity);                                 // parity
  target = t0 + 10U * etuUs;
  while ((int32_t)(micros() - target) < 0) { }

  digitalWrite(_pinIO, HIGH);                                   // stop / guard
  target = t0 + 11U * etuUs;
  while ((int32_t)(micros() - target) < 0) { }

  pinMode(_pinIO, INPUT);
  return true;
}

// ---------------------------------------------------------------------------
// Bit-bang T=0 — single byte receive
//
// Waits up to timeoutMicros for a falling edge (start bit), then samples
// the centre of each of the 8 data bits and the parity bit.  Parity errors
// are not enforced (the OATH applets we target retry at a higher level).
// ---------------------------------------------------------------------------
int SmartCard::_recvByteRaw(uint8_t* out, uint32_t timeoutMicros) {
  uint16_t etuUs = _etuUs;

  uint32_t startWait = micros();
  while (digitalRead(_pinIO) == HIGH) {
    if (!present())                            return -2;
    if ((micros() - startWait) > timeoutMicros) return -1;
  }
  uint32_t fallEdge = micros();

  // Sample data bits at fallEdge + (1.5 + i) * etu, i = 0..7
  uint8_t b = 0;
  for (uint8_t i = 0; i < 8; i++) {
    uint32_t target = fallEdge + ((uint32_t)etuUs * (3 + 2 * i)) / 2;
    while ((int32_t)(micros() - target) < 0) { }
    if (digitalRead(_pinIO)) b |= (uint8_t)(1U << i);
  }

  // Sample parity at 9.5 etu (read but don't enforce)
  uint32_t parTarget = fallEdge + ((uint32_t)etuUs * 19U) / 2;
  while ((int32_t)(micros() - parTarget) < 0) { }
  (void)digitalRead(_pinIO);

  // Wait through the rest of the parity slot so the next call does not
  // immediately see the trailing edge of this character.
  uint32_t endTarget = fallEdge + (uint32_t)etuUs * 10U;
  while ((int32_t)(micros() - endTarget) < 0) { }

  *out = b;
  return 0;
}

// ---------------------------------------------------------------------------
// Bit-bang T=0 — TS-byte receive with ETU auto-detection
//
// The card transmits TS first (always 0x3B for direct convention or
// 0x3F for inverse). For direct convention the line moves like this,
// each segment 1 etu wide:
//
//   start(L) bit0(H) bit1(H) bit2(L) bit3(H) bit4(H) bit5(H) bit6(L) bit7(L) parity stop
//      0       1       2       3       4       5       6       7       8        9     10
//
// So the time from the FIRST falling edge (start of start bit) to the
// SECOND falling edge (start of bit-2) is exactly 3 etu — independent
// of any clock divider in the path. We measure that interval and use
// it as the authoritative ETU for the rest of the session.
//
// After detection we still owe ourselves bits 3..7 + parity of TS to
// fill in the byte. We sample those at the corrected ETU.
// ---------------------------------------------------------------------------
int SmartCard::_receiveTSAuto(uint8_t* out, uint32_t timeoutMicros) {
  // Wait for first falling edge (start of start bit)
  uint32_t startWait = micros();
  while (digitalRead(_pinIO) == HIGH) {
    if (!present())                            return -2;
    if ((micros() - startWait) > timeoutMicros) return -1;
  }
  uint32_t fall1 = micros();

  // Wait for first rising edge (end of start bit, beginning of bit-0).
  // Generous timeout: a single bit at very slow clocks could be many ms.
  uint32_t edgeTimeout = (uint32_t)_etuUs * 16U;
  if (edgeTimeout < 5000) edgeTimeout = 5000;
  while (digitalRead(_pinIO) == LOW) {
    if (!present())                       return -2;
    if ((micros() - fall1) > edgeTimeout) return -1;
  }

  // Wait for second falling edge (bit-1 -> bit-2 transition, 3 etu after fall1)
  while (digitalRead(_pinIO) == HIGH) {
    if (!present())                       return -2;
    if ((micros() - fall1) > edgeTimeout) return -1;
  }
  uint32_t fall2 = micros();

  uint32_t newEtuUs = (fall2 - fall1 + 1) / 3;     // round to nearest
  if (newEtuUs == 0) return -1;
  _etuUs = (uint16_t)newEtuUs;

  // Sample the remaining 5 data bits + parity at the corrected ETU.
  // We already know bits 0, 1, 2 = 1, 1, 0 from the edges we observed.
  uint8_t b = 0x03;                                // bits 0,1 set; bit 2 clear
  for (uint8_t i = 3; i < 8; i++) {
    uint32_t target = fall1 + ((uint32_t)_etuUs * (3 + 2 * i)) / 2;
    while ((int32_t)(micros() - target) < 0) { }
    if (digitalRead(_pinIO)) b |= (uint8_t)(1U << i);
  }

  // Parity (read & ignore)
  uint32_t parTarget = fall1 + ((uint32_t)_etuUs * 19U) / 2;
  while ((int32_t)(micros() - parTarget) < 0) { }
  (void)digitalRead(_pinIO);

  // Wait through stop bit before returning, so the next byte's
  // falling edge isn't already passed by the time we look.
  uint32_t endTarget = fall1 + (uint32_t)_etuUs * 10U;
  while ((int32_t)(micros() - endTarget) < 0) { }

  *out = b;
  return 0;
}
//
// ISO 7816-3: minimum delay between leading edges of two consecutive
// characters in the same direction = (12 + N) etu, where N = TC1.
// Our _sendByteRaw consumes 11 etu, so the gap we still need to insert
// is (12 + N) - 11 = (1 + N) etu.
// ---------------------------------------------------------------------------
bool SmartCard::_txBytes(const uint8_t* buf, size_t len) {
  if (!buf || len == 0) return true;
  uint32_t gapUs = (uint32_t)(1 + _extraGuardEtu) * _etuUs;
  for (size_t i = 0; i < len; i++) {
    if (!present()) return false;
    if (i > 0) delayMicroseconds(gapUs);
    _sendByteRaw(buf[i]);
  }
  return true;
}

// ---------------------------------------------------------------------------
// Multi-byte receive: collect bytes until a timeout occurs or buf is full.
// firstTimeoutMicros applies to the FIRST byte; subsequent bytes use a
// shorter inter-byte timeout derived from ETU + extra guard.
// ---------------------------------------------------------------------------
size_t SmartCard::_rxBytes(uint8_t* buf, size_t maxLen,
                           uint32_t firstTimeoutMicros) {
  if (!buf || maxLen == 0) return 0;

  // Generous inter-byte budget: ~50 etu + extra guard, floored at 5 ms.
  uint32_t interByteUs = (uint32_t)(50 + _extraGuardEtu) * _etuUs;
  if (interByteUs < 5000) interByteUs = 5000;

  size_t   got     = 0;
  uint32_t timeout = firstTimeoutMicros;
  while (got < maxLen) {
    int r = _recvByteRaw(&buf[got], timeout);
    if (r != 0) break;
    got++;
    timeout = interByteUs;
  }
  return got;
}

// ---------------------------------------------------------------------------
// _transmitT0() — one full T=0 APDU exchange (procedure-byte protocol)
// Caller (transmit()) has already validated _active, present(), and apduLen.
// ---------------------------------------------------------------------------
int SmartCard::_transmitT0(const uint8_t* apdu, size_t apduLen,
                           uint8_t* respBuf, size_t* respLen) {
  size_t capacity = (respLen) ? *respLen : 0;
  if (respLen) *respLen = 0;

  // Parse case from total length
  uint8_t cla = apdu[0];
  uint8_t ins = apdu[1];
  uint8_t p1  = apdu[2];
  uint8_t p2  = apdu[3];

  uint8_t        lc      = 0;
  uint8_t        le      = 0;
  bool           hasData = false;
  bool           hasLe   = false;
  const uint8_t* data    = nullptr;

  if (apduLen == 4) {
    /* Case 1 */
  } else if (apduLen == 5) {
    le    = apdu[4];
    hasLe = true;                                  // Case 2
  } else {
    lc = apdu[4];
    if (apduLen == (size_t)5 + lc) {
      hasData = true;                              // Case 3
      data    = &apdu[5];
    } else if (apduLen == (size_t)6 + lc) {
      hasData = true;                              // Case 4
      hasLe   = true;
      data    = &apdu[5];
      le      = apdu[5 + lc];
    } else {
      return ERR_BAD_APDU;
    }
  }

  // P3 of the header: Lc for cases 3/4, Le for case 2, 0 for case 1.
  uint8_t header[5] = {
    cla, ins, p1, p2,
    (uint8_t)(hasData ? lc : (hasLe ? le : 0))
  };

  if (!_txBytes(header, 5)) return ERR_CARD_GONE;

  uint32_t wwt = _wwtMicros;
  if (wwt < MIN_BYTE_TIMEOUT_US) wwt = MIN_BYTE_TIMEOUT_US;

  uint8_t  recvBuf[SC_MAX_RESP_CHUNK];
  size_t   recvCount = 0;
  uint8_t  dataSent  = 0;

  while (true) {
    if (!present()) return ERR_CARD_GONE;

    uint8_t proc;
    int r = _recvByteRaw(&proc, wwt);
    if (r == -1) return ERR_TIMEOUT;
    if (r == -2) return ERR_CARD_GONE;

    // 0x60 = NULL byte (waiting time extension request) — ignore
    if (proc == 0x60) continue;

    // SW1 directly?  Range is 6X / 9X.
    if ((proc & 0xF0) == 0x60 || (proc & 0xF0) == 0x90) {
      uint8_t sw2;
      r = _recvByteRaw(&sw2, wwt);
      if (r != 0) return ERR_TIMEOUT;
      if (respLen) {
        size_t n = (recvCount < capacity) ? recvCount : capacity;
        if (n) memcpy(respBuf, recvBuf, n);
        *respLen = n;
      }
      return ((int)proc << 8) | sw2;
    }

    // ACK ALL — INS echo: card accepts/sends ALL remaining bytes
    if (proc == ins) {
      if (hasData && dataSent < lc) {
        if (!_txBytes(data + dataSent, lc - dataSent)) return ERR_CARD_GONE;
        dataSent = lc;
        // Loop again to read SW (or, for case 4, 61xx)
        continue;
      }
      if (hasLe && !hasData) {
        // Case 2: card now sends Le data bytes, then SW
        size_t toRecv = (le == 0) ? 256 : le;
        for (size_t i = 0; i < toRecv; i++) {
          uint8_t bb;
          r = _recvByteRaw(&bb, wwt);
          if (r != 0) return ERR_TIMEOUT;
          if (recvCount < sizeof(recvBuf)) recvBuf[recvCount++] = bb;
        }
        continue;     // Loop to read SW
      }
      continue;
    }

    // ACK ONE — INS xor 0xFF: send/receive exactly one more byte, then
    // expect another procedure byte.
    if (proc == (uint8_t)(ins ^ 0xFF)) {
      if (hasData && dataSent < lc) {
        if (!_txBytes(data + dataSent, 1)) return ERR_CARD_GONE;
        dataSent++;
      } else if (hasLe && !hasData) {
        uint8_t bb;
        r = _recvByteRaw(&bb, wwt);
        if (r != 0) return ERR_TIMEOUT;
        if (recvCount < sizeof(recvBuf)) recvBuf[recvCount++] = bb;
      }
      continue;
    }

    // Anything else — treat as SW1 and read SW2.
    {
      uint8_t sw2 = 0;
      r = _recvByteRaw(&sw2, wwt);
      if (respLen) {
        size_t n = (recvCount < capacity) ? recvCount : capacity;
        if (n) memcpy(respBuf, recvBuf, n);
        *respLen = n;
      }
      return ((int)proc << 8) | (r == 0 ? sw2 : 0);
    }
  }
}

// ---------------------------------------------------------------------------
// transmit() — public dispatcher; routes to _transmitT0 or _transmitT1
// based on the protocol detected in the ATR.
// ---------------------------------------------------------------------------
int SmartCard::transmit(const uint8_t* apdu, size_t apduLen,
                        uint8_t* respBuf, size_t* respLen) {
  if (!_active)    return ERR_NOT_ACTIVE;
  if (!present())  return ERR_CARD_GONE;
  if (apduLen < 4) return ERR_BAD_APDU;

  if (_protocol == PROTO_T1) {
    return _transmitT1(apdu, apduLen, respBuf, respLen);
  }
  return _transmitT0(apdu, apduLen, respBuf, respLen);
}

// ---------------------------------------------------------------------------
// _transmitT1() — one full T=1 APDU exchange (block protocol)
//
// Block format: NAD | PCB | LEN | INF[LEN] | LRC
//   NAD : node addressing, 0x00 = "any source/dest"
//   PCB : protocol control byte
//           0xxxxxxx = I-block;  bit 6 = N(S), bit 5 = M (more data)
//           10xxxxxx = R-block;  bit 4 = N(R), bits 0-3 = error code
//           11xxxxxx = S-block;  control (resync, IFS, abort, WTX, ...)
//   LEN : 0..254, length of INF
//   INF : payload (full APDU for I-blocks)
//   LRC : XOR of NAD..last INF byte
//
// We send the entire APDU as a single I-block (max 254 bytes — fine for
// every OATH command).  We handle multi-block CARD responses by acking
// each I-block with an R-block until the M bit clears.  We answer S-block
// WTX requests so the card can ask for more time during heavy crypto.
// ---------------------------------------------------------------------------
int SmartCard::_transmitT1(const uint8_t* apdu, size_t apduLen,
                           uint8_t* respBuf, size_t* respLen) {
  size_t capacity = (respLen) ? *respLen : 0;
  if (respLen) *respLen = 0;
  if (apduLen > 254) return ERR_BAD_APDU;

  // ----- Build & send I-block -----
  uint8_t blk[260];
  blk[0] = 0x00;                              // NAD
  blk[1] = _t1Ns ? 0x40 : 0x00;               // PCB: I-block, N(S), M=0
  blk[2] = (uint8_t)apduLen;
  memcpy(&blk[3], apdu, apduLen);
  uint8_t lrc = 0;
  for (size_t i = 0; i < (size_t)3 + apduLen; i++) lrc ^= blk[i];
  blk[3 + apduLen] = lrc;

  if (!_txBytes(blk, 4 + apduLen)) return ERR_CARD_GONE;
  _t1Ns ^= 1;

  // ----- Receive (possibly chained) response -----
  uint32_t bwtBase = (uint32_t)(11U + (1UL << _t1Bwi) * 960UL) * _etuUs;
  if (bwtBase < MIN_BYTE_TIMEOUT_US) bwtBase = MIN_BYTE_TIMEOUT_US;
  uint32_t cwtUs   = (uint32_t)(11U + (1UL << _t1Cwi)) * _etuUs;
  if (cwtUs < 5000) cwtUs = 5000;
  uint32_t bwtUs   = bwtBase;

  size_t  totalRx = 0;     // total INF bytes across all I-blocks
  size_t  writePos = 0;    // position in respBuf
  uint8_t lastSw1 = 0;     // SW from end of last block (used if respBuf truncated)
  uint8_t lastSw2 = 0;

  while (true) {
    if (!present()) return ERR_CARD_GONE;

    // Read header
    if (_recvByteRaw(&blk[0], bwtUs) != 0) return ERR_TIMEOUT;
    if (_recvByteRaw(&blk[1], cwtUs) != 0) return ERR_TIMEOUT;
    if (_recvByteRaw(&blk[2], cwtUs) != 0) return ERR_TIMEOUT;
    uint8_t infLen = blk[2];
    if (infLen > 254) return ERR_TIMEOUT;

    // Read INF + LRC
    for (uint16_t k = 0; k < (uint16_t)infLen + 1; k++) {
      if (_recvByteRaw(&blk[3 + k], cwtUs) != 0) return ERR_TIMEOUT;
    }

    // Verify LRC
    uint8_t lrcCheck = 0;
    for (uint16_t k = 0; k < (uint16_t)3 + infLen; k++) lrcCheck ^= blk[k];
    if (lrcCheck != blk[3 + infLen]) {
      Serial.printf("[SC] T=1 LRC mismatch (got %02X exp %02X, PCB=%02X)\n",
                    blk[3 + infLen], lrcCheck, blk[1]);
      return ERR_TIMEOUT;
    }

    bwtUs = bwtBase;            // reset BWT after any WTX extension
    uint8_t pcb = blk[1];

    // ---- I-block ----
    if ((pcb & 0x80) == 0) {
      bool moreData = (pcb & 0x20) != 0;
      uint8_t cardNs = (pcb >> 6) & 1;

      // Save SW candidates from the END of THIS block (in case it's last)
      if (infLen >= 2) {
        lastSw1 = blk[3 + infLen - 2];
        lastSw2 = blk[3 + infLen - 1];
      } else if (infLen == 1) {
        lastSw2 = blk[3];
        if (writePos > 0) lastSw1 = respBuf[writePos - 1];
      }

      // Append INF to caller's buffer (with truncation)
      for (uint16_t k = 0; k < infLen; k++) {
        if (writePos < capacity) respBuf[writePos++] = blk[3 + k];
      }
      totalRx += infLen;

      if (moreData) {
        // Send R-block to ack and request next I-block
        uint8_t rblk[4];
        rblk[0] = 0x00;
        rblk[1] = 0x80 | (((cardNs ^ 1) & 1) << 4);   // R-block, N(R)=cardNs^1
        rblk[2] = 0x00;
        rblk[3] = rblk[0] ^ rblk[1] ^ rblk[2];
        if (!_txBytes(rblk, 4)) return ERR_CARD_GONE;
        continue;
      }

      // Last I-block: extract SW1 SW2.
      if (totalRx < 2) return ERR_TIMEOUT;

      uint8_t sw1, sw2;
      if (writePos == totalRx && writePos >= 2) {
        // Everything fit in respBuf — SW are the last two written bytes
        sw1 = respBuf[writePos - 2];
        sw2 = respBuf[writePos - 1];
        writePos -= 2;
      } else {
        // Truncated; fall back to the SW we saved from the last block
        sw1 = lastSw1;
        sw2 = lastSw2;
        Serial.printf("[SC] T=1 response truncated: %u of %u bytes kept\n",
                      (unsigned)writePos, (unsigned)totalRx);
      }

      if (respLen) *respLen = writePos;
      return ((int)sw1 << 8) | sw2;
    }

    // ---- R-block from card (typically requesting our retransmission) ----
    if ((pcb & 0xC0) == 0x80) {
      Serial.printf("[SC] T=1 R-block PCB=%02X from card (no recovery)\n", pcb);
      return ERR_TIMEOUT;
    }

    // ---- S-block ----
    uint8_t sFn = pcb & 0x3F;
    if (sFn == 0x03 && infLen == 1) {
      // WTX request: card wants more time. Echo back the multiplier as
      // an S-block WTX response (PCB = 0xE3) and extend BWT for the next
      // block read.
      uint8_t wtxMul = blk[3];
      Serial.printf("[SC] T=1 WTX request: %ux BWT\n", wtxMul);

      uint8_t sblk[5];
      sblk[0] = 0x00;
      sblk[1] = 0xE3;
      sblk[2] = 0x01;
      sblk[3] = wtxMul;
      sblk[4] = sblk[0] ^ sblk[1] ^ sblk[2] ^ sblk[3];
      if (!_txBytes(sblk, 5)) return ERR_CARD_GONE;

      bwtUs = (wtxMul ? wtxMul : 1) * bwtBase;
      continue;
    }

    Serial.printf("[SC] T=1 unsupported S-block PCB=%02X\n", pcb);
    return ERR_TIMEOUT;
  }
}

// ---------------------------------------------------------------------------
// transmitChained() — for T=0, issue GET RESPONSE on SW=61xx and accumulate.
// For T=1 we DON'T attempt GET RESPONSE: many T=1 applets (including the
// Apex OATH applet on this card) return SW=6D00 if you send GET RESPONSE
// over T=1, because the proper T=1 way to get more data is I-block chaining
// (M-bit), which is handled inside transmit(). Long responses are made to
// fit in one I-block by negotiating IFSD up at session start (see
// _t1NegotiateIfsd called from powerOn).
// ---------------------------------------------------------------------------
int SmartCard::transmitChained(const uint8_t* apdu, size_t apduLen,
                               uint8_t* respBuf, size_t* respLen) {
  if (!_active)    return ERR_NOT_ACTIVE;
  if (apduLen < 4) return ERR_BAD_APDU;

  if (_protocol == PROTO_T1) {
    return transmit(apdu, apduLen, respBuf, respLen);
  }

  size_t capacity = (respLen) ? *respLen : 0;
  size_t total    = 0;
  if (respLen) *respLen = 0;

  size_t partLen = capacity;
  int    sw      = transmit(apdu, apduLen, respBuf, &partLen);
  if (sw < 0) {
    if (respLen) *respLen = 0;
    return sw;
  }
  total = partLen;

  uint8_t cla = apdu[0];
  while ((sw & 0xFF00) == 0x6100) {
    uint8_t rem = (uint8_t)(sw & 0xFF);
    uint8_t getResp[5] = { cla, 0xC0, 0x00, 0x00, rem };

    size_t partCap = (capacity > total) ? (capacity - total) : 0;
    if (partCap == 0) break;

    partLen = partCap;
    sw      = transmit(getResp, 5, respBuf + total, &partLen);
    if (sw < 0) {
      if (respLen) *respLen = total;
      return sw;
    }
    total += partLen;
  }

  if (respLen) *respLen = total;
  return sw;
}

// ---------------------------------------------------------------------------
// _t1NegotiateIfsd() — send S-block IFS request to set our IFSD.
//
// Wire format (5 bytes total, no INF chaining):
//   NAD  PCB  LEN=01  INF=newIfsd  LRC
//   00   C1   01      <ifsd>       <xor of all preceding>
//
// Card replies with IFS response S-block:
//   NAD  PCB  LEN=01  INF=newIfsd  LRC
//   00   E1   01      <ifsd>       <xor>
// ---------------------------------------------------------------------------
bool SmartCard::_t1NegotiateIfsd(uint8_t newIfsd) {
  if (_protocol != PROTO_T1) return true;
  if (newIfsd < 1 || newIfsd > 254) return false;

  uint8_t blk[5];
  blk[0] = 0x00;
  blk[1] = 0xC1;          // S-block, IFS request
  blk[2] = 0x01;
  blk[3] = newIfsd;
  blk[4] = blk[0] ^ blk[1] ^ blk[2] ^ blk[3];

  if (!_txBytes(blk, 5)) return false;

  // Generous timeouts here — first-byte BWT, then character-to-character.
  uint32_t firstUs = 200000;     // 200 ms is plenty before the card responds
  uint32_t charUs  = (uint32_t)(11U + (1UL << _t1Cwi)) * _etuUs;
  if (charUs < 5000) charUs = 5000;

  uint8_t rblk[5];
  if (_recvByteRaw(&rblk[0], firstUs) != 0) return false;
  for (int k = 1; k < 5; k++) {
    if (_recvByteRaw(&rblk[k], charUs) != 0) return false;
  }

  uint8_t lrcCheck = rblk[0] ^ rblk[1] ^ rblk[2] ^ rblk[3];
  if (lrcCheck != rblk[4])     return false;
  if (rblk[1] != 0xE1)         return false;   // wrong S-block response
  if (rblk[2] != 0x01)         return false;
  if (rblk[3] != newIfsd)      return false;

  _t1Ifsd = newIfsd;
  return true;
}
