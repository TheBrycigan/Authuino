/*
 * sc_interface.cpp
 * Smartcard driver for Authuino, on top of Authuino_SmartCard.
 *
 * The new SmartCard class handles the full T=0 procedure-byte protocol
 * (NULL 0x60, ACK ALL = INS, ACK ONE = INS xor 0xFF, direct SW1) and
 * GET RESPONSE chaining for SW=61xx.  This file only contains:
 *
 *   - A thin debug-logging wrapper around sc.transmitChained()
 *   - OATH-specific helpers (SELECT, LIST, CALCULATE ALL)
 *   - State management (SCState struct + cached credentials)
 */

#include "sc_interface.h"
#include "nvs_meta.h"
#include "nvs_aid.h"
#include "rtc_time.h"
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>
#include <esp_random.h>

// ---------------------------------------------------------------------------
// Module-private state
// ---------------------------------------------------------------------------
static SmartCard sc(SC_IO, SC_RSTIN, SC_CMDVCC, SC_OFFn, SC_CLK);
static SCState   _state = {};
static uint8_t   _atr[SC_MAX_ATR_BYTES];
static int       _atrLen = 0;

// OATH auth material parsed out of the SELECT response. These live only
// in module memory; the UI never sees them.
static uint8_t   _oathSalt[16];
static uint8_t   _oathSaltLen      = 0;
static uint8_t   _oathChallenge[8];
static uint8_t   _oathChallengeLen = 0;
static uint8_t   _oathAlgo         = 0;

// Card clock frequency. 4 MHz is well within the ISO 7816-3 1..5 MHz
// range and works for every OATH applet I've tested. If a particular
// card is fussy try 3.57 MHz (3571429) or 1 MHz (1000000).
static constexpr uint32_t CARD_CLK_HZ = 4000000;

// Set to 1 only when actively debugging APDU traffic. When enabled, the
// raw command and response bytes are printed to serial — these contain
// PUT secrets, the VALIDATE response (PIN-derived HMAC), and CALCULATE
// truncated TOTP results. Leave at 0 for normal operation.
#define SC_DEBUG_APDU  0

// ---------------------------------------------------------------------------
// Internal: APDU exchange wrapper that logs TX/RX
// ---------------------------------------------------------------------------
static int apdu_exchange(const uint8_t *cmd, size_t cmdLen,
                         uint8_t *respData, size_t *respLen) {
#if SC_DEBUG_APDU
  Serial.print("[SC] TX:");
  for (size_t i = 0; i < cmdLen && i < 40; i++) Serial.printf(" %02X", cmd[i]);
  if (cmdLen > 40) Serial.print(" ...");
  Serial.println();
#endif

  int sw = sc.transmitChained(cmd, cmdLen, respData, respLen);

  if (sw < 0) {
    const char *reason = "unknown";
    switch (sw) {
      case SmartCard::ERR_NOT_ACTIVE: reason = "not active";       break;
      case SmartCard::ERR_TIMEOUT:    reason = "timeout";          break;
      case SmartCard::ERR_CARD_GONE:  reason = "card removed";     break;
      case SmartCard::ERR_BAD_APDU:   reason = "bad APDU";         break;
      case SmartCard::ERR_NO_CLOCK:   reason = "no clock";         break;
    }
    Serial.printf("[SC] APDU error %d (%s)\n", sw, reason);
  } else {
#if SC_DEBUG_APDU
    Serial.printf("[SC] SW=%04X (%u data)", (uint16_t)sw, (unsigned)*respLen);
    if (*respLen > 0) {
      Serial.print(":");
      for (size_t i = 0; i < *respLen && i < 32; i++) Serial.printf(" %02X", respData[i]);
      if (*respLen > 32) Serial.print(" ...");
    }
    Serial.println();
#else
    // Production: print SW + payload size only, no payload bytes.
    if (sw != 0x9000) {
      Serial.printf("[SC] SW=%04X (%u data)\n", (uint16_t)sw, (unsigned)*respLen);
    }
#endif
  }
  return sw;
}

// ---------------------------------------------------------------------------
// Crypto helpers (mbedTLS)
// ---------------------------------------------------------------------------
static void hmac_sha1(const uint8_t* key, size_t keyLen,
                      const uint8_t* msg, size_t msgLen,
                      uint8_t out[20]) {
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
  mbedtls_md_hmac(info, key, keyLen, msg, msgLen, out);
}

static bool pbkdf2_hmac_sha1(const char* pin, const uint8_t* salt, size_t saltLen,
                             uint32_t iterations, uint8_t* out, size_t outLen) {
  int rc = mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA1,
                                         (const unsigned char*)pin, strlen(pin),
                                         salt, saltLen,
                                         iterations,
                                         (uint32_t)outLen, out);
  return rc == 0;
}

// ---------------------------------------------------------------------------
// OATH: parse SELECT response, extracting version / salt / challenge / algo.
// Sets _state.needsAuth based on whether tag 0x74 (challenge) is present.
// ---------------------------------------------------------------------------
static void parse_select_response(const uint8_t* data, size_t len) {
  _state.needsAuth        = false;
  _state.authenticated    = false;
  _state.attemptsRemaining = -1;
  _oathSaltLen      = 0;
  _oathChallengeLen = 0;
  _oathAlgo         = 0;

  size_t i = 0;
  while (i + 1 < len) {
    uint8_t tag  = data[i++];
    uint8_t tlen = data[i++];
    if (i + tlen > len) break;

    switch (tag) {
      case 0x71:  // Name / salt
        _oathSaltLen = (tlen <= sizeof(_oathSalt)) ? tlen : sizeof(_oathSalt);
        memcpy(_oathSalt, &data[i], _oathSaltLen);
        break;
      case 0x74:  // Challenge — presence => password required
        _state.needsAuth = true;
        _oathChallengeLen = (tlen <= sizeof(_oathChallenge)) ? tlen : sizeof(_oathChallenge);
        memcpy(_oathChallenge, &data[i], _oathChallengeLen);
        break;
      case 0x7B:  // Algorithm of validation key
        if (tlen >= 1) _oathAlgo = data[i];
        break;
      default:
        break;  // ignore unknown tags (version 0x79, etc.)
    }
    i += tlen;
  }
}

// ---------------------------------------------------------------------------
// OATH: SELECT applet
//
// Walks the stored AID list (set by user via Manage tile or AID serial
// commands), trying each in order. If none match — or if the list is
// empty — falls back to the compiled-in APEX_TOTP_AID. The first AID
// that returns SW=9000 wins; its parsed SELECT response populates
// _state.appletSelected, _state.needsAuth, etc.
// ---------------------------------------------------------------------------
static bool try_select_aid(const uint8_t* aid, uint8_t aid_len, const char* label) {
  if (!aid || aid_len == 0) return false;

  uint8_t cmd[5 + NVS_AID_MAX_BYTES];
  cmd[0] = 0x00; cmd[1] = 0xA4; cmd[2] = 0x04;
  cmd[3] = 0x00; cmd[4] = aid_len;
  memcpy(&cmd[5], aid, aid_len);

  uint8_t resp[128];
  size_t  respLen = sizeof(resp);
  int sw = apdu_exchange(cmd, 5 + aid_len, resp, &respLen);

  if (sw != 0x9000) {
    Serial.printf("[SC] SELECT %s -> SW=%04X (try next)\n",
                  label ? label : "(unnamed)", sw);
    return false;
  }

  parse_select_response(resp, respLen);
  Serial.printf("[SC] SELECT %s OK%s\n",
                label ? label : "(unnamed)",
                _state.needsAuth ? " (PIN required)" : "");
  return true;
}

static bool oath_select() {
  int n = nvs_aid_count();
  for (int i = 0; i < n; i++) {
    AidEntry e;
    if (!nvs_aid_get(i, &e)) continue;
    if (try_select_aid(e.aid, e.aid_len, e.name)) return true;
  }
  // Fallback: compiled-in default AID.
  return try_select_aid(APEX_TOTP_AID, APEX_AID_LEN, "default");
}

// ---------------------------------------------------------------------------
// OATH: VALIDATE — derive key from PIN, mutually authenticate with card.
//
// Wire format of the request body (32 bytes total):
//   75 14 [HMAC-SHA1(key, card_challenge)]   (20-byte response)
//   74 08 [our 8-byte random challenge]
//
// On success the card returns:
//   75 14 [HMAC-SHA1(key, our_challenge)]   then SW=9000
// We verify that ourselves to confirm the card knows the same key.
// ---------------------------------------------------------------------------
static bool oath_validate(const char* pin) {
  if (_oathChallengeLen == 0 || _oathSaltLen == 0) {
    Serial.println("[SC] VALIDATE: missing salt or challenge from SELECT");
    return false;
  }

  // 1. Derive 16-byte key via PBKDF2-HMAC-SHA1 (1000 iterations).
  uint8_t key[16];
  if (!pbkdf2_hmac_sha1(pin, _oathSalt, _oathSaltLen, 1000, key, sizeof(key))) {
    Serial.println("[SC] VALIDATE: PBKDF2 failed");
    return false;
  }

  // 2. Compute response = HMAC-SHA1(key, card_challenge) — full 20 bytes.
  uint8_t response[20];
  hmac_sha1(key, sizeof(key), _oathChallenge, _oathChallengeLen, response);

  // 3. Generate our own random 8-byte challenge.
  uint8_t myChallenge[8];
  esp_fill_random(myChallenge, sizeof(myChallenge));

  // 4. Build VALIDATE APDU.
  uint8_t cmd[5 + 2 + 20 + 2 + 8];
  cmd[0] = 0x00; cmd[1] = 0xA3; cmd[2] = 0x00; cmd[3] = 0x00;
  cmd[4] = 32;
  cmd[5] = 0x75; cmd[6] = 20;
  memcpy(&cmd[7], response, 20);
  cmd[27] = 0x74; cmd[28] = 8;
  memcpy(&cmd[29], myChallenge, 8);

  uint8_t respBuf[64];
  size_t  respLen = sizeof(respBuf);
  int sw = apdu_exchange(cmd, sizeof(cmd), respBuf, &respLen);

  bool ok = false;
  if (sw == 0x9000 && respLen >= 22 && respBuf[0] == 0x75 && respBuf[1] == 20) {
    // 5. Verify card's response = HMAC-SHA1(key, my_challenge).
    uint8_t expected[20];
    hmac_sha1(key, sizeof(key), myChallenge, sizeof(myChallenge), expected);

    // Constant-time-ish compare
    uint8_t diff = 0;
    for (int i = 0; i < 20; i++) diff |= expected[i] ^ respBuf[2 + i];
    if (diff == 0) {
      ok = true;
      _state.authenticated     = true;
      _state.needsAuth         = false;
      _state.attemptsRemaining = -1;
      Serial.println("[SC] VALIDATE OK — mutually authenticated");
    } else {
      Serial.println("[SC] VALIDATE: card response did not verify (key mismatch)");
    }
  } else if ((sw & 0xFFF0) == 0x63C0) {
    // PIN incorrect, with attempts remaining encoded in low nibble
    _state.attemptsRemaining = sw & 0x0F;
    Serial.printf("[SC] VALIDATE: wrong PIN, %d attempts remaining\n",
                  _state.attemptsRemaining);
  } else if (sw == 0x6983) {
    _state.attemptsRemaining = 0;
    Serial.println("[SC] VALIDATE: card LOCKED");
  } else if (sw == 0x6A80 || sw == 0x6982 || sw == 0x6984) {
    _state.attemptsRemaining = -1;   // applet doesn't report attempts
    Serial.printf("[SC] VALIDATE: wrong PIN (SW=%04X)\n", sw);
  } else {
    Serial.printf("[SC] VALIDATE failed SW=%04X\n", sw);
  }

  // Wipe key material before returning
  memset(key,        0, sizeof(key));
  memset(response,   0, sizeof(response));
  memset(myChallenge,0, sizeof(myChallenge));
  return ok;
}

// ---------------------------------------------------------------------------
// OATH: LIST credentials
// ---------------------------------------------------------------------------
static bool oath_list() {
  uint8_t cmd[]  = { 0x00, OATH_INS_LIST, 0x00, 0x00 };
  uint8_t resp[512];
  size_t  respLen = sizeof(resp);
  _state.numCredentials = 0;

  int sw = apdu_exchange(cmd, sizeof(cmd), resp, &respLen);
  if (sw != 0x9000) {
    Serial.printf("[SC] LIST failed SW=%04X\n", sw);
    return false;
  }

  // Response is a series of name-list TLVs:
  //   72 LL <algo> <name bytes...>
  size_t i = 0;
  while (i < respLen && _state.numCredentials < MAX_CREDENTIALS) {
    if (resp[i] != OATH_TAG_NAME_LIST) { i++; continue; }
    i++;
    if (i >= respLen) break;
    size_t tlvLen = resp[i++];
    if (tlvLen < 1 || i + tlvLen > respLen) break;
    i++;                                            // skip algo byte
    size_t nameLen = tlvLen - 1;
    size_t copy = (nameLen < MAX_CRED_NAME_LEN - 1) ? nameLen : (MAX_CRED_NAME_LEN - 1);
    memcpy(_state.credentials[_state.numCredentials].name, resp + i, copy);
    _state.credentials[_state.numCredentials].name[copy] = '\0';
    _state.credentials[_state.numCredentials].code[0]    = '\0';
    _state.credentials[_state.numCredentials].valid       = false;
    _state.numCredentials++;
    i += nameLen;
  }

  Serial.printf("[SC] Listed %d credential(s)\n", _state.numCredentials);

  // Stamp newly-seen credentials with their first-seen timestamp, then
  // reconcile NVS so any orphaned metadata (creds deleted by another
  // reader since we last saw them) is pruned. The card is truth.
  if (rtc_isRunning()) {
    uint32_t now = (uint32_t)rtc_epoch();
    for (int i = 0; i < _state.numCredentials; i++) {
      nvs_meta_ensure(_state.credentials[i].name, now);
    }
  }
  const char* names[MAX_CREDENTIALS];
  for (int i = 0; i < _state.numCredentials; i++) {
    names[i] = _state.credentials[i].name;
  }
  nvs_meta_reconcile(names, _state.numCredentials);
  return true;
}

// ---------------------------------------------------------------------------
// OATH: CALCULATE ALL TOTP
// ---------------------------------------------------------------------------
static bool oath_calc_all(time_t epochNow) {
  if (epochNow == 0) {
    Serial.println("[SC] CALC ALL skipped: epoch is 0");
    return false;
  }

  for (int i = 0; i < _state.numCredentials; i++) {
    _state.credentials[i].valid = false;
  }

  uint64_t step = (uint64_t)epochNow / 30;
  uint8_t challenge[8] = {
    (uint8_t)(step >> 56), (uint8_t)(step >> 48),
    (uint8_t)(step >> 40), (uint8_t)(step >> 32),
    (uint8_t)(step >> 24), (uint8_t)(step >> 16),
    (uint8_t)(step >>  8), (uint8_t)(step      )
  };

  uint8_t cmd[15];
  cmd[0] = 0x00; cmd[1] = OATH_INS_CALC_ALL;
  cmd[2] = 0x00; cmd[3] = 0x01;          // P2 = 0x01 = truncated response
  cmd[4] = 10;                            // Lc
  cmd[5] = OATH_TAG_CHALLENGE; cmd[6] = 8;
  memcpy(&cmd[7], challenge, 8);

  uint8_t resp[512];
  size_t  respLen = sizeof(resp);
  int sw = apdu_exchange(cmd, sizeof(cmd), resp, &respLen);
  if (sw != 0x9000) {
    Serial.printf("[SC] CALC ALL failed SW=%04X\n", sw);
    return false;
  }

  // Response: alternating (71 LL name) and (76 LL digits + 4 bytes raw) TLVs.
  // 77 (no response) appears for credentials that require touch/HOTP.
  size_t idx     = 0;
  int    credIdx = -1;
  while (idx < respLen) {
    uint8_t tag = resp[idx++];
    if (idx >= respLen) break;
    uint8_t tlen = resp[idx++];
    if (idx + tlen > respLen) break;

    if (tag == OATH_TAG_NAME) {
      char name[MAX_CRED_NAME_LEN];
      size_t copy = (tlen < MAX_CRED_NAME_LEN - 1) ? tlen : (MAX_CRED_NAME_LEN - 1);
      memcpy(name, resp + idx, copy);
      name[copy] = '\0';
      credIdx = -1;
      for (int ci = 0; ci < _state.numCredentials; ci++) {
        if (strncmp(_state.credentials[ci].name, name, MAX_CRED_NAME_LEN) == 0) {
          credIdx = ci;
          break;
        }
      }
    } else if (tag == OATH_TAG_RESP_TRUNC && credIdx >= 0 && tlen >= 5) {
      uint8_t  digits = resp[idx];
      uint32_t raw    = ((uint32_t)resp[idx + 1] << 24) |
                        ((uint32_t)resp[idx + 2] << 16) |
                        ((uint32_t)resp[idx + 3] <<  8) |
                        ((uint32_t)resp[idx + 4]);
      raw &= 0x7FFFFFFF;
      uint32_t mod = 1;
      for (int d = 0; d < digits; d++) mod *= 10;
      raw %= mod;
      snprintf(_state.credentials[credIdx].code, MAX_TOTP_DIGITS,
               "%0*lu", (int)digits, (unsigned long)raw);
      _state.credentials[credIdx].valid = true;
      credIdx = -1;
    } else if (tag == OATH_TAG_NO_RESP) {
      credIdx = -1;
    }
    idx += tlen;
  }

  // Summary only — never log per-credential codes or raw response bytes.
  int valid = 0;
  for (int i = 0; i < _state.numCredentials; i++) {
    if (_state.credentials[i].valid) valid++;
  }
  Serial.printf("[SC] CALC ALL: %d/%d code(s) computed\n",
                valid, _state.numCredentials);
  return true;
}

// ---------------------------------------------------------------------------
// Internal: ATR -> hex string for the UI
// ---------------------------------------------------------------------------
static void atr_to_str(const uint8_t *atr, int len, char *buf, int bufLen) {
  buf[0] = '\0';
  for (int i = 0; i < len && (int)(strlen(buf) + 4) < bufLen; i++) {
    char tmp[4];
    snprintf(tmp, sizeof(tmp), "%02X ", atr[i]);
    strncat(buf, tmp, bufLen - strlen(buf) - 1);
  }
}

// ---------------------------------------------------------------------------
// RFC 4648 base32 decode (used by PUT credential and the QR/URI parser).
// Accepts upper or lower case; ignores '=', ' ', and '-'.
// ---------------------------------------------------------------------------
int sc_base32_decode(const char* src, uint8_t* dst, size_t dstMax) {
  if (!src || !dst) return -1;
  uint32_t buf  = 0;
  int      bits = 0;
  size_t   out  = 0;

  for (; *src; src++) {
    char c = *src;
    if (c == '=' || c == ' ' || c == '-' || c == '\t' || c == '\r' || c == '\n')
      continue;
    if (c >= 'a' && c <= 'z') c -= ('a' - 'A');

    int v;
    if      (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= '2' && c <= '7') v = 26 + (c - '2');
    else                           return -1;     // invalid character

    buf  = (buf << 5) | (uint32_t)v;
    bits += 5;
    if (bits >= 8) {
      bits -= 8;
      if (out >= dstMax) return -1;               // output overflow
      dst[out++] = (uint8_t)((buf >> bits) & 0xFF);
    }
  }
  return (int)out;
}

// ---------------------------------------------------------------------------
// OATH: PUT credential
//   Wire format:
//     71 LL [name]
//     73 LL [type|algo] [digits] [secret bytes...]
//     7A 04 [imf big-endian]                (HOTP only, optional)
// ---------------------------------------------------------------------------
bool sc_put_credential(const char* name,
                       const uint8_t* secret, size_t secretLen,
                       uint8_t typeAlgo, uint8_t digits,
                       uint32_t imf) {
  if (!sc.isActive() || !_state.appletSelected) {
    Serial.println("[SC] PUT: applet not selected");
    return false;
  }
  if (_state.needsAuth && !_state.authenticated) {
    Serial.println("[SC] PUT: card requires PIN first");
    return false;
  }
  if (!name || !*name)                  { Serial.println("[SC] PUT: empty name");   return false; }
  if (!secret || secretLen == 0)        { Serial.println("[SC] PUT: empty secret"); return false; }

  size_t nameLen = strlen(name);
  if (nameLen > 64)                     { Serial.println("[SC] PUT: name too long");   return false; }
  if (secretLen > 64)                   { Serial.println("[SC] PUT: secret too long"); return false; }
  if (digits != 6 && digits != 7 && digits != 8) {
    Serial.printf("[SC] PUT: bad digits=%u\n", digits);
    return false;
  }

  // Build APDU body
  uint8_t body[200];
  size_t  i = 0;

  body[i++] = 0x71;
  body[i++] = (uint8_t)nameLen;
  memcpy(&body[i], name, nameLen);  i += nameLen;

  body[i++] = 0x73;
  body[i++] = (uint8_t)(2 + secretLen);
  body[i++] = typeAlgo;
  body[i++] = digits;
  memcpy(&body[i], secret, secretLen);  i += secretLen;

  // HOTP initial moving factor (only meaningful when type nibble = 0x10)
  if ((typeAlgo & 0xF0) == 0x10 && imf != 0) {
    body[i++] = 0x7A;
    body[i++] = 0x04;
    body[i++] = (uint8_t)(imf >> 24);
    body[i++] = (uint8_t)(imf >> 16);
    body[i++] = (uint8_t)(imf >>  8);
    body[i++] = (uint8_t)(imf      );
  }

  // Wrap in APDU
  uint8_t cmd[5 + sizeof(body)];
  cmd[0] = 0x00; cmd[1] = OATH_INS_PUT; cmd[2] = 0x00; cmd[3] = 0x00;
  cmd[4] = (uint8_t)i;
  memcpy(&cmd[5], body, i);

  uint8_t resp[16];
  size_t  respLen = sizeof(resp);
  int sw = apdu_exchange(cmd, 5 + i, resp, &respLen);

  if (sw == 0x9000) {
    Serial.printf("[SC] PUT OK: %s (%u-byte secret)\n", name, (unsigned)secretLen);
    // Refresh credential cache so the new entry is visible
    oath_list();
    return true;
  }
  Serial.printf("[SC] PUT failed SW=%04X\n", sw);
  return false;
}

// ---------------------------------------------------------------------------
// OATH: DELETE credential
//   Wire format: 71 LL [name]
// ---------------------------------------------------------------------------
bool sc_delete_credential(const char* name) {
  if (!sc.isActive() || !_state.appletSelected) {
    Serial.println("[SC] DELETE: applet not selected");
    return false;
  }
  if (_state.needsAuth && !_state.authenticated) {
    Serial.println("[SC] DELETE: card requires PIN first");
    return false;
  }
  if (!name || !*name) return false;
  size_t nameLen = strlen(name);
  if (nameLen > 64)    return false;

  uint8_t cmd[5 + 2 + 64];
  cmd[0] = 0x00; cmd[1] = OATH_INS_DELETE; cmd[2] = 0x00; cmd[3] = 0x00;
  cmd[4] = (uint8_t)(2 + nameLen);
  cmd[5] = 0x71;
  cmd[6] = (uint8_t)nameLen;
  memcpy(&cmd[7], name, nameLen);

  uint8_t resp[16];
  size_t  respLen = sizeof(resp);
  int sw = apdu_exchange(cmd, 7 + nameLen, resp, &respLen);

  if (sw == 0x9000) {
    Serial.printf("[SC] DELETE OK: %s\n", name);
    nvs_meta_remove(name);
    oath_list();
    return true;
  }
  Serial.printf("[SC] DELETE failed SW=%04X\n", sw);
  return false;
}

// ---------------------------------------------------------------------------
// OATH: SET CODE — change or remove the card's PIN.
//
// To set a new PIN:
//   Derive key from new PIN via PBKDF2 (same salt from SELECT).
//   Build: KEY tag (0x73) + type+key, CHALLENGE tag (0x74) + 8 bytes,
//          RESPONSE tag (0x75) + HMAC-SHA1(new_key, challenge).
//
// To remove the PIN:
//   Send just an empty KEY tag (0x73, len=0).
// ---------------------------------------------------------------------------
#define OATH_INS_SET_CODE  0x03

bool sc_set_pin(const char* newPin) {
  if (!sc.isActive() || !_state.appletSelected) {
    Serial.println("[SC] SET_CODE: applet not selected");
    return false;
  }
  if (_state.needsAuth && !_state.authenticated) {
    Serial.println("[SC] SET_CODE: must authenticate with current PIN first");
    return false;
  }

  uint8_t cmd[5 + 2 + 17 + 2 + 8 + 2 + 20];  // max size
  int dataLen = 0;

  bool removing = (!newPin || strlen(newPin) == 0);

  if (removing) {
    // Remove PIN: empty KEY tag
    cmd[5] = 0x73;
    cmd[6] = 0x00;
    dataLen = 2;
    Serial.println("[SC] SET_CODE: removing PIN");
  } else {
    // Set new PIN
    if (_oathSaltLen == 0) {
      Serial.println("[SC] SET_CODE: no salt from SELECT — cannot derive key");
      return false;
    }

    // 1. Derive 16-byte key from new PIN
    uint8_t key[16];
    if (!pbkdf2_hmac_sha1(newPin, _oathSalt, _oathSaltLen, 1000, key, sizeof(key))) {
      Serial.println("[SC] SET_CODE: PBKDF2 failed");
      return false;
    }

    // 2. Generate 8-byte challenge
    uint8_t challenge[8];
    esp_fill_random(challenge, sizeof(challenge));

    // 3. Compute response = HMAC-SHA1(key, challenge)
    uint8_t response[20];
    hmac_sha1(key, sizeof(key), challenge, sizeof(challenge), response);

    // 4. Build data: KEY(0x73) + CHALLENGE(0x74) + RESPONSE(0x75)
    int p = 5;
    cmd[p++] = 0x73;
    cmd[p++] = 17;      // 1 byte type + 16 bytes key
    cmd[p++] = 0x21;    // type: TOTP + SHA1 (authentication key type)
    memcpy(&cmd[p], key, 16); p += 16;

    cmd[p++] = 0x74;
    cmd[p++] = 8;
    memcpy(&cmd[p], challenge, 8); p += 8;

    cmd[p++] = 0x75;
    cmd[p++] = 20;
    memcpy(&cmd[p], response, 20); p += 20;

    dataLen = p - 5;

    // Wipe key material
    memset(key, 0, sizeof(key));
    memset(response, 0, sizeof(response));
    Serial.println("[SC] SET_CODE: setting new PIN");
  }

  cmd[0] = 0x00;
  cmd[1] = OATH_INS_SET_CODE;
  cmd[2] = 0x00;
  cmd[3] = 0x00;
  cmd[4] = (uint8_t)dataLen;

  uint8_t resp[16];
  size_t  respLen = sizeof(resp);
  int sw = apdu_exchange(cmd, 5 + dataLen, resp, &respLen);

  if (sw == 0x9000) {
    Serial.println("[SC] SET_CODE OK");
    // The card's auth key changed but our current session stays
    // authenticated — the OATH applet tracks auth as a session flag
    // set by VALIDATE, and SET_CODE doesn't reset it. So LIST /
    // CALC_ALL continue to work in this session without a fresh
    // VALIDATE. On the next reseat / SELECT, the user will need the
    // new PIN.
    //
    // For removal, the card no longer requires auth at all. Clear
    // needsAuth so the UI skips the PIN prompt on future sessions.
    if (removing) {
      _state.needsAuth = false;
    }
    return true;
  }
  Serial.printf("[SC] SET_CODE failed SW=%04X\n", sw);
  return false;
}

// ===========================================================================
// Public API
// ===========================================================================

void sc_init() {
  memset(&_state, 0, sizeof(_state));
  _state.attemptsRemaining = -1;
  _atrLen = 0;
  sc.begin();
  Serial.println("[SC] Smartcard interface initialised");
}

bool sc_poll() {
  bool prev = _state.cardPresent;
  bool now  = sc.present();

  if (now != prev) {
    _state.cardPresent = now;
    if (!now) {
      if (sc.isActive()) sc.powerOff();
      _state.appletSelected    = false;
      _state.needsAuth         = false;
      _state.authenticated     = false;
      _state.attemptsRemaining = -1;
      _state.numCredentials    = 0;
      for (int i = 0; i < MAX_CREDENTIALS; i++) {
        _state.credentials[i].valid   = false;
        _state.credentials[i].code[0] = '\0';
      }
      _state.atrStr[0] = '\0';
      // Wipe any leftover auth material
      memset(_oathSalt,      0, sizeof(_oathSalt));
      memset(_oathChallenge, 0, sizeof(_oathChallenge));
      _oathSaltLen      = 0;
      _oathChallengeLen = 0;
      Serial.println("[SC] Card removed");
    }
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Lightweight AID probe. Powers on the card, tries SELECT on each
// known AID, records which ones respond with SW=9000, then powers
// off. No session state is established — the card is left cold.
// ---------------------------------------------------------------------------
static bool probe_select(const uint8_t* aid, uint8_t aid_len) {
  if (!aid || aid_len == 0) return false;
  uint8_t cmd[5 + NVS_AID_MAX_BYTES];
  cmd[0] = 0x00; cmd[1] = 0xA4; cmd[2] = 0x04;
  cmd[3] = 0x00; cmd[4] = aid_len;
  memcpy(&cmd[5], aid, aid_len);
  uint8_t resp[256];
  size_t  respLen = sizeof(resp);
  int sw = apdu_exchange(cmd, 5 + aid_len, resp, &respLen);
  if (sw != 0x9000) {
    Serial.printf("[SC] probe SELECT (%u bytes AID) -> SW=%04X\n",
                  aid_len, sw);
  }
  return (sw == 0x9000);
}

CardProbeResult sc_probe_card() {
  CardProbeResult r = {};

  // Power on
  if (sc.isActive()) { sc.powerOff(); delay(50); }
  uint8_t atr[SC_MAX_ATR_BYTES];
  int atrLen = sc.powerOn(atr, SC_MAX_ATR_BYTES, CARD_CLK_HZ);
  if (atrLen <= 0) {
    Serial.printf("[SC] probe: no ATR (err %d)\n", atrLen);
    return r;
  }
  Serial.printf("[SC] probe: ATR OK (%d bytes)\n", atrLen);

  // OATH: try stored AIDs first, then default
  int n = nvs_aid_count();
  for (int i = 0; i < n && !r.oath; i++) {
    AidEntry e;
    if (!nvs_aid_get(i, &e)) continue;
    r.oath = probe_select(e.aid, e.aid_len);
  }
  if (!r.oath) r.oath = probe_select(APEX_TOTP_AID, APEX_AID_LEN);

  // FIDO2
  r.fido2 = probe_select(AID_FIDO2, AID_FIDO2_LEN);

  // PIV
  r.piv = probe_select(AID_PIV, AID_PIV_LEN);

  Serial.printf("[SC] probe: OATH=%d FIDO2=%d PIV=%d\n", r.oath, r.fido2, r.piv);

  // Power off — leave the card cold. The applet-specific session
  // (sc_run_session) will power on again when the user taps a tile.
  sc.powerOff();

  return r;
}

bool sc_run_session(time_t epochNow) {
  if (sc.isActive()) {
    sc.powerOff();
    delay(50);
  }

  memset(_atr, 0, sizeof(_atr));
  int atrLen = sc.powerOn(_atr, SC_MAX_ATR_BYTES, CARD_CLK_HZ);

  if (atrLen <= 0) {
    Serial.printf("[SC] powerOn() failed (err %d) — no ATR\n", atrLen);
    _state.cardPresent    = sc.present();
    _state.appletSelected = false;
    _state.needsAuth      = false;
    _state.authenticated  = false;
    return false;
  }

  _atrLen            = atrLen;
  _state.cardPresent = true;
  atr_to_str(_atr, _atrLen, _state.atrStr, sizeof(_state.atrStr));
  Serial.printf("[SC] ATR (%d bytes): %s\n", atrLen, _state.atrStr);
  Serial.printf("[SC] CLK = %lu Hz, ETU = %u us, extra guard = %u etu, protocol = T=%d\n",
                (unsigned long)sc.clockHz(),
                (unsigned)sc.etuMicros(),
                (unsigned)sc.extraGuardEtu(),
                (int)sc.protocol());

  // Brief settle window after ATR before first command.
  delay(10);

  if (!oath_select()) {
    _state.appletSelected = false;
    return false;
  }
  _state.appletSelected = true;

  // If the card requires authentication, stop here. The caller will
  // collect a PIN from the user and call sc_authenticate() to continue.
  if (_state.needsAuth) {
    return true;
  }

  if (!oath_list())            return false;
  return oath_calc_all(epochNow);
}

bool sc_authenticate(const char* pin, time_t epochNow) {
  if (!sc.isActive() || !_state.appletSelected) {
    Serial.println("[SC] authenticate: applet not selected");
    return false;
  }
  if (!_state.needsAuth) {
    // No auth required, but caller invoked us anyway — treat as success
    // and just refresh the credential set.
    if (!oath_list())            return false;
    return oath_calc_all(epochNow);
  }
  if (!pin || pin[0] == '\0') {
    Serial.println("[SC] authenticate: empty PIN rejected");
    return false;
  }

  bool ok = oath_validate(pin);
  if (!ok) return false;

  if (!oath_list())            return false;
  return oath_calc_all(epochNow);
}

bool sc_recalculate(time_t epochNow) {
  if (!sc.isActive() || !_state.appletSelected) {
    Serial.println("[SC] recalculate: not ready");
    return false;
  }
  if (_state.needsAuth && !_state.authenticated) {
    Serial.println("[SC] recalculate: card requires PIN");
    return false;
  }
  return oath_calc_all(epochNow);
}

void sc_deactivate() {
  if (sc.isActive()) sc.powerOff();
  _state.appletSelected = false;
  _state.needsAuth      = false;
  _state.authenticated  = false;
  memset(_oathSalt,      0, sizeof(_oathSalt));
  memset(_oathChallenge, 0, sizeof(_oathChallenge));
  _oathSaltLen      = 0;
  _oathChallengeLen = 0;
  Serial.println("[SC] Deactivated");
}

const SCState& sc_state() { return _state; }

// ---------------------------------------------------------------------------
// USB CCID bridge helpers (Phase 6)
// ---------------------------------------------------------------------------

int sc_get_atr(uint8_t* outBuf, size_t outBufSize) {
  if (_atrLen <= 0)              return 0;
  if ((int)outBufSize < _atrLen) return 0;
  memcpy(outBuf, _atr, _atrLen);
  return _atrLen;
}

bool sc_is_icc_active() {
  return sc.isActive();
}

int sc_recapture_atr(uint8_t* outBuf, size_t outBufSize) {
  if (!_state.cardPresent)       return 0;
  if (outBufSize < SC_MAX_ATR_BYTES) return 0;

  // Cycle the card. This invalidates any host-visible session state
  // (no harm — the host is the one driving us now), but it also
  // invalidates our own _state.appletSelected etc, so we deactivate
  // first to keep _state coherent.
  sc_deactivate();

  if (sc.isActive()) { sc.powerOff(); delay(50); }
  memset(_atr, 0, sizeof(_atr));
  int atrLen = sc.powerOn(_atr, SC_MAX_ATR_BYTES, CARD_CLK_HZ);
  if (atrLen < 2) {
    Serial.printf("[SC-CCID] powerOn failed: %d\n", atrLen);
    _atrLen = 0;
    return 0;
  }
  _atrLen = atrLen;
  atr_to_str(_atr, _atrLen, _state.atrStr, sizeof(_state.atrStr));
  Serial.printf("[SC-CCID] ATR captured (%d bytes)\n", atrLen);

  memcpy(outBuf, _atr, atrLen);
  return atrLen;
}

int sc_apdu_passthrough(const uint8_t* cmd, size_t cmdLen,
                        uint8_t* outBuf, size_t outBufSize) {
  if (!sc.isActive())            return SmartCard::ERR_NOT_ACTIVE;
  if (cmdLen < 4)                return SmartCard::ERR_BAD_APDU;
  // Reserve 2 bytes at the end for SW1 SW2.
  if (outBufSize < 2)            return SmartCard::ERR_BAD_APDU;

  size_t respLen = outBufSize - 2;
  int sw = sc.transmitChained(cmd, cmdLen, outBuf, &respLen);
  if (sw < 0) {
    Serial.printf("[SC-CCID] APDU passthrough error %d\n", sw);
    return sw;
  }
  outBuf[respLen]     = (uint8_t)((sw >> 8) & 0xFF);
  outBuf[respLen + 1] = (uint8_t)(sw & 0xFF);
  return (int)respLen + 2;
}

// ===========================================================================
// PIV (NIST SP 800-73-4) implementation
// ===========================================================================

static int s_piv_last_select_sw = 0;

bool sc_piv_select() {
  s_piv_last_select_sw = 0;
  if (!_state.cardPresent) return false;

  // ALWAYS cold-reset the card before SELECT. The card may be in a
  // confused state from earlier probe attempts that timed out (e.g.,
  // OATH probe on a PIV-only card). A power cycle gives us a known
  // clean slate. CCID host flow does the same thing per IccPowerOn.
  if (sc.isActive()) {
    sc.powerOff();
    delay(50);
  }
  int n = sc.powerOn(_atr, SC_MAX_ATR_BYTES, CARD_CLK_HZ);
  if (n < 2) return false;
  _atrLen = n;
  atr_to_str(_atr, _atrLen, _state.atrStr, sizeof(_state.atrStr));

  // Some cards (especially federal PIV with custom firmware) need
  // more idle time between the last ATR byte and the first APDU
  // than the ISO 7816-3 minimum of 12 etu. CCID flow naturally
  // provides this via USB protocol overhead between IccPowerOn
  // response and the next command. Direct path is too fast — give
  // the card breathing room before SELECT.
  delay(50);

  // Build SELECT manually so we can capture the SW even on failure
  // (try_select_aid drops it).
  uint8_t cmd[5 + 16];
  cmd[0] = 0x00; cmd[1] = 0xA4; cmd[2] = 0x04;
  cmd[3] = 0x00; cmd[4] = AID_PIV_LEN;
  memcpy(&cmd[5], AID_PIV, AID_PIV_LEN);

  uint8_t resp[256];
  size_t  respLen = sizeof(resp);
  int sw = apdu_exchange(cmd, 5 + AID_PIV_LEN, resp, &respLen);
  s_piv_last_select_sw = sw;
  return (sw == 0x9000);
}

int sc_piv_last_select_sw() {
  return s_piv_last_select_sw;
}

int sc_get_protocol() {
  if (!sc.isActive()) return -1;
  return (int)sc.protocol();
}

// Parse a BER-TLV length field at buf[*pos]. Advances *pos past the
// length bytes. Returns true on success and writes the decoded length
// to *outLen, false on malformed input.
static bool ber_parse_length(const uint8_t* buf, size_t bufLen,
                             size_t* pos, size_t* outLen) {
  if (*pos >= bufLen) return false;
  uint8_t b = buf[*pos];
  if (b < 0x80) {
    *outLen = b;
    *pos   += 1;
    return true;
  }
  uint8_t nbytes = b & 0x7F;
  if (nbytes == 0 || nbytes > 4) return false;  // we don't need >32-bit
  if (*pos + 1 + nbytes > bufLen) return false;
  size_t v = 0;
  for (uint8_t i = 0; i < nbytes; i++) v = (v << 8) | buf[*pos + 1 + i];
  *outLen = v;
  *pos   += 1 + nbytes;
  return true;
}

int sc_piv_get_data(uint8_t t1, uint8_t t2, uint8_t t3,
                    uint8_t* buf, size_t maxLen) {
  if (!buf || maxLen == 0) return 0;

  // PIV GET DATA: CLA=00 INS=CB P1=3F P2=FF Lc=05 [5C 03 t1 t2 t3] Le=00
  uint8_t cmd[11] = {0x00, 0xCB, 0x3F, 0xFF, 0x05,
                     0x5C, 0x03, t1, t2, t3, 0x00};

  // Some objects (certs) are large. Use a generous local buffer; we
  // copy only the inner payload to caller's buf.
  static uint8_t s_resp[2048];
  size_t respLen = sizeof(s_resp);
  int sw = apdu_exchange(cmd, sizeof(cmd), s_resp, &respLen);
  if (sw != 0x9000 || respLen < 2) {
    Serial.printf("[SC-PIV] GET DATA %02X%02X%02X SW=%04X\n", t1, t2, t3, sw);
    return 0;
  }

  // Outer wrapper is 0x53 LL ...
  if (s_resp[0] != 0x53) {
    Serial.printf("[SC-PIV] GET DATA outer tag 0x%02X (expected 0x53)\n", s_resp[0]);
    return 0;
  }
  size_t pos = 1;
  size_t innerLen = 0;
  if (!ber_parse_length(s_resp, respLen, &pos, &innerLen)) return 0;
  if (pos + innerLen > respLen) return 0;
  if (innerLen > maxLen) return 0;

  memcpy(buf, &s_resp[pos], innerLen);
  return (int)innerLen;
}

bool sc_piv_get_chuid_info(PivChuidInfo* out) {
  if (!out) return false;
  memset(out, 0, sizeof(*out));

  if (!sc_piv_select()) return false;

  uint8_t buf[512];
  int n = sc_piv_get_data(0x5F, 0xC1, 0x02, buf, sizeof(buf));
  if (n <= 0) return false;

  // Walk inner BER-TLV elements. Per NIST SP 800-73-4 the meaningful
  // tags are 0x30 (FASC-N), 0x34 (GUID), 0x35 (Expiration Date).
  size_t pos = 0;
  while (pos + 2 <= (size_t)n) {
    uint8_t tag = buf[pos++];
    size_t  len = 0;
    if (!ber_parse_length(buf, n, &pos, &len)) break;
    if (pos + len > (size_t)n) break;

    switch (tag) {
      case 0x30:
        if (len <= sizeof(out->fascn)) {
          memcpy(out->fascn, &buf[pos], len);
          out->fascn_len     = (uint8_t)len;
          out->fascn_present = true;
        }
        break;
      case 0x34:
        if (len == 16) {
          memcpy(out->guid, &buf[pos], 16);
          out->guid_present = true;
        }
        break;
      case 0x35:
        if (len == 8) {
          memcpy(out->expiry, &buf[pos], 8);
          out->expiry[8]      = '\0';
          out->expiry_present = true;
        }
        break;
      default:
        break;
    }
    pos += len;
  }

  return out->fascn_present || out->guid_present;
}