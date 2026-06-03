#pragma once

/*
 * sc_interface.h
 * Smartcard interface for Authuino, on top of the SmartCard driver
 * provided by the ESP-ISO7816 library (ESP_ISO7816.h).
 *
 * Public API is unchanged from the SCLib version:
 *   sc_init / sc_poll / sc_run_session / sc_recalculate / sc_deactivate
 *   sc_state    — current cached state (UI-friendly)
 *
 * NCN8024 pin mapping:
 *   IO      -> GPIO9   (half-duplex I/O)
 *   RSTIN   -> GPIO10
 *   CMDVCCn -> GPIO11  (active LOW)
 *   OFFn    -> GPIO43  (HIGH = card inserted)
 *   CLK     -> GPIO44  (LEDC PWM clock to NCN8024 CLKIN)
 */

#include <Arduino.h>
#include <ESP_ISO7816.h>          // ESP-ISO7816 library (libraries/ESP-ISO7816)

// ---------------------------------------------------------------------------
// Pin definitions
// ---------------------------------------------------------------------------
#define SC_IO       9
#define SC_RSTIN   10
#define SC_CMDVCC  11
#define SC_OFFn    43
#define SC_CLK     44

// ---------------------------------------------------------------------------
// OATH / APDU constants
// ---------------------------------------------------------------------------
static const uint8_t APEX_TOTP_AID[] = {
  0xA0, 0x00, 0x00, 0x07, 0x47, 0x00, 0x61, 0xFC, 0x54, 0xD5, 0x01
};
static const uint8_t APEX_AID_LEN = 11;

// Well-known applet AIDs for card probing. Each probe_*_AID is tested
// on card insert to discover which applets are available. The Main
// Menu dynamically shows tiles for discovered applets.
static const uint8_t AID_FIDO2[] = {
  0xA0, 0x00, 0x00, 0x06, 0x47, 0x2F, 0x00, 0x01
};
static const uint8_t AID_FIDO2_LEN = 8;

// Per NIST SP 800-73-4, the full PIV AID is the 5-byte NIST RID
// followed by the 6-byte PIV PIX. The 5-byte RID alone is enough for
// some lenient cards (Yubico) but federal/CAC cards require the full
// 11-byte AID.
static const uint8_t AID_PIV[] = {
  0xA0, 0x00, 0x00, 0x03, 0x08,           // NIST RID
  0x00, 0x00, 0x10, 0x00, 0x01, 0x00      // PIV PIX
};
static const uint8_t AID_PIV_LEN = 11;

// ---------------------------------------------------------------------------
// AID probe result. Populated by sc_probe_card() on card insert,
// consumed by the UI to build Main Menu tiles.
// ---------------------------------------------------------------------------
struct CardProbeResult {
  bool oath;       // OATH TOTP applet reachable
  bool fido2;      // FIDO2 applet reachable
  bool piv;        // PIV applet reachable
  // Future applets add bool fields here.
};

#define OATH_INS_PUT          0x01
#define OATH_INS_DELETE       0x02
#define OATH_INS_LIST         0xA1
#define OATH_INS_CALCULATE    0xA2
#define OATH_INS_CALC_ALL     0xA4
#define OATH_INS_SEND_REMAIN  0xA5

#define OATH_TAG_NAME         0x71   // CALCULATE ALL response, just the name
#define OATH_TAG_NAME_LIST    0x72   // LIST response, [algo][name]
#define OATH_TAG_RESP_TRUNC   0x76
#define OATH_TAG_NO_RESP      0x77
#define OATH_TAG_CHALLENGE    0x74
#define OATH_TAG_VERSION      0x79

#define MAX_CREDENTIALS       32
#define MAX_CRED_NAME_LEN     64
#define MAX_TOTP_DIGITS        9   // 6 or 8 digits + null

// ---------------------------------------------------------------------------
// Credential record
// ---------------------------------------------------------------------------
struct Credential {
  char name[MAX_CRED_NAME_LEN];
  char code[MAX_TOTP_DIGITS];
  bool valid;
};

// ---------------------------------------------------------------------------
// SC state (exposed for UI / dev screen)
// ---------------------------------------------------------------------------
struct SCState {
  bool       cardPresent;
  bool       appletSelected;

  // Authentication state. needsAuth is true when SELECT returned a
  // challenge (tag 0x74), meaning the card has a password set and we
  // must call sc_authenticate() before LIST / CALCULATE work.
  bool       needsAuth;
  bool       authenticated;
  int8_t     attemptsRemaining;   // -1 unknown, 0 locked, otherwise # left

  int        numCredentials;
  Credential credentials[MAX_CREDENTIALS];
  char       atrStr[80];     // human-readable ATR hex (room for full 32-byte ATR)
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void sc_init();
bool sc_poll();

// ---------------------------------------------------------------------------
// USB CCID bridge helpers (Phase 6).
//
// These let the USB CCID class driver in usb_ccid.cpp expose the
// inserted card to a host computer.
// ---------------------------------------------------------------------------

// Copy the ATR captured at last powerOn into outBuf. Returns the
// number of bytes written (0 if no ATR available, e.g. no card).
// outBufSize must be at least SC_MAX_ATR_BYTES.
int  sc_get_atr(uint8_t* outBuf, size_t outBufSize);

// Power the card off, power it back on, and report the freshly
// captured ATR. Used when the host issues PC_to_RDR_IccPowerOn so it
// gets a current ATR even if our session state was stale.
// Returns ATR length on success, 0 on failure.
int  sc_recapture_atr(uint8_t* outBuf, size_t outBufSize);

// Forward-only APDU exchange for the CCID XfrBlock command.
// Builds a CCID DataBlock-style response: writes the application
// data into outBuf followed by SW1 SW2.
// Returns total bytes written into outBuf (data + 2 SW bytes), or
// negative on error (one of SmartCard::ERR_*).
int  sc_apdu_passthrough(const uint8_t* cmd, size_t cmdLen,
                         uint8_t* outBuf, size_t outBufSize);

// Lightweight AID probe — tries SELECT on each known AID to discover
// what applets exist on the card. Does NOT start a full session, does
// NOT require a PIN. Call after card insertion is detected.
// Returns false if the card couldn't be reached (no ATR, etc.).
CardProbeResult sc_probe_card();

bool sc_run_session(time_t epochNow);

// If sc_state().needsAuth is true after sc_run_session(), call this with
// the user-entered PIN. On success returns true and the credential list
// + TOTP codes are populated. On failure check sc_state().attemptsRemaining
// (>0 = wrong PIN with that many tries left, 0 = card locked, -1 = unknown
// failure). The PIN buffer is wiped before the call returns.
bool sc_authenticate(const char* pin, time_t epochNow);

bool sc_recalculate(time_t epochNow);

// Add a credential. typeAlgo is a single byte: high nibble = OATH type
// (TOTP=0x20, HOTP=0x10), low nibble = hash algo (SHA1=0x01, SHA256=0x02,
// SHA512=0x03). For a standard Google-Authenticator-style TOTP credential
// pass typeAlgo=0x21 (TOTP+SHA1) and digits=6.
//   imf is the initial counter (HOTP only); pass 0 for TOTP.
// Card must be authenticated first if it has a password set.
bool sc_put_credential(const char* name,
                       const uint8_t* secret, size_t secretLen,
                       uint8_t typeAlgo, uint8_t digits,
                       uint32_t imf);

// Remove a credential by name. Card must be authenticated.
bool sc_delete_credential(const char* name);

// Change the card's PIN (OATH SET CODE command). Card must already be
// authenticated with the current PIN. Pass nullptr or "" to remove
// the PIN entirely (card will no longer require authentication).
bool sc_set_pin(const char* newPin);

// RFC 4648 base32 decode. Accepts uppercase or lowercase, strips '=', ' ', '-'.
// Returns number of bytes written, or -1 on invalid input.
int  sc_base32_decode(const char* src, uint8_t* dst, size_t dstMax);

void sc_deactivate();
const SCState& sc_state();

// True when the smartcard chip has been powered on (Vcc, CLK, RST
// raised) and is ready to receive APDUs. False if the chip is in
// the powered-off state (no card present, or post-IccPowerOff, or
// post-reset before IccPowerOn). The CCID status byte uses this to
// distinguish "card present and active" (0x00) from "card present
// but inactive" (0x01) — the latter prompts the host to issue
// IccPowerOn before sending APDUs.
bool sc_is_icc_active();

// ---------------------------------------------------------------------------
// PIV (NIST SP 800-73-4) operations.
//
// These are independent of the OATH session — calling sc_piv_select()
// re-targets the card to the PIV applet. Subsequent OATH operations
// would need to re-SELECT the OATH AID.
// ---------------------------------------------------------------------------

// CHUID (Cardholder Unique Identifier) decoded fields.
//
// On a real federal PIV card, FASC-N is the agency-issued credential
// number. On consumer PIV cards (Yubico, etc.) FASC-N is typically a
// placeholder of all 9s, and the GUID is the meaningful identifier.
struct PivChuidInfo {
  bool    fascn_present;
  uint8_t fascn[25];        // 25 bytes / 200 bits (per SP 800-73)
  uint8_t fascn_len;

  bool    guid_present;
  uint8_t guid[16];         // RFC 4122 UUID

  bool    expiry_present;
  char    expiry[9];        // YYYYMMDD + null terminator
};

// SELECT the PIV applet. Returns true on SW=9000.
bool sc_piv_select();

// Returns the SW from the most recent sc_piv_select() call.
// 0x9000 on success, ISO 7816 error SW on failure (e.g., 0x6A82
// = file not found, 0x6A86 = wrong P1/P2). 0 if SELECT never ran.
int sc_piv_last_select_sw();

// Diagnostic: returns the negotiated protocol from the most recent
// powerOn(). 0 = T=0 character protocol, 1 = T=1 block protocol.
// Federal PIV cards almost always use T=1; consumer cards (Yubico)
// typically use T=0. -1 if no card has been powered yet.
int sc_get_protocol();

// GET DATA wrapper. The 3-byte tag selects which data object to read
// (e.g., {0x5F, 0xC1, 0x02} = CHUID). Returns the length of the
// decoded payload (everything inside the outer 0x53 wrapper), or 0
// on failure. Caller must size buf for the largest object expected.
int sc_piv_get_data(uint8_t t1, uint8_t t2, uint8_t t3,
                    uint8_t* buf, size_t maxLen);

// Convenience: SELECT PIV (if not already selected), GET DATA CHUID,
// parse into the struct. Returns true if at least FASC-N or GUID
// could be extracted.
bool sc_piv_get_chuid_info(PivChuidInfo* out);