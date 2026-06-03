// nvs_settings.cpp — see nvs_settings.h
//
// Semantics (updated v0.4):
//   sleep_s = total idle seconds before deep sleep
//   dim_s   = dim duration (seconds). Dim fires at (sleep_s - dim_s)
//             idle. Constraint: dim_s <= sleep_s.
//   bri_pct = active backlight brightness 10..100

#include "nvs_settings.h"
#include <Preferences.h>

static const char* NS = "settings";

static const uint16_t DEF_DIM_S    = 15;
static const uint16_t DEF_SLEEP_S  = 30;
static const uint8_t  DEF_BRI_PCT  = 80;

static const uint16_t MIN_DIM_S    = 5,   MAX_DIM_S   = 115;  // sleep_max - 5
static const uint16_t MIN_SLEEP_S  = 15,  MAX_SLEEP_S = 120;  // min allows dim range [5,10]
static const uint8_t  MIN_BRI_PCT  = 10,  MAX_BRI_PCT = 100;

static Preferences _p;

static inline uint16_t clamp_u16(uint16_t v, uint16_t lo, uint16_t hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}
static inline uint8_t clamp_u8(uint8_t v, uint8_t lo, uint8_t hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

void nvs_settings_init() {
  if (_p.begin(NS, false)) {
    Serial.printf("[SETTINGS] open: sleep=%us dim_dur=%us bri=%u%%\n",
                  nvs_settings_get_sleep_s(),
                  nvs_settings_get_dim_s(),
                  nvs_settings_get_brightness_pct());
    _p.end();
  } else {
    Serial.println("[SETTINGS] NVS open FAILED — using defaults");
  }
}

uint16_t nvs_settings_get_dim_s() {
  if (!_p.begin(NS, true)) return DEF_DIM_S;
  uint16_t v = _p.getUShort("dim_s", DEF_DIM_S);
  _p.end();
  uint16_t s = DEF_SLEEP_S;
  if (_p.begin(NS, true)) {
    s = _p.getUShort("sleep_s", DEF_SLEEP_S);
    _p.end();
  }
  // dim duration must be >= MIN_DIM_S and <= sleep_s - 5 (minimum
  // 5-second separation so the dim stage is always a real period).
  uint16_t hi = (s > MIN_DIM_S + 5) ? (uint16_t)(s - 5) : MIN_DIM_S;
  return clamp_u16(v, MIN_DIM_S, hi);
}
void nvs_settings_set_dim_s(uint16_t s) {
  uint16_t sleep_s = DEF_SLEEP_S;
  if (_p.begin(NS, true)) {
    sleep_s = _p.getUShort("sleep_s", DEF_SLEEP_S);
    _p.end();
  }
  uint16_t hi = (sleep_s > MIN_DIM_S + 5) ? (uint16_t)(sleep_s - 5) : MIN_DIM_S;
  s = clamp_u16(s, MIN_DIM_S, hi);
  if (!_p.begin(NS, false)) return;
  _p.putUShort("dim_s", s);
  _p.end();
}

uint16_t nvs_settings_get_sleep_s() {
  if (!_p.begin(NS, true)) return DEF_SLEEP_S;
  uint16_t v = _p.getUShort("sleep_s", DEF_SLEEP_S);
  _p.end();
  return clamp_u16(v, MIN_SLEEP_S, MAX_SLEEP_S);
}
void nvs_settings_set_sleep_s(uint16_t s) {
  s = clamp_u16(s, MIN_SLEEP_S, MAX_SLEEP_S);
  if (!_p.begin(NS, false)) return;
  _p.putUShort("sleep_s", s);
  // If dim duration now exceeds (sleep - 5), pull it down so the
  // 5-second minimum separation is preserved.
  uint16_t d      = _p.getUShort("dim_s", DEF_DIM_S);
  uint16_t dim_hi = (s > MIN_DIM_S + 5) ? (uint16_t)(s - 5) : MIN_DIM_S;
  if (d > dim_hi) {
    _p.putUShort("dim_s", dim_hi);
  }
  _p.end();
}

uint8_t nvs_settings_get_brightness_pct() {
  if (!_p.begin(NS, true)) return DEF_BRI_PCT;
  uint8_t v = _p.getUChar("bri_pct", DEF_BRI_PCT);
  _p.end();
  return clamp_u8(v, MIN_BRI_PCT, MAX_BRI_PCT);
}
void nvs_settings_set_brightness_pct(uint8_t pct) {
  pct = clamp_u8(pct, MIN_BRI_PCT, MAX_BRI_PCT);
  if (!_p.begin(NS, false)) return;
  _p.putUChar("bri_pct", pct);
  _p.end();
}

// ---- Audio settings ----
static const uint8_t DEF_AUDIO_VOL    = 30;     // quiet by default
static const uint8_t DEF_AUDIO_EVENTS = 0x0F;   // all events on (gated by master)

bool nvs_settings_get_audio_enabled() {
  if (!_p.begin(NS, true)) return true;
  bool v = _p.getBool("audio_en", true);
  _p.end();
  return v;
}
void nvs_settings_set_audio_enabled(bool en) {
  if (!_p.begin(NS, false)) return;
  _p.putBool("audio_en", en);
  _p.end();
}

uint8_t nvs_settings_get_audio_volume() {
  if (!_p.begin(NS, true)) return DEF_AUDIO_VOL;
  uint8_t v = _p.getUChar("audio_vol", DEF_AUDIO_VOL);
  _p.end();
  return clamp_u8(v, 0, 100);
}
void nvs_settings_set_audio_volume(uint8_t pct) {
  pct = clamp_u8(pct, 0, 100);
  if (!_p.begin(NS, false)) return;
  _p.putUChar("audio_vol", pct);
  _p.end();
}

uint8_t nvs_settings_get_audio_events() {
  if (!_p.begin(NS, true)) return DEF_AUDIO_EVENTS;
  uint8_t v = _p.getUChar("audio_evt", DEF_AUDIO_EVENTS);
  _p.end();
  return v & 0x0F;
}
void nvs_settings_set_audio_events(uint8_t mask) {
  if (!_p.begin(NS, false)) return;
  _p.putUChar("audio_evt", mask & 0x0F);
  _p.end();
}