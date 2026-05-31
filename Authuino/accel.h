#pragma once

/*
 * accel.h — QMI8658 6-axis IMU driver (accel + gyro + temperature).
 *
 * The Waveshare 3.5" board carries a QMI8658C IMU on the shared I2C
 * bus. Default I2C address is 0x6B (AD0 high) but we probe 0x6A as
 * a fallback.
 *
 * Default config:
 *   Accelerometer: ±4g full scale, 1000 Hz ODR, LPF mode 0
 *   Gyroscope:     ±64dps full scale, 896.8 Hz ODR, LPF mode 3
 *   Temperature:   16-bit signed, 1/256 °C resolution
 *
 * API (synchronous, blocking on I2C):
 *   accel_init()        — configure + enable both sensors
 *   accel_is_ready()    — true if init succeeded
 *   accel_read_raw()    — raw int16 values
 *   accel_read_scaled() — physical units (g, dps)
 *   accel_read_temp_c() — degrees Celsius
 */

#include <Arduino.h>
#include "driver/i2c_master.h"

// Probe the QMI8658 on the shared I2C bus (tries 0x6B then 0x6A).
// Configures accel + gyro registers but leaves both DISABLED to save
// power. Call accel_set_enabled(true) before reading. Returns true
// on success.
bool  accel_init(i2c_master_bus_handle_t i2c_bus);

// True if accel_init() succeeded.
bool  accel_is_ready();

// Enable or disable both accel + gyro. Power difference at 1kHz/900Hz
// ODR is ~4mA, so disable when not viewing live data. Idempotent.
void  accel_set_enabled(bool en);

// True if both sensors are currently enabled.
bool  accel_is_enabled();

// Read raw 16-bit signed values. Any pointer may be nullptr to skip.
// Returns false on I2C error.
bool  accel_read_raw(int16_t *ax, int16_t *ay, int16_t *az,
                     int16_t *gx, int16_t *gy, int16_t *gz);

// Read scaled values: accel in g, gyro in dps. Returns false on error.
bool  accel_read_scaled(float *ax_g, float *ay_g, float *az_g,
                        float *gx_dps, float *gy_dps, float *gz_dps);

// Read temperature in degrees Celsius. Returns 0.0f on error.
float accel_read_temp_c();