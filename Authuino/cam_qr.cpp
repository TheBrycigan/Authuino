// cam_qr.cpp — camera + quirc implementation.
//
// The camera task captures one frame at a time into an esp_camera fb,
// copies it (byteswapping big-endian RGB565 to little-endian, which
// is what LVGL renders correctly here) into one of two view buffers
// that we own, and immediately returns the fb to the driver pool.
//
// A small mutex guards two pointers:
//   s_active_buf — camera task writes here
//   s_ready_buf  — set by camera task when a fresh copy is done;
//                  cleared by the loop when consumed.
//
// This design has two important properties:
//   - Camera fbs are never held across task iterations, so there's no
//     lifetime entanglement with esp_camera's internal allocator.
//   - The loop's LVGL widget points at a stable buffer that the camera
//     task won't overwrite until the loop consumes it — no tearing.
//
// quirc runs in the camera task after each copy, on the buffer we
// just wrote. On decode hit the payload goes in s_payload and
// s_detect is raised; the loop picks that up on its next poll.

#include "cam_qr.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_camera.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"

extern "C" {
#include "quirc.h"
}

// ---------------------------------------------------------------------------
// Pinmap — Waveshare ESP32-S3-Touch-LCD-3.5 OV5640
// ---------------------------------------------------------------------------
#define CAM_PIN_PWDN   -1
#define CAM_PIN_RESET  -1
#define CAM_PIN_XCLK   38
#define CAM_PIN_SIOD   -1   // SCCB reuses our shared i2c_master bus
#define CAM_PIN_SIOC   -1
#define CAM_PIN_VSYNC  17
#define CAM_PIN_HREF   18
#define CAM_PIN_PCLK   41
#define CAM_PIN_D9     21
#define CAM_PIN_D8     39
#define CAM_PIN_D7     40
#define CAM_PIN_D6     42
#define CAM_PIN_D5     46
#define CAM_PIN_D4     48
#define CAM_PIN_D3     47
#define CAM_PIN_D2     45
#define I2C_PORT_NUM    0    // must match sketch's i2cBusInit

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
static struct quirc* s_q = nullptr;

static TaskHandle_t     s_task        = nullptr;
static SemaphoreHandle_t s_mutex      = nullptr;  // protects s_ready_buf, s_detect, s_payload

static volatile bool s_stop_request   = false;
static volatile bool s_task_exited    = false;
static volatile bool s_camera_on      = false;

// View-buffer double buffer. The camera task owns the ACTIVE buffer
// (the one it's writing to); when a frame is done it swaps "ready" /
// "active" under the mutex. The loop reads s_ready_buf (stable while
// LVGL renders) and clears it to signal consumption.
//
// Each buffer is FRAME_W * FRAME_H * 2 bytes of PSRAM. Two total is
// fine (PSRAM is huge on this board). We own these — camera fbs are
// always returned to the pool immediately after we copy out of them,
// so no lifetime entanglement with esp_camera's internal allocator.
#define FRAME_W   320
#define FRAME_H   240
#define FRAME_BYTES (FRAME_W * FRAME_H * 2)

static uint8_t*  s_buf_a         = nullptr;
static uint8_t*  s_buf_b         = nullptr;
static uint8_t*  s_active_buf    = nullptr;  // camera writes here
static uint8_t*  s_ready_buf     = nullptr;  // non-null means "new frame for loop"

// Decode result
static volatile bool s_detect         = false;
static char          s_payload[512]   = {0};

// quirc state (owned entirely by the camera task)
static struct quirc_code s_qr_code;
static struct quirc_data s_qr_data;

// ---------------------------------------------------------------------------
// camera_config — matches manufacturer sketch (RGB565, QVGA, fb_count=2)
// Critical: sccb_i2c_port=0 + pin_sccb_*=-1 tells esp_camera to reuse
// the existing i2c_master bus on port 0 (which our sketch creates in
// i2cBusInit). This is THE key to LVGL+camera coexistence — no second
// SCCB master, no driver-level races with touch/AXP/TCA/RTC.
// ---------------------------------------------------------------------------
static camera_config_t make_config() {
  camera_config_t c = {};
  c.pin_pwdn      = CAM_PIN_PWDN;
  c.pin_reset     = CAM_PIN_RESET;
  c.pin_xclk      = CAM_PIN_XCLK;
  c.pin_sccb_sda  = CAM_PIN_SIOD;     // -1 — reuse shared bus
  c.pin_sccb_scl  = CAM_PIN_SIOC;     // -1
  c.sccb_i2c_port = I2C_PORT_NUM;     // 0 — shared with sketch
  c.pin_d7        = CAM_PIN_D9;
  c.pin_d6        = CAM_PIN_D8;
  c.pin_d5        = CAM_PIN_D7;
  c.pin_d4        = CAM_PIN_D6;
  c.pin_d3        = CAM_PIN_D5;
  c.pin_d2        = CAM_PIN_D4;
  c.pin_d1        = CAM_PIN_D3;
  c.pin_d0        = CAM_PIN_D2;
  c.pin_vsync     = CAM_PIN_VSYNC;
  c.pin_href      = CAM_PIN_HREF;
  c.pin_pclk      = CAM_PIN_PCLK;
  c.xclk_freq_hz  = 20000000;          // matches manufacturer port module
  c.ledc_timer    = LEDC_TIMER_0;
  c.ledc_channel  = LEDC_CHANNEL_0;
  c.pixel_format  = PIXFORMAT_RGB565;
  c.frame_size    = FRAMESIZE_QVGA;    // 320x240
  c.jpeg_quality  = 10;                // unused for RGB565
  c.fb_count      = 2;                 // double buffer so loop can hold
                                       //   one fb while camera fills another
  c.fb_location   = CAMERA_FB_IN_PSRAM;
  c.grab_mode     = CAMERA_GRAB_WHEN_EMPTY;
  return c;
}

// ---------------------------------------------------------------------------
// Public: init (once at boot). Allocates quirc.
// ---------------------------------------------------------------------------
void cam_qr_init() {
  if (s_q) return;
  s_q = quirc_new();
  if (!s_q) { Serial.println("[CAM] quirc_new FAILED"); return; }
  if (quirc_resize(s_q, FRAME_W, FRAME_H) < 0) {
    Serial.println("[CAM] quirc_resize FAILED");
    quirc_destroy(s_q); s_q = nullptr; return;
  }
  s_mutex = xSemaphoreCreateMutex();
  Serial.println("[CAM] quirc decoder ready");
}

// ---------------------------------------------------------------------------
// Convert an RGB565 frame into quirc's grayscale plane.
// Takes a buffer where bytes are already in LITTLE-endian pair order
// (as we'll have stored it after byteswap). Uses green channel only —
// faster than full BT.601 luma and fine for QR contrast.
// ---------------------------------------------------------------------------
static void frame_to_quirc_le(const uint16_t* rgb_le, int w, int h) {
  int qw, qh;
  uint8_t* gray = quirc_begin(s_q, &qw, &qh);
  if (qw != w || qh != h) { quirc_end(s_q); return; }
  const int n = w * h;
  for (int i = 0; i < n; i++) {
    // Little-endian RGB565: green lives in bits 5..10 of px.
    uint16_t px = rgb_le[i];
    gray[i] = (uint8_t)((px >> 3) & 0xFC);
  }
  quirc_end(s_q);
}

// Copy camera fb (big-endian RGB565 as OV5640 emits) into our view
// buffer while byteswapping to little-endian (what LVGL expects on
// this build — the whole rest of the UI renders correctly, so this
// is the target format). 32-bit word copy for speed.
static inline void swap_copy_rgb565(const uint16_t* src, uint16_t* dst, int n) {
  // Do it 2 pixels at a time with a 32-bit rotate-by-8 trick where
  // alignment permits. Fall back to per-pixel for the tail.
  const uint32_t* s32 = (const uint32_t*)src;
  uint32_t*       d32 = (uint32_t*)dst;
  int pairs = n / 2;
  for (int i = 0; i < pairs; i++) {
    uint32_t v = s32[i];
    // v holds two BE RGB565 pixels [p0_hi p0_lo p1_hi p1_lo].
    // We want each 16-bit pair byte-swapped: [p0_lo p0_hi p1_lo p1_hi].
    d32[i] = ((v & 0xFF00FF00U) >> 8) | ((v & 0x00FF00FFU) << 8);
  }
  if (n & 1) {
    uint16_t v = src[n - 1];
    dst[n - 1] = (uint16_t)((v >> 8) | (v << 8));
  }
}

// ---------------------------------------------------------------------------
// Camera task: capture, swap-copy into view buffer, decode, repeat.
// Core 0. Camera fbs are returned to the pool immediately after we
// copy out of them — no lifetime sharing with the loop.
// ---------------------------------------------------------------------------
static void cam_task(void* arg) {
  Serial.println("[CAM task] started");
  while (!s_stop_request) {
    // Don't keep capturing after a successful detect — the loop will
    // tear us down shortly. Just yield until stop request arrives.
    if (s_detect) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }

    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }

    if (fb->format == PIXFORMAT_RGB565 &&
        (int)fb->width == FRAME_W && (int)fb->height == FRAME_H &&
        s_active_buf != nullptr) {
      // Copy + byteswap into the active buffer.
      swap_copy_rgb565((const uint16_t*)fb->buf,
                       (uint16_t*)s_active_buf,
                       FRAME_W * FRAME_H);

      // The buffer we just wrote is still in s_active_buf at this
      // point (we're about to swap). Keep a local reference so decode
      // can read it safely even after the loop polls and clears
      // s_ready_buf.
      uint8_t* just_written = s_active_buf;

      // Publish to the loop. Under mutex: if a prior buffer was
      // queued and never consumed, put it back on our side as the
      // next active; otherwise swap active/ready via the spare.
      xSemaphoreTake(s_mutex, portMAX_DELAY);
      uint8_t* previously_ready = s_ready_buf;
      s_ready_buf  = just_written;
      // If the loop hadn't consumed the previous ready buffer, we
      // reuse it as the next active (it's safe to overwrite — the
      // loop isn't reading it). Otherwise flip to the spare.
      if (previously_ready != nullptr) {
        s_active_buf = previously_ready;
      } else {
        s_active_buf = (just_written == s_buf_a) ? s_buf_b : s_buf_a;
      }
      xSemaphoreGive(s_mutex);

      // Decode on just_written. The loop may be rendering it
      // concurrently but that's fine — decode only reads.
      if (s_q) {
        frame_to_quirc_le((const uint16_t*)just_written, FRAME_W, FRAME_H);
        int n = quirc_count(s_q);
        if (n > 0) {
          quirc_extract(s_q, 0, &s_qr_code);
          if (quirc_decode(&s_qr_code, &s_qr_data) == QUIRC_SUCCESS) {
            size_t L = s_qr_data.payload_len;
            if (L >= sizeof(s_payload)) L = sizeof(s_payload) - 1;
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            memcpy(s_payload, s_qr_data.payload, L);
            s_payload[L] = '\0';
            s_detect = true;
            xSemaphoreGive(s_mutex);
            Serial.printf("[CAM task] decode hit, %u bytes\n", (unsigned)L);
          }
        }
      }
    }

    // Done with the fb — return it to the pool so the driver can
    // refill. We never keep a reference across iterations.
    esp_camera_fb_return(fb);

    vTaskDelay(pdMS_TO_TICKS(1));
  }

  Serial.println("[CAM task] exiting");
  s_task_exited = true;
  vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
// Public: start. Inits camera, allocates view buffers if needed,
// spawns task. Returns false on cam init fail.
// ---------------------------------------------------------------------------
bool cam_qr_start_task() {
  if (!s_q || !s_mutex) { Serial.println("[CAM] not initialised"); return false; }
  if (s_camera_on) return true;

  Serial.println("[CAM] start_task: begin");

  // Lazily allocate the two view buffers in PSRAM. Total ~300 KB.
  if (!s_buf_a) {
    s_buf_a = (uint8_t*)heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  if (!s_buf_b) {
    s_buf_b = (uint8_t*)heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  if (!s_buf_a || !s_buf_b) {
    Serial.println("[CAM] view buffer alloc FAILED");
    return false;
  }
  // Initialise to black so the first viewfinder frame before capture
  // doesn't show garbage.
  memset(s_buf_a, 0, FRAME_BYTES);
  memset(s_buf_b, 0, FRAME_BYTES);

  // Clear state
  s_stop_request = false;
  s_task_exited  = false;
  s_detect       = false;
  s_payload[0]   = '\0';
  s_active_buf   = s_buf_a;   // camera will write here first
  s_ready_buf    = nullptr;   // nothing ready for the loop yet

  // Suppress the benign NACK noise esp_camera can emit on our bus
  // during teardown/init; restored in stop.
  esp_log_level_set("i2c.master", ESP_LOG_NONE);

  camera_config_t cfg = make_config();
  Serial.println("[CAM] start_task: calling esp_camera_init");
  uint32_t t0 = millis();
  esp_err_t err = esp_camera_init(&cfg);
  Serial.printf("[CAM] start_task: esp_camera_init returned 0x%x in %lums\n",
                err, (unsigned long)(millis() - t0));
  if (err != ESP_OK) {
    Serial.printf("[CAM] init failed: 0x%x\n", err);
    esp_log_level_set("i2c.master", ESP_LOG_WARN);
    return false;
  }
  sensor_t* s = esp_camera_sensor_get();
  if (s) {
    // Camera physically rotated 180° from factory orientation.
    // Previous working: vflip=0, hmirror=1. After 180° rotation:
    s->set_vflip(s, 1);
    s->set_hmirror(s, 0);
    Serial.printf("[CAM] sensor PID=0x%04X ready\n", s->id.PID);
  }
  s_camera_on = true;

  Serial.println("[CAM] start_task: spawning cam_task");
  // 24 KB stack: quirc_decode on marginal QRs recurses into Reed-Solomon
  // error correction and can consume several KB. 10 KB was insufficient
  // and produced stack-canary panics mid-scan. 24 KB leaves comfortable
  // headroom and is still cheap in PSRAM-abundant land.
  xTaskCreatePinnedToCore(cam_task, "cam_task", 1024 * 24, nullptr,
                          1 /*prio*/, &s_task, 0 /*Core 0*/);
  Serial.println("[CAM] start_task: done");
  return true;
}

// ---------------------------------------------------------------------------
// Public: stop. Signal task, wait, deinit.
// ---------------------------------------------------------------------------
void cam_qr_stop_task() {
  if (!s_camera_on) return;

  s_stop_request = true;
  // Wait up to 2s for the task to exit.
  uint32_t t0 = millis();
  while (!s_task_exited && (millis() - t0) < 2000) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (!s_task_exited) {
    Serial.println("[CAM] task stop timeout — forcing");
  }

  // View buffers stay allocated — next scan reuses them. They're
  // only freed on full cleanup (not implemented; not needed).

  esp_camera_deinit();
  s_camera_on = false;
  esp_log_level_set("i2c.master", ESP_LOG_WARN);
  Serial.println("[CAM] off");
}

// ---------------------------------------------------------------------------
// Public: poll. Called from loop.
// Returns CAM_POLL_NEW_FRAME and sets out_dsc to point at the
// just-published view buffer; the buffer stays valid until the NEXT
// call to cam_qr_poll (or cam_qr_stop_task). That's the natural
// lifetime for LVGL to render one frame's worth of dirty area.
// ---------------------------------------------------------------------------
int cam_qr_poll(lv_img_dsc_t* out_dsc, char* out_payload, size_t payload_sz) {
  if (!s_camera_on || !s_mutex) return CAM_POLL_NONE;

  uint8_t* ready = nullptr;
  bool     hit   = false;
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  ready        = s_ready_buf;
  s_ready_buf  = nullptr;           // signal consumption
  hit          = s_detect;
  if (hit && out_payload && payload_sz > 0) {
    size_t L = strnlen(s_payload, sizeof(s_payload));
    if (L >= payload_sz) L = payload_sz - 1;
    memcpy(out_payload, s_payload, L);
    out_payload[L] = '\0';
  }
  xSemaphoreGive(s_mutex);

  int rc = CAM_POLL_NONE;
  if (ready && out_dsc) {
    out_dsc->header.always_zero = 0;
    out_dsc->header.w           = FRAME_W;
    out_dsc->header.h           = FRAME_H;
    out_dsc->data_size          = FRAME_BYTES;
    out_dsc->header.cf          = LV_IMG_CF_TRUE_COLOR;
    out_dsc->data               = (const uint8_t*)ready;
    rc = CAM_POLL_NEW_FRAME;
  }
  if (hit) rc = CAM_POLL_DETECTED;
  return rc;
}

// ---------------------------------------------------------------------------
// otpauth URI parser
// Format (relevant parts):
//   otpauth://totp/ISSUER:ACCOUNT?secret=BASE32&issuer=ISSUER
//     &algorithm=SHA1|SHA256|SHA512&digits=6|7|8&period=30
// We honour secret, algorithm, digits, period. Issuer / account form
// the credential name: "PP/Issuer:Account" (PP = period, omitted for 30).
// ---------------------------------------------------------------------------
static int hex_val(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
static void url_decode(const char* s, size_t n, char* out, size_t out_sz) {
  size_t i = 0, j = 0;
  while (i < n && j + 1 < out_sz) {
    if (s[i] == '%' && i + 2 < n) {
      int h = hex_val(s[i+1]), l = hex_val(s[i+2]);
      if (h >= 0 && l >= 0) { out[j++] = (char)((h << 4) | l); i += 3; continue; }
    }
    if (s[i] == '+') { out[j++] = ' '; i++; continue; }
    out[j++] = s[i++];
  }
  out[j] = '\0';
}
// Base32 RFC4648 decode. Handles '=' padding (skipped) and common
// whitespace/dash separators. Returns bytes written, or -1 on invalid.
//
// Implementation note: we accumulate 5 bits per character into v, and
// emit 8-bit bytes from the low end whenever at least 8 bits have
// piled up. After each emission we MUST mask off the already-emitted
// bits from v, otherwise v overflows 32 bits after ~6-7 chars and
// subsequent output bytes corrupt. (First bug discovered when Google
// TOTP QRs produced 6-digit codes that didn't match the expected
// value — the first few secret bytes were correct, later ones wrong.)
static int base32_decode(const char* s, uint8_t* out, size_t out_sz) {
  uint32_t bits = 0, v = 0;
  size_t j = 0;
  for (; *s; s++) {
    char c = *s;
    int d = -1;
    if (c >= 'A' && c <= 'Z')      d = c - 'A';
    else if (c >= 'a' && c <= 'z') d = c - 'a';
    else if (c >= '2' && c <= '7') d = 26 + (c - '2');
    else if (c == '=' || c == ' ' || c == '-') continue;
    else return -1;
    v = (v << 5) | (uint32_t)d;
    bits += 5;
    if (bits >= 8) {
      bits -= 8;
      if (j >= out_sz) return -1;
      out[j++] = (uint8_t)((v >> bits) & 0xFF);
      // Drop the bits we just emitted — otherwise v overflows.
      v &= (uint32_t)((1u << bits) - 1u);
    }
  }
  return (int)j;
}

bool cam_qr_parse_otpauth(const char* uri, OtpAuthCred* out) {
  if (!uri || !out) return false;
  memset(out, 0, sizeof(*out));
  out->digits = 6;
  out->type_algo = 0x21;   // TOTP|SHA1 default
  out->period = 30;

  Serial.printf("[QR] raw URI (%u bytes): %s\n", (unsigned)strlen(uri), uri);

  const char* prefix = "otpauth://totp/";
  const size_t prefLen = strlen(prefix);
  if (strncasecmp(uri, prefix, prefLen) != 0) {
    Serial.println("[QR] URI prefix mismatch");
    return false;
  }

  const char* label = uri + prefLen;
  const char* q = strchr(label, '?');
  if (!q) return false;

  char labelDecoded[96];
  url_decode(label, (size_t)(q - label), labelDecoded, sizeof(labelDecoded));

  const char* params = q + 1;
  bool have_secret = false;
  while (*params) {
    const char* eq = strchr(params, '=');
    const char* amp = strchr(params, '&');
    if (!amp) amp = params + strlen(params);
    if (!eq || eq > amp) { params = (*amp ? amp + 1 : amp); continue; }
    size_t klen = (size_t)(eq - params);
    size_t vlen = (size_t)(amp - (eq + 1));
    char val[96];
    url_decode(eq + 1, vlen, val, sizeof(val));

    if (klen == 6 && strncasecmp(params, "secret", 6) == 0) {
      int n = base32_decode(val, out->secret, sizeof(out->secret));
      if (n > 0) { out->secret_len = (size_t)n; have_secret = true; }
    } else if (klen == 9 && strncasecmp(params, "algorithm", 9) == 0) {
      if      (strcasecmp(val, "SHA256") == 0) out->type_algo = 0x22;
      else if (strcasecmp(val, "SHA512") == 0) out->type_algo = 0x23;
      else                                     out->type_algo = 0x21;
    } else if (klen == 6 && strncasecmp(params, "digits", 6) == 0) {
      int d = atoi(val);
      if (d >= 6 && d <= 8) out->digits = (uint8_t)d;
    } else if (klen == 6 && strncasecmp(params, "period", 6) == 0) {
      int p = atoi(val);
      if (p > 0 && p < 600) out->period = (uint16_t)p;
    }

    params = (*amp ? amp + 1 : amp);
  }
  if (!have_secret) {
    Serial.println("[QR] no 'secret=' parameter found");
    return false;
  }
  Serial.printf("[QR] parsed secret: %u bytes\n", (unsigned)out->secret_len);

  // Credential name: prepend "PP/" if period != 30 (Apex convention).
  if (out->period == 30) {
    strncpy(out->name_for_card, labelDecoded, sizeof(out->name_for_card) - 1);
  } else {
    snprintf(out->name_for_card, sizeof(out->name_for_card),
             "%u/%s", (unsigned)out->period, labelDecoded);
  }
  return true;
}