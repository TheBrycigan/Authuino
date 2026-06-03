#pragma once

/*
 * usb_ccid.h - Phase 6a USB CCID interface scaffolding.
 *
 * Goal: get the device to enumerate as a composite USB device with
 *   - 1x CDC interface (provides the Serial console on USB-OTG)
 *   - 1x CCID interface (declares the device as a smart card reader)
 *
 * No CCID command processing yet; just the descriptors so the host
 * driver binds and the device shows up as "USB Smart Card Reader" in
 * Device Manager / lsusb / system_profiler.
 *
 * REQUIRED ARDUINO IDE SETTINGS:
 *   Tools -> USB Mode             = "USB-OTG (TinyUSB)"
 *   Tools -> USB CDC On Boot      = "Enabled"
 *   Tools -> USB Firmware MSC     = "Disabled"
 *   Tools -> USB DFU On Boot      = "Disabled"
 *
 * FLASHING:
 *   Auto-reset via DTR/RTS works through TinyUSB CDC most of the time.
 *   If the IDE can't auto-reset, hold BOOT, tap RESET, then hit Upload.
 */

#include <Arduino.h>

// Register the CCID interface with the TinyUSB stack. Call this from
// setup() BEFORE Serial.begin(). Has no effect when built in
// USB-Serial-JTAG mode (ARDUINO_USB_MODE == 1).
void usb_ccid_init();

// Bring up the USB stack and attach the device on the bus. First call
// initialises TinyUSB (chip switches from USB-Serial-JTAG to USB-OTG
// mode at this point); subsequent calls just re-connect the device
// after a previous detach. Call when entering USB Reader Mode.
//
// IMPORTANT: once attach has been called for the first time, the
// chip is in USB-OTG mode for the remainder of this boot — there's
// no way back to USB-Serial-JTAG without a reset.
void usb_ccid_attach();

// Detach the device from the host (soft-disconnect on D+/D-). The
// host sees an unplug event. The chip remains in USB-OTG mode but
// the device disappears from Device Manager. Call when leaving USB
// Reader Mode.
void usb_ccid_detach();

// True when a USB host has enumerated the device.
bool usb_ccid_is_mounted();

// Call from main loop — fires a one-time diagnostic dump of the
// assembled descriptor bytes after the host has been connected for
// 1 second. This lets us see the descriptor without losing output
// to early-boot serial drops.
void usb_ccid_tick();

// Live diagnostic counters for end-to-end APDU testing. These are
// updated by the CCID command handlers as the host sends commands.
struct UsbCcidLiveStats {
  uint32_t cmd_total;       // any CCID command received
  uint32_t cmd_get_status;  // 0x65 PC_to_RDR_GetSlotStatus
  uint32_t cmd_power_on;    // 0x62 PC_to_RDR_IccPowerOn
  uint32_t cmd_power_off;   // 0x63 PC_to_RDR_IccPowerOff
  uint32_t cmd_xfr_block;   // 0x6F PC_to_RDR_XfrBlock
  uint32_t cmd_other;       // anything else
  uint8_t  last_cmd_type;   // raw bMessageType byte
  uint8_t  last_status;     // bmICCStatus byte returned
  uint8_t  last_error;      // bError byte returned
  int      last_atr_len;    // result of last sc_recapture_atr()
  int      last_apdu_len;   // result of last sc_apdu_passthrough()
  bool     last_reader_active;
  bool     last_card_present;
};

void usb_ccid_get_stats(UsbCcidLiveStats* out);