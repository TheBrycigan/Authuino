// nvs_settings.h — persistent user preferences.
//
// Uses Arduino Preferences under the hood (same pattern as
// nvs_meta.cpp and nvs_aid.cpp). One NVS namespace "settings".
//
// All getters return the stored value if present, else a sensible
// default. All setters persist immediately.
//
// Value ranges are clamped on setter; callers can trust getter
// values to always be within [min..max] documented below.

#pragma once

#include <Arduino.h>

void nvs_settings_init();

// Dim duration in seconds — how many seconds of dimmed screen are
// shown before the device enters deep sleep. Dim state begins at
// (sleep_s - dim_s) seconds of idle. Constraint: dim_s <= sleep_s - 5
// (the setter enforces a 5-second minimum separation so the dim
// stage is always a real period). Range: 5..(sleep_s - 5).
// Default: 15.
uint16_t nvs_settings_get_dim_s();
void     nvs_settings_set_dim_s(uint16_t s);

// Total idle seconds before deep sleep. Range: 15..120. Default: 30.
// Minimum is 15 so that dim duration always has a real range
// (sleep - 5 >= 10). Setting this such that sleep - 5 is smaller
// than the current dim_s will pull dim_s down to maintain the
// 5-second minimum separation.
uint16_t nvs_settings_get_sleep_s();
void     nvs_settings_set_sleep_s(uint16_t s);

// Active (full-brightness) backlight level as a percentage. The
// "dim" state is a fixed ratio (20%) of this value — e.g. if active
// brightness is 80%, dim state will be 16%. Range: 10..100.
// Default: 80.
uint8_t  nvs_settings_get_brightness_pct();
void     nvs_settings_set_brightness_pct(uint8_t pct);

// ---- Audio settings ----
// Master audio enable. When false, audio_beep() is a no-op (handled
// by the call sites). Default: true.
bool     nvs_settings_get_audio_enabled();
void     nvs_settings_set_audio_enabled(bool en);

// DAC volume 0..100. Default: 30 (quiet).
uint8_t  nvs_settings_get_audio_volume();
void     nvs_settings_set_audio_volume(uint8_t pct);

// Per-event toggle bits packed into one byte:
//   bit 0: card insert/remove beeps
//   bit 1: PIN success/fail beeps
//   bit 2: TOTP last-5-second ticks
//   bit 3: keyboard tap clicks
// Default: all on (0x0F) — but only audible when master enable is on.
uint8_t  nvs_settings_get_audio_events();
void     nvs_settings_set_audio_events(uint8_t mask);

#define AUDIO_EVT_CARD     0x01
#define AUDIO_EVT_PIN      0x02
#define AUDIO_EVT_TICK     0x04
#define AUDIO_EVT_KEYTAP   0x08