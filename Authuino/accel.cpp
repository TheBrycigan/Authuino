#include "accel.h"

// QMI8658 I2C addresses
#define QMI_ADDR_LOW       0x6A   // AD0 = 0
#define QMI_ADDR_HIGH      0x6B   // AD0 = 1 (typical)

// Register map (subset)
#define QMI_WHO_AM_I       0x00
#define QMI_REVISION_ID    0x01
#define QMI_CTRL1          0x02   // Serial interface + sensor enable
#define QMI_CTRL2          0x03   // Accelerometer config (ODR, FS)
#define QMI_CTRL3          0x04   // Gyroscope config (ODR, FS)
#define QMI_CTRL5          0x06   // Sensor data processing (LPF)
#define QMI_CTRL7          0x08   // Enable accel/gyro/etc.
#define QMI_TEMP_L         0x33
#define QMI_AX_L           0x35
// Burst-read 0x35..0x40 to get all 12 bytes (6 accel + 6 gyro).

#define QMI_CHIP_ID        0x05   // expected WHO_AM_I value

static i2c_master_dev_handle_t s_dev   = nullptr;
static bool                    s_ready = false;
// LSB scaling depends on full-scale range. Defaults match init values.
static float s_acc_lsb_per_g    = 8192.0f;   // ±4g / 32768
static float s_gyro_lsb_per_dps = 512.0f;    // ±64dps / 32768

// ---------------------------------------------------------------------------
// I2C helpers
// ---------------------------------------------------------------------------
static bool qmi_write(uint8_t reg, uint8_t val) {
  uint8_t buf[2] = { reg, val };
  return i2c_master_transmit(s_dev, buf, 2, 100) == ESP_OK;
}

static bool qmi_read(uint8_t reg, uint8_t *buf, size_t len) {
  return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, 100) == ESP_OK;
}

static bool qmi_read_one(uint8_t reg, uint8_t *val) {
  return qmi_read(reg, val, 1);
}

// Probe one address. If WHO_AM_I matches, keeps the device handle and
// returns true. Otherwise removes the device and returns false.
static bool try_addr(i2c_master_bus_handle_t bus, uint8_t addr) {
  i2c_device_config_t dev_cfg = {};
  dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  dev_cfg.device_address  = addr;
  dev_cfg.scl_speed_hz    = 400000;

  i2c_master_dev_handle_t dev = nullptr;
  if (i2c_master_bus_add_device(bus, &dev_cfg, &dev) != ESP_OK) return false;

  uint8_t reg = QMI_WHO_AM_I;
  uint8_t id  = 0;
  bool ok = (i2c_master_transmit_receive(dev, &reg, 1, &id, 1, 100) == ESP_OK)
            && id == QMI_CHIP_ID;

  if (ok) {
    s_dev = dev;
    return true;
  }
  i2c_master_bus_rm_device(dev);
  return false;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool accel_init(i2c_master_bus_handle_t i2c_bus) {
  if (s_ready) return true;
  if (!i2c_bus) return false;

  // Try common addresses.
  if (!try_addr(i2c_bus, QMI_ADDR_HIGH) &&
      !try_addr(i2c_bus, QMI_ADDR_LOW)) {
    Serial.println("[ACCEL] QMI8658 not found at 0x6A/0x6B");
    return false;
  }

  uint8_t rev = 0;
  qmi_read_one(QMI_REVISION_ID, &rev);
  Serial.printf("[ACCEL] QMI8658 found, revision 0x%02X\n", rev);

  // CTRL1 = 0x40: address auto-increment ON, little endian, internal osc on.
  // (We rely on auto-increment for the 12-byte burst read.)
  if (!qmi_write(QMI_CTRL1, 0x40)) {
    Serial.println("[ACCEL] CTRL1 write failed");
    return false;
  }
  delay(10);

  // CTRL2 = 0x13:
  //   bits 6:4 = 001 → Accel full-scale ±4g
  //   bits 3:0 = 0011 → ODR 1000 Hz
  qmi_write(QMI_CTRL2, 0x13);
  s_acc_lsb_per_g = 8192.0f;     // 32768 LSB / 4 g

  // CTRL3 = 0x23:
  //   bits 6:4 = 010 → Gyro full-scale ±64 dps
  //   bits 3:0 = 0011 → ODR 896.8 Hz
  qmi_write(QMI_CTRL3, 0x23);
  s_gyro_lsb_per_dps = 512.0f;   // 32768 LSB / 64 dps

  // CTRL5 = 0x71: Low-pass filters
  //   bits 6:5 = 11, bit 4 = 1 → gyro LPF mode 3, enabled (0x70)
  //   bits 2:1 = 00, bit 0 = 1 → accel LPF mode 0, enabled (0x01)
  qmi_write(QMI_CTRL5, 0x71);

  // CTRL7 = 0x00: leave sensors DISABLED until accel_set_enabled(true).
  // The gyro alone draws ~3 mA at 900 Hz; we only want it on while the
  // Hardware screen is being viewed.
  qmi_write(QMI_CTRL7, 0x00);

  s_ready = true;
  return true;
}

bool accel_is_ready() {
  return s_ready;
}

static bool s_enabled = false;

void accel_set_enabled(bool en) {
  if (!s_ready) return;
  if (en == s_enabled) return;
  // CTRL7: bit0 = aEN (accel enable), bit1 = gEN (gyro enable).
  qmi_write(QMI_CTRL7, en ? 0x03 : 0x00);
  s_enabled = en;
  if (en) {
    // First samples take a few ms after enable; let the LPF settle
    // before the caller reads.
    delay(20);
  }
}

bool accel_is_enabled() {
  return s_enabled;
}

bool accel_read_raw(int16_t *ax, int16_t *ay, int16_t *az,
                    int16_t *gx, int16_t *gy, int16_t *gz) {
  if (!s_ready) return false;
  uint8_t buf[12];
  if (!qmi_read(QMI_AX_L, buf, sizeof(buf))) return false;
  if (ax) *ax = (int16_t)((uint16_t)buf[0]  | ((uint16_t)buf[1]  << 8));
  if (ay) *ay = (int16_t)((uint16_t)buf[2]  | ((uint16_t)buf[3]  << 8));
  if (az) *az = (int16_t)((uint16_t)buf[4]  | ((uint16_t)buf[5]  << 8));
  if (gx) *gx = (int16_t)((uint16_t)buf[6]  | ((uint16_t)buf[7]  << 8));
  if (gy) *gy = (int16_t)((uint16_t)buf[8]  | ((uint16_t)buf[9]  << 8));
  if (gz) *gz = (int16_t)((uint16_t)buf[10] | ((uint16_t)buf[11] << 8));
  return true;
}

bool accel_read_scaled(float *ax_g, float *ay_g, float *az_g,
                       float *gx_dps, float *gy_dps, float *gz_dps) {
  int16_t ax, ay, az, gx, gy, gz;
  if (!accel_read_raw(&ax, &ay, &az, &gx, &gy, &gz)) return false;
  if (ax_g)   *ax_g   = ax / s_acc_lsb_per_g;
  if (ay_g)   *ay_g   = ay / s_acc_lsb_per_g;
  if (az_g)   *az_g   = az / s_acc_lsb_per_g;
  if (gx_dps) *gx_dps = gx / s_gyro_lsb_per_dps;
  if (gy_dps) *gy_dps = gy / s_gyro_lsb_per_dps;
  if (gz_dps) *gz_dps = gz / s_gyro_lsb_per_dps;
  return true;
}

float accel_read_temp_c() {
  if (!s_ready) return 0.0f;
  uint8_t buf[2];
  if (!qmi_read(QMI_TEMP_L, buf, 2)) return 0.0f;
  int16_t raw = (int16_t)((uint16_t)buf[0] | ((uint16_t)buf[1] << 8));
  // QMI8658 temperature: signed 16-bit, 1/256 °C per LSB.
  return raw / 256.0f;
}