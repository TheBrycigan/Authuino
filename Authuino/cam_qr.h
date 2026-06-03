// cam_qr.h — QR scan module: camera capture + quirc decode.
//
// Architecture (Phase 2-lite):
//   - cam_qr_start_task spawns a FreeRTOS task on Core 0 that owns
//     the camera. It captures a frame, stashes the fb pointer for
//     the loop to pick up, then runs quirc_decode on that frame.
//   - The main loop (on Core 1) calls cam_qr_poll to fetch the
//     latest frame (or nothing) and, when a QR is decoded, the
//     payload. Only the loop touches LVGL — no mutex needed.
//   - cam_qr_stop_task tears the task down and deinits the camera.
//
// Lifecycle:
//     cam_qr_init();                      // once at boot
//     cam_qr_start_task();                // on QR screen enter
//     loop { cam_qr_poll(&dsc, pl, sz); } // until detect / cancel
//     cam_qr_stop_task();                 // on exit
//
// Threading:
//   - Only the camera task calls esp_camera_* and quirc_*.
//   - Only the loop task calls cam_qr_poll and touches the returned
//     lv_img_dsc_t.
//   - Shared state is protected by a tiny mutex (held for a pointer
//     swap only). No LVGL calls inside the lock.

#pragma once

#include <Arduino.h>
#include <lvgl.h>

// One-time init. Allocates quirc. Safe to call multiple times.
void cam_qr_init();

// Start the capture+decode task. Returns false on camera init
// failure. Safe to re-call after stop.
bool cam_qr_start_task();

// Tear down: signal task to stop, wait, deinit camera.
void cam_qr_stop_task();

// Poll return values.
enum : int {
  CAM_POLL_NONE     = 0,  // no update
  CAM_POLL_NEW_FRAME = 1, // new frame available; *out_dsc valid
  CAM_POLL_DETECTED = 2,  // QR decoded; payload filled. out_dsc may
                          //   also be updated with the frame that
                          //   contained it.
};

// Called from the loop. Picks up any new frame from the task
// (updating *out_dsc) and any detected payload (filling out_payload).
// Returns CAM_POLL_NEW_FRAME if there's a new frame to render,
// CAM_POLL_DETECTED if a QR was just decoded (payload is valid),
// CAM_POLL_NONE otherwise. A detect implies the caller should stop.
int cam_qr_poll(lv_img_dsc_t* out_dsc, char* out_payload, size_t payload_sz);

// Parse an otpauth://totp/... URI (as returned by cam_qr_poll) into
// a credential ready to push to the card. Returns false for unusable
// URIs (not TOTP, missing secret, etc.).
struct OtpAuthCred {
  char    name_for_card[64];
  uint8_t secret[64];
  size_t  secret_len;
  uint8_t type_algo;    // 0x21 SHA1 | 0x22 SHA256 | 0x23 SHA512
  uint8_t digits;       // 6, 7, or 8
  uint16_t period;      // seconds, typically 30
};
bool cam_qr_parse_otpauth(const char* uri, OtpAuthCred* out);
