// rtc_time.h — PCF85063 wrapper using the shared i2c_master bus.
//
// Same API as the previous SensorLib-backed version; the only thing
// that changes is the underlying transport (now uses g_rtc_dev, the
// device handle on g_i2c_bus that the main sketch sets up in
// i2cBusInit). This means the RTC shares one bus with touch, AXP,
// TCA and (eventually) the camera SCCB, which is the whole point
// of the architecture refactor.

#pragma once

#include <Arduino.h>
#include <time.h>

void   rtc_init();
bool   rtc_isRunning();
time_t rtc_epoch();
void   rtc_update();
void   rtc_dateTimeStr(char* buf, size_t sz);
void   rtc_setTime(int year, int month, int day,
                   int hour, int min, int sec);
