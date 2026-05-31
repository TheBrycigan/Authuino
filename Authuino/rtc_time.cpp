// rtc_time.cpp — direct PCF85063 driver over the shared i2c_master
// bus. Replaces the previous SensorLib-based implementation.
//
// PCF85063 register map (from datasheet Table 1):
//   0x00 Control_1
//   0x01 Control_2
//   0x02 Offset
//   0x03 RAM_byte
//   0x04 Seconds  (BCD; bit 7 = OS = clock-integrity flag, 1=invalid)
//   0x05 Minutes  (BCD)
//   0x06 Hours    (BCD, 24h when CTRL1[1]=0 — power-on default)
//   0x07 Days     (BCD)
//   0x08 Weekdays
//   0x09 Months   (BCD)
//   0x0A Years    (BCD, two-digit, century lives in user code)

#include "rtc_time.h"

#include <string.h>
#include "driver/i2c_master.h"

extern i2c_master_dev_handle_t g_rtc_dev;

#define RTC_REG_SECONDS  0x04

// ---------------------------------------------------------------------------
// State cache. We keep the last decoded epoch and the millis() value at
// which we read it; rtc_epoch() extrapolates by millis-delta so callers
// can poll cheaply without I2C traffic on every call. rtc_update()
// does the actual register read and refreshes the cache.
// ---------------------------------------------------------------------------
static bool      s_running    = false;
static time_t    s_lastEpoch  = 0;
static uint32_t  s_lastReadMs = 0;

static esp_err_t rtcReadRegs(uint8_t reg, uint8_t* buf, size_t n) {
  if (!g_rtc_dev) return ESP_FAIL;
  return i2c_master_transmit_receive(g_rtc_dev, &reg, 1, buf, n, 100);
}
static esp_err_t rtcWriteRegs(uint8_t reg, const uint8_t* buf, size_t n) {
  if (!g_rtc_dev) return ESP_FAIL;
  uint8_t tx[16];
  if (n + 1 > sizeof(tx)) return ESP_ERR_INVALID_SIZE;
  tx[0] = reg;
  memcpy(tx + 1, buf, n);
  return i2c_master_transmit(g_rtc_dev, tx, n + 1, 100);
}

static inline uint8_t bcdToDec(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
static inline uint8_t decToBcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

// ---------------------------------------------------------------------------
// Howard Hinnant's days_from_civil / civil_from_days — proleptic
// Gregorian calendar, no system mktime/localtime needed.
// Reference: https://howardhinnant.github.io/date_algorithms.html
// ---------------------------------------------------------------------------
static int32_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2 ? 1 : 0;
  int32_t  era = (y >= 0 ? y : y - 399) / 400;
  uint32_t yoe = (uint32_t)(y - era * 400);
  uint32_t doy = (uint32_t)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
  uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int32_t)doe - 719468;
}
static void daysToCivil(int32_t z, int* py, int* pm, int* pd) {
  z += 719468;
  int32_t  era = (z >= 0 ? z : z - 146096) / 146097;
  uint32_t doe = (uint32_t)(z - era * 146097);
  uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int      y   = (int)yoe + era * 400;
  uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  uint32_t mp  = (5 * doy + 2) / 153;
  int      d   = (int)(doy - (153 * mp + 2) / 5 + 1);
  int      m   = (int)mp + (mp < 10 ? 3 : -9);
  if (m <= 2) y++;
  *py = y; *pm = m; *pd = d;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void rtc_init() {
  uint8_t b = 0;
  if (rtcReadRegs(RTC_REG_SECONDS, &b, 1) != ESP_OK) {
    Serial.println("[RTC] PCF85063 not detected");
    s_running = false;
    return;
  }
  Serial.println("[RTC] PCF85063 initialised");
  rtc_update();   // populates cache and prints time if valid
  if (s_running) {
    char buf[40]; rtc_dateTimeStr(buf, sizeof(buf));
    Serial.printf("[RTC] Time OK: %s\n", buf);
  } else {
    Serial.println("[RTC] Clock invalid (OS flag set) — set time before use");
  }
}

bool rtc_isRunning() { return s_running; }

void rtc_update() {
  uint8_t r[7];
  if (rtcReadRegs(RTC_REG_SECONDS, r, 7) != ESP_OK) {
    s_running = false;
    return;
  }
  if (r[0] & 0x80) {   // OS flag — clock integrity lost (e.g. battery-out)
    s_running = false;
    return;
  }
  s_running = true;

  int sec   = bcdToDec(r[0] & 0x7F);
  int min   = bcdToDec(r[1] & 0x7F);
  int hour  = bcdToDec(r[2] & 0x3F);
  int day   = bcdToDec(r[3] & 0x3F);
  // r[4] = weekdays; we don't use it
  int month = bcdToDec(r[5] & 0x1F);
  int year  = bcdToDec(r[6]) + 2000;

  int32_t  days  = daysFromCivil(year, month, day);
  s_lastEpoch    = (time_t)days * 86400 + hour * 3600 + min * 60 + sec;
  s_lastReadMs   = millis();
}

time_t rtc_epoch() {
  // Cheap extrapolation from the last hardware read using the millis
  // delta. Callers that need fresh-from-RTC accuracy should call
  // rtc_update() first; for code-period bookkeeping etc. this is fine.
  uint32_t delta = millis() - s_lastReadMs;
  return s_lastEpoch + (time_t)(delta / 1000);
}

void rtc_dateTimeStr(char* buf, size_t sz) {
  if (!s_running) {
    snprintf(buf, sz, "[RTC not set]");
    return;
  }
  time_t  t          = rtc_epoch();
  int32_t days       = (int32_t)(t / 86400);
  int     dayOfTime  = (int)(t % 86400);
  if (dayOfTime < 0) { days--; dayOfTime += 86400; }
  int y, m, d;
  daysToCivil(days, &y, &m, &d);
  int hr = dayOfTime / 3600;
  int mn = (dayOfTime % 3600) / 60;
  int sc = dayOfTime % 60;
  snprintf(buf, sz, "%04d-%02d-%02d %02d:%02d:%02d", y, m, d, hr, mn, sc);
}

void rtc_setTime(int year, int month, int day,
                 int hour, int min, int sec) {
  uint8_t r[7];
  r[0] = decToBcd(sec)        & 0x7F;   // also clears OS flag (bit 7 = 0)
  r[1] = decToBcd(min)        & 0x7F;
  r[2] = decToBcd(hour)       & 0x3F;
  r[3] = decToBcd(day)        & 0x3F;
  r[4] = 0;                              // weekday — unused
  r[5] = decToBcd(month)      & 0x1F;
  r[6] = decToBcd(year - 2000);
  if (rtcWriteRegs(RTC_REG_SECONDS, r, 7) != ESP_OK) {
    Serial.println("[RTC] setTime: I2C write failed");
    return;
  }
  rtc_update();
  Serial.println("[RTC] Time set");
}
