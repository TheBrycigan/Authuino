#pragma once

/*
 * ui.h — LVGL screens for Authuino.
 *
 * Screen hierarchy:
 *
 *   Main Menu ─┬─ TOTP ──> TOTP Menu ──> View Codes / Add QR / Manual / Manage
 *              └─ Settings ──> Set Date/Time
 *
 * Battery indicator shown on Main Menu and Codes screen top bars.
 */

#include <Arduino.h>
#include <lvgl.h>

typedef void (*PinSubmitCb)(const char* pin);

// One-time setup. Call after lv_init() and a display driver have been
// registered. Loads the Main Menu screen.
void ui_init();

// Screen switches
void ui_show_main_menu();
void ui_show_totp_menu();
void ui_show_codes();
void ui_show_aid_manager();
void ui_show_pin_entry(const char* prompt, PinSubmitCb cb);
void ui_show_pin_change();
void ui_show_status(const char* msg);
void ui_show_stub(const char* title, const char* msg);

// PIV screens
void ui_show_piv_menu();
void ui_show_piv_info();

// Dynamic Main Menu tiles — called by the sketch after AID probing
// on card insert, and on card removal.
void ui_set_card_applets(bool oath, bool fido2, bool piv);
void ui_clear_card_applets();

// Settings screen
void ui_show_settings();

// Settings submenus
void ui_show_display();
void ui_show_hardware();
void ui_show_audio();

// USB Smart Card Reader mode — sub-screen reachable from Main Menu.
void ui_show_usb_reader();

// Date/time setter (sub-screen of Settings)
void ui_show_time_setter();

// Manual credential entry (sub-screen of TOTP Menu)
void ui_show_manual_entry();

// Transient toast notification. Floats near the bottom of whatever
// screen is active and auto-dismisses after ~2 seconds.
void ui_toast(const char* msg);

// QR scan screen
void ui_show_qr_scan();
void ui_qr_set_frame(const lv_img_dsc_t* dsc);
void ui_qr_set_hint(const char* text);

// Refresh content
void ui_refresh_codes();
void ui_update_timer();
void ui_hardware_tick();
void ui_usb_reader_tick();

// Physical button: 0 = UP / back, 1 = DOWN / scroll
void ui_handle_button(int btnIdx);

bool ui_is_on_pin_screen();
bool ui_is_on_codes_screen();
bool ui_is_on_settings_tree();

// ---------------------------------------------------------------------------
// Hooks: declared in ui.h, implemented in the sketch.
// ---------------------------------------------------------------------------
extern "C" {
  void onMainMenuTotpTap();
  void onTotpMenuViewCodes();
  void onTotpMenuAddQR();
  void onTotpMenuManualEntry();
  void onTotpMenuManage();
  void onDeleteSelected(const char* const* names, int count);

  // Backlight live preview (Settings brightness slider)
  void backlight_set_pct_live(int pct);

  // Battery / power queries (implemented in sketch via AXP2101)
  int  ui_get_battery_pct();   // 0..100 or -1 if no battery
  int  ui_get_battery_mv();    // millivolts, 0 if no battery
  bool ui_get_usb_status();    // true if USB VBUS present
  bool ui_get_charging();      // true if actively charging

  // USB Reader Mode — implemented in the sketch.
  bool ui_is_usb_reader_active();
  void ui_set_usb_reader_active(bool on);
}