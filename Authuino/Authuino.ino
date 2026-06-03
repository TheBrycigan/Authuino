/*
 * Authuino.ino  — v0.4
 * TOTP smartcard reader for Waveshare ESP32-S3-Touch-LCD-3.5
 *
 * File layout:
 *   Authuino.ino            - setup / loop / orchestration
 *   sc_interface.h/cpp      - OATH/PIV APDU layer + VALIDATE + CCID bridge
 *   rtc_time.h/cpp          - PCF85063 RTC wrapper
 *   ui.h/cpp                - LVGL screens (codes + PIN entry)
 *
 * The low-level ISO 7816-3 T=0/T=1 driver (the SmartCard class) lives in
 * the separate ESP-ISO7816 library at libraries/ESP-ISO7816 (vendored for
 * now; published as github.com/TheBrycigan/ESP-ISO7816 and switched to a
 * git submodule once available). See SETUP.md.
 */

#define FW_VERSION "v0.4"

#include <Arduino_GFX_Library.h>
#include <lvgl.h>
#include <esp_sleep.h>

#include "driver/i2c_master.h"

#include "sc_interface.h"
#include "rtc_time.h"
#include "nvs_meta.h"
#include "nvs_aid.h"
#include "nvs_settings.h"
#include "ui.h"
#include "audio.h"
#include "accel.h"
#include "usb_ccid.h"

// Required when USB CDC On Boot is Disabled (USB.h is otherwise auto-
// included via the CDC stack). We need it for the explicit USB.begin()
// call in setup(). CDC On Boot is Disabled because the CDC + CCID
// composite produces a configuration descriptor Windows rejects, even
// with manual ordering. Trade-off: no USB serial; debug must use UART
// or LCD output.
#if !ARDUINO_USB_MODE && !ARDUINO_USB_CDC_ON_BOOT
#include "USB.h"
#endif
#include "cam_qr.h"

// ---------------------------------------------------------------------------
// Display / Touch
// ---------------------------------------------------------------------------
#define GFX_BL      6
#define SPI_MISO    2
#define SPI_MOSI    1
#define SPI_SCLK    5
#define LCD_CS     -1
#define LCD_DC      3
#define LCD_RST    -1
#define LCD_W     320
#define LCD_H     480
#define I2C_SDA     8
#define I2C_SCL     7
#define I2C_PORT_NUM 0

// Backlight PWM. 5 kHz, 10-bit resolution.
#define BL_LEDC_FREQ_HZ  5000
#define BL_LEDC_BITS     10
#define BL_LEDC_MAX     ((1 << BL_LEDC_BITS) - 1)

// Dim state brightness ratio (relative to active pct). 20% means a
// user's 80% active becomes 16% when dimmed.
#define BL_DIM_RATIO_NUM  1
#define BL_DIM_RATIO_DEN  5

static void backlight_init() {
  ledcAttach(GFX_BL, BL_LEDC_FREQ_HZ, BL_LEDC_BITS);
}
// pct is 0..100. Applies a simple linear mapping to the LEDC range.
static void backlight_set_pct(int pct) {
  if (pct < 0)   pct = 0;
  if (pct > 100) pct = 100;
  uint32_t duty = ((uint32_t)pct * BL_LEDC_MAX) / 100;
  ledcWrite(GFX_BL, duty);
}

// I2C slave addresses
#define ADDR_AXP    0x34
#define ADDR_TCA    0x20
#define ADDR_TOUCH  0x38
#define ADDR_RTC    0x51

// Shared I2C bus handle. The whole point of the architecture is that
// every I2C device on this board (touch, AXP, TCA, RTC, eventually
// camera SCCB) talks through one bus handle so the IDF i2c_master
// driver serializes everything internally and there are no
// driver-level races.
//
// Exposed as g_i2c_bus / g_rtc_dev so rtc_time.cpp (and later cam_qr)
// can attach without having to recreate the bus.
i2c_master_bus_handle_t g_i2c_bus  = nullptr;
i2c_master_dev_handle_t g_rtc_dev  = nullptr;
static i2c_master_dev_handle_t s_axp_dev   = nullptr;
static i2c_master_dev_handle_t s_tca_dev   = nullptr;
static i2c_master_dev_handle_t s_touch_dev = nullptr;

Arduino_DataBus *bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, SPI_SCLK, SPI_MOSI, SPI_MISO);
Arduino_GFX     *gfx = new Arduino_ST7796(bus, LCD_RST, 0, true, LCD_W, LCD_H);

// ---------------------------------------------------------------------------
// I2C low-level helpers — register-byte reads and writes through the
// shared bus. These are the building blocks for the per-peripheral
// helpers below (AXP, TCA, FT6X36 touch).
// ---------------------------------------------------------------------------
static esp_err_t devRead(i2c_master_dev_handle_t dev, uint8_t reg,
                         uint8_t* buf, size_t n) {
  if (!dev) return ESP_FAIL;
  return i2c_master_transmit_receive(dev, &reg, 1, buf, n, 100);
}
static esp_err_t devWrite(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val) {
  if (!dev) return ESP_FAIL;
  uint8_t b[2] = { reg, val };
  return i2c_master_transmit(dev, b, 2, 100);
}

// ---------------------------------------------------------------------------
// I2C bus initialization. Replaces Wire.begin + TCA9554 lib + SensorLib
// touch.begin from the old code path. Creates one bus handle on
// I2C_PORT_NUM=0 and registers each peripheral as a device on it.
// ---------------------------------------------------------------------------
static void i2cBusInit() {
  i2c_master_bus_config_t cfg = {};
  cfg.clk_source              = I2C_CLK_SRC_DEFAULT;
  cfg.i2c_port                = I2C_PORT_NUM;
  cfg.scl_io_num              = (gpio_num_t)I2C_SCL;
  cfg.sda_io_num              = (gpio_num_t)I2C_SDA;
  cfg.glitch_ignore_cnt       = 7;
  cfg.flags.enable_internal_pullup = 1;
  if (i2c_new_master_bus(&cfg, &g_i2c_bus) != ESP_OK) {
    Serial.println("[I2C] bus init FAILED");
    return;
  }

  i2c_device_config_t dev_cfg = {};
  dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  dev_cfg.scl_speed_hz    = 400000;

  dev_cfg.device_address = ADDR_AXP;
  i2c_master_bus_add_device(g_i2c_bus, &dev_cfg, &s_axp_dev);
  dev_cfg.device_address = ADDR_TCA;
  i2c_master_bus_add_device(g_i2c_bus, &dev_cfg, &s_tca_dev);
  dev_cfg.device_address = ADDR_TOUCH;
  i2c_master_bus_add_device(g_i2c_bus, &dev_cfg, &s_touch_dev);
  dev_cfg.device_address = ADDR_RTC;
  i2c_master_bus_add_device(g_i2c_bus, &dev_cfg, &g_rtc_dev);

  Serial.println("[I2C] bus initialised, devices added (AXP/TCA/touch/RTC)");
}

// ---------------------------------------------------------------------------
// TCA9554 IO expander helpers. Replaces the TCA9554 Arduino library.
// Registers: 0x00=Input, 0x01=Output, 0x02=Polarity, 0x03=Config (1=in,
// 0=out). We mirror config and output state in RAM so each operation
// is a single write rather than a read-modify-write.
// ---------------------------------------------------------------------------
static uint8_t tcaCfgMirror = 0xFF;   // power-on default: all pins are inputs
static uint8_t tcaOutMirror = 0xFF;   // power-on default

static void tcaSetDir(uint8_t pin, bool isOutput) {
  if (isOutput) tcaCfgMirror &= ~(1u << pin);
  else          tcaCfgMirror |=  (1u << pin);
  devWrite(s_tca_dev, 0x03, tcaCfgMirror);
}
static void tcaWrite(uint8_t pin, bool high) {
  if (high) tcaOutMirror |=  (1u << pin);
  else      tcaOutMirror &= ~(1u << pin);
  devWrite(s_tca_dev, 0x01, tcaOutMirror);
}
static int tcaRead(uint8_t pin) {
  uint8_t v = 0;
  if (devRead(s_tca_dev, 0x00, &v, 1) != ESP_OK) return 1;
  return (v >> pin) & 1;
}

// ---------------------------------------------------------------------------
// FT6X36 touch read. Replaces SensorLib's TouchDrvFT6X36.
// Register 0x02 is TD_STATUS (low 4 bits = number of touch points);
// then the first touch's coords in 0x03..0x06 (XH, XL, YH, YL).
// ---------------------------------------------------------------------------
static bool touchGetPoint(int16_t* x, int16_t* y) {
  uint8_t buf[5];
  if (devRead(s_touch_dev, 0x02, buf, 5) != ESP_OK) return false;
  if ((buf[0] & 0x0F) == 0) return false;
  *x = ((buf[1] & 0x0F) << 8) | buf[2];
  *y = ((buf[3] & 0x0F) << 8) | buf[4];
  return true;
}

// ---------------------------------------------------------------------------
// Buttons
//
// On-board K1 = hardware RESET (don't wire to a GPIO).
// On-board K2 = GPIO0 (BOOT button) -> used here as UP / back.
// On-board K3 = AXP2101 PWRON pin -> polling on the shared I2C bus
//   was unreliable in practice, so we don't use it. Holding K3 for
//   ~6+ seconds still powers the unit off via the AXP hardware path.
// ---------------------------------------------------------------------------
#define BTN_UP            0

static const char* BTN_NAMES[] = { "UP" };

#define BTN_MIN_LOW_MS    30
#define BTN_COOLDOWN_MS  300

static uint32_t btnUpLowSince  = 0;
static uint32_t btnUpLastEvent = 0;
static bool     btnUpWasLow    = false;

// AXP2101 PMIC — register helpers and battery monitoring.
// Register map from the XPowersLib AXP2101Constants.h.
#define AXP_ADDR              0x34

// Status registers
#define AXP_STATUS1           0x00   // bit5=vbusGood, bit3=battConnect
#define AXP_STATUS2           0x01   // bits[7:5]=chargeState (0=standby,1=chg,2=dischg)
#define AXP_ADC_CHAN_CTRL     0x30   // bit0=battV, bit2=vbusV, bit3=sysV
#define AXP_ADC_BATT_H       0x34   // battery voltage H5L8 (mV)
#define AXP_ADC_BATT_L       0x35
#define AXP_ADC_VBUS_H       0x38   // VBUS voltage H6L8 (mV)
#define AXP_ADC_VBUS_L       0x39
#define AXP_BAT_DET_CTRL     0x68   // bit0=enable battery detection
#define AXP_FUEL_GAUGE_CTRL  0xA2   // bit0=enable fuel gauge
#define AXP_BAT_PERCENT      0xA4   // 0..100 direct read

static uint8_t axpReadReg(uint8_t reg) {
  uint8_t v = 0;
  return (devRead(s_axp_dev, reg, &v, 1) == ESP_OK) ? v : 0;
}

static bool axpWriteReg(uint8_t reg, uint8_t val) {
  return devWrite(s_axp_dev, reg, val) == ESP_OK;
}

// Enable ADC channels and fuel gauge. Call once at boot.
static void axpInitBattery() {
  // Enable battery detection
  uint8_t det = axpReadReg(AXP_BAT_DET_CTRL);
  axpWriteReg(AXP_BAT_DET_CTRL, det | 0x01);
  // Enable ADC channels: batt voltage (bit0), vbus voltage (bit2), sys voltage (bit3)
  uint8_t adc = axpReadReg(AXP_ADC_CHAN_CTRL);
  axpWriteReg(AXP_ADC_CHAN_CTRL, adc | 0x0D);
  // Enable fuel gauge
  uint8_t fg = axpReadReg(AXP_FUEL_GAUGE_CTRL);
  axpWriteReg(AXP_FUEL_GAUGE_CTRL, fg | 0x01);
  Serial.println("[AXP] Battery monitoring enabled");
}

// Re-enable all external peripheral LDO rails on the AXP2101.
// Called once at boot. Required because:
//   1. We turn most rails off before deep sleep to save power.
//   2. The AXP2101 retains rail-enable state across full power cycles.
//      An earlier firmware iteration that turned ALDO2/3 off would
//      leave them off forever otherwise.
//
// Reg 0x90: ALDO1 (b0), ALDO2 (b1), ALDO3 (b2), ALDO4 (b3),
//           BLDO1 (b4), BLDO2 (b5), DLDO1 (b6), CPUSLDO (b7)
// Reg 0x91: DLDO2 (b0)
//
// We OR in 0x7F to set all external rail bits (b0..b6) and never
// touch bit 7 (CPUSLDO is internal to the AXP — manipulating it can
// brick the regulator config).
static void axpEnablePeripheralRails() {
  uint8_t r90 = axpReadReg(0x90);
  r90 |= 0x7F;     // ALDO1-4 + BLDO1-2 + DLDO1; preserves CPUSLDO bit
  axpWriteReg(0x90, r90);

  uint8_t r91 = axpReadReg(0x91);
  r91 |= 0x01;     // DLDO2
  axpWriteReg(0x91, r91);
  Serial.printf("[AXP] Peripheral rails enabled (0x90=0x%02X, 0x91=0x%02X)\n",
                r90, r91);
}

// Battery percentage (0..100), or -1 if no battery.
int axpBatteryPercent() {
  if (!(axpReadReg(AXP_STATUS1) & 0x08)) return -1; // no battery
  return (int)axpReadReg(AXP_BAT_PERCENT);
}

// Battery voltage in millivolts, or 0 if no battery.
uint16_t axpBatteryMv() {
  if (!(axpReadReg(AXP_STATUS1) & 0x08)) return 0;
  uint8_t h = axpReadReg(AXP_ADC_BATT_H);
  uint8_t l = axpReadReg(AXP_ADC_BATT_L);
  return (uint16_t)(((h & 0x1F) << 8) | l);
}

// USB VBUS connected?
bool axpIsUsb() {
  return (axpReadReg(AXP_STATUS1) & 0x20) != 0;
}

// Currently charging?
bool axpIsCharging() {
  return ((axpReadReg(AXP_STATUS2) >> 5) & 0x07) == 0x01;
}

// Extern C wrappers for ui.cpp
extern "C" {
  int  ui_get_battery_pct()  { return axpBatteryPercent(); }
  int  ui_get_battery_mv()   { return (int)axpBatteryMv(); }
  bool ui_get_usb_status()   { return axpIsUsb(); }
  bool ui_get_charging()     { return axpIsCharging(); }
}

// ---------------------------------------------------------------------------
// LVGL plumbing
// ---------------------------------------------------------------------------
static lv_disp_draw_buf_t draw_buf;
static lv_color_t *disp_buf1;
static lv_color_t *disp_buf2;
static lv_disp_drv_t  disp_drv;
static lv_indev_drv_t indev_drv;

// ---------------------------------------------------------------------------
// Idle / sleep (three-stage: active → dim → deep sleep)
//
// On the Codes screen we use the cycle-based rule (align sleep with
// 30s TOTP windows); on every other screen we use the timer-based
// rule from nvs_settings (dim_s then sleep_s after last activity).
//
// The Codes-screen path gets an equivalent two-stage: dim at the
// first cycle boundary where the dim timeout has elapsed, sleep at
// the boundary where the sleep timeout has elapsed.
// ---------------------------------------------------------------------------
static uint32_t lastActivityMs    = 0;
static int      idleCycleCount    = 0;
static uint32_t lastCycleBoundary = 0;
static bool     sessionInProgress = false;
static bool     screenDimmed      = false;

// USB Reader Mode flag — when true, the device:
//   1. Allows the host CCID driver to access the smart card (handlers
//      in Phase 6c will check this).
//   2. Does NOT enter deep sleep on idle (host might lose connection).
//   3. Continues to dim the backlight normally on idle.
// Set/cleared by ui_show_usb_reader / on_back_to_main_menu.
static bool     usbReaderActive   = false;

void recordActivity() {
  lastActivityMs = millis();
  idleCycleCount = 0;
  if (screenDimmed) {
    // User did something — undo the dim.
    backlight_set_pct(nvs_settings_get_brightness_pct());
    screenDimmed = false;
  }
}

bool ui_is_usb_reader_active() {
  return usbReaderActive;
}
void ui_set_usb_reader_active(bool on) {
  usbReaderActive = on;
  Serial.printf("[USBR] reader mode %s\n", on ? "ON" : "OFF");
  if (on) recordActivity();   // reset idle so dim doesn't fire instantly
}

// Drive the three stages. Called from loop().
void checkSleep() {
  if (sessionInProgress) return;

  uint32_t idle_ms  = millis() - lastActivityMs;
  uint16_t dim_s    = nvs_settings_get_dim_s();     // dim duration before sleep
  uint16_t sleep_s  = nvs_settings_get_sleep_s();   // total idle before sleep
  bool     onCodes  = ui_is_on_codes_screen() && rtc_isRunning();

  // Dim fires at (sleep_s - dim_s) idle. Guard against underflow if
  // user somehow got dim_s > sleep_s.
  uint32_t dim_start_ms = (sleep_s > dim_s)
                          ? (uint32_t)(sleep_s - dim_s) * 1000UL
                          : 0UL;

  // Stage 1 — active → dim.
  if (!screenDimmed && idle_ms >= dim_start_ms) {
    uint8_t active = nvs_settings_get_brightness_pct();
    uint8_t dim    = (uint8_t)((active * BL_DIM_RATIO_NUM) / BL_DIM_RATIO_DEN);
    if (dim < 2) dim = 2;
    backlight_set_pct(dim);
    screenDimmed = true;
    Serial.printf("[SLEEP] dimmed at %us idle (dim duration=%us)\n",
                  (unsigned)(dim_start_ms / 1000), (unsigned)dim_s);
  }

  // Stage 2 — dim → deep sleep.
  bool shouldSleep = false;
  if (onCodes) {
    // Codes screen: sleep on cycle boundaries once the sleep timer
    // has elapsed. This keeps the final code-refresh aligned.
    uint32_t boundary = (uint32_t)(rtc_epoch() / 30) * 30;
    if (boundary != lastCycleBoundary) {
      bool activeThisCycle = idle_ms < 30000;
      idleCycleCount    = activeThisCycle ? 0 : idleCycleCount + 1;
      lastCycleBoundary = boundary;
      Serial.printf("[SLEEP] cycle boundary - idle cycles: %d  idle_ms=%lu\n",
                    idleCycleCount, (unsigned long)idle_ms);
    }
    shouldSleep = (idle_ms >= (uint32_t)sleep_s * 1000UL);
  } else {
    if (idle_ms >= (uint32_t)sleep_s * 1000UL) {
      Serial.printf("[SLEEP] Idle %us on %s screen\n",
                    (unsigned)sleep_s,
                    ui_is_on_pin_screen() ? "PIN" : "menu");
      shouldSleep = true;
    }
  }

  if (shouldSleep) {
    // Reader Mode keeps the device alive ONLY when USB is actually
    // connected. If the user enters Reader Mode but no host is plugged
    // in, allow sleep to save battery. (Without this, leaving the
    // device on the Reader screen unattended would never sleep.)
    if (usbReaderActive && axpIsUsb()) {
      idleCycleCount = 0;
      return;
    }
    Serial.println("[SLEEP] Entering deep sleep");
    sc_deactivate();
    backlight_set_pct(0);

    // NOTE: We previously experimented with disabling AXP2101
    // peripheral LDO rails before sleep to reduce idle current
    // (deep sleep was measuring ~36 mA average, much higher than
    // the ESP32-S3's ~10 µA spec). The disabling worked, but
    // some rails turned out to be needed for touch / TCA / RTC,
    // and the AXP2101 retains rail-enable state across resets,
    // bricking those peripherals until a full battery
    // disconnect-reconnect cycle. Reverting to a simple sleep
    // flow until we have proper measurement equipment to map
    // exactly which rail powers what.

    delay(200);
    esp_sleep_enable_ext0_wakeup((gpio_num_t)BTN_UP, 0);
    esp_deep_sleep_start();
  }
}

// ---------------------------------------------------------------------------
// LVGL display / touch callbacks
// ---------------------------------------------------------------------------
static void disp_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px) {
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;
#if LV_COLOR_16_SWAP
  gfx->draw16bitBeRGBBitmap(area->x1, area->y1, (uint16_t*)&px->full, w, h);
#else
  gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t*)&px->full, w, h);
#endif
  lv_disp_flush_ready(drv);
}

static void touch_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  int16_t x = 0, y = 0;
  if (touchGetPoint(&x, &y)) {
    data->state   = LV_INDEV_STATE_PR;
    data->point.x = x;
    data->point.y = y;
    recordActivity();
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
}

static void lcd_reset() {
  tcaWrite(1, true);  delay(10);
  tcaWrite(1, false); delay(10);
  tcaWrite(1, true);  delay(200);
}

// ---------------------------------------------------------------------------
// Card / auth orchestration
//
// New flow (Phase A):
//   - Card insert lands on Main Menu with a single "TOTP" tile. No SELECT
//     happens automatically. (Future: Main Menu will hold tiles for other
//     applets too.)
//   - Tapping "TOTP" runs sc_run_session() (SELECT). If the applet needs a
//     PIN, the PIN screen is shown next. After validate, we land on the
//     TOTP Menu (View Codes / Add via QR / Manual / Manage).
//   - Authentication state persists for as long as the card is inserted.
//     Re-entering the TOTP Menu after backing out doesn't re-prompt.
// ---------------------------------------------------------------------------
static void onPinSubmit(const char* pin);
static void enterTotpMenu();

static void promptForPin(const char* msg) {
  ui_show_pin_entry(msg, onPinSubmit);
  lv_timer_handler();
}

static void onPinSubmit(const char* pin) {
  Serial.println("[MAIN] PIN submitted, calling VALIDATE...");
  sessionInProgress = true;
  ui_show_status("Authenticating...");
  lv_timer_handler();

  bool ok = sc_authenticate(pin, rtc_epoch());
  sessionInProgress = false;

  bool pin_evt_on = (nvs_settings_get_audio_events() & AUDIO_EVT_PIN) != 0;

  if (ok) {
    if (pin_evt_on) {
      // Two-tone ascending: success
      audio_beep(800,  60);
      audio_beep(1600, 60);
    }
    ui_show_totp_menu();
    recordActivity();
    return;
  }

  if (pin_evt_on) {
    // Low buzz: fail
    audio_beep(200, 250);
  }

  const SCState& s = sc_state();
  char msg[64];
  if (s.attemptsRemaining > 0) {
    snprintf(msg, sizeof(msg), "Wrong PIN - %d attempt%s left",
             s.attemptsRemaining, s.attemptsRemaining == 1 ? "" : "s");
    promptForPin(msg);
  } else if (s.attemptsRemaining == 0) {
    ui_show_status("Card locked");
  } else {
    promptForPin("Wrong PIN");
  }
}

static void enterTotpMenu() {
  // Run SELECT lazily — only the first time within a card session.
  if (!sc_state().appletSelected) {
    sessionInProgress = true;
    ui_show_status("Reading card...");
    lv_timer_handler();
    rtc_update();
    sc_run_session(rtc_epoch());
    sessionInProgress = false;
  }

  const SCState& s = sc_state();
  if (!s.appletSelected) {
    ui_show_status("Card error - applet not available");
    return;
  }
  if (s.needsAuth && !s.authenticated) {
    promptForPin("Unlock TOTP");
    return;
  }
  ui_show_totp_menu();
}

// ---------------------------------------------------------------------------
// Hooks called by ui.cpp on tile / back events
// ---------------------------------------------------------------------------
// Exposed for ui.cpp — Settings brightness slider calls this live
// while the user drags so the new level is visible immediately.
extern "C" void backlight_set_pct_live(int pct) {
  backlight_set_pct(pct);
}

extern "C" void onMainMenuTotpTap()     { enterTotpMenu(); }
extern "C" void onTotpMenuViewCodes() {
  // Force a fresh CALCULATE before showing — the user may have lingered
  // on TOTP Menu through one or more 30s cycles, and we don't poll there.
  if (sc_state().appletSelected &&
      (!sc_state().needsAuth || sc_state().authenticated)) {
    sessionInProgress = true;
    sc_recalculate(rtc_epoch());
    sessionInProgress = false;
  }
  ui_show_codes();
}
// ---------------------------------------------------------------------------
// QR scan mode (blocking). Called from the Add-via-QR tile.
//
// Architecture note: this runs on the Arduino loop task (Core 1). We
// keep servicing lv_timer_handler + the serial console + the I2C
// peripherals here, exactly as loop() does, while the camera task
// (Core 0) produces frames and runs quirc_decode. Because only this
// one task touches LVGL, no mutex is needed — we're single-threaded
// from LVGL's perspective.
// ---------------------------------------------------------------------------
static void runQrScanMode() {
  Serial.println("[QR] scan mode entered");
  ui_show_qr_scan();
  lv_timer_handler();   // let LVGL paint the new screen before we block on camera init

  // Camera SCCB probe is sometimes flaky on the first attempt —
  // retry once before giving up. Between attempts we deinit the
  // camera fully so state is clean.
  bool cam_ok = cam_qr_start_task();
  if (!cam_ok) {
    Serial.println("[QR] camera init failed, retrying once");
    cam_qr_stop_task();
    delay(100);
    cam_ok = cam_qr_start_task();
  }
  if (!cam_ok) {
    Serial.println("[QR] camera init failed permanently");
    ui_show_totp_menu();
    lv_obj_invalidate(lv_scr_act());
    return;
  }
  Serial.println("[QR] scan loop begin");

  // K2 debounce: require a fresh release-then-press (user may have
  // navigated here holding K2).
  bool k2WasReleased = (digitalRead(BTN_UP) == HIGH);

  bool        detected    = false;
  bool        cancelled   = false;
  uint32_t    deadline    = millis() + 60000;  // 60 s auto-timeout
  char        payload[512]= {0};
  lv_img_dsc_t dsc        = {};

  while ((int32_t)(deadline - millis()) > 0) {
    lv_timer_handler();

    // K2 cancel
    if (digitalRead(BTN_UP) == HIGH) {
      k2WasReleased = true;
    } else if (k2WasReleased) {
      cancelled = true;
      while (digitalRead(BTN_UP) == LOW) delay(10);  // wait for release
      break;
    }

    int r = cam_qr_poll(&dsc, payload, sizeof(payload));
    if (r == CAM_POLL_NEW_FRAME) {
      ui_qr_set_frame(&dsc);
    } else if (r == CAM_POLL_DETECTED) {
      if (dsc.data) ui_qr_set_frame(&dsc);
      detected = true;
      break;
    }

    delay(1);   // let IDLE on Core 1 run; also paces loop vs LVGL task
  }

  cam_qr_stop_task();
  Serial.printf("[QR] exit: detected=%d cancelled=%d\n", detected, cancelled);

  // All exit paths return to the TOTP menu. We deliberately do NOT
  // call ui_show_status() here — that routes the user back to the
  // Main Menu (it's intended as a transient banner on the "insert
  // card" screen) and with the TOTP tile hidden, they can end up
  // stranded. Status is already reported on the serial console for
  // diagnostics.
  if (cancelled) {
    ui_show_totp_menu();
    lv_obj_invalidate(lv_scr_act());
    return;
  }
  if (!detected) {
    Serial.println("[QR] no QR detected");
    ui_show_totp_menu();
    lv_obj_invalidate(lv_scr_act());
    return;
  }

  // Parse & save.
  ui_qr_set_hint("Saving credential...");
  lv_timer_handler();

  OtpAuthCred cred;
  if (!cam_qr_parse_otpauth(payload, &cred)) {
    Serial.println("[QR] not a usable otpauth:// TOTP URI");
    ui_show_totp_menu();
    lv_obj_invalidate(lv_scr_act());
    return;
  }
  Serial.printf("[QR] parsed name='%s' digits=%u algo=0x%02X period=%us\n",
                cred.name_for_card, cred.digits, cred.type_algo,
                (unsigned)cred.period);

  if (!sc_state().authenticated) {
    Serial.println("[QR] card not authenticated");
    memset(cred.secret, 0, sizeof(cred.secret));
    ui_show_totp_menu();
    lv_obj_invalidate(lv_scr_act());
    return;
  }

  bool ok = sc_put_credential(cred.name_for_card,
                              cred.secret, cred.secret_len,
                              cred.type_algo, cred.digits,
                              0 /* imf — TOTP */);
  memset(cred.secret, 0, sizeof(cred.secret));
  Serial.printf("[QR] sc_put_credential: %s\n", ok ? "OK" : "FAIL");

  if (ok) {
    sc_recalculate(rtc_epoch());
  }
  ui_show_totp_menu();
  lv_obj_invalidate(lv_scr_act());
  ui_toast(ok ? "Credential added" : "Card rejected");
}

// The QR tile tap fires this from inside an LVGL click callback
// (on_totp_qr_tile -> onTotpMenuAddQR). LVGL is not re-entrant, so
// we cannot call runQrScanMode directly from here — nested calls to
// lv_timer_handler are undefined. Instead, set a flag and let loop()
// pick it up on the next iteration, after the click callback has
// returned and LVGL is back to idle.
static volatile bool s_qr_scan_pending = false;

extern "C" void onTotpMenuAddQR()       { s_qr_scan_pending = true; }
extern "C" void onTotpMenuManualEntry() { ui_show_manual_entry(); }
extern "C" void onTotpMenuManage() {
  // "Change PIN" tile on TOTP Menu. Allow if authenticated OR if the
  // card has no PIN (then the user is setting a PIN for the first
  // time, or re-setting after a removal).
  const SCState& s = sc_state();
  if (s.needsAuth && !s.authenticated) {
    ui_toast("Unlock card first");
    return;
  }
  if (!s.appletSelected) {
    ui_toast("Select TOTP first");
    return;
  }
  ui_show_pin_change();
}

extern "C" void onDeleteSelected(const char* const* names, int count) {
  if (count <= 0) return;
  Serial.printf("[MAIN] Deleting %d credential(s)\n", count);
  sessionInProgress = true;
  for (int i = 0; i < count; i++) {
    if (names[i]) sc_delete_credential(names[i]);
  }
  sc_recalculate(rtc_epoch());
  sessionInProgress = false;
}

// Card / state changes from the main loop
static void handleCardInserted() {
  Serial.println("[MAIN] Card inserted");
  recordActivity();

  // Show "Reading card..." while we probe
  ui_show_status("Reading card...");
  lv_timer_handler();

  // Probe the card for known applets (no PIN required).
  CardProbeResult probe = sc_probe_card();
  // DEBUG: force PIV tile visible regardless of probe result, so we
  // can inspect the SELECT failure on-screen via Card Info. Revert
  // once federal-PIV probe is working.
  ui_set_card_applets(probe.oath, probe.fido2, /*piv*/ true);
}

// ---------------------------------------------------------------------------
// Buttons
// ---------------------------------------------------------------------------
void onButtonPress(int idx) {
  Serial.printf("[BTN] %s\n", BTN_NAMES[idx]);
  recordActivity();
  // Click beep on K2 / hardware buttons. Same toggle category as
  // tile/keyboard clicks. 2-second boot grace.
  if (millis() >= 2000 &&
      (nvs_settings_get_audio_events() & AUDIO_EVT_KEYTAP)) {
    audio_click(12);
  }
  ui_handle_button(idx);
}

void handleButtons() {
  uint32_t now = millis();

  // UP (GPIO0): debounced edge detect on falling edge release
  bool upLow = (digitalRead(BTN_UP) == LOW);
  if (upLow && !btnUpWasLow) {
    btnUpWasLow  = true;
    btnUpLowSince = now;
  } else if (!upLow && btnUpWasLow) {
    btnUpWasLow = false;
    uint32_t held = now - btnUpLowSince;
    if (held >= BTN_MIN_LOW_MS && (now - btnUpLastEvent) >= BTN_COOLDOWN_MS) {
      btnUpLastEvent = now;
      onButtonPress(0);     // 0 = UP
    }
  }
}

// ---------------------------------------------------------------------------
// Serial commands
// ---------------------------------------------------------------------------
void handleSerial() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;

  if (line.startsWith("TIME ")) {
    int y, mo, d, h, mi, s;
    if (sscanf(line.c_str() + 5, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6) {
      rtc_setTime(y, mo, d, h, mi, s);
      ui_refresh_codes();
    } else {
      Serial.println("Usage: TIME YYYY-MM-DD HH:MM:SS");
    }
  } else if (line.startsWith("PIN ")) {
    // For testing without the touch UI: PIN <pin-string>
    onPinSubmit(line.c_str() + 4);
  } else if (line == "SCAN") {
    rtc_update();
    handleCardInserted();
  } else if (line == "STATUS") {
    char dtStr[40]; rtc_dateTimeStr(dtStr, sizeof(dtStr));
    const SCState &s = sc_state();
    Serial.printf("FW: %s\n", FW_VERSION);
    Serial.printf("RTC: %s\n", dtStr);
    Serial.printf("Battery: %d%%  %umV  USB:%s  Charging:%s\n",
                  axpBatteryPercent(), axpBatteryMv(),
                  axpIsUsb() ? "YES" : "NO",
                  axpIsCharging() ? "YES" : "NO");
    Serial.printf("Card: %s  Applet: %s  NeedsPIN: %s  Authed: %s  Creds: %d\n",
                  s.cardPresent ? "YES" : "NO",
                  s.appletSelected ? "YES" : "NO",
                  s.needsAuth ? "YES" : "NO",
                  s.authenticated ? "YES" : "NO",
                  s.numCredentials);
    for (int i = 0; i < s.numCredentials; i++) {
      Serial.printf("  [%d] %-40s  %s\n", i,
                    s.credentials[i].name,
                    s.credentials[i].valid ? "[code present]" : "[no code]");
    }
  } else if (line == "BTN") {
    Serial.println("Button states (LOW = pressed):");
    Serial.printf("  UP   GPIO%-2d         level=%s\n", BTN_UP,
                  digitalRead(BTN_UP) == LOW ? "LOW (pressed)" : "HIGH");
  } else if (line.startsWith("TOTP ")) {
    String t = line.substring(5); t.trim();

    if (t.startsWith("PUT ")) {
      // TOTP PUT <n> <base32-secret> [digits] [type]
      //   digits defaults to 6
      //   type defaults to 0x21 (TOTP+SHA1). Hex byte (e.g. 0x22 for TOTP+SHA256).
      String rest = t.substring(4); rest.trim();
      int sp1 = rest.indexOf(' ');
      if (sp1 < 0) { Serial.println("Usage: TOTP PUT <n> <base32> [digits] [typeHex]"); return; }
      String name = rest.substring(0, sp1);
      String tail = rest.substring(sp1 + 1); tail.trim();

      int sp2 = tail.indexOf(' ');
      String b32 = (sp2 < 0) ? tail : tail.substring(0, sp2);
      int     digits   = 6;
      uint8_t typeAlgo = 0x21;
      if (sp2 >= 0) {
        String tail2 = tail.substring(sp2 + 1); tail2.trim();
        int sp3 = tail2.indexOf(' ');
        String dStr = (sp3 < 0) ? tail2 : tail2.substring(0, sp3);
        digits = dStr.toInt();
        if (sp3 >= 0) {
          String tStr = tail2.substring(sp3 + 1); tStr.trim();
          typeAlgo = (uint8_t)strtol(tStr.c_str(), nullptr, 0);
        }
      }

      uint8_t secret[64];
      int secretLen = sc_base32_decode(b32.c_str(), secret, sizeof(secret));
      if (secretLen <= 0) { Serial.println("[ERR] bad base32 secret"); return; }
      Serial.printf("[MAIN] TOTP PUT '%s' digits=%d type=0x%02X secretLen=%d\n",
                    name.c_str(), digits, typeAlgo, secretLen);
      if (sc_put_credential(name.c_str(), secret, secretLen,
                            typeAlgo, (uint8_t)digits, 0)) {
        sessionInProgress = true;
        sc_recalculate(rtc_epoch());
        sessionInProgress = false;
        ui_refresh_codes();
      }
      memset(secret, 0, sizeof(secret));

    } else if (t.startsWith("DEL ")) {
      String name = t.substring(4); name.trim();
      if (sc_delete_credential(name.c_str())) {
        sessionInProgress = true;
        sc_recalculate(rtc_epoch());
        sessionInProgress = false;
        ui_refresh_codes();
      }

    } else if (t == "LIST") {
      const SCState &s = sc_state();
      Serial.printf("Credentials on card: %d\n", s.numCredentials);
      for (int i = 0; i < s.numCredentials; i++) {
        Serial.printf("  [%d] %-40s  %s\n", i,
                      s.credentials[i].name,
                      s.credentials[i].valid ? "[code present]" : "[no code]");
      }

    } else if (t.startsWith("AID ")) {
      String rest = t.substring(4); rest.trim();
      if (rest == "LIST") {
        int n = nvs_aid_count();
        Serial.printf("Stored AIDs: %d / %d (highest priority first)\n",
                      n, NVS_AID_MAX_COUNT);
        for (int i = 0; i < n; i++) {
          AidEntry e;
          if (!nvs_aid_get(i, &e)) continue;
          Serial.printf("  [%d] %-20s ", i, e.name);
          for (int b = 0; b < e.aid_len; b++) Serial.printf("%02X", e.aid[b]);
          Serial.println();
        }
        Serial.println("  (default APEX AID is always tried as final fallback)");
      } else if (rest.startsWith("ADD ")) {
        String tt = rest.substring(4); tt.trim();
        int sp = tt.indexOf(' ');
        if (sp < 0) { Serial.println("Usage: TOTP AID ADD <hex> <n>"); return; }
        String hex  = tt.substring(0, sp);
        String name = tt.substring(sp + 1); name.trim();
        // Strip optional ':'/'-'/space separators from hex.
        String clean = "";
        for (size_t k = 0; k < hex.length(); k++) {
          char c = hex.charAt(k);
          if (c == ' ' || c == ':' || c == '-') continue;
          clean += c;
        }
        if ((clean.length() & 1) || clean.length() == 0 ||
            clean.length() / 2 > NVS_AID_MAX_BYTES) {
          Serial.printf("[ERR] AID hex must be 1..%d bytes (even hex digits)\n",
                        NVS_AID_MAX_BYTES);
          return;
        }
        uint8_t aid[NVS_AID_MAX_BYTES];
        uint8_t aid_len = (uint8_t)(clean.length() / 2);
        for (uint8_t b = 0; b < aid_len; b++) {
          char hi = clean.charAt(b*2), lo = clean.charAt(b*2 + 1);
          auto h2v = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
          };
          int hv = h2v(hi), lv = h2v(lo);
          if (hv < 0 || lv < 0) { Serial.println("[ERR] AID hex parse error"); return; }
          aid[b] = (uint8_t)((hv << 4) | lv);
        }
        if (!nvs_aid_add(aid, aid_len, name.c_str())) {
          Serial.println("[ERR] AID add failed (list full?)");
        }
      } else if (rest.startsWith("DEL ")) {
        int idx = rest.substring(4).toInt();
        if (!nvs_aid_remove(idx)) Serial.println("[ERR] TOTP AID DEL: bad index");
      } else {
        Serial.println("Usage: TOTP AID LIST | TOTP AID ADD <hex> <n> | TOTP AID DEL <idx>");
      }

    } else {
      Serial.println("Usage: TOTP PUT|DEL|LIST|AID <subcmd>");
    }
  } else if (line == "HELP") {
    Serial.println("Authuino " FW_VERSION);
    Serial.println("General:");
    Serial.println("  TIME YYYY-MM-DD HH:MM:SS    Set RTC (UTC)");
    Serial.println("  SCAN                         Run card session");
    Serial.println("  PIN <pin>                    Submit PIN (testing)");
    Serial.println("  BTN                          Read raw button GPIO states");
    Serial.println("  STATUS                       Print firmware/RTC/card status");
    Serial.println("TOTP:");
    Serial.println("  TOTP PUT <n> <b32> [digits] [t]   Add credential");
    Serial.println("  TOTP DEL <n>                       Delete credential by name");
    Serial.println("  TOTP LIST                          List cached credentials");
    Serial.println("  TOTP AID LIST                      List stored AIDs");
    Serial.println("  TOTP AID ADD <hex> <n>             Add AID (newest = highest priority)");
    Serial.println("  TOTP AID DEL <idx>                 Remove AID by index");
    Serial.println("  HELP                         This message");
  } else {
    Serial.printf("Unknown: %s (try HELP)\n", line.c_str());
  }
}

// ---------------------------------------------------------------------------
// 30s TOTP refresh
// ---------------------------------------------------------------------------
static uint32_t lastToTpStep = 0;
static int8_t   lastTickSecs = -1;   // last "seconds remaining" we ticked on

void checkToTpRefresh() {
  if (!rtc_isRunning()) return;
  uint32_t step = (uint32_t)(rtc_epoch() / 30);
  if (step == lastToTpStep) {
    // Same step — check if we should emit a last-5s tick.
    if (ui_is_on_codes_screen() &&
        (nvs_settings_get_audio_events() & AUDIO_EVT_TICK)) {
      time_t now = rtc_epoch();
      int secs_into = (int)(now % 30);
      int secs_left = 30 - secs_into;       // 1..30
      if (secs_left <= 5 && secs_left >= 1 && secs_left != lastTickSecs) {
        lastTickSecs = secs_left;
        audio_click(8);                     // metronome-style tick
      }
    }
    return;
  }
  lastToTpStep = step;
  lastTickSecs = -1;                         // reset for new window

  // Only poll the card when the user is actually looking at codes.
  // (TOTP Menu, Main Menu, stub screens, PIN screen — all silent.)
  if (!ui_is_on_codes_screen()) return;

  const SCState& s = sc_state();
  if (s.appletSelected && (!s.needsAuth || s.authenticated)) {
    Serial.println("[TOTP] New 30s cycle");
    sessionInProgress = true;
    sc_recalculate(rtc_epoch());
    sessionInProgress = false;
    ui_refresh_codes();
  }
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
void setup() {
  // CCID-only USB at boot. CDC + CCID composite is broken in
  // arduino-esp32 v3.2.1 regardless of ordering, so we live without
  // USB serial. Debug output goes to UART (Serial.print routes to
  // hardware UART since CDC On Boot is Disabled).
  usb_ccid_init();
#if !ARDUINO_USB_MODE && !ARDUINO_USB_CDC_ON_BOOT
  USB.begin();
#endif

  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== Authuino " FW_VERSION " ===");

  pinMode(BTN_UP, INPUT_PULLUP);
  btnUpWasLow    = false;
  btnUpLastEvent = millis();

  // Initialise the shared I2C bus (replaces Wire.begin + TCA9554 lib +
  // SensorLib TouchDrvFT6X36 init). Everything that talks I2C —
  // touch, AXP, TCA, RTC, eventually camera SCCB — uses g_i2c_bus.
  i2cBusInit();
  // Defensively re-enable all peripheral rails on the AXP2101 in case
  // prior firmware iterations left some disabled. The AXP retains
  // rail-enable state in NVRAM across full power cycles, so this
  // recovery path is needed at least once after running any code that
  // disabled rails. Safe to call every boot — it only ORs bits on.
  axpEnablePeripheralRails();
  delay(200);
  tcaSetDir(1, true);   // EXIO1 = LCD reset, output
  axpInitBattery();     // enable ADC + fuel gauge for battery monitoring
  lcd_reset();

  // FT6X36 needs no init beyond the bus device handle (s_touch_dev,
  // registered in i2cBusInit). Just announce status.
  Serial.println("[OK] Touch initialised");

  if (!gfx->begin()) Serial.println("[ERR] GFX init failed");
  else               Serial.println("[OK] Display initialised");
  gfx->fillScreen(RGB565_BLACK);
  backlight_init();
  // Brightness comes from NVS (init'd below); until then run dark to
  // avoid flashing full-white for a frame on cold boot.
  backlight_set_pct(0);

  // LVGL
  lv_init();
  uint32_t bufSize = LCD_W * 120;
  disp_buf1 = (lv_color_t*)heap_caps_malloc(bufSize * 2, MALLOC_CAP_DEFAULT | MALLOC_CAP_8BIT);
  disp_buf2 = (lv_color_t*)heap_caps_malloc(bufSize * 2, MALLOC_CAP_DEFAULT | MALLOC_CAP_8BIT);
  lv_disp_draw_buf_init(&draw_buf, disp_buf1, disp_buf2, bufSize);
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res  = LCD_W; disp_drv.ver_res = LCD_H;
  disp_drv.flush_cb = disp_flush; disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);
  lv_indev_drv_init(&indev_drv);
  indev_drv.type    = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = touch_read;
  lv_indev_drv_register(&indev_drv);

  ui_init();

  rtc_init();
  nvs_meta_init();
  nvs_aid_init();
  nvs_settings_init();
  cam_qr_init();
  sc_init();

  // Audio codec — uses the shared I2C bus and I2S0. Non-fatal if it
  // fails; beep calls are gated on audio_is_ready().
  audio_init(g_i2c_bus);

  // Accelerometer / gyroscope — shared I2C bus. Non-fatal if missing.
  accel_init(g_i2c_bus);

  // Now we have saved brightness — apply it.
  backlight_set_pct(nvs_settings_get_brightness_pct());

  // Boot beep deliberately disabled — kept here as reference for
  // when audio settings UI is built. Enable from settings later.
  // if (audio_is_ready()) {
  //   audio_beep(800,  40);
  //   audio_beep(1600, 40);
  // }

  recordActivity();
  lastCycleBoundary = rtc_isRunning() ? (uint32_t)(rtc_epoch() / 30) * 30 : 0;
  lastToTpStep      = rtc_isRunning() ? (uint32_t)(rtc_epoch() / 30) : 0;

  ui_show_main_menu();
  Serial.println("[OK] Ready — insert card or send HELP");
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------
void loop() {
  lv_timer_handler();

  // Pick up any pending mode-switches. LVGL event callbacks can't
  // safely call blocking modes that re-enter lv_timer_handler, so
  // they set a flag; we act on it here with LVGL back at idle.
  if (s_qr_scan_pending) {
    s_qr_scan_pending = false;
    runQrScanMode();
  }

  // Refresh RTC cache periodically. rtc_epoch() extrapolates from
  // millis() between reads, so we only need to hit hardware every
  // few seconds. Calling rtc_update() every loop tick (~100 Hz)
  // floods the shared I2C bus and starves touch polling.
  static uint32_t lastRtcMs = 0;
  uint32_t nowMs = millis();
  if (nowMs - lastRtcMs >= 5000) {
    rtc_update();
    lastRtcMs = nowMs;
  }

  ui_update_timer();
  ui_hardware_tick();
  ui_usb_reader_tick();
  usb_ccid_tick();
  checkToTpRefresh();

  bool cardChanged = sc_poll();
  if (cardChanged) {
    bool card_evt_on = (nvs_settings_get_audio_events() & AUDIO_EVT_CARD) != 0;
    static uint32_t lastCardBeepMs = 0;
    bool boot_quiet = millis() < 2000;
    bool beep_ok = card_evt_on && !boot_quiet && (millis() - lastCardBeepMs > 500);
    bool on_reader_screen = ui_is_usb_reader_active();
    if (sc_state().cardPresent) {
      if (beep_ok) { audio_beep(1500, 50); lastCardBeepMs = millis(); }
      // Probe the card for applets, but do NOT switch screens if the
      // user is on the USB Reader screen (host owns the card there)
      // or already drilling around in Settings.
      if (on_reader_screen) {
        CardProbeResult probe = sc_probe_card();
        // DEBUG: see comment in setup() — force PIV tile on.
        ui_set_card_applets(probe.oath, probe.fido2, /*piv*/ true);
      } else {
        handleCardInserted();
      }
    } else {
      if (beep_ok) { audio_beep(800, 50); lastCardBeepMs = millis(); }
      ui_clear_card_applets();
      // Don't yank the user out of the Settings tree or the USB
      // Reader screen just because the card changed state.
      if (!ui_is_on_settings_tree() && !on_reader_screen) {
        ui_show_main_menu();
      }
    }
  }

  handleButtons();
  handleSerial();
  checkSleep();
  delay(10);
}