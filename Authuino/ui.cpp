/*
 * ui.cpp — Authuino LVGL screens (Phase A: menu hierarchy).
 *
 * Screens managed here:
 *   scr_main_menu  — Main Menu (top): "Insert smartcard" or a TOTP tile.
 *   scr_totp_menu  — TOTP Menu: 2x2 tile grid (View / QR / Manual / Manage).
 *   scr_codes      — View Codes: scrollable vertical tile list.
 *   scr_pin        — PIN entry (numeric kbd, mode keys cycle to text/symbol).
 *   scr_stub       — Generic placeholder for unimplemented features.
 *
 * Sub-screens (totp_menu, codes, stub) get a top bar with a Back button.
 * The physical UP button routes through ui_handle_button() to the same
 * action.
 */

#include "ui.h"
#include "sc_interface.h"
#include "rtc_time.h"
#include "nvs_meta.h"
#include "nvs_aid.h"
#include "nvs_settings.h"
#include "audio.h"
#include "accel.h"
#include "usb_ccid.h"

#define FW_VERSION "v0.3"
#define LCD_W       320
#define LCD_H       480

#define COL_BG          0x0D1117
#define COL_TILE_BG     0x161B22
#define COL_TILE_HOVER  0x21262D
#define COL_TXT_DIM     0x8B949E
#define COL_TXT_NORM    0xC9D1D9
#define COL_TXT_FAINT   0x30363D
#define COL_GREEN       0x3FB950
#define COL_BLUE        0x58A6FF
#define COL_RED         0xF85149
#define COL_YELLOW      0xE3B341

#define TOPBAR_H        40
#define CODE_TILE_H     90
#define CODE_TILE_GAP    6

// ---------------------------------------------------------------------------
// Module-private state
// ---------------------------------------------------------------------------
static lv_obj_t* scr_main_menu = nullptr;
static lv_obj_t* scr_totp_menu = nullptr;
static lv_obj_t* scr_codes     = nullptr;
static lv_obj_t* scr_pin       = nullptr;
static lv_obj_t* scr_stub      = nullptr;
static lv_obj_t* scr_aid       = nullptr;
static lv_obj_t* scr_qr        = nullptr;
static lv_obj_t* scr_settings  = nullptr;
static lv_obj_t* scr_display   = nullptr;   // Settings > Display
static lv_obj_t* scr_hardware  = nullptr;   // Settings > Hardware (Phase 3b+)
static lv_obj_t* scr_time      = nullptr;   // date/time setter
static lv_obj_t* scr_manual    = nullptr;   // manual credential entry
static lv_obj_t* scr_usb_reader = nullptr;  // Main Menu > USB Reader (Phase 6)

// Toast widget
static lv_obj_t* toast_obj     = nullptr;
static lv_obj_t* toast_lbl     = nullptr;
static lv_timer_t* toast_timer = nullptr;

// Battery indicator (shared label, updated by timer)
static lv_obj_t* mm_batt_lbl   = nullptr;    // Main Menu top-left
static lv_timer_t* batt_timer  = nullptr;

// Main menu widgets
static lv_obj_t* mm_lbl_status = nullptr;     // "Insert smartcard" or hint

// TOTP menu has no dynamic widgets; it's static once built.

// Codes screen widgets
static lv_obj_t* c_lbl_title       = nullptr;     // "Codes (N)" / "N selected"
static lv_obj_t* c_btn_back        = nullptr;
static lv_obj_t* c_btn_cancel      = nullptr;     // shown in select mode
static lv_obj_t* c_dd_sort         = nullptr;     // sort-mode dropdown
static lv_obj_t* c_btn_select      = nullptr;
static lv_obj_t* c_btn_delete      = nullptr;     // shown in select mode
static lv_obj_t* c_tile_box        = nullptr;
static lv_obj_t* c_tile_obj [MAX_CREDENTIALS] = {0};
static lv_obj_t* c_tile_name[MAX_CREDENTIALS] = {0};
static lv_obj_t* c_tile_code[MAX_CREDENTIALS] = {0};
static lv_obj_t* c_tile_bar [MAX_CREDENTIALS] = {0};
static int       c_tile_count  = 0;

// Sort modes — Phase C2 adds Custom (long-press to enter reorder gesture).
typedef enum {
  SORT_CARD   = 0,
  SORT_ALPHA  = 1,
  SORT_DATE   = 2,
  SORT_CUSTOM = 3,
  SORT_COUNT  = 4
} SortMode;

static SortMode  sort_mode      = SORT_CARD;
static int       display_order[MAX_CREDENTIALS] = {0};

// Select / delete mode
static bool      select_mode    = false;
static bool      tile_selected[MAX_CREDENTIALS] = {0};
static lv_obj_t* delete_msgbox  = nullptr;

// Reorder mode (Phase C2) — entered by long-pressing a tile.
// reorder_active_pos is the *display position* of the tile being dragged
// (-1 means not in reorder mode).
static int       reorder_active_pos = -1;
static lv_coord_t reorder_total_dy  = 0;

// Stub screen widgets
static lv_obj_t* st_lbl_title  = nullptr;
static lv_obj_t* st_lbl_body   = nullptr;

// QR scan screen widgets
static lv_obj_t* qr_img        = nullptr;    // lv_img widget receiving camera frames
static lv_obj_t* qr_lbl_hint   = nullptr;    // bottom status line

// Settings screen widgets
static lv_obj_t* s_dim_slider  = nullptr;
static lv_obj_t* s_dim_val     = nullptr;
static lv_obj_t* s_sleep_slider= nullptr;
static lv_obj_t* s_sleep_val   = nullptr;
static lv_obj_t* s_bri_slider  = nullptr;
static lv_obj_t* s_bri_val     = nullptr;

// Time setter widgets (6 rollers: Y M D H M S)
static lv_obj_t* ts_roll_y  = nullptr;
static lv_obj_t* ts_roll_mo = nullptr;
static lv_obj_t* ts_roll_d  = nullptr;
static lv_obj_t* ts_roll_h  = nullptr;
static lv_obj_t* ts_roll_mi = nullptr;
static lv_obj_t* ts_roll_s  = nullptr;

// Manual entry screen widgets
static lv_obj_t* me_ta_issuer  = nullptr;
static lv_obj_t* me_ta_account = nullptr;
static lv_obj_t* me_ta_secret  = nullptr;
static lv_obj_t* me_dd_algo    = nullptr;
static lv_obj_t* me_dd_digits  = nullptr;
static lv_obj_t* me_dd_period  = nullptr;
static lv_obj_t* me_keyboard   = nullptr;
static lv_obj_t* me_form       = nullptr;   // scrollable container

// PIN change screen widgets
static lv_obj_t* scr_pin_change = nullptr;
static lv_obj_t* pc_ta_new      = nullptr;
static lv_obj_t* pc_ta_confirm  = nullptr;
static lv_obj_t* pc_keyboard    = nullptr;

// PIN screen widgets
static lv_obj_t* p_lbl_prompt  = nullptr;
static lv_obj_t* p_textarea    = nullptr;
static lv_obj_t* p_keyboard    = nullptr;
static PinSubmitCb pin_callback = nullptr;
static char        pin_temp[65] = {0};

// Forward declarations
static void build_main_menu();
static void build_totp_menu();
static void build_codes_screen();
static void build_pin_screen();
static void build_stub_screen();
static void build_qr_screen();
static void build_settings_screen();
static void build_display_screen();
static void build_hardware_screen();
static void build_audio_screen();
static void build_usb_reader_screen();
static void build_time_setter_screen();
static void build_manual_entry_screen();
static void build_pin_change_screen();
static void rebuild_code_tiles(int newCount);
static void batt_update_cb(lv_timer_t* t);

static void on_totp_view_tile(lv_event_t* e);
static void on_totp_qr_tile(lv_event_t* e);
static void on_totp_manual_tile(lv_event_t* e);
static void on_totp_manage_tile(lv_event_t* e);
static void on_back_to_main_menu(lv_event_t* e);
static void on_back_to_totp_menu(lv_event_t* e);
static void on_back_to_settings(lv_event_t* e);
static void kb_tap_beep_cb(lv_event_t* e);
static void tile_click_beep_cb(lv_event_t* e);
static void kb_event_cb(lv_event_t* e);

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Builds a top bar at the top of `parent` with a Back button on the left.
// `back_cb` is invoked when Back is tapped. `title` shown centred.
static void make_top_bar(lv_obj_t* parent, const char* title,
                         lv_event_cb_t back_cb) {
  lv_obj_t* bar = lv_obj_create(parent);
  lv_obj_set_size(bar, LCD_W, TOPBAR_H);
  lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_bg_color(bar, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_border_width(bar, 0, 0);
  lv_obj_set_style_pad_all(bar, 0, 0);
  lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

  if (back_cb) {
    lv_obj_t* back = lv_btn_create(bar);
    lv_obj_set_size(back, 64, 32);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(COL_TILE_BG), 0);
    lv_obj_set_style_radius(back, 6, 0);
    lv_obj_t* lbl = lv_label_create(back);
    lv_label_set_text(lbl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(lbl);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COL_TXT_NORM), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(back, tile_click_beep_cb, LV_EVENT_CLICKED, nullptr);
  }

  lv_obj_t* t = lv_label_create(bar);
  lv_label_set_text(t, title ? title : "");
  lv_obj_set_style_text_color(t, lv_color_hex(COL_TXT_NORM), 0);
  lv_obj_set_style_text_font(t, &lv_font_montserrat_16, 0);
  lv_obj_align(t, LV_ALIGN_CENTER, 0, 0);
}

// Helper to build a generic square tile (used in TOTP menu grid and the
// Main Menu's centred TOTP entry). `size` is both width and height in px.
static lv_obj_t* make_grid_tile(lv_obj_t* parent, const char* label,
                                lv_event_cb_t click_cb, int size = 140) {
  lv_obj_t* tile = lv_btn_create(parent);
  lv_obj_set_size(tile, size, size);
  lv_obj_set_style_bg_color(tile, lv_color_hex(COL_TILE_BG), 0);
  lv_obj_set_style_bg_color(tile, lv_color_hex(COL_TILE_HOVER), LV_STATE_PRESSED);
  lv_obj_set_style_radius(tile, 12, 0);
  lv_obj_set_style_border_width(tile, 0, 0);

  lv_obj_t* lbl = lv_label_create(tile);
  lv_label_set_text(lbl, label);
  lv_obj_set_style_text_color(lbl, lv_color_hex(COL_TXT_NORM), 0);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
  lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(lbl, size - 20);
  lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(lbl);

  if (click_cb) lv_obj_add_event_cb(tile, click_cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_event_cb(tile, tile_click_beep_cb, LV_EVENT_CLICKED, nullptr);
  return tile;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
void ui_init() {
  build_main_menu();
  build_totp_menu();
  build_codes_screen();
  build_pin_screen();
  build_stub_screen();
  build_qr_screen();
  build_settings_screen();
  build_display_screen();
  build_hardware_screen();
  build_audio_screen();
  build_usb_reader_screen();
  build_time_setter_screen();
  build_manual_entry_screen();
  build_pin_change_screen();
  lv_scr_load(scr_main_menu);

  // Start battery indicator timer (5s interval, runs forever).
  batt_timer = lv_timer_create(batt_update_cb, 5000, nullptr);
  batt_update_cb(nullptr);  // immediate first update
}

// ---------------------------------------------------------------------------
// Battery indicator timer — updates the Main Menu battery label every 5s.
// ---------------------------------------------------------------------------
static void batt_update_cb(lv_timer_t* t) {
  (void)t;
  int pct = ui_get_battery_pct();
  bool charging = ui_get_charging();
  bool usb = ui_get_usb_status();

  char buf[24];
  if (pct < 0) {
    snprintf(buf, sizeof(buf), usb ? LV_SYMBOL_USB : "");
  } else {
    const char* icon = charging ? LV_SYMBOL_CHARGE : LV_SYMBOL_BATTERY_FULL;
    snprintf(buf, sizeof(buf), "%s %d%%", icon, pct);
  }
  if (mm_batt_lbl) lv_label_set_text(mm_batt_lbl, buf);
}

// ---------------------------------------------------------------------------
// Main Menu — dynamic 2-column tile grid
//
// Tiles are created at init for each possible applet + Settings.
// On card insert, the sketch calls ui_set_card_applets() which
// shows/hides applet tiles and re-lays them out in a 2-column grid.
// Settings is always visible at the end.
// ---------------------------------------------------------------------------

// Tile slot IDs (order = priority in the grid, top-left to bottom-right)
enum MenuTile {
  TILE_TOTP = 0,
  TILE_FIDO2,
  TILE_PIV,
  TILE_USB_READER,
  // Future applets go here
  TILE_SETTINGS,   // always last
  TILE_COUNT
};

static lv_obj_t* mm_tiles[TILE_COUNT]    = {};
static bool      mm_tile_vis[TILE_COUNT] = {};   // current visibility
static const char* mm_tile_labels[TILE_COUNT] = {
  "TOTP",
  "FIDO2",
  "PIV",
  LV_SYMBOL_USB "\nUSB Reader",
  LV_SYMBOL_SETTINGS "\nSettings"
};

// Lay out all currently-visible tiles in a 2-column grid.
// Called whenever visibility changes.
static void mm_layout_tiles() {
  const int MARGIN_X  = 16;
  const int GAP_X     = 16;
  const int TILE_W    = (LCD_W - 2 * MARGIN_X - GAP_X) / 2;  // 136
  const int TILE_H    = 90;
  const int GAP_Y     = 12;
  const int START_Y   = 80;   // below header + status

  int col = 0, row = 0;
  for (int i = 0; i < TILE_COUNT; i++) {
    if (!mm_tiles[i]) continue;
    if (!mm_tile_vis[i]) {
      lv_obj_add_flag(mm_tiles[i], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    lv_obj_clear_flag(mm_tiles[i], LV_OBJ_FLAG_HIDDEN);
    int x = MARGIN_X + col * (TILE_W + GAP_X);
    int y = START_Y  + row * (TILE_H + GAP_Y);
    lv_obj_set_pos(mm_tiles[i], x, y);
    lv_obj_set_size(mm_tiles[i], TILE_W, TILE_H);
    col++;
    if (col >= 2) { col = 0; row++; }
  }
}

// Callbacks for each tile
static void on_tile_totp(lv_event_t* e)       { onMainMenuTotpTap(); }
static void on_tile_fido2(lv_event_t* e)      { ui_toast("FIDO2 - coming soon"); }
static void on_tile_piv(lv_event_t* e)        { ui_show_piv_menu(); }
static void on_tile_usb_reader(lv_event_t* e) { ui_show_usb_reader(); }
static void on_tile_settings(lv_event_t* e)   { ui_show_settings(); }

static lv_event_cb_t mm_tile_cbs[TILE_COUNT] = {
  on_tile_totp,
  on_tile_fido2,
  on_tile_piv,
  on_tile_usb_reader,
  on_tile_settings,
};

static void build_main_menu() {
  scr_main_menu = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(scr_main_menu, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_pad_all(scr_main_menu, 0, 0);
  lv_obj_clear_flag(scr_main_menu, LV_OBJ_FLAG_SCROLLABLE);

  // Header label (top): "Authuino"
  lv_obj_t* hdr = lv_label_create(scr_main_menu);
  lv_label_set_text(hdr, "Authuino");
  lv_obj_set_style_text_color(hdr, lv_color_hex(COL_TXT_NORM), 0);
  lv_obj_set_style_text_font(hdr, &lv_font_montserrat_20, 0);
  lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 12);

  // Battery indicator — top-left, updated by a global timer.
  mm_batt_lbl = lv_label_create(scr_main_menu);
  lv_label_set_text(mm_batt_lbl, "");
  lv_obj_set_style_text_color(mm_batt_lbl, lv_color_hex(COL_TXT_DIM), 0);
  lv_obj_set_style_text_font(mm_batt_lbl, &lv_font_montserrat_12, 0);
  lv_obj_align(mm_batt_lbl, LV_ALIGN_TOP_LEFT, 8, 14);

  // Status / hint label below the header
  mm_lbl_status = lv_label_create(scr_main_menu);
  lv_label_set_text(mm_lbl_status, "Insert smartcard");
  lv_obj_set_style_text_color(mm_lbl_status, lv_color_hex(COL_TXT_DIM), 0);
  lv_obj_set_style_text_font(mm_lbl_status, &lv_font_montserrat_14, 0);
  lv_label_set_long_mode(mm_lbl_status, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_width(mm_lbl_status, LCD_W - 24);
  lv_obj_set_style_text_align(mm_lbl_status, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(mm_lbl_status, LV_ALIGN_TOP_MID, 0, 46);

  // Create all tiles (initially hidden except Settings)
  for (int i = 0; i < TILE_COUNT; i++) {
    lv_obj_t* tile = lv_btn_create(scr_main_menu);
    lv_obj_set_style_bg_color(tile, lv_color_hex(COL_TILE_BG), 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(COL_TILE_HOVER), LV_STATE_PRESSED);
    lv_obj_set_style_radius(tile, 12, 0);
    lv_obj_set_style_border_width(tile, 0, 0);

    lv_obj_t* lbl = lv_label_create(tile);
    lv_label_set_text(lbl, mm_tile_labels[i]);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COL_TXT_NORM), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(lbl, 120);
    lv_obj_center(lbl);

    lv_obj_add_event_cb(tile, mm_tile_cbs[i], LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(tile, tile_click_beep_cb, LV_EVENT_CLICKED, nullptr);
    mm_tiles[i] = tile;
    mm_tile_vis[i] = (i == TILE_SETTINGS || i == TILE_USB_READER);
    // Settings + USB Reader visible by default; card-applet tiles
    // (TOTP/FIDO2/PIV) become visible when the card reports them.
  }

  mm_layout_tiles();

  // Version label bottom-right
  lv_obj_t* v = lv_label_create(scr_main_menu);
  lv_label_set_text(v, FW_VERSION);
  lv_obj_set_style_text_color(v, lv_color_hex(COL_TXT_FAINT), 0);
  lv_obj_set_style_text_font(v, &lv_font_montserrat_12, 0);
  lv_obj_align(v, LV_ALIGN_BOTTOM_RIGHT, -6, -6);
}

void ui_show_main_menu() {
  if (lv_scr_act() != scr_main_menu) lv_scr_load(scr_main_menu);

  // Restore the status label to reflect the current state. Other
  // screens (PIN entry, TOTP session) may have overwritten it with
  // transient messages like "Authenticating..." via ui_show_status().
  bool hasApplets = mm_tile_vis[TILE_TOTP] || mm_tile_vis[TILE_FIDO2] ||
                    mm_tile_vis[TILE_PIV];
  if (hasApplets) {
    lv_label_set_text(mm_lbl_status, "Card detected");
  } else if (sc_state().cardPresent) {
    lv_label_set_text(mm_lbl_status, "No known applets");
  } else {
    lv_label_set_text(mm_lbl_status, "Insert smartcard");
  }
}

// Called by the sketch after sc_probe_card() to update tile visibility.
void ui_set_card_applets(bool oath, bool fido2, bool piv) {
  mm_tile_vis[TILE_TOTP]     = oath;
  mm_tile_vis[TILE_FIDO2]    = fido2;
  mm_tile_vis[TILE_PIV]      = piv;
  mm_tile_vis[TILE_SETTINGS] = true;  // always visible

  if (oath || fido2 || piv) {
    lv_label_set_text(mm_lbl_status, "Card detected");
  } else if (sc_state().cardPresent) {
    lv_label_set_text(mm_lbl_status, "No known applets");
  } else {
    lv_label_set_text(mm_lbl_status, "Insert smartcard");
  }
  mm_layout_tiles();
}

// Called when card is removed — hide all applet tiles, keep Settings.
void ui_clear_card_applets() {
  mm_tile_vis[TILE_TOTP]     = false;
  mm_tile_vis[TILE_FIDO2]    = false;
  mm_tile_vis[TILE_PIV]      = false;
  mm_tile_vis[TILE_SETTINGS] = true;
  lv_label_set_text(mm_lbl_status, "Insert smartcard");
  mm_layout_tiles();
}

// ---------------------------------------------------------------------------
// TOTP Menu — 2x2 tile grid
// ---------------------------------------------------------------------------
static void build_totp_menu() {
  scr_totp_menu = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(scr_totp_menu, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_pad_all(scr_totp_menu, 0, 0);
  lv_obj_clear_flag(scr_totp_menu, LV_OBJ_FLAG_SCROLLABLE);

  make_top_bar(scr_totp_menu, "TOTP", on_back_to_main_menu);

  // Grid container — 2 columns x 2 rows of tiles via flex with wrap.
  // Math: usable width = LCD_W - 2*pad = 320 - 24 = 296.
  //       tile + gap + tile = 132 + 16 + 132 = 280, fits comfortably.
  lv_obj_t* grid = lv_obj_create(scr_totp_menu);
  lv_obj_set_size(grid, LCD_W, LCD_H - TOPBAR_H);
  lv_obj_align(grid, LV_ALIGN_TOP_MID, 0, TOPBAR_H);
  lv_obj_set_style_bg_color(grid, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_border_width(grid, 0, 0);
  lv_obj_set_style_pad_all(grid, 12, 0);
  lv_obj_set_style_pad_row(grid, 16, 0);
  lv_obj_set_style_pad_column(grid, 16, 0);
  lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(grid,
                        LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

  make_grid_tile(grid, "View\nCodes",   on_totp_view_tile,   132);
  make_grid_tile(grid, "Add via\nQR",   on_totp_qr_tile,     132);
  make_grid_tile(grid, "Manual\nEntry", on_totp_manual_tile, 132);
  make_grid_tile(grid, "Change\nPIN",   on_totp_manage_tile, 132);
}

void ui_show_totp_menu() {
  if (lv_scr_act() != scr_totp_menu) lv_scr_load(scr_totp_menu);
}

static void on_totp_view_tile  (lv_event_t* e) { onTotpMenuViewCodes(); }
static void on_totp_qr_tile    (lv_event_t* e) { onTotpMenuAddQR(); }
static void on_totp_manual_tile(lv_event_t* e) { onTotpMenuManualEntry(); }
static void on_totp_manage_tile(lv_event_t* e) { onTotpMenuManage(); }

static void on_back_to_main_menu(lv_event_t* e) {
  ui_show_main_menu();
}

// ===========================================================================
// PIV Menu — 2x2 tile grid (Info / Certs / PIN / Sign).
// ===========================================================================

static lv_obj_t* scr_piv_menu = nullptr;
static lv_obj_t* scr_piv_info = nullptr;
static lv_obj_t* piv_info_lbl = nullptr;

static void on_back_from_piv_menu(lv_event_t* e) { ui_show_main_menu(); }
static void on_back_from_piv_info(lv_event_t* e) { ui_show_piv_menu(); }

static void on_piv_info_tile (lv_event_t* e)  { ui_show_piv_info(); }
static void on_piv_certs_tile(lv_event_t* e)  { ui_toast("Certs - coming soon"); }
static void on_piv_pin_tile  (lv_event_t* e)  { ui_toast("PIN - coming soon"); }
static void on_piv_sign_tile (lv_event_t* e)  { ui_toast("Sign - coming soon"); }

static void build_piv_menu() {
  scr_piv_menu = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(scr_piv_menu, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_pad_all(scr_piv_menu, 0, 0);
  lv_obj_clear_flag(scr_piv_menu, LV_OBJ_FLAG_SCROLLABLE);

  make_top_bar(scr_piv_menu, "PIV", on_back_from_piv_menu);

  lv_obj_t* grid = lv_obj_create(scr_piv_menu);
  lv_obj_set_size(grid, LCD_W, LCD_H - TOPBAR_H);
  lv_obj_align(grid, LV_ALIGN_TOP_MID, 0, TOPBAR_H);
  lv_obj_set_style_bg_color(grid, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_border_width(grid, 0, 0);
  lv_obj_set_style_pad_all(grid, 12, 0);
  lv_obj_set_style_pad_row(grid, 16, 0);
  lv_obj_set_style_pad_column(grid, 16, 0);
  lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(grid,
                        LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

  make_grid_tile(grid, "Card\nInfo",     on_piv_info_tile,  132);
  make_grid_tile(grid, "Certs",          on_piv_certs_tile, 132);
  make_grid_tile(grid, "PIN\nMgmt",      on_piv_pin_tile,   132);
  make_grid_tile(grid, "Sign",           on_piv_sign_tile,  132);
}

void ui_show_piv_menu() {
  if (!scr_piv_menu) build_piv_menu();
  if (lv_scr_act() != scr_piv_menu) lv_scr_load(scr_piv_menu);
}

// Format a 16-byte GUID in standard 8-4-4-4-12 hex form.
static void format_guid(const uint8_t* g, char* out, size_t outLen) {
  snprintf(out, outLen,
           "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
           "%02x%02x%02x%02x%02x%02x",
           g[0],  g[1],  g[2],  g[3],
           g[4],  g[5],
           g[6],  g[7],
           g[8],  g[9],
           g[10], g[11], g[12], g[13], g[14], g[15]);
}

// Format a YYYYMMDD string as YYYY-MM-DD for readability.
static void format_expiry(const char* yyyymmdd, char* out, size_t outLen) {
  if (strlen(yyyymmdd) != 8) { snprintf(out, outLen, "%s", yyyymmdd); return; }
  snprintf(out, outLen, "%c%c%c%c-%c%c-%c%c",
           yyyymmdd[0], yyyymmdd[1], yyyymmdd[2], yyyymmdd[3],
           yyyymmdd[4], yyyymmdd[5],
           yyyymmdd[6], yyyymmdd[7]);
}

// FASC-N is BCD-packed; we don't try to decode the agency fields for
// display, just hex-dump it. Real federal cards have meaningful FASC-Ns
// (agency code, system code, credential number, etc.); consumer PIV
// cards typically have a placeholder value of all 9s.
static void format_fascn(const uint8_t* f, uint8_t flen,
                         char* out, size_t outLen) {
  size_t pos = 0;
  for (uint8_t i = 0; i < flen && pos + 3 < outLen; i++) {
    pos += snprintf(out + pos, outLen - pos, "%02x", f[i]);
  }
}

static void build_piv_info() {
  scr_piv_info = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(scr_piv_info, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_pad_all(scr_piv_info, 0, 0);
  lv_obj_clear_flag(scr_piv_info, LV_OBJ_FLAG_SCROLLABLE);

  make_top_bar(scr_piv_info, "Card Info", on_back_from_piv_info);

  piv_info_lbl = lv_label_create(scr_piv_info);
  lv_obj_set_width(piv_info_lbl, LCD_W - 24);
  lv_obj_align(piv_info_lbl, LV_ALIGN_TOP_LEFT, 12, TOPBAR_H + 12);
  lv_label_set_long_mode(piv_info_lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_color(piv_info_lbl, lv_color_hex(COL_TXT_NORM), 0);
  lv_obj_set_style_text_font(piv_info_lbl, &lv_font_montserrat_14, 0);
  lv_label_set_text(piv_info_lbl, "Reading card...");
}

void ui_show_piv_info() {
  if (!scr_piv_info) build_piv_info();
  lv_label_set_text(piv_info_lbl, "Reading card...");
  if (lv_scr_act() != scr_piv_info) lv_scr_load(scr_piv_info);

  // Force LVGL to paint the "Reading..." label before we block on
  // the smartcard exchange (which can take ~50-200ms).
  lv_timer_handler();

  PivChuidInfo info;
  bool ok = sc_piv_get_chuid_info(&info);

  // Capture diagnostic info that's useful in both success and
  // failure paths.
  const char* atrStr = sc_state().atrStr;
  int proto          = sc_get_protocol();
  const char* protoStr = (proto == 0) ? "T=0" : (proto == 1) ? "T=1" : "?";

  if (!ok) {
    int sw = sc_piv_last_select_sw();
    char buf[512];
    if (sw == 0) {
      snprintf(buf, sizeof(buf),
               "Could not power the card.\n\n"
               "ATR: %s\n"
               "Proto: %s\n",
               atrStr, protoStr);
    } else if (sw == 0x9000) {
      snprintf(buf, sizeof(buf),
               "PIV applet selected,\n"
               "but CHUID read failed.\n\n"
               "ATR: %s\n"
               "Proto: %s",
               atrStr, protoStr);
    } else if (sw == -2 || sw == -3 || sw == -1 || sw == -4 || sw == -5) {
      // Negative SW = transport error, not an ISO 7816 status word.
      const char* errStr;
      switch (sw) {
        case -1: errStr = "not active"; break;
        case -2: errStr = "TIMEOUT (card not responding)"; break;
        case -3: errStr = "card removed"; break;
        case -4: errStr = "bad APDU"; break;
        case -5: errStr = "no clock"; break;
        default: errStr = "unknown";
      }
      snprintf(buf, sizeof(buf),
               "PIV SELECT transport error.\n\n"
               "Code: %d (%s)\n\n"
               "ATR: %s\n"
               "Proto: %s",
               sw, errStr, atrStr, protoStr);
    } else {
      const char* hint = "";
      switch (sw) {
        case 0x6A82: hint = "(file/applet not found)"; break;
        case 0x6A86: hint = "(wrong P1/P2)"; break;
        case 0x6A87: hint = "(Lc inconsistent)"; break;
        case 0x6700: hint = "(wrong length)"; break;
        case 0x6982: hint = "(security not satisfied)"; break;
        case 0x6985: hint = "(conditions not satisfied)"; break;
        case 0x6E00: hint = "(class not supported)"; break;
        case 0x6D00: hint = "(INS not supported)"; break;
        default:     hint = "";
      }
      snprintf(buf, sizeof(buf),
               "PIV SELECT failed.\n\n"
               "SW = %04X %s\n\n"
               "ATR: %s\n"
               "Proto: %s",
               sw, hint, atrStr, protoStr);
    }
    lv_label_set_text(piv_info_lbl, buf);
    return;
  }

  char out[512];
  size_t pos = 0;
  if (info.guid_present) {
    char gstr[40];
    format_guid(info.guid, gstr, sizeof(gstr));
    pos += snprintf(out + pos, sizeof(out) - pos, "GUID:\n%s\n\n", gstr);
  }
  if (info.expiry_present) {
    char estr[16];
    format_expiry(info.expiry, estr, sizeof(estr));
    pos += snprintf(out + pos, sizeof(out) - pos, "Expires: %s\n\n", estr);
  }
  if (info.fascn_present) {
    char fstr[64];
    format_fascn(info.fascn, info.fascn_len, fstr, sizeof(fstr));
    pos += snprintf(out + pos, sizeof(out) - pos, "FASC-N:\n%s\n", fstr);
  }
  lv_label_set_text(piv_info_lbl, out);
}

// ---------------------------------------------------------------------------
// Codes screen — top bar with Sort + Select buttons, scrollable tile list,
// per-tile period bar, multi-select mode with confirm-delete.
// ---------------------------------------------------------------------------

// Yubico OATH naming convention encodes non-default periods as a "DD/"
// prefix on the credential name (e.g. "60/Issuer:account"). These two
// helpers decode that on the fly so we don't have to touch the Credential
// struct in sc_interface.

static uint16_t period_of(const Credential& c) {
  int slashPos = -1;
  for (int i = 0; i < 4 && c.name[i]; i++) {
    if (c.name[i] == '/') { slashPos = i; break; }
  }
  if (slashPos <= 0) return 30;
  uint16_t parsed = 0;
  for (int i = 0; i < slashPos; i++) {
    if (c.name[i] < '0' || c.name[i] > '9') return 30;
    parsed = parsed * 10 + (uint16_t)(c.name[i] - '0');
  }
  if (parsed == 0 || parsed > 600) return 30;
  return parsed;
}

static const char* display_name(const Credential& c) {
  int slashPos = -1;
  for (int i = 0; i < 4 && c.name[i]; i++) {
    if (c.name[i] == '/') { slashPos = i; break; }
  }
  if (slashPos <= 0) return c.name;
  for (int i = 0; i < slashPos; i++) {
    if (c.name[i] < '0' || c.name[i] > '9') return c.name;
  }
  return c.name + slashPos + 1;
}

// The dropdown's option strings — order MUST match SortMode enum.
static const char SORT_OPTIONS[] = "Card\nA-Z\nDate\nCustom";

// Build display_order[] based on sort_mode and the current credential set.
static void update_display_order(int n) {
  for (int i = 0; i < n && i < MAX_CREDENTIALS; i++) display_order[i] = i;
  if (n <= 1) return;

  const SCState& s = sc_state();

  if (sort_mode == SORT_ALPHA) {
    // Insertion sort, comparing display names case-insensitively.
    for (int i = 1; i < n; i++) {
      int key = display_order[i];
      const char* keyName = display_name(s.credentials[key]);
      int j = i - 1;
      while (j >= 0) {
        const char* cmpName = display_name(s.credentials[display_order[j]]);
        if (strcasecmp(cmpName, keyName) <= 0) break;
        display_order[j + 1] = display_order[j];
        j--;
      }
      display_order[j + 1] = key;
    }
  } else if (sort_mode == SORT_DATE) {
    // Newest first (highest add_date first). Cache dates locally so we
    // hit NVS once per credential rather than O(N²) times during the sort.
    // Creds with no entry (date == 0, e.g. RTC was never set when first
    // seen) sort to the bottom.
    uint32_t dates[MAX_CREDENTIALS];
    for (int i = 0; i < n; i++) {
      dates[i] = nvs_meta_get_add_date(s.credentials[i].name);
    }
    for (int i = 1; i < n; i++) {
      int key = display_order[i];
      uint32_t keyDate = dates[key];
      int j = i - 1;
      while (j >= 0 && dates[display_order[j]] < keyDate) {
        display_order[j + 1] = display_order[j];
        j--;
      }
      display_order[j + 1] = key;
    }
  } else if (sort_mode == SORT_CUSTOM) {
    // Sort by user-assigned custom_pos ascending. UNSET (0xFFFF) is the
    // largest 16-bit value, so unset creds naturally land at the bottom.
    uint16_t pos[MAX_CREDENTIALS];
    for (int i = 0; i < n; i++) {
      pos[i] = nvs_meta_get_custom_pos(s.credentials[i].name);
    }
    for (int i = 1; i < n; i++) {
      int key = display_order[i];
      uint16_t keyPos = pos[key];
      int j = i - 1;
      while (j >= 0 && pos[display_order[j]] > keyPos) {
        display_order[j + 1] = display_order[j];
        j--;
      }
      display_order[j + 1] = key;
    }
  }
  // SORT_CARD: leave as identity — already set above.
}

// Count currently selected credentials.
static int count_selected() {
  int n = 0;
  for (int i = 0; i < MAX_CREDENTIALS; i++) if (tile_selected[i]) n++;
  return n;
}

// ---------------------------------------------------------------------------
// Top-bar mode swap — show Back/Sort/Select in normal mode, Cancel/Delete
// in select mode. Title in the centre is always present, content varies.
// ---------------------------------------------------------------------------
static void update_top_bar() {
  bool sel = select_mode;
  if (c_btn_back)   sel ? lv_obj_add_flag  (c_btn_back,   LV_OBJ_FLAG_HIDDEN)
                        : lv_obj_clear_flag(c_btn_back,   LV_OBJ_FLAG_HIDDEN);
  if (c_btn_cancel) sel ? lv_obj_clear_flag(c_btn_cancel, LV_OBJ_FLAG_HIDDEN)
                        : lv_obj_add_flag  (c_btn_cancel, LV_OBJ_FLAG_HIDDEN);
  if (c_dd_sort)    sel ? lv_obj_add_flag  (c_dd_sort,    LV_OBJ_FLAG_HIDDEN)
                        : lv_obj_clear_flag(c_dd_sort,    LV_OBJ_FLAG_HIDDEN);
  if (c_btn_select) sel ? lv_obj_add_flag  (c_btn_select, LV_OBJ_FLAG_HIDDEN)
                        : lv_obj_clear_flag(c_btn_select, LV_OBJ_FLAG_HIDDEN);
  if (c_btn_delete) sel ? lv_obj_clear_flag(c_btn_delete, LV_OBJ_FLAG_HIDDEN)
                        : lv_obj_add_flag  (c_btn_delete, LV_OBJ_FLAG_HIDDEN);

  if (c_lbl_title) {
    if (sel) {
      char buf[24];
      snprintf(buf, sizeof(buf), "%d selected", count_selected());
      lv_label_set_text(c_lbl_title, buf);
    } else {
      lv_label_set_text(c_lbl_title, "Codes");
    }
  }

  // Disable Delete when nothing's selected, for a visual cue.
  if (c_btn_delete) {
    if (count_selected() > 0) lv_obj_clear_state(c_btn_delete, LV_STATE_DISABLED);
    else                      lv_obj_add_state  (c_btn_delete, LV_STATE_DISABLED);
  }
}

// Apply selection / period-bar visual to a single tile.
static void update_tile_visual(int displayPos) {
  if (displayPos < 0 || displayPos >= MAX_CREDENTIALS) return;
  if (!c_tile_obj[displayPos]) return;
  int credIdx = display_order[displayPos];
  bool isSel = select_mode && tile_selected[credIdx];
  lv_obj_set_style_bg_color(c_tile_obj[displayPos],
      lv_color_hex(isSel ? COL_BLUE : COL_TILE_BG), 0);
}

static void refresh_all_tile_visuals() {
  for (int i = 0; i < c_tile_count; i++) update_tile_visual(i);
}

static void enter_select_mode() {
  if (select_mode) return;
  select_mode = true;
  for (int i = 0; i < MAX_CREDENTIALS; i++) tile_selected[i] = false;
  update_top_bar();
  refresh_all_tile_visuals();
}

static void exit_select_mode() {
  if (!select_mode) return;
  select_mode = false;
  for (int i = 0; i < MAX_CREDENTIALS; i++) tile_selected[i] = false;
  update_top_bar();
  refresh_all_tile_visuals();
}

// ---------------------------------------------------------------------------
// Event handlers (top-bar buttons, tiles, msgbox)
// ---------------------------------------------------------------------------
static void on_codes_back_btn   (lv_event_t* e);
static void on_codes_cancel_btn (lv_event_t* e);
static void on_sort_changed     (lv_event_t* e);
static void on_codes_select_btn (lv_event_t* e);
static void on_codes_delete_btn (lv_event_t* e);
static void on_tile_event       (lv_event_t* e);
static void on_delete_msgbox    (lv_event_t* e);

// Reorder-mode helpers (Phase C2)
static void enter_reorder_mode(int pos);
static void exit_reorder_mode();
static void commit_reorder();
static void capture_current_order_as_custom_if_empty();
static void save_all_custom_positions();

static void rebuild_code_tiles(int newCount);

// ---------------------------------------------------------------------------
static void build_codes_screen() {
  scr_codes = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(scr_codes, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_pad_all(scr_codes, 0, 0);
  lv_obj_clear_flag(scr_codes, LV_OBJ_FLAG_SCROLLABLE);

  // ---- Custom top bar (we don't use make_top_bar here because we need
  // ----  Sort + Select buttons in addition to Back) --------------------
  lv_obj_t* bar = lv_obj_create(scr_codes);
  lv_obj_set_size(bar, LCD_W, TOPBAR_H);
  lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_bg_color(bar, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_border_width(bar, 0, 0);
  lv_obj_set_style_pad_all(bar, 0, 0);
  lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

  // Back button (left, normal mode)
  c_btn_back = lv_btn_create(bar);
  lv_obj_set_size(c_btn_back, 44, 32);
  lv_obj_align(c_btn_back, LV_ALIGN_LEFT_MID, 4, 0);
  lv_obj_set_style_bg_color(c_btn_back, lv_color_hex(COL_TILE_BG), 0);
  lv_obj_set_style_radius(c_btn_back, 6, 0);
  {
    lv_obj_t* l = lv_label_create(c_btn_back);
    lv_label_set_text(l, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(l, lv_color_hex(COL_TXT_NORM), 0);
    lv_obj_center(l);
  }
  lv_obj_add_event_cb(c_btn_back, on_codes_back_btn, LV_EVENT_CLICKED, nullptr);

  // Cancel button (left, select mode) — same slot as Back
  c_btn_cancel = lv_btn_create(bar);
  lv_obj_set_size(c_btn_cancel, 44, 32);
  lv_obj_align(c_btn_cancel, LV_ALIGN_LEFT_MID, 4, 0);
  lv_obj_set_style_bg_color(c_btn_cancel, lv_color_hex(COL_TILE_BG), 0);
  lv_obj_set_style_radius(c_btn_cancel, 6, 0);
  {
    lv_obj_t* l = lv_label_create(c_btn_cancel);
    lv_label_set_text(l, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(l, lv_color_hex(COL_TXT_NORM), 0);
    lv_obj_center(l);
  }
  lv_obj_add_event_cb(c_btn_cancel, on_codes_cancel_btn, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_flag(c_btn_cancel, LV_OBJ_FLAG_HIDDEN);

  // Sort dropdown (middle-left, normal mode) — fixed "Sort" label;
  // the open list shows Card / A-Z / Date / Custom with the current
  // mode highlighted.
  c_dd_sort = lv_dropdown_create(bar);
  lv_dropdown_set_text(c_dd_sort, "Sort");
  lv_dropdown_set_options_static(c_dd_sort, SORT_OPTIONS);
  lv_dropdown_set_selected(c_dd_sort, sort_mode);
  lv_obj_set_size(c_dd_sort, 80, 32);
  lv_obj_align(c_dd_sort, LV_ALIGN_LEFT_MID, 54, 0);
  lv_obj_set_style_bg_color    (c_dd_sort, lv_color_hex(COL_TILE_BG), 0);
  lv_obj_set_style_text_color  (c_dd_sort, lv_color_hex(COL_TXT_NORM), 0);
  lv_obj_set_style_text_font   (c_dd_sort, &lv_font_montserrat_12, 0);
  lv_obj_set_style_border_width(c_dd_sort, 0, 0);
  lv_obj_set_style_radius      (c_dd_sort, 6, 0);
  lv_obj_add_event_cb(c_dd_sort, on_sort_changed, LV_EVENT_VALUE_CHANGED, nullptr);

  // Title label (centre) — "Codes" or "N selected"
  c_lbl_title = lv_label_create(bar);
  lv_label_set_text(c_lbl_title, "Codes");
  lv_obj_set_style_text_color(c_lbl_title, lv_color_hex(COL_TXT_NORM), 0);
  lv_obj_set_style_text_font(c_lbl_title, &lv_font_montserrat_14, 0);
  lv_obj_align(c_lbl_title, LV_ALIGN_CENTER, 30, 0);

  // Select button (right, normal mode)
  c_btn_select = lv_btn_create(bar);
  lv_obj_set_size(c_btn_select, 64, 32);
  lv_obj_align(c_btn_select, LV_ALIGN_RIGHT_MID, -4, 0);
  lv_obj_set_style_bg_color(c_btn_select, lv_color_hex(COL_TILE_BG), 0);
  lv_obj_set_style_radius(c_btn_select, 6, 0);
  {
    lv_obj_t* l = lv_label_create(c_btn_select);
    lv_label_set_text(l, "Select");
    lv_obj_set_style_text_color(l, lv_color_hex(COL_TXT_NORM), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
    lv_obj_center(l);
  }
  lv_obj_add_event_cb(c_btn_select, on_codes_select_btn, LV_EVENT_CLICKED, nullptr);

  // Delete button (right, select mode) — same slot
  c_btn_delete = lv_btn_create(bar);
  lv_obj_set_size(c_btn_delete, 64, 32);
  lv_obj_align(c_btn_delete, LV_ALIGN_RIGHT_MID, -4, 0);
  lv_obj_set_style_bg_color(c_btn_delete, lv_color_hex(COL_RED), 0);
  lv_obj_set_style_bg_color(c_btn_delete, lv_color_hex(0x4D2026), LV_STATE_DISABLED);
  lv_obj_set_style_radius(c_btn_delete, 6, 0);
  {
    lv_obj_t* l = lv_label_create(c_btn_delete);
    lv_label_set_text(l, LV_SYMBOL_TRASH);
    lv_obj_set_style_text_color(l, lv_color_hex(COL_TXT_NORM), 0);
    lv_obj_center(l);
  }
  lv_obj_add_event_cb(c_btn_delete, on_codes_delete_btn, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_flag(c_btn_delete, LV_OBJ_FLAG_HIDDEN);

  // ---- Scrollable tile container (everything below the top bar) -------
  c_tile_box = lv_obj_create(scr_codes);
  lv_obj_set_size(c_tile_box, LCD_W, LCD_H - TOPBAR_H);
  lv_obj_align(c_tile_box, LV_ALIGN_TOP_MID, 0, TOPBAR_H);
  lv_obj_set_style_bg_color(c_tile_box, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_border_width(c_tile_box, 0, 0);
  lv_obj_set_style_pad_all(c_tile_box, 6, 0);
  lv_obj_set_style_pad_row(c_tile_box, CODE_TILE_GAP, 0);
  lv_obj_set_flex_flow(c_tile_box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(c_tile_box,
                        LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_scroll_dir(c_tile_box, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(c_tile_box, LV_SCROLLBAR_MODE_AUTO);
}

// ---------------------------------------------------------------------------
static void rebuild_code_tiles(int newCount) {
  if (!c_tile_box) return;
  lv_obj_clean(c_tile_box);
  for (int i = 0; i < MAX_CREDENTIALS; i++) {
    c_tile_obj [i] = nullptr;
    c_tile_name[i] = nullptr;
    c_tile_code[i] = nullptr;
    c_tile_bar [i] = nullptr;
  }

  for (int i = 0; i < newCount && i < MAX_CREDENTIALS; i++) {
    lv_obj_t* tile = lv_obj_create(c_tile_box);
    lv_obj_set_size(tile, LCD_W - 24, CODE_TILE_H);
    lv_obj_set_style_bg_color(tile, lv_color_hex(COL_TILE_BG), 0);
    lv_obj_set_style_radius(tile, 8, 0);
    lv_obj_set_style_border_width(tile, 0, 0);
    lv_obj_set_style_pad_all(tile, 10, 0);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tile, on_tile_event, LV_EVENT_ALL,
                        (void*)(intptr_t)i);
    c_tile_obj[i] = tile;

    lv_obj_t* name = lv_label_create(tile);
    lv_obj_set_style_text_color(name, lv_color_hex(COL_TXT_DIM), 0);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(name, LCD_W - 50);
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0, 0);
    c_tile_name[i] = name;

    lv_obj_t* code = lv_label_create(tile);
    lv_obj_set_style_text_color(code, lv_color_hex(COL_GREEN), 0);
    lv_obj_set_style_text_font(code, &lv_font_montserrat_28, 0);
    lv_obj_align(code, LV_ALIGN_LEFT_MID, 0, 6);
    c_tile_code[i] = code;

    // Period countdown bar — full width at start of each period, shrinks
    // to 0 by the end. Width updated in ui_update_timer().
    lv_obj_t* bar = lv_obj_create(tile);
    lv_obj_set_size(bar, 0, 3);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COL_GREEN), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 2, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    c_tile_bar[i] = bar;
  }
  c_tile_count = newCount;
}

void ui_show_codes() {
  if (lv_scr_act() != scr_codes) lv_scr_load(scr_codes);
  exit_reorder_mode();    // safety in case last visit was interrupted
  exit_select_mode();
  ui_refresh_codes();
}

void ui_refresh_codes() {
  if (lv_scr_act() != scr_codes) return;

  const SCState& s = sc_state();

  if (!s.cardPresent || !s.appletSelected ||
      (s.needsAuth && !s.authenticated)) {
    if (c_lbl_title) lv_label_set_text(c_lbl_title, "Card / auth lost");
    if (c_tile_count) rebuild_code_tiles(0);
    return;
  }
  if (!rtc_isRunning()) {
    if (c_lbl_title) lv_label_set_text(c_lbl_title, "Set time first");
    return;
  }
  if (s.numCredentials == 0) {
    if (c_lbl_title) lv_label_set_text(c_lbl_title, "No codes");
    if (c_tile_count) rebuild_code_tiles(0);
    return;
  }

  if (s.numCredentials != c_tile_count) rebuild_code_tiles(s.numCredentials);
  update_display_order(s.numCredentials);

  for (int pos = 0; pos < s.numCredentials && pos < MAX_CREDENTIALS; pos++) {
    int credIdx = display_order[pos];
    const Credential& c = s.credentials[credIdx];
    if (c_tile_name[pos]) lv_label_set_text(c_tile_name[pos], display_name(c));
    if (c_tile_code[pos]) lv_label_set_text(c_tile_code[pos],
        c.valid ? c.code : "------");
  }
  refresh_all_tile_visuals();
  update_top_bar();
}

void ui_update_timer() {
  if (lv_scr_act() != scr_codes) return;
  if (!rtc_isRunning())          return;

  const SCState& s = sc_state();
  if (!s.appletSelected || (s.needsAuth && !s.authenticated)) return;

  // Get sub-second resolution by anchoring millis() to the most
  // recent RTC second tick. The RTC has 1-second granularity but we
  // want the bar to animate smoothly across each second.
  static time_t   s_last_now_s  = 0;
  static uint32_t s_last_now_ms = 0;
  time_t   now_s  = rtc_epoch();
  uint32_t now_ms = millis();
  if (now_s != s_last_now_s) {
    s_last_now_s  = now_s;
    s_last_now_ms = now_ms;
  }
  uint32_t frac_ms = now_ms - s_last_now_ms;
  if (frac_ms > 1000) frac_ms = 1000;   // clamp if we miss a tick

  // Tile is LCD_W-24 wide with pad_all=10, so the inner content area
  // (where the bar lives) is LCD_W-24-20 wide. Without subtracting the
  // padding the bar's right edge spills past the tile's right padding.
  int maxBarW = LCD_W - 24 - 20;

  for (int pos = 0; pos < s.numCredentials && pos < MAX_CREDENTIALS; pos++) {
    if (!c_tile_bar[pos]) continue;
    int credIdx = display_order[pos];
    uint16_t period = period_of(s.credentials[credIdx]);
    if (period == 0) period = 30;

    uint32_t period_ms     = (uint32_t)period * 1000;
    uint32_t period_pos_s  = (uint32_t)(now_s % period);
    uint32_t elapsed_ms    = period_pos_s * 1000 + frac_ms;
    if (elapsed_ms > period_ms) elapsed_ms = period_ms;
    uint32_t remaining_ms  = period_ms - elapsed_ms;

    // Bar width: maxBarW at the start of the period, shrinking
    // smoothly to 0 when 1 second remains. The last second is shown
    // empty — by then the user should have read the code, and the
    // empty bar warns them the next code is imminent.
    int w;
    if (remaining_ms <= 1000) {
      w = 0;
    } else {
      // Map (1000, period_ms] → (0, maxBarW].
      w = (int)(((remaining_ms - 1000) * (uint32_t)maxBarW) / (period_ms - 1000));
    }
    lv_obj_set_width(c_tile_bar[pos], w);

    // Color the bar and code text based on remaining seconds:
    //   <=  5s : red
    //   <= 10s : yellow
    //   else   : green
    uint32_t remaining_s = (remaining_ms + 999) / 1000;   // ceil
    uint32_t color = COL_GREEN;
    if      (remaining_s <=  5) color = COL_RED;
    else if (remaining_s <= 10) color = COL_YELLOW;
    lv_obj_set_style_bg_color(c_tile_bar[pos], lv_color_hex(color), 0);
    if (c_tile_code[pos]) {
      lv_obj_set_style_text_color(c_tile_code[pos], lv_color_hex(color), 0);
    }
  }
}

// ---------------------------------------------------------------------------
// Top-bar handlers
// ---------------------------------------------------------------------------
static void on_codes_back_btn(lv_event_t* e)   { ui_show_totp_menu(); }
static void on_codes_cancel_btn(lv_event_t* e) { exit_select_mode(); }

// Used by the stub screen's top bar (and any other sub-screen below the
// TOTP menu that needs a plain "back to TOTP menu" handler).
static void on_back_to_totp_menu(lv_event_t* e) { ui_show_totp_menu(); }

static void on_sort_changed(lv_event_t* e) {
  lv_obj_t* dd = lv_event_get_target(e);
  uint16_t  sel = lv_dropdown_get_selected(dd);
  if (sel >= SORT_COUNT) return;

  SortMode prev = sort_mode;
  sort_mode = (SortMode)sel;

  // First entry into Custom: seed positions from the order that was just
  // visible, so the layout doesn't shuffle. On subsequent entries we
  // leave saved positions alone.
  if (sort_mode == SORT_CUSTOM && prev != SORT_CUSTOM) {
    capture_current_order_as_custom_if_empty();
  }

  ui_refresh_codes();
}

static void on_codes_select_btn(lv_event_t* e) {
  enter_select_mode();
}

static void on_codes_delete_btn(lv_event_t* e) {
  if (count_selected() == 0) return;
  if (delete_msgbox) return;   // already open

  static const char* btns[] = { "Delete", "Cancel", "" };
  char buf[48];
  int n = count_selected();
  snprintf(buf, sizeof(buf), "Delete %d code%s?\nThis cannot be undone.",
           n, n == 1 ? "" : "s");

  delete_msgbox = lv_msgbox_create(nullptr, "Confirm", buf, btns, false);
  lv_obj_center(delete_msgbox);
  lv_obj_add_event_cb(delete_msgbox, on_delete_msgbox, LV_EVENT_VALUE_CHANGED, nullptr);
}

static void on_delete_msgbox(lv_event_t* e) {
  lv_obj_t* mb = lv_event_get_current_target(e);
  uint16_t btn = lv_msgbox_get_active_btn(mb);

  if (btn == 0) {
    // "Delete"
    char names[MAX_CREDENTIALS][MAX_CRED_NAME_LEN];
    const char* ptrs[MAX_CREDENTIALS];
    int count = 0;
    const SCState& s = sc_state();
    for (int i = 0; i < s.numCredentials && count < MAX_CREDENTIALS; i++) {
      if (tile_selected[i]) {
        strncpy(names[count], s.credentials[i].name, MAX_CRED_NAME_LEN - 1);
        names[count][MAX_CRED_NAME_LEN - 1] = '\0';
        ptrs[count] = names[count];
        count++;
      }
    }
    onDeleteSelected(ptrs, count);     // sketch performs actual deletes
    exit_select_mode();
    ui_refresh_codes();                // pick up the new credential set
  }

  lv_msgbox_close(mb);
  delete_msgbox = nullptr;
}

// ---------------------------------------------------------------------------
// Tile event router
//   CLICKED       — toggle selection in select mode
//   LONG_PRESSED  — enter reorder mode (auto-switches sort to Custom if
//                   not already there); ignored in select mode
//   PRESSING      — drag the active reorder tile vertically (translate_y)
//   RELEASED      — commit the reorder, persist new positions, redraw
// ---------------------------------------------------------------------------
static void on_tile_event(lv_event_t* e) {
  lv_event_code_t code = lv_event_get_code(e);
  int pos = (int)(intptr_t)lv_event_get_user_data(e);
  if (pos < 0 || pos >= MAX_CREDENTIALS) return;

  switch (code) {
    case LV_EVENT_CLICKED: {
      if (!select_mode) return;
      int credIdx = display_order[pos];
      tile_selected[credIdx] = !tile_selected[credIdx];
      update_tile_visual(pos);
      update_top_bar();
      break;
    }

    case LV_EVENT_LONG_PRESSED: {
      if (select_mode)             return;       // not allowed in select
      if (reorder_active_pos >= 0) return;       // already reordering
      if (sort_mode != SORT_CUSTOM) return;      // only allowed in Custom

      // Make sure positions are seeded the first time anyone reorders.
      // No-op once any cred has a saved position.
      capture_current_order_as_custom_if_empty();
      enter_reorder_mode(pos);
      break;
    }

    case LV_EVENT_PRESSING: {
      if (reorder_active_pos != pos) break;
      lv_indev_t* indev = lv_indev_get_act();
      if (!indev) break;
      lv_point_t vect;
      lv_indev_get_vect(indev, &vect);
      reorder_total_dy += vect.y;
      lv_obj_set_style_translate_y(c_tile_obj[pos], reorder_total_dy, 0);
      break;
    }

    case LV_EVENT_RELEASED: {
      if (reorder_active_pos != pos) break;
      commit_reorder();
      break;
    }

    default: break;
  }
}

// ---------------------------------------------------------------------------
// Reorder-mode lifecycle (Phase C2)
// ---------------------------------------------------------------------------
static void enter_reorder_mode(int pos) {
  if (pos < 0 || pos >= c_tile_count) return;
  reorder_active_pos = pos;
  reorder_total_dy   = 0;

  // Disable scrolling on the tile container so vertical drags don't get
  // hijacked by the scroll machinery.
  lv_obj_clear_flag(c_tile_box, LV_OBJ_FLAG_SCROLLABLE);

  // Visual cue: shrink + dim the non-moving tiles, lift the active one
  // with a shadow.
  for (int i = 0; i < c_tile_count; i++) {
    if (i == pos || !c_tile_obj[i]) continue;
    lv_obj_set_style_transform_zoom(c_tile_obj[i], 230, 0);  // ~90%
    lv_obj_set_style_opa(c_tile_obj[i], LV_OPA_70, 0);
  }
  if (c_tile_obj[pos]) {
    lv_obj_set_style_shadow_width(c_tile_obj[pos], 16, 0);
    lv_obj_set_style_shadow_color(c_tile_obj[pos], lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa  (c_tile_obj[pos], LV_OPA_50, 0);
  }

  Serial.printf("[UI] Reorder: started at pos %d\n", pos);
}

static void exit_reorder_mode() {
  if (reorder_active_pos < 0) return;

  // Reset visuals on every tile (cheap and avoids state-leak bugs).
  for (int i = 0; i < c_tile_count; i++) {
    if (!c_tile_obj[i]) continue;
    lv_obj_set_style_transform_zoom(c_tile_obj[i], 256, 0);  // 100%
    lv_obj_set_style_opa          (c_tile_obj[i], LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width (c_tile_obj[i], 0, 0);
    lv_obj_set_style_translate_y  (c_tile_obj[i], 0, 0);
  }

  lv_obj_add_flag(c_tile_box, LV_OBJ_FLAG_SCROLLABLE);

  reorder_active_pos = -1;
  reorder_total_dy   = 0;
}

static void commit_reorder() {
  if (reorder_active_pos < 0) return;
  int from = reorder_active_pos;

  // Round drag distance to nearest slot.
  int slot_h = CODE_TILE_H + CODE_TILE_GAP;
  int slots_moved;
  if (reorder_total_dy >= 0) slots_moved = (reorder_total_dy + slot_h / 2) / slot_h;
  else                       slots_moved = (reorder_total_dy - slot_h / 2) / slot_h;

  int target = from + slots_moved;
  if (target < 0)             target = 0;
  if (target >= c_tile_count) target = c_tile_count - 1;

  if (target != from) {
    int moved = display_order[from];
    if (target < from) {
      for (int i = from; i > target; i--) display_order[i] = display_order[i - 1];
    } else {
      for (int i = from; i < target; i++) display_order[i] = display_order[i + 1];
    }
    display_order[target] = moved;
    save_all_custom_positions();
    Serial.printf("[UI] Reorder: %d -> %d (dy=%d)\n",
                  from, target, (int)reorder_total_dy);
  } else {
    Serial.printf("[UI] Reorder: no change (dy=%d)\n", (int)reorder_total_dy);
  }

  exit_reorder_mode();
  ui_refresh_codes();
}

// ---------------------------------------------------------------------------
// Custom-position helpers (NVS-backed)
// ---------------------------------------------------------------------------
static void capture_current_order_as_custom_if_empty() {
  const SCState& s = sc_state();
  bool any_set = false;
  for (int i = 0; i < s.numCredentials; i++) {
    if (nvs_meta_get_custom_pos(s.credentials[i].name)
        != NVS_META_CUSTOM_POS_UNSET) {
      any_set = true;
      break;
    }
  }
  if (!any_set) {
    for (int i = 0; i < s.numCredentials; i++) {
      int credIdx = display_order[i];
      nvs_meta_set_custom_pos(s.credentials[credIdx].name, (uint16_t)i);
    }
    Serial.printf("[UI] Captured initial custom order (%d creds)\n",
                  s.numCredentials);
  }
}

static void save_all_custom_positions() {
  const SCState& s = sc_state();
  for (int i = 0; i < s.numCredentials; i++) {
    int credIdx = display_order[i];
    nvs_meta_set_custom_pos(s.credentials[credIdx].name, (uint16_t)i);
  }
}

// ---------------------------------------------------------------------------
// PIN entry screen
// ---------------------------------------------------------------------------
static void build_pin_screen() {
  scr_pin = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(scr_pin, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_pad_all(scr_pin, 0, 0);

  // Back / cancel button (top-left). Mirrors the back affordance on every
  // other sub-screen and gives the user a way out of PIN entry without
  // going through a successful auth or pulling the card.
  lv_obj_t* back = lv_btn_create(scr_pin);
  lv_obj_set_size(back, 44, 32);
  lv_obj_align(back, LV_ALIGN_TOP_LEFT, 4, 4);
  lv_obj_set_style_bg_color(back, lv_color_hex(COL_TILE_BG), 0);
  lv_obj_set_style_radius(back, 6, 0);
  {
    lv_obj_t* l = lv_label_create(back);
    lv_label_set_text(l, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(l, lv_color_hex(COL_TXT_NORM), 0);
    lv_obj_center(l);
  }
  lv_obj_add_event_cb(back, on_back_to_main_menu, LV_EVENT_CLICKED, nullptr);

  p_lbl_prompt = lv_label_create(scr_pin);
  lv_label_set_text(p_lbl_prompt, "Enter PIN");
  lv_obj_set_style_text_color(p_lbl_prompt, lv_color_hex(COL_TXT_NORM), 0);
  lv_obj_set_style_text_font(p_lbl_prompt, &lv_font_montserrat_16, 0);
  lv_label_set_long_mode(p_lbl_prompt, LV_LABEL_LONG_SCROLL_CIRCULAR);
  // Narrowed so the label sits to the right of the back button without
  // overlapping it when the prompt grows (e.g. "Wrong PIN, N attempts left").
  lv_obj_set_width(p_lbl_prompt, LCD_W - 100);
  lv_obj_align(p_lbl_prompt, LV_ALIGN_TOP_MID, 0, 12);

  p_textarea = lv_textarea_create(scr_pin);
  lv_textarea_set_password_mode(p_textarea, true);
  lv_textarea_set_one_line(p_textarea, true);
  lv_textarea_set_max_length(p_textarea, 64);
  lv_textarea_set_placeholder_text(p_textarea, "PIN");
  lv_obj_set_width(p_textarea, LCD_W - 80);   // leave room for toggle btn
  lv_obj_align(p_textarea, LV_ALIGN_TOP_LEFT, 16, 50);
  lv_obj_set_style_text_font(p_textarea, &lv_font_montserrat_20, 0);
  lv_obj_set_style_bg_color(p_textarea, lv_color_hex(0xFFFFFF), LV_PART_CURSOR);

  // Keyboard mode toggle — top-right next to the textarea.
  // Label reflects the mode you'll switch TO: "ABC" = switch to letters,
  // "123" = switch back to numbers.
  lv_obj_t* kbtog = lv_btn_create(scr_pin);
  lv_obj_set_size(kbtog, 50, 40);
  lv_obj_align(kbtog, LV_ALIGN_TOP_RIGHT, -16, 50);
  lv_obj_set_style_bg_color(kbtog, lv_color_hex(COL_TILE_BG), 0);
  lv_obj_set_style_bg_color(kbtog, lv_color_hex(COL_TILE_HOVER), LV_STATE_PRESSED);
  lv_obj_set_style_radius(kbtog, 6, 0);
  lv_obj_set_style_border_width(kbtog, 0, 0);
  lv_obj_t* kbtl = lv_label_create(kbtog);
  lv_label_set_text(kbtl, "ABC");
  lv_obj_set_style_text_color(kbtl, lv_color_hex(COL_TXT_NORM), 0);
  lv_obj_set_style_text_font(kbtl, &lv_font_montserrat_14, 0);
  lv_obj_center(kbtl);
  // Store the label pointer as user data so the callback can update it.
  lv_obj_set_user_data(kbtog, kbtl);
  lv_obj_add_event_cb(kbtog, [](lv_event_t* e) {
    lv_obj_t* btn = lv_event_get_target(e);
    lv_obj_t* lbl = (lv_obj_t*)lv_obj_get_user_data(btn);
    if (!p_keyboard) return;
    lv_keyboard_mode_t cur = lv_keyboard_get_mode(p_keyboard);
    if (cur == LV_KEYBOARD_MODE_NUMBER) {
      lv_keyboard_set_mode(p_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
      if (lbl) lv_label_set_text(lbl, "123");
    } else {
      lv_keyboard_set_mode(p_keyboard, LV_KEYBOARD_MODE_NUMBER);
      if (lbl) lv_label_set_text(lbl, "ABC");
    }
  }, LV_EVENT_CLICKED, nullptr);

  p_keyboard = lv_keyboard_create(scr_pin);
  lv_keyboard_set_textarea(p_keyboard, p_textarea);
  lv_keyboard_set_mode(p_keyboard, LV_KEYBOARD_MODE_NUMBER);
  lv_obj_set_size(p_keyboard, LCD_W, 280);
  lv_obj_align(p_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_add_event_cb(p_keyboard, kb_event_cb, LV_EVENT_ALL, nullptr);
  lv_obj_add_event_cb(p_keyboard, kb_tap_beep_cb, LV_EVENT_VALUE_CHANGED, nullptr);
}

static void kb_event_cb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_READY) return;
  const char* pin = lv_textarea_get_text(p_textarea);
  strncpy(pin_temp, pin ? pin : "", sizeof(pin_temp) - 1);
  pin_temp[sizeof(pin_temp) - 1] = '\0';
  lv_textarea_set_text(p_textarea, "");
  if (pin_callback) {
    PinSubmitCb cb = pin_callback;
    pin_callback = nullptr;
    cb(pin_temp);
  }
  memset(pin_temp, 0, sizeof(pin_temp));
}

void ui_show_pin_entry(const char* prompt, PinSubmitCb cb) {
  pin_callback = cb;
  if (p_lbl_prompt) lv_label_set_text(p_lbl_prompt, prompt ? prompt : "Enter PIN");
  if (p_textarea)   lv_textarea_set_text(p_textarea, "");
  // Reset to numeric keyboard each time (most PINs are numeric).
  if (p_keyboard)   lv_keyboard_set_mode(p_keyboard, LV_KEYBOARD_MODE_NUMBER);
  if (lv_scr_act() != scr_pin) lv_scr_load(scr_pin);
}

// ---------------------------------------------------------------------------
// Stub screen (for "coming soon" tiles)
// ---------------------------------------------------------------------------
static void build_stub_screen() {
  scr_stub = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(scr_stub, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_pad_all(scr_stub, 0, 0);
  lv_obj_clear_flag(scr_stub, LV_OBJ_FLAG_SCROLLABLE);

  make_top_bar(scr_stub, "", on_back_to_totp_menu);

  st_lbl_title = lv_label_create(scr_stub);
  lv_label_set_text(st_lbl_title, "");
  lv_obj_set_style_text_color(st_lbl_title, lv_color_hex(COL_TXT_NORM), 0);
  lv_obj_set_style_text_font(st_lbl_title, &lv_font_montserrat_20, 0);
  lv_obj_align(st_lbl_title, LV_ALIGN_CENTER, 0, -20);

  st_lbl_body = lv_label_create(scr_stub);
  lv_label_set_text(st_lbl_body, "");
  lv_obj_set_style_text_color(st_lbl_body, lv_color_hex(COL_TXT_DIM), 0);
  lv_obj_set_style_text_font(st_lbl_body, &lv_font_montserrat_14, 0);
  lv_obj_align(st_lbl_body, LV_ALIGN_CENTER, 0, 20);
}

void ui_show_stub(const char* title, const char* msg) {
  if (st_lbl_title) lv_label_set_text(st_lbl_title, title ? title : "");
  if (st_lbl_body)  lv_label_set_text(st_lbl_body,  msg   ? msg   : "");
  if (lv_scr_act() != scr_stub) lv_scr_load(scr_stub);
}

// ---------------------------------------------------------------------------
// Settings screen
//
// Three sliders + a date/time button. Semantics:
//   Sleep timeout:   total idle seconds before deep sleep  (10..120)
//   Dim duration:    seconds of dimmed screen before sleep  (5..sleep_s)
//                    Dim fires at (sleep - dim) idle.
//   Brightness:      active backlight percent              (10..100)
//
// Each slider has a value label on the right. Tapping the label
// opens a numeric keypad modal for precise entry.
// Back button returns to Main Menu.
// ---------------------------------------------------------------------------

// Generic keyboard tap beep — attached to every keyboard's
// LV_EVENT_VALUE_CHANGED so the user gets per-key audio feedback.
// Gated on AUDIO_EVT_KEYTAP. Brief, high-frequency click.
static void kb_tap_beep_cb(lv_event_t* e) {
  if (millis() < 2000) return;
  if (nvs_settings_get_audio_events() & AUDIO_EVT_KEYTAP) {
    audio_click(8);   // light click
  }
}

// Tile/button click beep — same toggle as keyboard taps; slightly
// heavier click so menu navigation feels distinct from typing.
// 2-second boot grace period so phantom touch events during display
// init / LVGL warm-up don't produce a buzzer.
static void tile_click_beep_cb(lv_event_t* e) {
  if (millis() < 2000) return;
  if (nvs_settings_get_audio_events() & AUDIO_EVT_KEYTAP) {
    audio_click(12);   // medium click
  }
}

// Numeric keypad modal -------------------------------------------------------
//
// A transient full-screen overlay parented to lv_layer_top() with a
// numeric textarea + keyboard. On OK, a callback is invoked with the
// parsed value. On Cancel, the callback is not called.

static lv_obj_t*   nk_modal     = nullptr;
static lv_obj_t*   nk_textarea  = nullptr;
static lv_obj_t*   nk_title_lbl = nullptr;
static void (*nk_cb)(int) = nullptr;
static int         nk_min       = 0;
static int         nk_max       = 100;
static int         nk_initial   = 0;     // fallback if user submits empty

static void nk_close() {
  if (nk_modal) {
    lv_obj_del(nk_modal);
    nk_modal = nullptr;
    nk_textarea = nullptr;
    nk_title_lbl = nullptr;
  }
  nk_cb = nullptr;
}

static void nk_on_ok(lv_event_t* e) {
  if (!nk_textarea) return;
  const char* txt = lv_textarea_get_text(nk_textarea);
  // If user didn't type anything, keep the original value.
  int v = (txt && *txt) ? atoi(txt) : nk_initial;
  if (v < nk_min) v = nk_min;
  if (v > nk_max) v = nk_max;
  void (*cb)(int) = nk_cb;
  nk_close();
  if (cb) cb(v);
}

static void nk_on_cancel(lv_event_t* e) {
  nk_close();
}

// Open the keypad. title is a label shown at the top; initial is the
// current/default value (shown as placeholder so the user knows what
// they'll get if they submit without typing); min/max are inclusive
// bounds for validation.
static void num_keypad_open(const char* title, int initial, int minV, int maxV,
                            void (*cb)(int)) {
  if (nk_modal) nk_close();   // already open; close old

  nk_cb      = cb;
  nk_min     = minV;
  nk_max     = maxV;
  nk_initial = initial;

  // Full-screen dim overlay on the top layer
  nk_modal = lv_obj_create(lv_layer_top());
  lv_obj_set_size(nk_modal, LCD_W, LCD_H);
  lv_obj_set_pos(nk_modal, 0, 0);
  lv_obj_set_style_bg_color(nk_modal, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_bg_opa(nk_modal, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(nk_modal, 0, 0);
  lv_obj_set_style_pad_all(nk_modal, 0, 0);
  lv_obj_clear_flag(nk_modal, LV_OBJ_FLAG_SCROLLABLE);

  // Title
  nk_title_lbl = lv_label_create(nk_modal);
  lv_label_set_text(nk_title_lbl, title ? title : "Enter value");
  lv_obj_set_style_text_color(nk_title_lbl, lv_color_hex(COL_TXT_NORM), 0);
  lv_obj_set_style_text_font(nk_title_lbl, &lv_font_montserrat_16, 0);
  lv_obj_align(nk_title_lbl, LV_ALIGN_TOP_MID, 0, 12);

  // Range + current-value hint
  char hint[48];
  snprintf(hint, sizeof(hint), "%d .. %d  (current: %d)", minV, maxV, initial);
  lv_obj_t* rng = lv_label_create(nk_modal);
  lv_label_set_text(rng, hint);
  lv_obj_set_style_text_color(rng, lv_color_hex(COL_TXT_DIM), 0);
  lv_obj_set_style_text_font(rng, &lv_font_montserrat_12, 0);
  lv_obj_align(rng, LV_ALIGN_TOP_MID, 0, 32);

  // Empty textarea with current value as placeholder — first keypress
  // goes into a clean field; OK without typing keeps the current value.
  nk_textarea = lv_textarea_create(nk_modal);
  lv_textarea_set_one_line(nk_textarea, true);
  lv_textarea_set_max_length(nk_textarea, 4);
  lv_textarea_set_text(nk_textarea, "");
  {
    char ibuf[8]; snprintf(ibuf, sizeof(ibuf), "%d", initial);
    lv_textarea_set_placeholder_text(nk_textarea, ibuf);
  }
  lv_obj_set_width(nk_textarea, 160);
  lv_obj_align(nk_textarea, LV_ALIGN_TOP_MID, 0, 54);
  lv_obj_set_style_text_align(nk_textarea, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(nk_textarea, &lv_font_montserrat_20, 0);

  // OK / Cancel buttons
  lv_obj_t* ok_btn = lv_btn_create(nk_modal);
  lv_obj_set_size(ok_btn, 90, 36);
  lv_obj_align(ok_btn, LV_ALIGN_TOP_LEFT, 40, 108);
  lv_obj_set_style_bg_color(ok_btn, lv_color_hex(COL_GREEN), 0);
  lv_obj_set_style_radius(ok_btn, 8, 0);
  lv_obj_t* okl = lv_label_create(ok_btn);
  lv_label_set_text(okl, LV_SYMBOL_OK " OK");
  lv_obj_set_style_text_color(okl, lv_color_hex(0x000000), 0);
  lv_obj_center(okl);
  lv_obj_add_event_cb(ok_btn, nk_on_ok, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* ca_btn = lv_btn_create(nk_modal);
  lv_obj_set_size(ca_btn, 90, 36);
  lv_obj_align(ca_btn, LV_ALIGN_TOP_RIGHT, -40, 108);
  lv_obj_set_style_bg_color(ca_btn, lv_color_hex(COL_TILE_BG), 0);
  lv_obj_set_style_radius(ca_btn, 8, 0);
  lv_obj_t* cal = lv_label_create(ca_btn);
  lv_label_set_text(cal, LV_SYMBOL_CLOSE " Cancel");
  lv_obj_set_style_text_color(cal, lv_color_hex(COL_TXT_NORM), 0);
  lv_obj_center(cal);
  lv_obj_add_event_cb(ca_btn, nk_on_cancel, LV_EVENT_CLICKED, nullptr);

  // Numeric keyboard
  lv_obj_t* kb = lv_keyboard_create(nk_modal);
  lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
  lv_keyboard_set_textarea(kb, nk_textarea);
  lv_obj_set_size(kb, LCD_W, LCD_H - 160);
  lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  // Pressing the keyboard's checkmark also confirms
  lv_obj_add_event_cb(kb, nk_on_ok, LV_EVENT_READY, nullptr);
  lv_obj_add_event_cb(kb, nk_on_cancel, LV_EVENT_CANCEL, nullptr);
  lv_obj_add_event_cb(kb, kb_tap_beep_cb, LV_EVENT_VALUE_CHANGED, nullptr);
}

// Settings slider callbacks --------------------------------------------------

static void on_dim_slider(lv_event_t* e) {
  uint16_t v = (uint16_t)lv_slider_get_value(s_dim_slider);
  // NVS setter will enforce dim <= sleep - 5; re-read to reflect clamp.
  nvs_settings_set_dim_s(v);
  uint16_t actual = nvs_settings_get_dim_s();
  if (actual != v) lv_slider_set_value(s_dim_slider, actual, LV_ANIM_OFF);
  char buf[16]; snprintf(buf, sizeof(buf), "%us", (unsigned)actual);
  lv_label_set_text(s_dim_val, buf);
}

static void on_sleep_slider(lv_event_t* e) {
  uint16_t v = (uint16_t)lv_slider_get_value(s_sleep_slider);
  char buf[16]; snprintf(buf, sizeof(buf), "%us", (unsigned)v);
  lv_label_set_text(s_sleep_val, buf);
  nvs_settings_set_sleep_s(v);
  // Sleep changed → dim max changes too. Update dim slider's range
  // and value to keep the two axes consistent.
  uint16_t new_dim_max = (v > 10) ? (uint16_t)(v - 5) : 6;
  if (new_dim_max < 6) new_dim_max = 6;   // guard against degenerate slider range
  lv_slider_set_range(s_dim_slider, 5, new_dim_max);
  uint16_t d = nvs_settings_get_dim_s();
  lv_slider_set_value(s_dim_slider, d, LV_ANIM_OFF);
  char buf2[16]; snprintf(buf2, sizeof(buf2), "%us", (unsigned)d);
  lv_label_set_text(s_dim_val, buf2);
}

static void on_bri_slider(lv_event_t* e) {
  uint8_t v = (uint8_t)lv_slider_get_value(s_bri_slider);
  char buf[16]; snprintf(buf, sizeof(buf), "%u%%", (unsigned)v);
  lv_label_set_text(s_bri_val, buf);
  nvs_settings_set_brightness_pct(v);
  backlight_set_pct_live(v);
}

// Value-label tap handlers (open keypad) -------------------------------------

static void on_dim_val_tap(lv_event_t* e) {
  uint16_t sleep_v = nvs_settings_get_sleep_s();
  int hi = (sleep_v > 10) ? (int)(sleep_v - 5) : 5;
  num_keypad_open("Dim duration (s)",
                  (int)nvs_settings_get_dim_s(),
                  5, hi,
                  [](int v) {
                    nvs_settings_set_dim_s((uint16_t)v);
                    uint16_t actual = nvs_settings_get_dim_s();
                    if (s_dim_slider) lv_slider_set_value(s_dim_slider, actual, LV_ANIM_OFF);
                    char buf[16]; snprintf(buf, sizeof(buf), "%us", (unsigned)actual);
                    if (s_dim_val) lv_label_set_text(s_dim_val, buf);
                  });
}

static void on_sleep_val_tap(lv_event_t* e) {
  num_keypad_open("Sleep timeout (s)",
                  (int)nvs_settings_get_sleep_s(),
                  15, 120,
                  [](int v) {
                    nvs_settings_set_sleep_s((uint16_t)v);
                    uint16_t actual = nvs_settings_get_sleep_s();
                    if (s_sleep_slider) lv_slider_set_value(s_sleep_slider, actual, LV_ANIM_OFF);
                    char buf[16]; snprintf(buf, sizeof(buf), "%us", (unsigned)actual);
                    if (s_sleep_val) lv_label_set_text(s_sleep_val, buf);
                    // Rescale dim slider range and re-sync value/label.
                    uint16_t new_dim_max = (actual > 10) ? (uint16_t)(actual - 5) : 6;
                    if (new_dim_max < 6) new_dim_max = 6;   // guard against degenerate slider range
                    if (s_dim_slider) lv_slider_set_range(s_dim_slider, 5, new_dim_max);
                    uint16_t d = nvs_settings_get_dim_s();
                    if (s_dim_slider) lv_slider_set_value(s_dim_slider, d, LV_ANIM_OFF);
                    char buf2[16]; snprintf(buf2, sizeof(buf2), "%us", (unsigned)d);
                    if (s_dim_val) lv_label_set_text(s_dim_val, buf2);
                  });
}

static void on_bri_val_tap(lv_event_t* e) {
  num_keypad_open("Brightness (%)",
                  (int)nvs_settings_get_brightness_pct(),
                  10, 100,
                  [](int v) {
                    nvs_settings_set_brightness_pct((uint8_t)v);
                    uint8_t actual = nvs_settings_get_brightness_pct();
                    if (s_bri_slider) lv_slider_set_value(s_bri_slider, actual, LV_ANIM_OFF);
                    char buf[16]; snprintf(buf, sizeof(buf), "%u%%", (unsigned)actual);
                    if (s_bri_val) lv_label_set_text(s_bri_val, buf);
                    backlight_set_pct_live(actual);
                  });
}

// Build one labelled slider row with a tappable value readout.
static lv_obj_t* make_slider_row(lv_obj_t* parent, const char* label,
                                 int min, int max, int init,
                                 int y, lv_obj_t** out_val_lbl,
                                 lv_event_cb_t val_tap_cb) {
  lv_obj_t* l = lv_label_create(parent);
  lv_label_set_text(l, label);
  lv_obj_set_style_text_color(l, lv_color_hex(COL_TXT_NORM), 0);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
  lv_obj_align(l, LV_ALIGN_TOP_LEFT, 16, y + 4);

  // Value label wrapped in a clickable button for easy tap target.
  lv_obj_t* val_btn = lv_btn_create(parent);
  lv_obj_set_size(val_btn, 60, 24);
  lv_obj_align(val_btn, LV_ALIGN_TOP_RIGHT, -14, y);
  lv_obj_set_style_bg_color(val_btn, lv_color_hex(COL_TILE_BG), 0);
  lv_obj_set_style_bg_color(val_btn, lv_color_hex(COL_TILE_HOVER), LV_STATE_PRESSED);
  lv_obj_set_style_radius(val_btn, 6, 0);
  lv_obj_set_style_border_width(val_btn, 0, 0);
  lv_obj_set_style_pad_all(val_btn, 0, 0);

  lv_obj_t* val = lv_label_create(val_btn);
  lv_obj_set_style_text_color(val, lv_color_hex(COL_TXT_NORM), 0);
  lv_obj_set_style_text_font(val, &lv_font_montserrat_14, 0);
  lv_obj_center(val);
  if (val_tap_cb) {
    lv_obj_add_event_cb(val_btn, val_tap_cb, LV_EVENT_CLICKED, nullptr);
  }

  // Slider sits well below the label/button row to avoid visual overlap.
  lv_obj_t* sl = lv_slider_create(parent);
  lv_obj_set_size(sl, LCD_W - 48, 14);
  lv_obj_align(sl, LV_ALIGN_TOP_MID, 0, y + 36);
  lv_slider_set_range(sl, min, max);
  lv_slider_set_value(sl, init, LV_ANIM_OFF);

  *out_val_lbl = val;
  return sl;
}

static void build_display_screen() {
  scr_display = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(scr_display, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_pad_all(scr_display, 0, 0);
  lv_obj_clear_flag(scr_display, LV_OBJ_FLAG_SCROLLABLE);

  make_top_bar(scr_display, "Display", on_back_to_settings);

  const int COL_Y0 = TOPBAR_H + 14;
  const int ROW_H  = 72;

  // Sleep timeout (first: it bounds the dim duration)
  s_sleep_slider = make_slider_row(scr_display, "Sleep timeout",
                                   15, 120,
                                   nvs_settings_get_sleep_s(),
                                   COL_Y0, &s_sleep_val,
                                   on_sleep_val_tap);
  {
    char buf[16]; snprintf(buf, sizeof(buf), "%us", (unsigned)nvs_settings_get_sleep_s());
    lv_label_set_text(s_sleep_val, buf);
  }
  lv_obj_add_event_cb(s_sleep_slider, on_sleep_slider, LV_EVENT_VALUE_CHANGED, nullptr);

  // Dim duration — range is [5, sleep_s - 5], dynamic
  uint16_t sleep_cur = nvs_settings_get_sleep_s();
  uint16_t dim_max   = (sleep_cur > 10) ? (uint16_t)(sleep_cur - 5) : 6;
  s_dim_slider = make_slider_row(scr_display, "Dim duration",
                                 5, dim_max,
                                 nvs_settings_get_dim_s(),
                                 COL_Y0 + ROW_H, &s_dim_val,
                                 on_dim_val_tap);
  {
    char buf[16]; snprintf(buf, sizeof(buf), "%us", (unsigned)nvs_settings_get_dim_s());
    lv_label_set_text(s_dim_val, buf);
  }
  lv_obj_add_event_cb(s_dim_slider, on_dim_slider, LV_EVENT_VALUE_CHANGED, nullptr);

  // Brightness
  s_bri_slider = make_slider_row(scr_display, "Brightness",
                                 10, 100,
                                 nvs_settings_get_brightness_pct(),
                                 COL_Y0 + 2 * ROW_H, &s_bri_val,
                                 on_bri_val_tap);
  {
    char buf[16]; snprintf(buf, sizeof(buf), "%u%%", (unsigned)nvs_settings_get_brightness_pct());
    lv_label_set_text(s_bri_val, buf);
  }
  lv_obj_add_event_cb(s_bri_slider, on_bri_slider, LV_EVENT_VALUE_CHANGED, nullptr);
}

void ui_show_display() {
  // Re-sync sliders from NVS on every show
  uint16_t sleep_cur = nvs_settings_get_sleep_s();
  uint16_t dim_max   = (sleep_cur > 10) ? (uint16_t)(sleep_cur - 5) : 6;
  if (s_sleep_slider) lv_slider_set_value(s_sleep_slider, sleep_cur, LV_ANIM_OFF);
  if (s_dim_slider)   lv_slider_set_range(s_dim_slider, 5, dim_max);
  if (s_dim_slider)   lv_slider_set_value(s_dim_slider, nvs_settings_get_dim_s(), LV_ANIM_OFF);
  if (s_bri_slider)   lv_slider_set_value(s_bri_slider, nvs_settings_get_brightness_pct(), LV_ANIM_OFF);
  {
    char buf[16];
    snprintf(buf, sizeof(buf), "%us", (unsigned)nvs_settings_get_dim_s());
    if (s_dim_val) lv_label_set_text(s_dim_val, buf);
    snprintf(buf, sizeof(buf), "%us", (unsigned)sleep_cur);
    if (s_sleep_val) lv_label_set_text(s_sleep_val, buf);
    snprintf(buf, sizeof(buf), "%u%%", (unsigned)nvs_settings_get_brightness_pct());
    if (s_bri_val) lv_label_set_text(s_bri_val, buf);
  }
  if (lv_scr_act() != scr_display) lv_scr_load(scr_display);
}

// ---------------------------------------------------------------------------
// Settings hub — 4-tile menu for submenus.
// Display, Date/Time, Hardware, AID Management. Each tile opens the
// corresponding submenu screen; back from submenu returns here; back
// from here returns to Main Menu.
// ---------------------------------------------------------------------------

// Helper: one full-width menu tile with icon glyph + label. Parent
// should be a vertical-flex container (or we align manually).
static lv_obj_t* make_settings_tile(lv_obj_t* parent, const char* icon,
                                    const char* label, lv_event_cb_t cb) {
  lv_obj_t* t = lv_btn_create(parent);
  lv_obj_set_size(t, LCD_W - 32, 56);
  lv_obj_set_style_bg_color(t, lv_color_hex(COL_TILE_BG), 0);
  lv_obj_set_style_bg_color(t, lv_color_hex(COL_TILE_HOVER), LV_STATE_PRESSED);
  lv_obj_set_style_radius(t, 10, 0);
  lv_obj_set_style_border_width(t, 0, 0);
  lv_obj_set_style_pad_all(t, 0, 0);

  // Icon on the left
  lv_obj_t* ic = lv_label_create(t);
  lv_label_set_text(ic, icon ? icon : "");
  lv_obj_set_style_text_color(ic, lv_color_hex(COL_TXT_NORM), 0);
  lv_obj_set_style_text_font(ic, &lv_font_montserrat_20, 0);
  lv_obj_align(ic, LV_ALIGN_LEFT_MID, 18, 0);

  // Label centered
  lv_obj_t* l = lv_label_create(t);
  lv_label_set_text(l, label ? label : "");
  lv_obj_set_style_text_color(l, lv_color_hex(COL_TXT_NORM), 0);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
  lv_obj_align(l, LV_ALIGN_LEFT_MID, 52, 0);

  // Chevron on the right
  lv_obj_t* ch = lv_label_create(t);
  lv_label_set_text(ch, LV_SYMBOL_RIGHT);
  lv_obj_set_style_text_color(ch, lv_color_hex(COL_TXT_FAINT), 0);
  lv_obj_set_style_text_font(ch, &lv_font_montserrat_16, 0);
  lv_obj_align(ch, LV_ALIGN_RIGHT_MID, -16, 0);

  if (cb) lv_obj_add_event_cb(t, cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_event_cb(t, tile_click_beep_cb, LV_EVENT_CLICKED, nullptr);
  return t;
}

static void build_settings_screen() {
  scr_settings = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(scr_settings, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_pad_all(scr_settings, 0, 0);
  lv_obj_clear_flag(scr_settings, LV_OBJ_FLAG_SCROLLABLE);

  make_top_bar(scr_settings, "Settings", on_back_to_main_menu);

  // Vertical stack of tiles
  lv_obj_t* stack = lv_obj_create(scr_settings);
  lv_obj_set_size(stack, LCD_W, LCD_H - TOPBAR_H);
  lv_obj_align(stack, LV_ALIGN_TOP_MID, 0, TOPBAR_H);
  lv_obj_set_style_bg_opa(stack, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(stack, 0, 0);
  lv_obj_set_style_pad_all(stack, 16, 0);
  lv_obj_set_flex_flow(stack, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(stack, 12, 0);
  lv_obj_set_flex_align(stack, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  make_settings_tile(stack, LV_SYMBOL_EYE_OPEN, "Display",
                     [](lv_event_t*) { ui_show_display(); });
  make_settings_tile(stack, LV_SYMBOL_BELL, "Date / Time",
                     [](lv_event_t*) { ui_show_time_setter(); });
  make_settings_tile(stack, LV_SYMBOL_AUDIO, "Audio",
                     [](lv_event_t*) { ui_show_audio(); });
  make_settings_tile(stack, LV_SYMBOL_SETTINGS, "Hardware",
                     [](lv_event_t*) { ui_show_hardware(); });
  make_settings_tile(stack, LV_SYMBOL_LIST, "AID Management",
                     [](lv_event_t*) { ui_show_aid_manager(); });
}

void ui_show_settings() {
  if (lv_scr_act() != scr_settings) lv_scr_load(scr_settings);
}

// ---------------------------------------------------------------------------
// Hardware submenu — stub, filled in by Phase 3b+.
// ---------------------------------------------------------------------------
// Hardware screen — live IMU + system info viewer. Refreshed by
// ui_hardware_tick() called from the main loop's poll cycle.
static lv_obj_t* hw_status_lbl  = nullptr;
static lv_obj_t* hw_acc_lbl     = nullptr;
static lv_obj_t* hw_gyro_lbl    = nullptr;
static lv_obj_t* hw_temp_lbl    = nullptr;
static lv_obj_t* hw_batt_lbl    = nullptr;
static lv_obj_t* hw_usb_lbl     = nullptr;

static void build_hardware_screen() {
  scr_hardware = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(scr_hardware, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_pad_all(scr_hardware, 0, 0);
  lv_obj_clear_flag(scr_hardware, LV_OBJ_FLAG_SCROLLABLE);

  make_top_bar(scr_hardware, "Hardware", on_back_to_settings);

  // Body container with vertical layout.
  lv_obj_t* body = lv_obj_create(scr_hardware);
  lv_obj_set_size(body, LCD_W, LCD_H - TOPBAR_H);
  lv_obj_align(body, LV_ALIGN_TOP_MID, 0, TOPBAR_H);
  lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(body, 0, 0);
  lv_obj_set_style_pad_all(body, 16, 0);
  lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(body, 8, 0);
  lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

  // Helper: builds a "Heading" + "monospace value line(s)" block.
  auto add_section = [&](const char* heading, lv_obj_t** out_val) {
    lv_obj_t* h = lv_label_create(body);
    lv_label_set_text(h, heading);
    lv_obj_set_style_text_color(h, lv_color_hex(COL_TXT_DIM), 0);
    lv_obj_set_style_text_font(h, &lv_font_montserrat_12, 0);

    lv_obj_t* v = lv_label_create(body);
    lv_obj_set_style_text_color(v, lv_color_hex(COL_TXT_NORM), 0);
    lv_obj_set_style_text_font(v, &lv_font_montserrat_16, 0);
    lv_label_set_text(v, "—");
    *out_val = v;
  };

  add_section("QMI8658 IMU",   &hw_status_lbl);
  add_section("Accelerometer (g)", &hw_acc_lbl);
  add_section("Gyroscope (dps)",   &hw_gyro_lbl);
  add_section("Temperature",       &hw_temp_lbl);
  add_section("Battery",           &hw_batt_lbl);
  add_section("USB",               &hw_usb_lbl);
}

void ui_show_hardware() {
  if (lv_scr_act() != scr_hardware) lv_scr_load(scr_hardware);
  // Power up the IMU only while this screen is being viewed.
  accel_set_enabled(true);
  // Set the static-once label.
  if (hw_status_lbl) {
    if (accel_is_ready()) lv_label_set_text(hw_status_lbl, "Connected");
    else                  lv_label_set_text(hw_status_lbl, "NOT FOUND");
  }
}

// Called from the main loop (rate-limited internally) to refresh
// the live values when the Hardware screen is active. Also handles
// powering the IMU down when the screen is no longer active, so we
// don't burn ~4 mA on the gyro 24/7.
void ui_hardware_tick() {
  if (lv_scr_act() != scr_hardware) {
    if (accel_is_enabled()) accel_set_enabled(false);
    return;
  }

  static uint32_t lastMs = 0;
  uint32_t now = millis();
  if (now - lastMs < 100) return;   // 10 Hz update
  lastMs = now;

  if (accel_is_ready()) {
    float ax, ay, az, gx, gy, gz;
    if (accel_read_scaled(&ax, &ay, &az, &gx, &gy, &gz)) {
      char buf[80];
      snprintf(buf, sizeof(buf), "X %+6.2f  Y %+6.2f  Z %+6.2f", ax, ay, az);
      if (hw_acc_lbl)  lv_label_set_text(hw_acc_lbl, buf);
      snprintf(buf, sizeof(buf), "X %+6.1f  Y %+6.1f  Z %+6.1f", gx, gy, gz);
      if (hw_gyro_lbl) lv_label_set_text(hw_gyro_lbl, buf);
    }
    char tbuf[24];
    snprintf(tbuf, sizeof(tbuf), "%.1f \xC2\xB0""C", accel_read_temp_c());
    if (hw_temp_lbl) lv_label_set_text(hw_temp_lbl, tbuf);
  }

  // Battery info — pulled via the existing ui_get_* wrappers
  // declared in ui.h (which the sketch implements).
  if (hw_batt_lbl) {
    char buf[64];
    int pct  = ui_get_battery_pct();
    int mv   = ui_get_battery_mv();
    bool usb = ui_get_usb_status();
    bool chg = ui_get_charging();
    if (pct >= 0) {
      snprintf(buf, sizeof(buf), "%d%%  %d mV  %s%s",
               pct, mv, usb ? "USB " : "", chg ? "Charging" : "");
    } else {
      snprintf(buf, sizeof(buf), "No battery");
    }
    lv_label_set_text(hw_batt_lbl, buf);
  }

  // USB CCID host-side enumeration state.
  if (hw_usb_lbl) {
    bool vbus    = ui_get_usb_status();
    bool mounted = usb_ccid_is_mounted();
    const char* s;
    if      (mounted)  s = "Host connected (CCID enumerated)";
    else if (vbus)     s = "VBUS present, not enumerated";
    else               s = "Disconnected";
    lv_label_set_text(hw_usb_lbl, s);
  }
}

// ---------------------------------------------------------------------------
// USB Reader Mode screen — Phase 6.
//
// Reachable from the Main Menu's "USB Reader" tile. Sets the reader-
// mode flag (which the Phase 6c CCID handlers will check to decide
// whether to expose the smartcard to the host). The flag also tells
// checkSleep() to skip deep sleep so an active host session isn't
// dropped.
// ---------------------------------------------------------------------------
static lv_obj_t* ur_status_lbl = nullptr;   // host link state
static lv_obj_t* ur_card_lbl   = nullptr;   // card insertion state
static lv_obj_t* ur_debug_lbl  = nullptr;   // CCID activity debug
static lv_obj_t* ur_hint_lbl   = nullptr;

static void on_back_from_usb_reader(lv_event_t* e) {
  ui_set_usb_reader_active(false);
  ui_show_main_menu();
}

static void build_usb_reader_screen() {
  scr_usb_reader = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(scr_usb_reader, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_pad_all(scr_usb_reader, 0, 0);
  lv_obj_clear_flag(scr_usb_reader, LV_OBJ_FLAG_SCROLLABLE);

  make_top_bar(scr_usb_reader, "USB Reader", on_back_from_usb_reader);

  lv_obj_t* body = lv_obj_create(scr_usb_reader);
  lv_obj_set_size(body, LCD_W, LCD_H - TOPBAR_H);
  lv_obj_align(body, LV_ALIGN_TOP_MID, 0, TOPBAR_H);
  lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(body, 0, 0);
  lv_obj_set_style_pad_all(body, 16, 0);
  lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(body, 14, 0);
  lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

  auto add_section = [&](const char* heading, lv_obj_t** out_val) {
    lv_obj_t* h = lv_label_create(body);
    lv_label_set_text(h, heading);
    lv_obj_set_style_text_color(h, lv_color_hex(COL_TXT_DIM), 0);
    lv_obj_set_style_text_font(h, &lv_font_montserrat_12, 0);

    lv_obj_t* v = lv_label_create(body);
    lv_obj_set_style_text_color(v, lv_color_hex(COL_TXT_NORM), 0);
    lv_obj_set_style_text_font(v, &lv_font_montserrat_16, 0);
    lv_label_set_text(v, "-");
    *out_val = v;
  };

  add_section("Host", &ur_status_lbl);
  add_section("Smart Card", &ur_card_lbl);

  // CCID activity debug — shows live counters of host commands so we
  // can verify the host is talking to us and isolate where APDU
  // exchange breaks down. Small fixed-width font for dense data.
  ur_debug_lbl = lv_label_create(body);
  lv_obj_set_style_text_color(ur_debug_lbl, lv_color_hex(COL_TXT_DIM), 0);
  lv_obj_set_style_text_font(ur_debug_lbl, &lv_font_montserrat_12, 0);
  lv_label_set_long_mode(ur_debug_lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(ur_debug_lbl, LCD_W - 32);
  lv_label_set_text(ur_debug_lbl, "(no host activity)");

  ur_hint_lbl = lv_label_create(body);
  lv_label_set_long_mode(ur_hint_lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(ur_hint_lbl, LCD_W - 32);
  lv_obj_set_style_text_color(ur_hint_lbl, lv_color_hex(COL_TXT_DIM), 0);
  lv_obj_set_style_text_font(ur_hint_lbl, &lv_font_montserrat_12, 0);
  lv_label_set_text(ur_hint_lbl,
    "Reader mode is active. The host computer can now access the "
    "inserted smartcard. Press K2 or the back arrow to exit and "
    "block host access. The screen will dim but the device will not "
    "sleep while in this mode.");
}

void ui_show_usb_reader() {
  if (lv_scr_act() != scr_usb_reader) lv_scr_load(scr_usb_reader);
  ui_set_usb_reader_active(true);
}

// Refresh live status while the reader screen is showing. Called from
// the main loop alongside ui_hardware_tick.
void ui_usb_reader_tick() {
  if (lv_scr_act() != scr_usb_reader) return;

  static uint32_t lastMs = 0;
  uint32_t now = millis();
  if (now - lastMs < 250) return;   // 4 Hz update is plenty
  lastMs = now;

  if (ur_status_lbl) {
    bool vbus    = ui_get_usb_status();
    bool mounted = usb_ccid_is_mounted();
    const char* s;
    if      (mounted)  s = "Host connected";
    else if (vbus)     s = "USB cable present, host not seen";
    else               s = "Not connected";
    lv_label_set_text(ur_status_lbl, s);
  }
  if (ur_card_lbl) {
    const SCState& s = sc_state();
    if (s.cardPresent) {
      lv_label_set_text(ur_card_lbl, "Inserted (host can access)");
    } else {
      lv_label_set_text(ur_card_lbl, "Not inserted");
    }
  }
  if (ur_debug_lbl) {
    UsbCcidLiveStats st;
    usb_ccid_get_stats(&st);
    if (st.cmd_total == 0) {
      lv_label_set_text(ur_debug_lbl, "(no host activity)");
    } else {
      char buf[256];
      snprintf(buf, sizeof(buf),
        "Cmds: total=%lu  status=%lu  on=%lu  off=%lu  xfr=%lu  oth=%lu\n"
        "Last: type=0x%02X st=0x%02X err=0x%02X\n"
        "Gates: rdr=%d card=%d  ATR=%d  APDU=%d",
        (unsigned long)st.cmd_total,
        (unsigned long)st.cmd_get_status,
        (unsigned long)st.cmd_power_on,
        (unsigned long)st.cmd_power_off,
        (unsigned long)st.cmd_xfr_block,
        (unsigned long)st.cmd_other,
        st.last_cmd_type, st.last_status, st.last_error,
        (int)st.last_reader_active, (int)st.last_card_present,
        st.last_atr_len, st.last_apdu_len);
      lv_label_set_text(ur_debug_lbl, buf);
    }
  }
}

// ---------------------------------------------------------------------------
// Audio settings screen — master enable, volume, per-event toggles,
// test buttons.
// ---------------------------------------------------------------------------
static lv_obj_t* scr_audio        = nullptr;
static lv_obj_t* a_master_sw      = nullptr;
static lv_obj_t* a_vol_slider     = nullptr;
static lv_obj_t* a_vol_val        = nullptr;
static lv_obj_t* a_evt_card_sw    = nullptr;
static lv_obj_t* a_evt_pin_sw     = nullptr;
static lv_obj_t* a_evt_tick_sw    = nullptr;
static lv_obj_t* a_evt_keytap_sw  = nullptr;

static bool a_syncing = false;   // suppresses click beeps during ui_show_audio

static void on_back_to_audio_parent(lv_event_t* e) { ui_show_settings(); }

static void on_audio_master_changed(lv_event_t* e) {
  bool on = lv_obj_has_state(a_master_sw, LV_STATE_CHECKED);
  nvs_settings_set_audio_enabled(on);
  if (!a_syncing && (nvs_settings_get_audio_events() & AUDIO_EVT_KEYTAP)) {
    audio_click(12);
  }
}

static void on_audio_vol_changed(lv_event_t* e) {
  uint8_t v = (uint8_t)lv_slider_get_value(a_vol_slider);
  char buf[8]; snprintf(buf, sizeof(buf), "%u%%", (unsigned)v);
  lv_label_set_text(a_vol_val, buf);
  nvs_settings_set_audio_volume(v);
  audio_set_volume(v);
}

static void on_audio_evt_changed(lv_event_t* e) {
  uint8_t mask = 0;
  if (lv_obj_has_state(a_evt_card_sw,   LV_STATE_CHECKED)) mask |= AUDIO_EVT_CARD;
  if (lv_obj_has_state(a_evt_pin_sw,    LV_STATE_CHECKED)) mask |= AUDIO_EVT_PIN;
  if (lv_obj_has_state(a_evt_tick_sw,   LV_STATE_CHECKED)) mask |= AUDIO_EVT_TICK;
  if (lv_obj_has_state(a_evt_keytap_sw, LV_STATE_CHECKED)) mask |= AUDIO_EVT_KEYTAP;
  nvs_settings_set_audio_events(mask);
  if (!a_syncing && (mask & AUDIO_EVT_KEYTAP)) {
    audio_click(12);
  }
}

static void on_audio_vol_tap(lv_event_t* e) {
  num_keypad_open("Volume (%)",
                  (int)nvs_settings_get_audio_volume(),
                  0, 100,
                  [](int v) {
                    if (v < 0)   v = 0;
                    if (v > 100) v = 100;
                    nvs_settings_set_audio_volume((uint8_t)v);
                    if (a_vol_slider) lv_slider_set_value(a_vol_slider, v, LV_ANIM_OFF);
                    char buf[8]; snprintf(buf, sizeof(buf), "%u%%", (unsigned)v);
                    if (a_vol_val) lv_label_set_text(a_vol_val, buf);
                    audio_set_volume((uint8_t)v);
                  });
}

// Build one row: label on left, switch on right.
static lv_obj_t* make_switch_row(lv_obj_t* parent, const char* label,
                                 bool initial_on, lv_event_cb_t cb) {
  lv_obj_t* row = lv_obj_create(parent);
  lv_obj_set_size(row, LCD_W - 32, 36);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_set_style_pad_all(row, 0, 0);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* l = lv_label_create(row);
  lv_label_set_text(l, label);
  lv_obj_set_style_text_color(l, lv_color_hex(COL_TXT_NORM), 0);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
  lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);

  lv_obj_t* sw = lv_switch_create(row);
  lv_obj_set_size(sw, 44, 22);
  lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
  if (initial_on) lv_obj_add_state(sw, LV_STATE_CHECKED);
  if (cb) lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, nullptr);
  // NB: no auto click beep on switches — `cb` itself can beep if it
  // wants. Otherwise programmatic state syncs (e.g. ui_show_audio
  // re-applying NVS values to the switches) would fire VALUE_CHANGED
  // and produce a buzzer-like cascade of beeps.
  return sw;
}

static void build_audio_screen() {
  scr_audio = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(scr_audio, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_pad_all(scr_audio, 0, 0);
  lv_obj_clear_flag(scr_audio, LV_OBJ_FLAG_SCROLLABLE);

  make_top_bar(scr_audio, "Audio", on_back_to_audio_parent);

  lv_obj_t* stack = lv_obj_create(scr_audio);
  lv_obj_set_size(stack, LCD_W, LCD_H - TOPBAR_H);
  lv_obj_align(stack, LV_ALIGN_TOP_MID, 0, TOPBAR_H);
  lv_obj_set_style_bg_opa(stack, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(stack, 0, 0);
  lv_obj_set_style_pad_all(stack, 16, 0);
  lv_obj_set_flex_flow(stack, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(stack, 8, 0);
  lv_obj_set_flex_align(stack, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  // Master enable
  bool master_on = nvs_settings_get_audio_enabled();
  a_master_sw = make_switch_row(stack, "Audio enabled", master_on, on_audio_master_changed);

  // Volume row (label + value, slider below)
  lv_obj_t* vol_row = lv_obj_create(stack);
  lv_obj_set_size(vol_row, LCD_W - 32, 70);
  lv_obj_set_style_bg_opa(vol_row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(vol_row, 0, 0);
  lv_obj_set_style_pad_all(vol_row, 0, 0);
  lv_obj_clear_flag(vol_row, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* vl = lv_label_create(vol_row);
  lv_label_set_text(vl, "Volume");
  lv_obj_set_style_text_color(vl, lv_color_hex(COL_TXT_NORM), 0);
  lv_obj_set_style_text_font(vl, &lv_font_montserrat_14, 0);
  lv_obj_align(vl, LV_ALIGN_TOP_LEFT, 0, 0);

  // Value label wrapped in a clickable button (matches display sliders'
  // pattern). Tap opens the numeric keypad to type a precise value.
  lv_obj_t* val_btn = lv_btn_create(vol_row);
  lv_obj_set_size(val_btn, 60, 24);
  lv_obj_align(val_btn, LV_ALIGN_TOP_RIGHT, 0, -2);
  lv_obj_set_style_bg_color(val_btn, lv_color_hex(COL_TILE_BG), 0);
  lv_obj_set_style_bg_color(val_btn, lv_color_hex(COL_TILE_HOVER), LV_STATE_PRESSED);
  lv_obj_set_style_radius(val_btn, 6, 0);
  lv_obj_set_style_border_width(val_btn, 0, 0);
  lv_obj_set_style_pad_all(val_btn, 0, 0);
  lv_obj_add_event_cb(val_btn, on_audio_vol_tap, LV_EVENT_CLICKED, nullptr);

  a_vol_val = lv_label_create(val_btn);
  lv_obj_set_style_text_color(a_vol_val, lv_color_hex(COL_TXT_NORM), 0);
  lv_obj_set_style_text_font(a_vol_val, &lv_font_montserrat_14, 0);
  lv_obj_center(a_vol_val);
  {
    char buf[8]; snprintf(buf, sizeof(buf), "%u%%", (unsigned)nvs_settings_get_audio_volume());
    lv_label_set_text(a_vol_val, buf);
  }

  a_vol_slider = lv_slider_create(vol_row);
  lv_obj_set_size(a_vol_slider, LCD_W - 32, 14);
  lv_obj_align(a_vol_slider, LV_ALIGN_BOTTOM_MID, 0, -10);
  lv_slider_set_range(a_vol_slider, 0, 100);
  lv_slider_set_value(a_vol_slider, nvs_settings_get_audio_volume(), LV_ANIM_OFF);
  lv_obj_add_event_cb(a_vol_slider, on_audio_vol_changed, LV_EVENT_VALUE_CHANGED, nullptr);

  // Event toggles
  uint8_t evt = nvs_settings_get_audio_events();
  a_evt_card_sw   = make_switch_row(stack, "Card insert / remove", (evt & AUDIO_EVT_CARD)   != 0, on_audio_evt_changed);
  a_evt_pin_sw    = make_switch_row(stack, "PIN success / fail",   (evt & AUDIO_EVT_PIN)    != 0, on_audio_evt_changed);
  a_evt_tick_sw   = make_switch_row(stack, "TOTP last-5s tick",    (evt & AUDIO_EVT_TICK)   != 0, on_audio_evt_changed);
  a_evt_keytap_sw = make_switch_row(stack, "Keyboard taps",        (evt & AUDIO_EVT_KEYTAP) != 0, on_audio_evt_changed);

  // Test button
  lv_obj_t* test_btn = lv_btn_create(stack);
  lv_obj_set_size(test_btn, LCD_W - 32, 36);
  lv_obj_set_style_bg_color(test_btn, lv_color_hex(COL_TILE_BG), 0);
  lv_obj_set_style_bg_color(test_btn, lv_color_hex(COL_TILE_HOVER), LV_STATE_PRESSED);
  lv_obj_set_style_radius(test_btn, 8, 0);
  lv_obj_set_style_border_width(test_btn, 0, 0);
  lv_obj_t* tbl = lv_label_create(test_btn);
  lv_label_set_text(tbl, LV_SYMBOL_PLAY " Test beep");
  lv_obj_set_style_text_color(tbl, lv_color_hex(COL_TXT_NORM), 0);
  lv_obj_set_style_text_font(tbl, &lv_font_montserrat_14, 0);
  lv_obj_center(tbl);
  lv_obj_add_event_cb(test_btn, [](lv_event_t*) {
    audio_beep(1200, 80);
  }, LV_EVENT_CLICKED, nullptr);
}

void ui_show_audio() {
  // Guard so on_audio_*_changed handlers don't beep on these
  // programmatic state writes.
  a_syncing = true;

  // Re-sync from NVS each time
  if (a_master_sw) {
    if (nvs_settings_get_audio_enabled()) lv_obj_add_state(a_master_sw, LV_STATE_CHECKED);
    else                                  lv_obj_clear_state(a_master_sw, LV_STATE_CHECKED);
  }
  if (a_vol_slider) {
    lv_slider_set_value(a_vol_slider, nvs_settings_get_audio_volume(), LV_ANIM_OFF);
    char buf[8]; snprintf(buf, sizeof(buf), "%u%%", (unsigned)nvs_settings_get_audio_volume());
    if (a_vol_val) lv_label_set_text(a_vol_val, buf);
  }
  uint8_t evt = nvs_settings_get_audio_events();
  if (a_evt_card_sw)   { (evt & AUDIO_EVT_CARD)   ? lv_obj_add_state(a_evt_card_sw,   LV_STATE_CHECKED) : lv_obj_clear_state(a_evt_card_sw,   LV_STATE_CHECKED); }
  if (a_evt_pin_sw)    { (evt & AUDIO_EVT_PIN)    ? lv_obj_add_state(a_evt_pin_sw,    LV_STATE_CHECKED) : lv_obj_clear_state(a_evt_pin_sw,    LV_STATE_CHECKED); }
  if (a_evt_tick_sw)   { (evt & AUDIO_EVT_TICK)   ? lv_obj_add_state(a_evt_tick_sw,   LV_STATE_CHECKED) : lv_obj_clear_state(a_evt_tick_sw,   LV_STATE_CHECKED); }
  if (a_evt_keytap_sw) { (evt & AUDIO_EVT_KEYTAP) ? lv_obj_add_state(a_evt_keytap_sw, LV_STATE_CHECKED) : lv_obj_clear_state(a_evt_keytap_sw, LV_STATE_CHECKED); }

  a_syncing = false;

  if (lv_scr_act() != scr_audio) lv_scr_load(scr_audio);
}

// ---------------------------------------------------------------------------
// Toast — transient notification
//
// A single reusable widget parented to the top layer (lv_layer_top)
// so it floats above the active screen. A timer auto-deletes it
// after 2s, or it gets replaced if another toast arrives first.
// ---------------------------------------------------------------------------

static void toast_dismiss_cb(lv_timer_t* t) {
  (void)t;
  if (toast_obj) {
    lv_obj_del(toast_obj);
    toast_obj = nullptr;
    toast_lbl = nullptr;
  }
  if (toast_timer) {
    lv_timer_del(toast_timer);
    toast_timer = nullptr;
  }
}

void ui_toast(const char* msg) {
  if (!msg) return;

  // Tear down any previous toast so the new one takes over cleanly.
  if (toast_timer) { lv_timer_del(toast_timer); toast_timer = nullptr; }
  if (toast_obj)   { lv_obj_del(toast_obj);     toast_obj = nullptr; toast_lbl = nullptr; }

  lv_obj_t* parent = lv_layer_top();

  toast_obj = lv_obj_create(parent);
  lv_obj_set_style_bg_color(toast_obj, lv_color_hex(COL_TILE_HOVER), 0);
  lv_obj_set_style_bg_opa(toast_obj, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(toast_obj, lv_color_hex(COL_TXT_FAINT), 0);
  lv_obj_set_style_border_width(toast_obj, 1, 0);
  lv_obj_set_style_radius(toast_obj, 10, 0);
  lv_obj_set_style_pad_all(toast_obj, 12, 0);
  lv_obj_clear_flag(toast_obj, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(toast_obj, LV_OBJ_FLAG_CLICKABLE);

  toast_lbl = lv_label_create(toast_obj);
  lv_label_set_text(toast_lbl, msg);
  lv_obj_set_style_text_color(toast_lbl, lv_color_hex(COL_TXT_NORM), 0);
  lv_obj_set_style_text_font(toast_lbl, &lv_font_montserrat_16, 0);
  lv_obj_center(toast_lbl);

  // Size: fit label plus padding, min 180 wide.
  lv_obj_update_layout(toast_lbl);
  lv_coord_t lw = lv_obj_get_width(toast_lbl);
  lv_coord_t tw = lw + 48;
  if (tw < 180) tw = 180;
  if (tw > LCD_W - 20) tw = LCD_W - 20;
  lv_obj_set_size(toast_obj, tw, 48);
  lv_obj_align(toast_obj, LV_ALIGN_BOTTOM_MID, 0, -30);

  toast_timer = lv_timer_create(toast_dismiss_cb, 2000, nullptr);
  lv_timer_set_repeat_count(toast_timer, 1);
}

// ---------------------------------------------------------------------------
// Date / Time setter screen
//
// Six rollers: Year (2024..2035), Month (01..12), Day (01..31),
// Hour (00..23), Minute (00..59), Second (00..59).
// A "Set" button applies the selected values via rtc_setTime().
// Back returns to Settings.
// ---------------------------------------------------------------------------

static void on_back_to_settings(lv_event_t* e) { ui_show_settings(); }

// Helper: build a newline-separated option string for a range [lo..hi],
// zero-padded to `width` digits. Caller must free() the result.
static char* make_roller_opts(int lo, int hi, int width) {
  int count = hi - lo + 1;
  // Each entry is `width` digits + '\n', minus trailing '\n' + '\0'
  size_t sz = (size_t)(count * (width + 1));
  char* buf = (char*)malloc(sz);
  if (!buf) return nullptr;
  char* p = buf;
  for (int i = lo; i <= hi; i++) {
    if (i > lo) *p++ = '\n';
    if (width == 4) p += sprintf(p, "%04d", i);
    else            p += sprintf(p, "%02d", i);
  }
  *p = '\0';
  return buf;
}

static void build_time_setter_screen() {
  scr_time = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(scr_time, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_pad_all(scr_time, 0, 0);
  lv_obj_clear_flag(scr_time, LV_OBJ_FLAG_SCROLLABLE);

  make_top_bar(scr_time, "Set Date / Time", on_back_to_settings);

  // Generate roller option strings
  char* opt_y  = make_roller_opts(2024, 2035, 4);
  char* opt_mo = make_roller_opts(1, 12, 2);
  char* opt_d  = make_roller_opts(1, 31, 2);
  char* opt_h  = make_roller_opts(0, 23, 2);
  char* opt_mi = make_roller_opts(0, 59, 2);
  char* opt_s  = make_roller_opts(0, 59, 2);

  // Roller styling helper
  auto make_roller = [&](lv_obj_t* parent, const char* opts, int visible_rows,
                         lv_coord_t x, lv_coord_t y, lv_coord_t w) -> lv_obj_t* {
    lv_obj_t* r = lv_roller_create(parent);
    lv_roller_set_options(r, opts, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(r, visible_rows);
    lv_obj_set_width(r, w);
    lv_obj_set_pos(r, x, y);
    lv_obj_set_style_bg_color(r, lv_color_hex(COL_TILE_BG), 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(COL_TILE_HOVER), LV_PART_SELECTED);
    lv_obj_set_style_text_color(r, lv_color_hex(COL_TXT_NORM), 0);
    lv_obj_set_style_text_color(r, lv_color_hex(0xFFFFFF), LV_PART_SELECTED);
    lv_obj_set_style_text_font(r, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_font(r, &lv_font_montserrat_20, LV_PART_SELECTED);
    lv_obj_set_style_border_width(r, 0, 0);
    return r;
  };

  // --------------- Layout constants ---------------
  // Both rows are centered independently. Rollers have uniform gap
  // between them; labels are centered above their roller.
  const int GAP      = 12;
  const int DATE_W[] = {96, 80, 80};   // Year wider for 4 digits
  const int TIME_W   = 80;             // all three equal
  const int DATE_TOTAL = DATE_W[0] + GAP + DATE_W[1] + GAP + DATE_W[2]; // 280
  const int TIME_TOTAL = TIME_W * 3 + GAP * 2;                          // 264
  const int DATE_X0  = (LCD_W - DATE_TOTAL) / 2;   // 20
  const int TIME_X0  = (LCD_W - TIME_TOTAL) / 2;   // 28

  const int DATE_LBL_Y = TOPBAR_H + 12;
  const int DATE_ROLL_Y = DATE_LBL_Y + 18;
  const int TIME_LBL_Y  = DATE_ROLL_Y + 105;
  const int TIME_ROLL_Y  = TIME_LBL_Y + 18;

  // Date rollers + labels (centered above each roller)
  const char* date_labels[] = {"Year", "Month", "Day"};
  int dx = DATE_X0;
  lv_obj_t* date_rolls[3];
  for (int i = 0; i < 3; i++) {
    // Label: same width and X as the roller, text centered
    lv_obj_t* l = lv_label_create(scr_time);
    lv_label_set_text(l, date_labels[i]);
    lv_obj_set_style_text_color(l, lv_color_hex(COL_TXT_DIM), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
    lv_obj_set_width(l, DATE_W[i]);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(l, dx, DATE_LBL_Y);

    date_rolls[i] = make_roller(scr_time,
                                i == 0 ? opt_y : (i == 1 ? opt_mo : opt_d),
                                3, dx, DATE_ROLL_Y, DATE_W[i]);
    dx += DATE_W[i] + GAP;
  }
  ts_roll_y  = date_rolls[0];
  ts_roll_mo = date_rolls[1];
  ts_roll_d  = date_rolls[2];

  // Time rollers + labels (same pattern, uniform widths)
  const char* time_labels[] = {"Hour", "Min", "Sec"};
  const char* time_opts[]   = {opt_h, opt_mi, opt_s};
  int tx = TIME_X0;
  lv_obj_t* time_rolls[3];
  for (int i = 0; i < 3; i++) {
    lv_obj_t* l = lv_label_create(scr_time);
    lv_label_set_text(l, time_labels[i]);
    lv_obj_set_style_text_color(l, lv_color_hex(COL_TXT_DIM), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
    lv_obj_set_width(l, TIME_W);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(l, tx, TIME_LBL_Y);

    time_rolls[i] = make_roller(scr_time, time_opts[i],
                                3, tx, TIME_ROLL_Y, TIME_W);
    tx += TIME_W + GAP;
  }
  ts_roll_h  = time_rolls[0];
  ts_roll_mi = time_rolls[1];
  ts_roll_s  = time_rolls[2];

  // "Set" button
  lv_obj_t* set_btn = lv_btn_create(scr_time);
  lv_obj_set_size(set_btn, 160, 44);
  lv_obj_align(set_btn, LV_ALIGN_BOTTOM_MID, 0, -40);
  lv_obj_set_style_bg_color(set_btn, lv_color_hex(COL_GREEN), 0);
  lv_obj_set_style_radius(set_btn, 8, 0);
  lv_obj_t* sl = lv_label_create(set_btn);
  lv_label_set_text(sl, LV_SYMBOL_OK " Set Time");
  lv_obj_set_style_text_color(sl, lv_color_hex(0x000000), 0);
  lv_obj_set_style_text_font(sl, &lv_font_montserrat_16, 0);
  lv_obj_center(sl);
  lv_obj_add_event_cb(set_btn, [](lv_event_t*) {
    int y  = (int)lv_roller_get_selected(ts_roll_y) + 2024;
    int mo = (int)lv_roller_get_selected(ts_roll_mo) + 1;
    int d  = (int)lv_roller_get_selected(ts_roll_d) + 1;
    int h  = (int)lv_roller_get_selected(ts_roll_h);
    int mi = (int)lv_roller_get_selected(ts_roll_mi);
    int s  = (int)lv_roller_get_selected(ts_roll_s);
    rtc_setTime(y, mo, d, h, mi, s);
    ui_toast("Time updated");
    ui_show_settings();
  }, LV_EVENT_CLICKED, nullptr);

  // Free the temporary option strings (LVGL copies them internally)
  free(opt_y); free(opt_mo); free(opt_d);
  free(opt_h); free(opt_mi); free(opt_s);
}

void ui_show_time_setter() {
  // Pre-set rollers to current RTC time
  if (rtc_isRunning()) {
    time_t  t    = rtc_epoch();
    int32_t days = (int32_t)(t / 86400);
    int     tod  = (int)(t % 86400);
    if (tod < 0) { days--; tod += 86400; }
    int y, m, d;
    // Use the same civil-date algorithm from rtc_time.cpp.
    // Inlined here since the function is static in that module.
    {
      int32_t z = days + 719468;
      int32_t era = (z >= 0 ? z : z - 146096) / 146097;
      uint32_t doe = (uint32_t)(z - era * 146097);
      uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
      y = (int)yoe + era * 400;
      uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
      uint32_t mp  = (5 * doy + 2) / 153;
      d = (int)(doy - (153 * mp + 2) / 5 + 1);
      m = (int)mp + (mp < 10 ? 3 : -9);
      if (m <= 2) y++;
    }
    int hr = tod / 3600;
    int mn = (tod % 3600) / 60;
    int sc = tod % 60;

    if (ts_roll_y)  lv_roller_set_selected(ts_roll_y,  (uint16_t)(y - 2024), LV_ANIM_OFF);
    if (ts_roll_mo) lv_roller_set_selected(ts_roll_mo, (uint16_t)(m - 1),    LV_ANIM_OFF);
    if (ts_roll_d)  lv_roller_set_selected(ts_roll_d,  (uint16_t)(d - 1),    LV_ANIM_OFF);
    if (ts_roll_h)  lv_roller_set_selected(ts_roll_h,  (uint16_t)hr,         LV_ANIM_OFF);
    if (ts_roll_mi) lv_roller_set_selected(ts_roll_mi, (uint16_t)mn,         LV_ANIM_OFF);
    if (ts_roll_s)  lv_roller_set_selected(ts_roll_s,  (uint16_t)sc,         LV_ANIM_OFF);
  }
  if (lv_scr_act() != scr_time) lv_scr_load(scr_time);
}

// ---------------------------------------------------------------------------
// Manual Entry screen
//
// Scrollable form with text fields for Issuer, Account, Secret (base32),
// dropdowns for Algorithm/Digits/Period, and a Save button. On-screen
// keyboard appears when a text field is tapped.
// ---------------------------------------------------------------------------

// Keyboard management: show when a textarea is focused, hide otherwise.
// Resize the form container so it fills the space above the keyboard
// when visible, and the full screen when hidden.
static void me_ta_focus_cb(lv_event_t* e) {
  lv_obj_t* ta = lv_event_get_target(e);
  if (!me_keyboard || !me_form) return;
  lv_keyboard_set_textarea(me_keyboard, ta);
  if (ta == me_ta_secret) {
    lv_keyboard_set_mode(me_keyboard, LV_KEYBOARD_MODE_TEXT_UPPER);
  } else {
    lv_keyboard_set_mode(me_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
  }
  lv_obj_clear_flag(me_keyboard, LV_OBJ_FLAG_HIDDEN);
  // Shrink form to make room for keyboard
  lv_obj_set_height(me_form, LCD_H - TOPBAR_H - 200);
  lv_obj_scroll_to_view(ta, LV_ANIM_ON);
}

static void me_kb_ready_cb(lv_event_t* e) {
  if (me_keyboard) lv_obj_add_flag(me_keyboard, LV_OBJ_FLAG_HIDDEN);
  // Expand form back to full height
  if (me_form) lv_obj_set_height(me_form, LCD_H - TOPBAR_H);
}

// Helper: create a labelled text area row in the form
static lv_obj_t* me_make_field(lv_obj_t* parent, const char* label,
                               const char* placeholder, int maxLen) {
  lv_obj_t* l = lv_label_create(parent);
  lv_label_set_text(l, label);
  lv_obj_set_style_text_color(l, lv_color_hex(COL_TXT_DIM), 0);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
  lv_obj_set_width(l, lv_pct(90));

  lv_obj_t* ta = lv_textarea_create(parent);
  lv_textarea_set_one_line(ta, true);
  lv_textarea_set_max_length(ta, maxLen);
  lv_textarea_set_placeholder_text(ta, placeholder);
  lv_obj_set_width(ta, lv_pct(90));
  // Use default LVGL textarea theme (white background, dark text,
  // visible cursor) — same as PIN entry screen.
  lv_obj_add_event_cb(ta, me_ta_focus_cb, LV_EVENT_FOCUSED, nullptr);
  return ta;
}

// Helper: create a labelled dropdown row
static lv_obj_t* me_make_dropdown(lv_obj_t* parent, const char* label,
                                  const char* options) {
  lv_obj_t* l = lv_label_create(parent);
  lv_label_set_text(l, label);
  lv_obj_set_style_text_color(l, lv_color_hex(COL_TXT_DIM), 0);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
  lv_obj_set_width(l, lv_pct(90));

  lv_obj_t* dd = lv_dropdown_create(parent);
  lv_dropdown_set_options(dd, options);
  lv_obj_set_width(dd, lv_pct(90));
  // Hide keyboard when dropdown is focused (it handles its own list)
  // and restore form to full height.
  lv_obj_add_event_cb(dd, [](lv_event_t*) {
    if (me_keyboard) lv_obj_add_flag(me_keyboard, LV_OBJ_FLAG_HIDDEN);
    if (me_form) lv_obj_set_height(me_form, LCD_H - TOPBAR_H);
  }, LV_EVENT_FOCUSED, nullptr);
  return dd;
}

// Save handler
static void me_on_save(lv_event_t* e) {
  const char* issuer  = lv_textarea_get_text(me_ta_issuer);
  const char* account = lv_textarea_get_text(me_ta_account);
  const char* secret  = lv_textarea_get_text(me_ta_secret);

  if (!issuer || strlen(issuer) == 0) {
    ui_toast("Issuer is required");
    return;
  }
  if (!secret || strlen(secret) < 4) {
    ui_toast("Secret too short");
    return;
  }

  // Decode base32 secret
  uint8_t secBuf[64];
  int secLen = sc_base32_decode(secret, secBuf, sizeof(secBuf));
  if (secLen <= 0) {
    ui_toast("Invalid base32 secret");
    return;
  }

  // Algorithm
  uint16_t algoIdx = lv_dropdown_get_selected(me_dd_algo);
  uint8_t typeAlgo;
  switch (algoIdx) {
    case 1:  typeAlgo = 0x22; break;  // SHA256
    case 2:  typeAlgo = 0x23; break;  // SHA512
    default: typeAlgo = 0x21; break;  // SHA1
  }

  // Digits
  uint16_t digIdx = lv_dropdown_get_selected(me_dd_digits);
  uint8_t digits = (digIdx == 1) ? 7 : (digIdx == 2) ? 8 : 6;

  // Period
  uint16_t perIdx = lv_dropdown_get_selected(me_dd_period);
  uint16_t period = (perIdx == 1) ? 60 : 30;

  // Build credential name: "PP/Issuer:Account" or "Issuer:Account"
  char name[MAX_CRED_NAME_LEN];
  if (account && strlen(account) > 0) {
    if (period != 30)
      snprintf(name, sizeof(name), "%u/%s:%s", period, issuer, account);
    else
      snprintf(name, sizeof(name), "%s:%s", issuer, account);
  } else {
    if (period != 30)
      snprintf(name, sizeof(name), "%u/%s", period, issuer);
    else
      snprintf(name, sizeof(name), "%s", issuer);
  }

  // Check card state
  if (!sc_state().authenticated) {
    memset(secBuf, 0, sizeof(secBuf));
    ui_toast("Card not unlocked");
    return;
  }

  Serial.printf("[MANUAL] saving '%s' (%d-byte secret, algo=0x%02X, %d digits, %ds)\n",
                name, secLen, typeAlgo, digits, period);

  bool ok = sc_put_credential(name, secBuf, secLen, typeAlgo, digits, 0);
  memset(secBuf, 0, sizeof(secBuf));

  if (ok) {
    sc_recalculate(rtc_epoch());
    // Clear fields for next use
    lv_textarea_set_text(me_ta_issuer, "");
    lv_textarea_set_text(me_ta_account, "");
    lv_textarea_set_text(me_ta_secret, "");
    lv_dropdown_set_selected(me_dd_algo, 0);
    lv_dropdown_set_selected(me_dd_digits, 0);
    lv_dropdown_set_selected(me_dd_period, 0);
    ui_show_totp_menu();
    ui_toast("Credential added");
  } else {
    ui_toast("Card rejected");
  }
}

static void build_manual_entry_screen() {
  scr_manual = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(scr_manual, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_pad_all(scr_manual, 0, 0);
  lv_obj_clear_flag(scr_manual, LV_OBJ_FLAG_SCROLLABLE);

  make_top_bar(scr_manual, "Manual Entry", on_back_to_totp_menu);

  // Scrollable form container (above the keyboard area)
  me_form = lv_obj_create(scr_manual);
  lv_obj_set_size(me_form, LCD_W, LCD_H - TOPBAR_H - 200);  // leave room for keyboard
  lv_obj_set_pos(me_form, 0, TOPBAR_H);
  lv_obj_set_style_bg_opa(me_form, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(me_form, 0, 0);
  lv_obj_set_style_pad_all(me_form, 8, 0);
  lv_obj_set_flex_flow(me_form, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(me_form, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(me_form, 4, 0);

  // Form fields
  me_ta_issuer  = me_make_field(me_form, "Issuer", "e.g. Google", 48);
  me_ta_account = me_make_field(me_form, "Account (optional)", "e.g. user@example.com", 48);
  me_ta_secret  = me_make_field(me_form, "Secret (base32)", "e.g. JBSWY3DPEHPK3PXP", 128);
  me_dd_algo    = me_make_dropdown(me_form, "Algorithm", "SHA1\nSHA256\nSHA512");
  me_dd_digits  = me_make_dropdown(me_form, "Digits", "6\n7\n8");
  me_dd_period  = me_make_dropdown(me_form, "Period", "30s\n60s");

  // Save button
  lv_obj_t* save_btn = lv_btn_create(me_form);
  lv_obj_set_size(save_btn, lv_pct(90), 40);
  lv_obj_set_style_bg_color(save_btn, lv_color_hex(COL_GREEN), 0);
  lv_obj_set_style_radius(save_btn, 8, 0);
  lv_obj_t* sl = lv_label_create(save_btn);
  lv_label_set_text(sl, LV_SYMBOL_OK " Save");
  lv_obj_set_style_text_color(sl, lv_color_hex(0x000000), 0);
  lv_obj_set_style_text_font(sl, &lv_font_montserrat_16, 0);
  lv_obj_center(sl);
  lv_obj_add_event_cb(save_btn, me_on_save, LV_EVENT_CLICKED, nullptr);

  // On-screen keyboard (initially hidden, shown on textarea focus)
  me_keyboard = lv_keyboard_create(scr_manual);
  lv_obj_set_size(me_keyboard, LCD_W, 200);
  lv_obj_align(me_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_add_flag(me_keyboard, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(me_keyboard, me_kb_ready_cb, LV_EVENT_READY, nullptr);
  lv_obj_add_event_cb(me_keyboard, me_kb_ready_cb, LV_EVENT_CANCEL, nullptr);
  lv_obj_add_event_cb(me_keyboard, kb_tap_beep_cb, LV_EVENT_VALUE_CHANGED, nullptr);
}

void ui_show_manual_entry() {
  if (lv_scr_act() != scr_manual) lv_scr_load(scr_manual);
  // Reset to full height (keyboard hidden)
  if (me_keyboard) lv_obj_add_flag(me_keyboard, LV_OBJ_FLAG_HIDDEN);
  if (me_form) lv_obj_set_height(me_form, LCD_H - TOPBAR_H);
}

// ---------------------------------------------------------------------------
// PIN Change screen
//
// Two fields: New PIN and Confirm PIN. Number keyboard.
// Set button calls sc_set_pin(). Leave blank to remove PIN.
// ---------------------------------------------------------------------------

static void pc_ta_focus(lv_event_t* e) {
  lv_obj_t* ta = lv_event_get_target(e);
  if (pc_keyboard) {
    lv_keyboard_set_textarea(pc_keyboard, ta);
    lv_obj_clear_flag(pc_keyboard, LV_OBJ_FLAG_HIDDEN);
  }
}

static void pc_kb_done(lv_event_t* e) {
  if (pc_keyboard) lv_obj_add_flag(pc_keyboard, LV_OBJ_FLAG_HIDDEN);
}

static void pc_on_set(lv_event_t* e) {
  const char* newPin     = lv_textarea_get_text(pc_ta_new);
  const char* confirmPin = lv_textarea_get_text(pc_ta_confirm);

  bool removing = (!newPin || strlen(newPin) == 0);

  if (!removing) {
    if (!confirmPin || strcmp(newPin, confirmPin) != 0) {
      ui_toast("PINs don't match");
      return;
    }
    if (strlen(newPin) < 4) {
      ui_toast("PIN too short (min 4)");
      return;
    }
  }

  bool ok = sc_set_pin(removing ? nullptr : newPin);

  // Clear fields
  lv_textarea_set_text(pc_ta_new, "");
  lv_textarea_set_text(pc_ta_confirm, "");

  if (ok) {
    ui_toast(removing ? "PIN removed" : "PIN changed");
    ui_show_totp_menu();
  } else {
    ui_toast("PIN change failed");
  }
}

static void build_pin_change_screen() {
  scr_pin_change = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(scr_pin_change, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_pad_all(scr_pin_change, 0, 0);
  lv_obj_clear_flag(scr_pin_change, LV_OBJ_FLAG_SCROLLABLE);

  make_top_bar(scr_pin_change, "Change PIN", on_back_to_totp_menu);

  // Hint
  lv_obj_t* hint = lv_label_create(scr_pin_change);
  lv_label_set_text(hint, "Leave blank to remove PIN");
  lv_obj_set_style_text_color(hint, lv_color_hex(COL_TXT_DIM), 0);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
  lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, TOPBAR_H + 8);

  // New PIN label + textarea
  lv_obj_t* l1 = lv_label_create(scr_pin_change);
  lv_label_set_text(l1, "New PIN");
  lv_obj_set_style_text_color(l1, lv_color_hex(COL_TXT_DIM), 0);
  lv_obj_set_style_text_font(l1, &lv_font_montserrat_12, 0);
  lv_obj_align(l1, LV_ALIGN_TOP_LEFT, 16, TOPBAR_H + 30);

  pc_ta_new = lv_textarea_create(scr_pin_change);
  lv_textarea_set_password_mode(pc_ta_new, true);
  lv_textarea_set_one_line(pc_ta_new, true);
  lv_textarea_set_max_length(pc_ta_new, 64);
  lv_textarea_set_placeholder_text(pc_ta_new, "New PIN");
  lv_obj_set_width(pc_ta_new, LCD_W - 32);
  lv_obj_align(pc_ta_new, LV_ALIGN_TOP_MID, 0, TOPBAR_H + 46);
  lv_obj_set_style_text_font(pc_ta_new, &lv_font_montserrat_16, 0);
  lv_obj_add_event_cb(pc_ta_new, pc_ta_focus, LV_EVENT_FOCUSED, nullptr);

  // Confirm PIN label + textarea
  lv_obj_t* l2 = lv_label_create(scr_pin_change);
  lv_label_set_text(l2, "Confirm PIN");
  lv_obj_set_style_text_color(l2, lv_color_hex(COL_TXT_DIM), 0);
  lv_obj_set_style_text_font(l2, &lv_font_montserrat_12, 0);
  lv_obj_align(l2, LV_ALIGN_TOP_LEFT, 16, TOPBAR_H + 86);

  pc_ta_confirm = lv_textarea_create(scr_pin_change);
  lv_textarea_set_password_mode(pc_ta_confirm, true);
  lv_textarea_set_one_line(pc_ta_confirm, true);
  lv_textarea_set_max_length(pc_ta_confirm, 64);
  lv_textarea_set_placeholder_text(pc_ta_confirm, "Confirm PIN");
  lv_obj_set_width(pc_ta_confirm, LCD_W - 32);
  lv_obj_align(pc_ta_confirm, LV_ALIGN_TOP_MID, 0, TOPBAR_H + 102);
  lv_obj_set_style_text_font(pc_ta_confirm, &lv_font_montserrat_16, 0);
  lv_obj_add_event_cb(pc_ta_confirm, pc_ta_focus, LV_EVENT_FOCUSED, nullptr);

  // Set button
  lv_obj_t* btn = lv_btn_create(scr_pin_change);
  lv_obj_set_size(btn, 180, 40);
  lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, TOPBAR_H + 148);
  lv_obj_set_style_bg_color(btn, lv_color_hex(COL_GREEN), 0);
  lv_obj_set_style_radius(btn, 8, 0);
  lv_obj_t* bl = lv_label_create(btn);
  lv_label_set_text(bl, LV_SYMBOL_OK " Set PIN");
  lv_obj_set_style_text_color(bl, lv_color_hex(0x000000), 0);
  lv_obj_set_style_text_font(bl, &lv_font_montserrat_16, 0);
  lv_obj_center(bl);
  lv_obj_add_event_cb(btn, pc_on_set, LV_EVENT_CLICKED, nullptr);

  // Number keyboard
  pc_keyboard = lv_keyboard_create(scr_pin_change);
  lv_keyboard_set_mode(pc_keyboard, LV_KEYBOARD_MODE_NUMBER);
  lv_obj_set_size(pc_keyboard, LCD_W, 200);
  lv_obj_align(pc_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_add_flag(pc_keyboard, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(pc_keyboard, pc_kb_done, LV_EVENT_READY, nullptr);
  lv_obj_add_event_cb(pc_keyboard, pc_kb_done, LV_EVENT_CANCEL, nullptr);
  lv_obj_add_event_cb(pc_keyboard, kb_tap_beep_cb, LV_EVENT_VALUE_CHANGED, nullptr);
}

void ui_show_pin_change() {
  // Clear any residual text each time we enter
  if (pc_ta_new)     lv_textarea_set_text(pc_ta_new, "");
  if (pc_ta_confirm) lv_textarea_set_text(pc_ta_confirm, "");
  if (pc_keyboard)   lv_obj_add_flag(pc_keyboard, LV_OBJ_FLAG_HIDDEN);
  if (scr_pin_change && lv_scr_act() != scr_pin_change) {
    lv_scr_load(scr_pin_change);
  }
}

// ---------------------------------------------------------------------------
// QR scan screen
//
// Layout: top bar with Back; 320x240 camera viewfinder sitting just
// below the bar (LVGL image widget with source updated from the loop
// via ui_qr_set_frame); a status hint at the bottom.
//
// The image widget's source is an lv_img_dsc_t whose .data points
// directly into the camera's frame buffer (RGB565, big-endian). This
// is lifetime-sensitive: the camera module must not return the fb
// to the pool while the widget still points at it. The handoff
// protocol in cam_qr.cpp ensures this by holding the fb until the
// next fb swap replaces it.
// ---------------------------------------------------------------------------
static void build_qr_screen() {
  scr_qr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(scr_qr, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_pad_all(scr_qr, 0, 0);
  lv_obj_clear_flag(scr_qr, LV_OBJ_FLAG_SCROLLABLE);

  make_top_bar(scr_qr, "Scan QR", on_back_to_totp_menu);

  qr_img = lv_img_create(scr_qr);
  lv_obj_set_size(qr_img, 320, 240);
  lv_obj_align(qr_img, LV_ALIGN_TOP_MID, 0, TOPBAR_H + 10);

  qr_lbl_hint = lv_label_create(scr_qr);
  lv_label_set_text(qr_lbl_hint, "Point camera at QR code  (K2 cancel)");
  lv_obj_set_style_text_color(qr_lbl_hint, lv_color_hex(COL_TXT_DIM), 0);
  lv_obj_set_style_text_font(qr_lbl_hint, &lv_font_montserrat_14, 0);
  lv_obj_align(qr_lbl_hint, LV_ALIGN_BOTTOM_MID, 0, -40);
}

void ui_show_qr_scan() {
  if (lv_scr_act() != scr_qr) lv_scr_load(scr_qr);
}

void ui_qr_set_frame(const lv_img_dsc_t* dsc) {
  if (!qr_img || !dsc) return;
  lv_img_set_src(qr_img, dsc);
  lv_obj_invalidate(qr_img);
}

void ui_qr_set_hint(const char* text) {
  if (!qr_lbl_hint) return;
  lv_label_set_text(qr_lbl_hint, text ? text : "");
}

// ---------------------------------------------------------------------------
// Manage AIDs screen
//
// List of stored AIDs with per-row delete (with confirm). Each row shows
// its priority index (0 = highest), name, and AID hex bytes. A footer
// permanently displays the compiled-in default APEX TOTP AID, which is
// always tried as a final fallback. Adding new AIDs is via serial only
// for now (`TOTP AID ADD <hex> <name>`); touchscreen hex entry would be
// painful on this panel.
// ---------------------------------------------------------------------------
static lv_obj_t* aid_list_box       = nullptr;
static lv_obj_t* aid_msgbox         = nullptr;
static int       aid_pending_delete = -1;

static void rebuild_aid_list();
static void on_aid_del_btn   (lv_event_t* e);
static void on_aid_del_msgbox(lv_event_t* e);

static void format_aid_hex(char* out, size_t outsz,
                           const uint8_t* aid, uint8_t len) {
  out[0] = '\0';
  for (uint8_t b = 0; b < len; b++) {
    size_t n = strlen(out);
    if (n + 4 >= outsz) break;
    snprintf(out + n, outsz - n, b == 0 ? "%02X" : " %02X", aid[b]);
  }
}

static void build_aid_screen() {
  scr_aid = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(scr_aid, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_pad_all(scr_aid, 0, 0);
  lv_obj_clear_flag(scr_aid, LV_OBJ_FLAG_SCROLLABLE);

  make_top_bar(scr_aid, "Manage AIDs", on_back_to_settings);

  // Footer (anchored to bottom): default AID reference.
  const int footer_h = 70;
  lv_obj_t* footer = lv_obj_create(scr_aid);
  lv_obj_set_size(footer, LCD_W, footer_h);
  lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_color(footer, lv_color_hex(COL_TILE_BG), 0);
  lv_obj_set_style_border_width(footer, 0, 0);
  lv_obj_set_style_pad_all(footer, 8, 0);
  lv_obj_clear_flag(footer, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* def_label = lv_label_create(footer);
  lv_label_set_text(def_label, "Default (lowest priority):");
  lv_obj_set_style_text_color(def_label, lv_color_hex(COL_TXT_DIM), 0);
  lv_obj_set_style_text_font (def_label, &lv_font_montserrat_12, 0);
  lv_obj_align(def_label, LV_ALIGN_TOP_LEFT, 0, 0);

  char def_hex[NVS_AID_MAX_BYTES * 3 + 16];
  strcpy(def_hex, "APEX TOTP ");
  size_t L = strlen(def_hex);
  for (int b = 0; b < APEX_AID_LEN && L + 4 < sizeof(def_hex); b++) {
    snprintf(def_hex + L, sizeof(def_hex) - L,
             b == 0 ? "%02X" : " %02X", APEX_TOTP_AID[b]);
    L = strlen(def_hex);
  }
  lv_obj_t* def_hex_lbl = lv_label_create(footer);
  lv_label_set_text(def_hex_lbl, def_hex);
  lv_obj_set_style_text_color(def_hex_lbl, lv_color_hex(COL_TXT_NORM), 0);
  lv_obj_set_style_text_font (def_hex_lbl, &lv_font_montserrat_12, 0);
  lv_label_set_long_mode(def_hex_lbl, LV_LABEL_LONG_DOT);
  lv_obj_set_width(def_hex_lbl, LCD_W - 16);
  lv_obj_align(def_hex_lbl, LV_ALIGN_TOP_LEFT, 0, 16);

  // List container (fills the area between top bar and footer).
  aid_list_box = lv_obj_create(scr_aid);
  lv_obj_set_size(aid_list_box, LCD_W, LCD_H - TOPBAR_H - footer_h);
  lv_obj_align(aid_list_box, LV_ALIGN_TOP_MID, 0, TOPBAR_H);
  lv_obj_set_style_bg_color    (aid_list_box, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_border_width(aid_list_box, 0, 0);
  lv_obj_set_style_pad_all     (aid_list_box, 6, 0);
  lv_obj_set_style_pad_row     (aid_list_box, 6, 0);
  lv_obj_set_flex_flow (aid_list_box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(aid_list_box,
                        LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_scroll_dir    (aid_list_box, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(aid_list_box, LV_SCROLLBAR_MODE_AUTO);
}

static void rebuild_aid_list() {
  if (!aid_list_box) return;
  lv_obj_clean(aid_list_box);

  int n = nvs_aid_count();
  if (n == 0) {
    lv_obj_t* empty = lv_label_create(aid_list_box);
    lv_label_set_text(empty,
                      "No user AIDs stored.\nDefault is tried automatically.");
    lv_obj_set_style_text_color(empty, lv_color_hex(COL_TXT_DIM), 0);
    lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
    return;
  }

  for (int i = 0; i < n; i++) {
    AidEntry e;
    if (!nvs_aid_get(i, &e)) continue;

    lv_obj_t* row = lv_obj_create(aid_list_box);
    lv_obj_set_size(row, LCD_W - 24, 70);
    lv_obj_set_style_bg_color    (row, lv_color_hex(COL_TILE_BG), 0);
    lv_obj_set_style_radius      (row, 8, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all     (row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    char prio[8];
    snprintf(prio, sizeof(prio), "[%d]", i);
    lv_obj_t* prio_lbl = lv_label_create(row);
    lv_label_set_text(prio_lbl, prio);
    lv_obj_set_style_text_color(prio_lbl, lv_color_hex(COL_BLUE), 0);
    lv_obj_set_style_text_font (prio_lbl, &lv_font_montserrat_12, 0);
    lv_obj_align(prio_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t* name_lbl = lv_label_create(row);
    lv_label_set_text(name_lbl, e.name);
    lv_obj_set_style_text_color(name_lbl, lv_color_hex(COL_TXT_NORM), 0);
    lv_obj_set_style_text_font (name_lbl, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name_lbl, LCD_W - 24 - 16 - 30 - 50);
    lv_obj_align(name_lbl, LV_ALIGN_TOP_LEFT, 30, 0);

    char hex_str[NVS_AID_MAX_BYTES * 3 + 4];
    format_aid_hex(hex_str, sizeof(hex_str), e.aid, e.aid_len);
    lv_obj_t* hex_lbl = lv_label_create(row);
    lv_label_set_text(hex_lbl, hex_str);
    lv_obj_set_style_text_color(hex_lbl, lv_color_hex(COL_TXT_DIM), 0);
    lv_obj_set_style_text_font (hex_lbl, &lv_font_montserrat_12, 0);
    lv_label_set_long_mode(hex_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(hex_lbl, LCD_W - 24 - 16 - 50);
    lv_obj_align(hex_lbl, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_t* del_btn = lv_btn_create(row);
    lv_obj_set_size(del_btn, 40, 40);
    lv_obj_align(del_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(del_btn, lv_color_hex(COL_RED), 0);
    lv_obj_set_style_radius  (del_btn, 6, 0);
    lv_obj_t* trash = lv_label_create(del_btn);
    lv_label_set_text(trash, LV_SYMBOL_TRASH);
    lv_obj_set_style_text_color(trash, lv_color_hex(COL_TXT_NORM), 0);
    lv_obj_center(trash);
    lv_obj_add_event_cb(del_btn, on_aid_del_btn,
                        LV_EVENT_CLICKED, (void*)(intptr_t)i);
  }
}

static void on_aid_del_btn(lv_event_t* e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (aid_msgbox) return;
  aid_pending_delete = idx;

  AidEntry entry;
  char msg[80];
  if (nvs_aid_get(idx, &entry)) {
    snprintf(msg, sizeof(msg), "Remove AID '%s'?\nThis cannot be undone.",
             entry.name);
  } else {
    snprintf(msg, sizeof(msg), "Remove this AID?");
  }
  static const char* btns[] = { "Remove", "Cancel", "" };
  aid_msgbox = lv_msgbox_create(nullptr, "Confirm", msg, btns, false);
  lv_obj_center(aid_msgbox);
  lv_obj_add_event_cb(aid_msgbox, on_aid_del_msgbox,
                      LV_EVENT_VALUE_CHANGED, nullptr);
}

static void on_aid_del_msgbox(lv_event_t* e) {
  lv_obj_t* mb = lv_event_get_current_target(e);
  uint16_t btn = lv_msgbox_get_active_btn(mb);
  if (btn == 0 && aid_pending_delete >= 0) {
    nvs_aid_remove(aid_pending_delete);
    rebuild_aid_list();
  }
  lv_msgbox_close(mb);
  aid_msgbox = nullptr;
  aid_pending_delete = -1;
}

void ui_show_aid_manager() {
  if (!scr_aid) build_aid_screen();
  if (lv_scr_act() != scr_aid) lv_scr_load(scr_aid);
  rebuild_aid_list();
}

// ---------------------------------------------------------------------------
// Generic status (used by sketch for transient messages like "Reading card",
// "Authenticating", etc.). Routed to the Main Menu's status label.
// ---------------------------------------------------------------------------
void ui_show_status(const char* msg) {
  if (lv_scr_act() != scr_main_menu) lv_scr_load(scr_main_menu);
  if (mm_lbl_status) lv_label_set_text(mm_lbl_status, msg ? msg : "");
}

// ---------------------------------------------------------------------------
// Physical buttons
// ---------------------------------------------------------------------------
void ui_handle_button(int btnIdx) {
  lv_obj_t* cur = lv_scr_act();

  if (btnIdx == 0) {
    // UP = back / cancel.
    // In select mode on the Codes screen, treat UP as "Cancel" first —
    // back navigation requires a second press.
    if (cur == scr_codes && reorder_active_pos >= 0) { exit_reorder_mode(); return; }
    if (cur == scr_codes && select_mode)             { exit_select_mode();  return; }

    if      (cur == scr_codes || cur == scr_stub || cur == scr_manual || cur == scr_pin_change) ui_show_totp_menu();
    else if (cur == scr_totp_menu)                                   ui_show_main_menu();
    else if (cur == scr_piv_menu)                                    ui_show_main_menu();
    else if (cur == scr_piv_info)                                    ui_show_piv_menu();
    else if (cur == scr_pin)                                         ui_show_main_menu();
    else if (cur == scr_settings)                                    ui_show_main_menu();
    else if (cur == scr_usb_reader)                                  { ui_set_usb_reader_active(false); ui_show_main_menu(); }
    else if (cur == scr_display || cur == scr_hardware || cur == scr_audio ||
             cur == scr_time    || cur == scr_aid)                   ui_show_settings();
    // Main menu: ignore (no higher menu).
    return;
  }

  if (btnIdx == 1) {
    // DOWN (K3) = scroll the codes tile list one tile downward, wrap at end.
    if (cur != scr_codes || !c_tile_box || c_tile_count == 0) return;

    lv_coord_t maxY = lv_obj_get_scroll_bottom(c_tile_box);
    lv_coord_t step = CODE_TILE_H + CODE_TILE_GAP;
    if (maxY <= 0) {
      lv_obj_scroll_to_y(c_tile_box, 0, LV_ANIM_ON);
    } else {
      lv_obj_scroll_by(c_tile_box, 0, -step, LV_ANIM_ON);
    }
  }
}

bool ui_is_on_pin_screen() {
  return lv_scr_act() == scr_pin;
}

bool ui_is_on_codes_screen() {
  return lv_scr_act() == scr_codes;
}

bool ui_is_on_settings_tree() {
  lv_obj_t* cur = lv_scr_act();
  return cur == scr_settings || cur == scr_display ||
         cur == scr_hardware || cur == scr_audio   ||
         cur == scr_time     || cur == scr_aid;
}