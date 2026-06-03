// audio.cpp — see audio.h
//
// Minimal ES8311 driver for beep generation. Configures the codec
// over I2C with a known-working register sequence (slave mode, 16 kHz,
// 16-bit) and uses ESP-IDF v5 I2S standard driver to clock samples in.
//
// Tone generation: alternating ±amplitude int16 samples at half-period
// intervals = a square wave. Square waves are harsh but trivial to
// generate, and for short UI feedback beeps the harshness is fine.
// We can swap in a sine LUT later if desired.

#include "audio.h"
#include "nvs_settings.h"
#include "driver/i2s_std.h"
#include <math.h>

// I2S pin map (Waveshare 3.5" schematic)
static const int I2S_MCLK = 12;
static const int I2S_BCLK = 13;
static const int I2S_LRCK = 15;
static const int I2S_DOUT = 16;
static const int I2S_DIN  = 14;

// ES8311 I2C address
static const uint8_t ES8311_ADDR = 0x18;

// Audio config
static const uint32_t SAMPLE_RATE = 16000;

static i2s_chan_handle_t  s_tx_chan = nullptr;
static i2c_master_dev_handle_t s_codec_dev = nullptr;
static bool s_ready = false;

// ---------------------------------------------------------------------------
// ES8311 register I/O
// ---------------------------------------------------------------------------
static bool es_write(uint8_t reg, uint8_t val) {
  if (!s_codec_dev) return false;
  uint8_t buf[2] = { reg, val };
  return i2c_master_transmit(s_codec_dev, buf, 2, 100) == ESP_OK;
}

// Canonical ES8311 init sequence (slave mode, MCLK from ESP, 16 kHz mono,
// internal MCLK divider for ADC/DAC). Source: WinnerMicro forum,
// cross-checked with ES8311 datasheet register descriptions.
//
// NOTE: register 0x32 (DAC volume) is left at 0xBF (0 dB) here — that's
// just volume, not mute. Real silence is provided after init by
// writing reg 0x31 mute bits (DSM + DEM mute). audio_beep() temporarily
// clears those bits while streaming a tone.
static const uint8_t ES8311_INIT[][2] = {
  {0x00, 0x1F}, {0x45, 0x00}, {0x01, 0x30}, {0x02, 0x90}, {0x03, 0x19},
  {0x16, 0x03}, {0x04, 0x19}, {0x05, 0x00}, {0x06, 0x0F}, {0x07, 0x01},
  {0x08, 0xFF}, {0x0B, 0x00}, {0x0C, 0x00}, {0x10, 0x1F}, {0x11, 0x7F},
  {0x00, 0xC0}, {0x0D, 0x01}, {0x01, 0x3F}, {0x14, 0x1A}, {0x12, 0x00},
  {0x13, 0x10}, {0x09, 0x0C}, {0x0A, 0x0C}, {0x0E, 0x02}, {0x0F, 0x44},
  {0x15, 0x00}, {0x1B, 0x05}, {0x1C, 0x65}, {0x17, 0xFF}, // ADC vol max
  {0x37, 0x08},
  {0x32, 0xBF}, // DAC volume 0 dB (mute is via reg 0x31, set after init)
  {0x44, 0x00}, // loopback off
};

// ---------------------------------------------------------------------------
// I2S init
// ---------------------------------------------------------------------------
// I2S init — channel created but NOT enabled yet. We enable it
// just before the first beep so the codec doesn't hear DMA garbage.
static bool i2s_setup() {
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  // auto_clear: DMA fills consumed descriptors with zeros automatically.
  // Without this, the DMA ring loops the last written samples forever
  // once we stop writing — sounds like a buzzer with our short tones.
  // With this, silence between beeps is "free" (the driver clears the
  // ring as DMA reads it), so audio_click/beep don't need to write
  // their own long silence trails. That keeps each call short and
  // responsive, so fast user taps aren't dropped while we block in
  // i2s_channel_write.
  chan_cfg.auto_clear = true;
  esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx_chan, nullptr);
  if (err != ESP_OK) {
    Serial.printf("[AUDIO] i2s_new_channel failed: 0x%x\n", err);
    return false;
  }

  i2s_std_config_t std_cfg = {};
  std_cfg.clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE);
  std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG((i2s_data_bit_width_t)16, I2S_SLOT_MODE_STEREO);

  std_cfg.gpio_cfg.mclk = (gpio_num_t)I2S_MCLK;
  std_cfg.gpio_cfg.bclk = (gpio_num_t)I2S_BCLK;
  std_cfg.gpio_cfg.ws   = (gpio_num_t)I2S_LRCK;
  std_cfg.gpio_cfg.dout = (gpio_num_t)I2S_DOUT;
  std_cfg.gpio_cfg.din  = (gpio_num_t)I2S_DIN;

  err = i2s_channel_init_std_mode(s_tx_chan, &std_cfg);
  if (err != ESP_OK) {
    Serial.printf("[AUDIO] i2s_channel_init_std_mode failed: 0x%x\n", err);
    return false;
  }
  // NB: i2s_channel_enable() deferred to first audio_beep() call.
  return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool audio_init(i2c_master_bus_handle_t i2c_bus) {
  if (s_ready) return true;
  if (!i2c_bus) {
    Serial.println("[AUDIO] init: null i2c bus");
    return false;
  }

  // Register ES8311 on the shared I2C bus.
  i2c_device_config_t dev_cfg = {};
  dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  dev_cfg.device_address  = ES8311_ADDR;
  dev_cfg.scl_speed_hz    = 400000;
  esp_err_t err = i2c_master_bus_add_device(i2c_bus, &dev_cfg, &s_codec_dev);
  if (err != ESP_OK) {
    Serial.printf("[AUDIO] codec i2c add_device failed: 0x%x\n", err);
    return false;
  }

  // Bring up I2S first — codec needs MCLK present before its registers
  // settle properly.
  if (!i2s_setup()) return false;
  delay(20);

  // Apply the canonical init sequence.
  size_t n = sizeof(ES8311_INIT) / sizeof(ES8311_INIT[0]);
  for (size_t i = 0; i < n; i++) {
    if (!es_write(ES8311_INIT[i][0], ES8311_INIT[i][1])) {
      Serial.printf("[AUDIO] codec write reg 0x%02X failed\n", ES8311_INIT[i][0]);
      return false;
    }
  }

  // Hard mute the DAC via reg 0x31 (DSM + DEM mute bits). Reg 0x32
  // is volume only — 0x00 there is -95.5 dB, NOT silence; DMA
  // underruns can still produce audible output. Reg 0x31 mute is
  // unconditional and survives any I2S misbehaviour.
  es_write(0x31, 0x60);
  s_ready = true;
  Serial.println("[AUDIO] ES8311 initialised (hard-muted)");
  return true;
}

// Track current state to avoid redundant register writes.
static bool s_dac_unmuted = false;
static uint8_t s_current_vol_reg = 0x00;
static bool s_i2s_enabled = false;

// Push the user's stored volume to register 0x32. Mute state is
// independent (handled by reg 0x31).
static void apply_dac_volume() {
  uint8_t pct = nvs_settings_get_audio_volume();
  uint8_t v;
  if (pct == 0) {
    v = 0x00;
  } else {
    // Linear from 0x80 (-32 dB) at 1% to 0xFF (+32 dB) at 100%.
    v = (uint8_t)(0x80 + ((uint16_t)(pct - 1) * 127) / 99);
  }
  if (v != s_current_vol_reg) {
    es_write(0x32, v);
    s_current_vol_reg = v;
  }
}

void audio_set_volume(uint8_t pct) {
  if (!s_ready) return;
  apply_dac_volume();
}

// Ensure I2S running and DAC unmuted. Idempotent after first call.
// Returns false if activation failed (e.g. I2S channel error).
static bool ensure_audio_active() {
  if (!s_i2s_enabled) {
    esp_err_t err = i2s_channel_enable(s_tx_chan);
    if (err != ESP_OK) {
      Serial.printf("[AUDIO] i2s_channel_enable failed: 0x%x\n", err);
      return false;
    }
    s_i2s_enabled = true;
  }
  if (!s_dac_unmuted) {
    es_write(0x31, 0x00);   // clear DSM + DEM mute bits
    s_dac_unmuted = true;
    delay(15);              // codec de-mute ramp
  }
  apply_dac_volume();
  return true;
}

// Write a small silence buffer so the DAC's last sample isn't held
// indefinitely (the I2S `auto_clear` config fills DMA ring zeros as
// they're consumed, but writing one block of silence here ensures
// the very next DMA frame after our tone is also zero). Tiny and
// non-blocking enough not to hurt UI responsiveness.
static void drain_dma_silence() {
  static int16_t silence[256] = {0};   // 64 stereo frames = 4ms
  size_t written = 0;
  i2s_channel_write(s_tx_chan, silence, sizeof(silence), &written, 50);
}

void audio_beep(uint16_t freqHz, uint16_t durMs) {
  if (!s_ready || freqHz < 20 || durMs == 0) return;
  if (!nvs_settings_get_audio_enabled()) return;
  if (nvs_settings_get_audio_volume() == 0) return;

  if (durMs > 1000) durMs = 1000;
  if (durMs < 12)   durMs = 12;

  if (!ensure_audio_active()) return;

  // Square wave: alternate +A and -A samples every (samples_per_period/2).
  const int16_t AMP = 16000;    // ~50% of int16 full scale
  uint32_t samples_per_period = SAMPLE_RATE / freqHz;
  if (samples_per_period < 2)   samples_per_period = 2;
  if (samples_per_period > 128) samples_per_period = 128;
  uint32_t half = samples_per_period / 2;

  static int16_t buf[256];   // up to 128 stereo frames
  for (uint32_t i = 0; i < samples_per_period; i++) {
    int16_t s = (i < half) ? AMP : -AMP;
    buf[i * 2 + 0] = s;
    buf[i * 2 + 1] = s;
  }

  uint32_t total_frames = ((uint32_t)SAMPLE_RATE * durMs) / 1000;
  size_t   written      = 0;
  uint32_t frames_done  = 0;
  while (frames_done < total_frames) {
    uint32_t want_frames = total_frames - frames_done;
    if (want_frames > samples_per_period) want_frames = samples_per_period;
    size_t want_bytes = want_frames * 4;
    if (i2s_channel_write(s_tx_chan, buf, want_bytes, &written, 100) != ESP_OK) break;
    frames_done += written / 4;
    if (written == 0) break;
  }

  drain_dma_silence();
}

// Broadband noise burst with linear decay envelope. Sounds like a
// real mechanical click (percussive, not tonal). Used for tap/click
// UI events where a tone would feel like a "tink" or "beep".
void audio_click(uint16_t durMs) {
  if (!s_ready) return;
  if (!nvs_settings_get_audio_enabled()) return;
  if (nvs_settings_get_audio_volume() == 0) return;

  if (durMs > 30) durMs = 30;
  if (durMs < 5)  durMs = 5;

  if (!ensure_audio_active()) return;

  uint32_t total_frames = ((uint32_t)SAMPLE_RATE * durMs) / 1000;
  if (total_frames > 480) total_frames = 480;   // cap at 30 ms

  static int16_t buf[480 * 2];   // 480 stereo frames = 30ms at 16kHz
  // xorshift32 PRNG, seeded from micros() so consecutive clicks
  // don't sound identical. Decay envelope makes the burst end
  // smoothly rather than cutting hard (which would itself sound
  // like a click on top of the click).
  uint32_t seed = micros() | 1;
  const int16_t BASE_AMP = 18000;   // peak amplitude — full envelope at i=0
  for (uint32_t i = 0; i < total_frames; i++) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    int16_t noise = (int16_t)(seed & 0xFFFF);   // -32768..+32767
    // Linear decay: full amplitude at i=0, 0 at i=total_frames.
    int16_t amp = (int16_t)((uint32_t)BASE_AMP * (total_frames - i) / total_frames);
    int16_t s = (int16_t)(((int32_t)noise * amp) / 32768);
    buf[i * 2 + 0] = s;
    buf[i * 2 + 1] = s;
  }

  size_t written = 0;
  i2s_channel_write(s_tx_chan, buf, total_frames * 4, &written, 100);

  drain_dma_silence();
}

bool audio_is_ready() {
  return s_ready;
}