#pragma once

/*
 * audio.h — ES8311 codec init + beep generation.
 *
 * The Waveshare 3.5" board has an ES8311 mono audio codec on the
 * shared I2C bus (address 0x18). The codec is fed a 16 kHz I2S
 * stream from I2S0 of the ESP32-S3.
 *
 * Pin map (per Waveshare schematic):
 *   GPIO 12 = MCLK
 *   GPIO 13 = BCLK (bit clock)
 *   GPIO 15 = LRCK (word select)
 *   GPIO 16 = DOUT (data to codec, our TX)
 *   GPIO 14 = DIN  (data from codec, our RX — unused for now)
 *
 * API (synchronous; safe to call from main loop, blocks for the
 * tone duration):
 *     audio_init()                — codec + I2S setup, call once in setup()
 *     audio_beep(freqHz, durMs)   — square-wave beep
 *     audio_set_volume(pct)       — 0..100
 *     audio_is_ready()            — returns true if init succeeded
 */

#include <Arduino.h>
#include "driver/i2c_master.h"

// Initialise the ES8311 over the shared I2C bus and bring up I2S0.
// Returns true on success. Safe to call multiple times — re-init is
// idempotent.
bool audio_init(i2c_master_bus_handle_t i2c_bus);

// Set DAC volume. 0 = mute, 100 = max. Persists until next call.
void audio_set_volume(uint8_t pct);

// Generate a square-wave beep at freqHz for durMs milliseconds.
// Blocks until done (typical durations are 30-200ms — short enough
// not to cause UI lag). Frequency range: ~50 Hz to 8 kHz.
void audio_beep(uint16_t freqHz, uint16_t durMs);

// Generate a percussive click sound — broadband noise with a quick
// decay envelope. Sounds like a real button click rather than a
// short tone (which reads as a "tink" or "beep"). durMs: 5-30,
// 8-12 is typical for UI clicks.
void audio_click(uint16_t durMs);

// True if audio_init() succeeded. Use to gate beep calls so failed
// codec init doesn't crash subsequent beeps.
bool audio_is_ready();