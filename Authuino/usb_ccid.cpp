#include "usb_ccid.h"
#include <Preferences.h>

// ----------------------------------------------------------------------
// Master enable flag.
//
// Set to 1 to compile the CCID class driver code in.
// USB_CCID_ENABLED=0 leaves the device as plain CDC (recovery state).
//
// USB_CCID_REGISTER_DESCRIPTOR additionally controls whether the CCID
// interface descriptor is inserted into the configuration descriptor.
// Set to 0 for "diagnostic mode" — the class driver is registered with
// TinyUSB via the weak-symbol override, but no CCID interface exists,
// so the override is exercised by enumeration of the existing CDC
// interfaces (TinyUSB calls our open() for each interface to ask if
// we claim it; we always say no). We can then see from serial whether
// the override is even taking effect.
// ----------------------------------------------------------------------
#define USB_CCID_ENABLED               1
#define USB_CCID_REGISTER_DESCRIPTOR   1

// When 1, open() actually opens the bulk endpoints. When 0, we still
// claim the interface but skip endpoint setup (EPs stay closed).
//
// Now 1 because we're testing whether moving from EP4 to EP3 fixes
// enumeration.
#define USB_CCID_OPEN_ENDPOINTS        1

// When 1, we arm an OUT-endpoint receive transfer.
#define USB_CCID_ARM_RX_XFER           0

// When 1, register the descriptor in the VENDOR slot rather than the
// CUSTOM slot. The previous CUSTOM run got desc_built=1 but mounted=0
// with 11 bus resets and 0 open() calls — Windows rejected the
// configuration descriptor before SET_CONFIGURATION. The hypothesis:
// the CUSTOM slot in arduino-esp32 v3.2.1 doesn't properly update the
// parent configuration descriptor's wTotalLength / bNumInterfaces.
// VENDOR is a "real" interface slot that arduino-esp32 may treat
// differently.
#define USB_CCID_USE_VENDOR_SLOT       0

#if !ARDUINO_USB_MODE && USB_CCID_ENABLED

#include "USB.h"

extern "C" {
  // arduino-esp32 wrapper API for adding interfaces to the configuration
  // descriptor. Forward-declared so we don't need the private header.
  typedef enum {
    USB_INTERFACE_MSC,
    USB_INTERFACE_DFU,
    USB_INTERFACE_HID,
    USB_INTERFACE_VENDOR,
    USB_INTERFACE_CDC,
    USB_INTERFACE_CDC2,
    USB_INTERFACE_MIDI,
    USB_INTERFACE_CUSTOM,
    USB_INTERFACE_MAX
  } tinyusb_interface_t;

  typedef uint16_t (*tinyusb_descriptor_cb_t)(uint8_t* dst, uint8_t* itf);

  esp_err_t tinyusb_enable_interface(tinyusb_interface_t interface,
                                     uint16_t descriptor_len,
                                     tinyusb_descriptor_cb_t cb);
  uint8_t   tinyusb_add_string_descriptor(const char* str);
}

// Pull in TinyUSB device-side API so we can register a custom class
// driver and drive the bulk endpoints ourselves.
extern "C" {
#include "tusb.h"
#include "device/usbd.h"
#include "device/usbd_pvt.h"
}

// Pull in the smartcard layer for the CCID -> sc_interface bridge.
#include "sc_interface.h"
#include "ui.h"

// ---------------------------------------------------------------------------
// Endpoint addresses
//
// CRITICAL: arduino-esp32's TinyUSB build appears to limit usable endpoints
// to EP1-EP3. Earlier attempts using EP4 (0x04/0x84) caused 11 bus resets
// and "code 10" on the composite — Windows couldn't enumerate the device.
// USBView capture of the ESP32-S3 ROM bootloader showed it uses EP3 (0x02
// OUT for vendor data, 0x83 IN for vendor data) successfully alongside CDC,
// so we copy that proven-working pattern.
//
// CDC owns: 0x82 IN (notify), 0x01 OUT (data), 0x81 IN (data)
// We take:  0x03 OUT (CCID command), 0x83 IN (CCID response)
//
// Interrupt IN endpoint is intentionally omitted: on ESP32-S3 with CDC
// enabled, EP 0x85 has no backing FIFO. CCID class drivers handle the
// no-interrupt case fine; they poll PC_to_RDR_GetSlotStatus instead.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Endpoint addresses
//
// CCID-only descriptor (no CDC), so EP1 and EP2 are entirely free.
// We put the interrupt-IN endpoint on 0x82 (EP2 IN), matching the
// bootloader's CDC notify pattern — that proves the dwc2 controller
// services interrupt traffic on EP2 reliably. (Earlier attempt used
// EP4/0x84 and broke string descriptor handling, suggesting EP4 has
// allocation constraints we don't understand.)
// ---------------------------------------------------------------------------
#define EPNUM_CCID_OUT     0x03
#define EPNUM_CCID_IN      0x83
#define EPNUM_CCID_NOTIFY  0x82

#define CCID_BULK_PSIZE    64
#define CCID_INT_PSIZE     8       // small notification packets only
#define CCID_INT_INTERVAL  16      // poll every 16 ms
#define CCID_MSG_HDR_LEN   10
#define CCID_MAX_MSG_LEN   280

// ---------------------------------------------------------------------------
// CCID class-specific functional descriptor (54 bytes)
// ---------------------------------------------------------------------------
static const uint8_t CCID_FUNC_DESC[54] = {
  54,                          // bLength
  0x21,                        // bDescriptorType = SmartCard (class-specific)
  0x10, 0x01,                  // bcdCCID = 1.10
  0x00,                        // bMaxSlotIndex = 0 (1 slot total)
  0x07,                        // bVoltageSupport = 5V | 3V | 1.8V

  0x03, 0x00, 0x00, 0x00,      // dwProtocols = T=0 | T=1
  0xA0, 0x0F, 0x00, 0x00,      // dwDefaultClock = 4000 kHz
  0x88, 0x13, 0x00, 0x00,      // dwMaximumClock = 5000 kHz
  0x00,                        // bNumClockSupported = default only

  0x80, 0x25, 0x00, 0x00,      // dwDataRate = 9600 bps
  0x80, 0x70, 0x01, 0x00,      // dwMaxDataRate = 96000 bps
  0x00,                        // bNumDataRatesSupported = default only

  0xFE, 0x00, 0x00, 0x00,      // dwMaxIFSD = 254
  0x00, 0x00, 0x00, 0x00,      // dwSynchProtocols = none
  0x00, 0x00, 0x00, 0x00,      // dwMechanical = none

  // dwFeatures = 0x000400FE: auto-everything + short/extended APDU
  // level exchange (host gives us complete APDUs, we manage T=0/T=1).
  0xFE, 0x00, 0x04, 0x00,

  0x0F, 0x01, 0x00, 0x00,      // dwMaxCCIDMsgLen = 271
  0xFF,                        // bClassGetResponse
  0xFF,                        // bClassEnvelope
  0x00, 0x00,                  // wLcdLayout = none
  0x00,                        // bPINSupport = none
  0x01,                        // bMaxCCIDBusySlots = 1
};

// ---------------------------------------------------------------------------
// Configuration descriptor builder for the CCID interface.
//
// Layout written into the buffer (77 bytes total):
//   - 1x interface descriptor (9 bytes)
//   - 1x CCID functional descriptor (54 bytes)
//   - 2x endpoint descriptors (7 bytes each: bulk OUT + bulk IN)
//
// Note: an Interface Association Descriptor (IAD) was tried in an
// earlier iteration on the theory that arduino-esp32's IAD-mode device
// (bDeviceClass=0xEF) needed it for Windows to recognise CCID as a
// separate function. In practice it triggered a Code 10 error on the
// composite parent and broke COM6, so it's removed. The CCID class
// driver below still runs and answers host commands; if Windows
// doesn't auto-bind without the IAD we'll handle that another way.
// ---------------------------------------------------------------------------
// Interface (9) + functional (54) + bulk EPs (7 x 2) + interrupt EP (7) = 84
#define CCID_TOTAL_DESC_LEN  (9 + 54 + 7 + 7 + 7)

static uint8_t  s_desc_cache[CCID_TOTAL_DESC_LEN];
static bool     s_desc_built = false;
static uint8_t  s_iface_num  = 0xFF;
static uint8_t  s_str_idx    = 0;

static uint16_t ccid_load_descriptor(uint8_t* dst, uint8_t* itf) {
  uint8_t str_idx = tinyusb_add_string_descriptor("Authuino CCID");
  uint8_t  iface  = *itf;
  s_iface_num = iface;
  s_str_idx   = str_idx;
  uint8_t* p = dst;

  // Interface descriptor
  *p++ = 9;
  *p++ = 0x04;                 // INTERFACE
  *p++ = iface;
  *p++ = 0x00;
  *p++ = 0x03;                 // bNumEndpoints = 3 (bulk OUT + bulk IN + interrupt IN)
  *p++ = 0x0B;                 // CCID
  *p++ = 0x00;
  *p++ = 0x00;
  *p++ = str_idx;

  // CCID functional descriptor
  memcpy(p, CCID_FUNC_DESC, sizeof(CCID_FUNC_DESC));
  p += sizeof(CCID_FUNC_DESC);

  // Bulk OUT
  *p++ = 7;
  *p++ = 0x05;
  *p++ = EPNUM_CCID_OUT;       // 0x03 OUT
  *p++ = 0x02;                 // bulk
  *p++ = (uint8_t)(CCID_BULK_PSIZE & 0xFF);
  *p++ = (uint8_t)(CCID_BULK_PSIZE >> 8);
  *p++ = 0x00;

  // Bulk IN
  *p++ = 7;
  *p++ = 0x05;
  *p++ = EPNUM_CCID_IN;        // 0x83 IN
  *p++ = 0x02;
  *p++ = (uint8_t)(CCID_BULK_PSIZE & 0xFF);
  *p++ = (uint8_t)(CCID_BULK_PSIZE >> 8);
  *p++ = 0x00;

  // Interrupt IN (RDR_to_PC_NotifySlotChange)
  *p++ = 7;
  *p++ = 0x05;
  *p++ = EPNUM_CCID_NOTIFY;    // 0x82 IN
  *p++ = 0x03;                 // interrupt
  *p++ = (uint8_t)(CCID_INT_PSIZE & 0xFF);
  *p++ = (uint8_t)(CCID_INT_PSIZE >> 8);
  *p++ = CCID_INT_INTERVAL;

  *itf += 1;

  memcpy(s_desc_cache, dst, CCID_TOTAL_DESC_LEN);
  s_desc_built = true;
  return CCID_TOTAL_DESC_LEN;
}

// ===========================================================================
// Diagnostic counters
// ===========================================================================
//
// Track how many times each TinyUSB callback runs. Printed periodically
// from usb_ccid_tick() so we can see activity even if early-boot prints
// were missed (host monitor opens after boot completes).
// ===========================================================================
static volatile uint32_t s_diag_override_calls   = 0;
static volatile uint32_t s_diag_init_calls       = 0;
static volatile uint32_t s_diag_reset_calls      = 0;
static volatile uint32_t s_diag_open_calls       = 0;
static volatile uint32_t s_diag_open_claimed     = 0;
static volatile uint8_t  s_diag_last_open_class  = 0xFF;
static volatile uint32_t s_diag_control_calls    = 0;
static volatile uint8_t  s_diag_last_control_req = 0xFF;
static volatile uint32_t s_diag_xfer_calls       = 0;
static volatile uint8_t  s_diag_last_xfer_ep     = 0xFF;

// ===========================================================================
// TinyUSB class driver
// ===========================================================================
//
// arduino-esp32 inserts our descriptor into the configuration descriptor
// (above), but TinyUSB has no built-in class driver for class 0x0B.
// We register one here by overriding the weak `usbd_app_driver_get_cb`
// symbol. The driver claims the CCID interface during enumeration,
// opens the bulk endpoints, and pumps CCID protocol messages between
// the host and the smartcard interface.
// ===========================================================================

static uint8_t  s_rhport       = 0;
static uint8_t  s_ep_in        = 0;    // bulk IN
static uint8_t  s_ep_out       = 0;    // bulk OUT
static uint8_t  s_ep_int       = 0;    // interrupt IN (NotifySlotChange)
static uint8_t  s_itf_active   = 0xFF;
static uint8_t  s_rx_buf[CCID_MAX_MSG_LEN] __attribute__((aligned(4)));
static uint8_t  s_tx_buf[CCID_MAX_MSG_LEN] __attribute__((aligned(4)));

// Forward decls
static void ccid_handle_command(uint32_t len);
static void ccid_send_response(uint32_t total_len);
static void ccid_send_slot_status(uint8_t seq, uint8_t status, uint8_t error);
static void ccid_send_data_block(uint8_t seq, uint8_t status, uint8_t error,
                                 const uint8_t* data, uint32_t len);
static void ccid_send_parameters(uint8_t seq, uint8_t status, uint8_t error);

// Compute the bStatus byte:
//   bits 0-1: bmICCStatus  (00=present+active, 10=not present)
//   bits 6-7: bmCommandStatus (00=ok, 01=failed)
// When the user has NOT entered Reader Mode, or no card is in the slot,
// we tell the host "no card present, command failed". This is what
// gates host access to the smartcard.
// Encodes bmICCStatus (bits 1-0) and bmCommandStatus (bits 7-6) per
// CCID 1.1 spec.
//   00 = ICC present and active (powered, clocked, ready for APDUs)
//   01 = ICC present and inactive (host should call IccPowerOn first)
//   10 = no ICC present
// Reporting "active" when the card is unpowered makes the host skip
// IccPowerOn and try APDUs directly, which then fail with MUTE.
static uint8_t ccid_status_byte() {
  bool reader = ui_is_usb_reader_active();
  bool card   = sc_state().cardPresent;
  if (!reader || !card) return 0x42;            // no card + cmd-failed
  if (!sc_is_icc_active()) return 0x01;          // present, not powered
  return 0x00;                                   // present and active
}

// --- TinyUSB driver callbacks ---------------------------------------------

static void ccid_drv_init(void) {
  s_diag_init_calls++;
  s_ep_in      = 0;
  s_ep_out     = 0;
  s_ep_int     = 0;
  s_itf_active = 0xFF;
}

static void ccid_drv_reset(uint8_t rhport) {
  s_diag_reset_calls++;
  (void)rhport;
  s_ep_in      = 0;
  s_ep_out     = 0;
  s_ep_int     = 0;
  s_itf_active = 0xFF;
}

static volatile uint32_t s_diag_ep_open_attempts  = 0;
static volatile uint32_t s_diag_ep_open_failures  = 0;
static volatile uint32_t s_diag_xfer_arm_attempts = 0;
static volatile bool     s_diag_should_arm_rx     = false;

static uint16_t ccid_drv_open(uint8_t rhport,
                              tusb_desc_interface_t const* desc_itf,
                              uint16_t max_len) {
  s_diag_open_calls++;
  s_diag_last_open_class = desc_itf->bInterfaceClass;

  // Only claim CCID interfaces (class 0x0B).
  if (desc_itf->bInterfaceClass != 0x0B) return 0;

  s_diag_open_claimed++;

  s_rhport     = rhport;
  s_itf_active = desc_itf->bInterfaceNumber;

  uint8_t const* p   = (uint8_t const*)desc_itf + sizeof(tusb_desc_interface_t);
  uint8_t const* end = (uint8_t const*)desc_itf + max_len;
  uint16_t consumed  = sizeof(tusb_desc_interface_t);

  // Skip CCID functional descriptor (any descriptor with type 0x21).
  if (p + 2 < end && p[1] == 0x21) {
    consumed += p[0];
    p        += p[0];
  }

  // Open each endpoint (gated for diagnostic).
  for (uint8_t i = 0; i < desc_itf->bNumEndpoints && p + 7 <= end; i++) {
    if (p[1] == TUSB_DESC_ENDPOINT) {
      tusb_desc_endpoint_t const* ep = (tusb_desc_endpoint_t const*)p;
      uint8_t addr  = ep->bEndpointAddress;
      uint8_t type  = ep->bmAttributes.xfer;  // 2 = bulk, 3 = interrupt
      bool    is_in = (addr & 0x80) != 0;

      if (is_in) {
        if (type == 0x03) s_ep_int = addr;   // interrupt IN
        else              s_ep_in  = addr;   // bulk IN
      } else {
                          s_ep_out = addr;   // bulk OUT
      }
#if USB_CCID_OPEN_ENDPOINTS
      s_diag_ep_open_attempts++;
      bool ok = usbd_edpt_open(rhport, ep);
      if (!ok) s_diag_ep_open_failures++;
#endif
      consumed += sizeof(tusb_desc_endpoint_t);
      p        += sizeof(tusb_desc_endpoint_t);
    }
  }

#if USB_CCID_ARM_RX_XFER
  // Arm the OUT endpoint to receive the first CCID command, only if
  // configured to do so. Disabled by default during diagnostics —
  // arming during open() seems to cause issues; we defer to tick().
  if (s_ep_out) {
    s_diag_xfer_arm_attempts++;
    usbd_edpt_xfer(rhport, s_ep_out, s_rx_buf, CCID_BULK_PSIZE);
  }
#else
  // Tell the tick handler to arm the receive after enumeration.
  s_diag_should_arm_rx = true;
#endif

  return consumed;
}

static bool ccid_drv_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                     tusb_control_request_t const* req) {
  s_diag_control_calls++;
  s_diag_last_control_req = req->bRequest;

  // Only respond to interface-targeted class requests for our interface.
  if (req->bmRequestType_bit.recipient != TUSB_REQ_RCPT_INTERFACE) return false;
  if (req->wIndex != s_itf_active)                                  return false;
  if (req->bmRequestType_bit.type != TUSB_REQ_TYPE_CLASS)           return false;

  switch (req->bRequest) {
    case 0x01:  // CCID_REQUEST_ABORT
      if (stage == CONTROL_STAGE_SETUP) tud_control_status(rhport, req);
      return true;
    case 0x02:  // CCID_REQUEST_GET_CLOCK_FREQUENCIES
    case 0x03:  // CCID_REQUEST_GET_DATA_RATES
      // We declared bNumClockSupported / bNumDataRatesSupported = 0,
      // so reply with an empty data stage.
      if (stage == CONTROL_STAGE_SETUP) tud_control_xfer(rhport, req, NULL, 0);
      return true;
  }
  return false;
}

static bool ccid_drv_xfer_cb(uint8_t rhport, uint8_t ep_addr,
                             xfer_result_t result, uint32_t xferred_bytes) {
  s_diag_xfer_calls++;
  s_diag_last_xfer_ep = ep_addr;
  (void)result;
  if (ep_addr == s_ep_out) {
    if (xferred_bytes >= CCID_MSG_HDR_LEN) {
      ccid_handle_command(xferred_bytes);
    }
    // Re-arm for the next command. Single-packet length to stay
    // within the RX FIFO budget; subsequent packets re-arm again.
    usbd_edpt_xfer(rhport, s_ep_out, s_rx_buf, CCID_BULK_PSIZE);
  }
  return true;
}

// Driver descriptor used by usbd_app_driver_get_cb. Initialised at
// static-init time alongside our descriptor registration.
static usbd_class_driver_t s_ccid_driver = {};

// --- CCID protocol logic --------------------------------------------------

// Live stats updated as commands stream in. Read by UI for real-time
// debugging on the Reader Mode screen.
static volatile uint32_t s_live_cmd_total       = 0;
static volatile uint32_t s_live_cmd_get_status  = 0;
static volatile uint32_t s_live_cmd_power_on    = 0;
static volatile uint32_t s_live_cmd_power_off   = 0;
static volatile uint32_t s_live_cmd_xfr_block   = 0;
static volatile uint32_t s_live_cmd_other       = 0;
static volatile uint8_t  s_live_last_cmd_type   = 0;
static volatile uint8_t  s_live_last_status     = 0;
static volatile uint8_t  s_live_last_error      = 0;
static volatile int      s_live_last_atr_len    = -999;
static volatile int      s_live_last_apdu_len   = -999;
static volatile bool     s_live_last_reader     = false;
static volatile bool     s_live_last_card       = false;

static void ccid_handle_command(uint32_t len) {
  uint8_t  msg_type = s_rx_buf[0];
  uint32_t data_len =  (uint32_t)s_rx_buf[1]
                    | ((uint32_t)s_rx_buf[2] << 8)
                    | ((uint32_t)s_rx_buf[3] << 16)
                    | ((uint32_t)s_rx_buf[4] << 24);
  uint8_t  seq      = s_rx_buf[6];
  (void)len;

  s_live_cmd_total++;
  s_live_last_cmd_type = msg_type;

  switch (msg_type) {
    case 0x65: {  // PC_to_RDR_GetSlotStatus
      s_live_cmd_get_status++;
      uint8_t st = ccid_status_byte();
      s_live_last_status = st;
      s_live_last_error  = 0;
      ccid_send_slot_status(seq, st, 0x00);
      break;
    }

    case 0x62: {  // PC_to_RDR_IccPowerOn
      s_live_cmd_power_on++;
      bool reader = ui_is_usb_reader_active();
      bool card   = sc_state().cardPresent;
      s_live_last_reader = reader;
      s_live_last_card   = card;
      if (!reader || !card) {
        s_live_last_status = 0x42;
        s_live_last_error  = 0xFE;
        ccid_send_data_block(seq, 0x42, 0xFE, NULL, 0);
        break;
      }
      uint8_t atr[33];
      int n = sc_recapture_atr(atr, sizeof(atr));
      s_live_last_atr_len = n;
      if (n <= 0) {
        s_live_last_status = 0x42;
        s_live_last_error  = 0xFE;
        ccid_send_data_block(seq, 0x42, 0xFE, NULL, 0);
      } else {
        s_live_last_status = 0x00;
        s_live_last_error  = 0x00;
        ccid_send_data_block(seq, 0x00, 0x00, atr, (uint32_t)n);
      }
      break;
    }

    case 0x63: {  // PC_to_RDR_IccPowerOff
      s_live_cmd_power_off++;
      uint8_t st = ccid_status_byte();
      s_live_last_status = st;
      s_live_last_error  = 0;
      ccid_send_slot_status(seq, st, 0x00);
      break;
    }

    case 0x6F: {  // PC_to_RDR_XfrBlock
      s_live_cmd_xfr_block++;
      bool reader = ui_is_usb_reader_active();
      bool card   = sc_state().cardPresent;
      s_live_last_reader = reader;
      s_live_last_card   = card;
      if (!reader || !card) {
        s_live_last_status = 0x42;
        s_live_last_error  = 0xFE;
        ccid_send_data_block(seq, 0x42, 0xFE, NULL, 0);
        break;
      }
      const uint8_t* apdu = &s_rx_buf[CCID_MSG_HDR_LEN];
      uint8_t resp[270];
      int n = sc_apdu_passthrough(apdu, data_len, resp, sizeof(resp));
      s_live_last_apdu_len = n;
      if (n < 0) {
        s_live_last_status = 0x42;
        s_live_last_error  = 0xFE;
        ccid_send_data_block(seq, 0x42, 0xFE, NULL, 0);
      } else {
        s_live_last_status = 0x00;
        s_live_last_error  = 0x00;
        ccid_send_data_block(seq, 0x00, 0x00, resp, (uint32_t)n);
      }
      break;
    }

    case 0x61:    // PC_to_RDR_SetParameters
    case 0x6C: {  // PC_to_RDR_GetParameters
      s_live_cmd_other++;
      ccid_send_parameters(seq, ccid_status_byte(), 0x00);
      break;
    }

    case 0x6B: {  // PC_to_RDR_ResetParameters
      s_live_cmd_other++;
      ccid_send_parameters(seq, ccid_status_byte(), 0x00);
      break;
    }

    case 0x6E: {  // PC_to_RDR_Escape
      s_live_cmd_other++;
      ccid_send_data_block(seq, ccid_status_byte() | 0x40, 0x00, NULL, 0);
      break;
    }

    case 0x6A: {  // PC_to_RDR_Abort
      s_live_cmd_other++;
      ccid_send_slot_status(seq, ccid_status_byte(), 0x00);
      break;
    }

    default: {
      s_live_cmd_other++;
      ccid_send_slot_status(seq, ccid_status_byte() | 0x40, 0x00);
      break;
    }
  }
}

static void ccid_send_response(uint32_t total_len) {
  if (s_ep_in == 0) return;
  if (total_len > sizeof(s_tx_buf)) total_len = sizeof(s_tx_buf);
  // Don't block if a previous IN xfer is in flight; just drop. The
  // host will retry. This avoids any chance of blocking inside a
  // TinyUSB callback context.
  if (usbd_edpt_busy(s_rhport, s_ep_in)) return;
  usbd_edpt_xfer(s_rhport, s_ep_in, s_tx_buf, total_len);
}

static void ccid_send_slot_status(uint8_t seq, uint8_t status, uint8_t error) {
  s_tx_buf[0] = 0x81;
  s_tx_buf[1] = 0; s_tx_buf[2] = 0; s_tx_buf[3] = 0; s_tx_buf[4] = 0;
  s_tx_buf[5] = 0;        // bSlot
  s_tx_buf[6] = seq;
  s_tx_buf[7] = status;
  s_tx_buf[8] = error;
  s_tx_buf[9] = 0x00;     // bClockStatus
  ccid_send_response(CCID_MSG_HDR_LEN);
}

static void ccid_send_data_block(uint8_t seq, uint8_t status, uint8_t error,
                                 const uint8_t* data, uint32_t len) {
  if (len > CCID_MAX_MSG_LEN - CCID_MSG_HDR_LEN) {
    len    = 0;
    status = 0x42;
    error  = 0xFE;
  }
  s_tx_buf[0] = 0x80;
  s_tx_buf[1] = (uint8_t)(len & 0xFF);
  s_tx_buf[2] = (uint8_t)((len >> 8) & 0xFF);
  s_tx_buf[3] = (uint8_t)((len >> 16) & 0xFF);
  s_tx_buf[4] = (uint8_t)((len >> 24) & 0xFF);
  s_tx_buf[5] = 0x00;
  s_tx_buf[6] = seq;
  s_tx_buf[7] = status;
  s_tx_buf[8] = error;
  s_tx_buf[9] = 0x00;     // bChainParameter
  if (data && len > 0) memcpy(&s_tx_buf[CCID_MSG_HDR_LEN], data, len);
  ccid_send_response(CCID_MSG_HDR_LEN + len);
}

static void ccid_send_parameters(uint8_t seq, uint8_t status, uint8_t error) {
  // 5-byte payload for T=0 (most cards). dwLength = 5.
  uint32_t len = 5;
  s_tx_buf[0]  = 0x82;
  s_tx_buf[1]  = (uint8_t)len;
  s_tx_buf[2]  = 0; s_tx_buf[3] = 0; s_tx_buf[4] = 0;
  s_tx_buf[5]  = 0x00;    // bSlot
  s_tx_buf[6]  = seq;
  s_tx_buf[7]  = status;
  s_tx_buf[8]  = error;
  s_tx_buf[9]  = 0x00;    // bProtocolNum = T=0
  s_tx_buf[10] = 0x11;    // bmFindexDindex (default Fi=372, Di=1)
  s_tx_buf[11] = 0x00;    // bmTCCKST0
  s_tx_buf[12] = 0x00;    // bGuardTimeT0
  s_tx_buf[13] = 0x0A;    // bWaitingIntegerT0 (default)
  s_tx_buf[14] = 0x00;    // bClockStop
  ccid_send_response(CCID_MSG_HDR_LEN + len);
}

// ---------------------------------------------------------------------------
// RDR_to_PC_NotifySlotChange — sent on the interrupt-IN endpoint when
// the host-visible slot state (Reader Mode active AND card present)
// flips. Tells usbccid.sys to re-poll IccPowerOn rather than relying
// on cached state from the previous enumeration.
//
// Per CCID 1.1 spec §6.3.1, payload is bMessageType (0x50) + 1 byte
// per 4 slots. For our 1 slot, slot 0 occupies bits 0-1 of byte[0]:
//   bit 0 = bmSlotICCStateChange  (1 = state changed)
//   bit 1 = bmSlotICCState        (1 = ICC present, 0 = no ICC)
// We always set the StateChange bit when sending (we only send on
// transitions).
// ---------------------------------------------------------------------------
static volatile bool s_last_visible_present  = false;
static volatile bool s_first_notify_pending  = true;
static uint8_t       s_int_buf[2] __attribute__((aligned(4)));

static void ccid_check_and_notify_slot_change() {
  if (s_ep_int == 0 || !tud_mounted()) return;

  bool reader  = ui_is_usb_reader_active();
  bool card    = sc_state().cardPresent;
  bool present = reader && card;

  if (!s_first_notify_pending && present == s_last_visible_present) return;
  if (usbd_edpt_busy(s_rhport, s_ep_int)) return;

  s_int_buf[0] = 0x50;
  // Per CCID 1.1 §6.3.1:
  //   bit 0 = bmSlotICCState        (0 = no ICC, 1 = ICC present)
  //   bit 1 = bmSlotICCStateChange  (0 = no change, 1 = changed)
  // We always set the change bit because we only send on a transition.
  s_int_buf[1] = (present ? 0x01 : 0x00) | 0x02;
  if (usbd_edpt_xfer(s_rhport, s_ep_int, s_int_buf, 2)) {
    s_last_visible_present = present;
    s_first_notify_pending = false;
  }
}

// ---------------------------------------------------------------------------
// Override TinyUSB's weak `usbd_app_driver_get_cb` so our driver gets
// installed during TinyUSB init.
// ---------------------------------------------------------------------------
extern "C" usbd_class_driver_t const* usbd_app_driver_get_cb(uint8_t* driver_count) {
  s_diag_override_calls++;
  *driver_count = 1;
  return &s_ccid_driver;
}

// ---------------------------------------------------------------------------
// Static-init: register the descriptor callback AND populate the driver
// struct. Both must happen before initArduino() runs USBSerial.begin(),
// which kicks off TinyUSB.
// ---------------------------------------------------------------------------
static esp_err_t   s_init_result = ESP_FAIL;
static const char* s_slot_used   = "(none)";

struct CCIDStaticRegistrar {
  CCIDStaticRegistrar() {
    // Populate the class driver table.
    s_ccid_driver.name            = "CCID";
    s_ccid_driver.init            = ccid_drv_init;
    s_ccid_driver.reset           = ccid_drv_reset;
    s_ccid_driver.open            = ccid_drv_open;
    s_ccid_driver.control_xfer_cb = ccid_drv_control_xfer_cb;
    s_ccid_driver.xfer_cb         = ccid_drv_xfer_cb;

    // Register the CCID interface descriptor at static-init time, so
    // it's in place before arduino-esp32's USB.begin() runs.
#if USB_CCID_REGISTER_DESCRIPTOR
#if USB_CCID_USE_VENDOR_SLOT
    s_init_result = tinyusb_enable_interface(USB_INTERFACE_VENDOR,
                                             CCID_TOTAL_DESC_LEN,
                                             ccid_load_descriptor);
    if (s_init_result == ESP_OK) { s_slot_used = "VENDOR"; return; }

    s_init_result = tinyusb_enable_interface(USB_INTERFACE_CUSTOM,
                                             CCID_TOTAL_DESC_LEN,
                                             ccid_load_descriptor);
    if (s_init_result == ESP_OK) { s_slot_used = "CUSTOM"; return; }
#else
    s_init_result = tinyusb_enable_interface(USB_INTERFACE_CUSTOM,
                                             CCID_TOTAL_DESC_LEN,
                                             ccid_load_descriptor);
    if (s_init_result == ESP_OK) { s_slot_used = "CUSTOM"; return; }

    s_init_result = tinyusb_enable_interface(USB_INTERFACE_VENDOR,
                                             CCID_TOTAL_DESC_LEN,
                                             ccid_load_descriptor);
    if (s_init_result == ESP_OK) { s_slot_used = "VENDOR"; }
#endif
#else
    s_init_result = ESP_OK;
    s_slot_used   = "(diag - no descriptor)";
#endif
  }
};

static CCIDStaticRegistrar s_ccid_registrar;

// ===========================================================================
// NVS-backed diagnostic log
// ===========================================================================
//
// USB callbacks update RAM counters. Periodically (from usb_ccid_tick,
// running in the main loop) we commit a snapshot of those counters to
// NVS. If the composite breaks and Serial dies, the user can flash a
// recovery build (USB_CCID_ENABLED=0) and on next boot we read the
// previous run's snapshot from NVS and dump it to Serial.
// ===========================================================================
static Preferences s_diag_prefs;

static void diag_save_to_nvs() {
  if (!s_diag_prefs.begin("ccid_diag", false)) return;
  s_diag_prefs.putUInt ("override",  s_diag_override_calls);
  s_diag_prefs.putUInt ("init",      s_diag_init_calls);
  s_diag_prefs.putUInt ("reset",     s_diag_reset_calls);
  s_diag_prefs.putUInt ("open",      s_diag_open_calls);
  s_diag_prefs.putUInt ("claimed",   s_diag_open_claimed);
  s_diag_prefs.putUChar("lastcls",   s_diag_last_open_class);
  s_diag_prefs.putUInt ("epatts",    s_diag_ep_open_attempts);
  s_diag_prefs.putUInt ("epfail",    s_diag_ep_open_failures);
  s_diag_prefs.putUInt ("xferarm",   s_diag_xfer_arm_attempts);
  s_diag_prefs.putUInt ("ctrl",      s_diag_control_calls);
  s_diag_prefs.putUChar("lastreq",   s_diag_last_control_req);
  s_diag_prefs.putUInt ("xfer",      s_diag_xfer_calls);
  s_diag_prefs.putUChar("lastep",    s_diag_last_xfer_ep);
  s_diag_prefs.putUInt ("mounted",   tud_mounted() ? 1 : 0);
  s_diag_prefs.putString("slot",     s_slot_used);
  s_diag_prefs.putUInt ("initcode",  (uint32_t)s_init_result);
  s_diag_prefs.putUInt ("descbuilt", s_desc_built ? 1 : 0);
  s_diag_prefs.putUInt ("opens",     USB_CCID_OPEN_ENDPOINTS);
  s_diag_prefs.putUInt ("arms",      USB_CCID_ARM_RX_XFER);
  s_diag_prefs.putUInt ("uptime",    millis());
  uint32_t saves = s_diag_prefs.getUInt("saves", 0) + 1;
  s_diag_prefs.putUInt ("saves",     saves);
  s_diag_prefs.end();
}

static void diag_dump_from_nvs() {
  if (!s_diag_prefs.begin("ccid_diag", true)) {
    Serial.println("[CCID-DIAG-NVS] no namespace yet (first run, or wiped)");
    return;
  }
  Serial.println("[CCID-DIAG-NVS] === Previous run snapshot ===");
  Serial.printf("  saves        = %u\n",  s_diag_prefs.getUInt("saves", 0));
  Serial.printf("  uptime_ms    = %u\n",  s_diag_prefs.getUInt("uptime", 0));
  Serial.printf("  flags        = OPEN_EPS=%u ARM_RX=%u\n",
                s_diag_prefs.getUInt("opens", 99),
                s_diag_prefs.getUInt("arms", 99));
  Serial.printf("  desc_built   = %u  slot=%s  init_code=0x%X\n",
                s_diag_prefs.getUInt("descbuilt", 0),
                s_diag_prefs.getString("slot", "?").c_str(),
                s_diag_prefs.getUInt("initcode", 0));
  Serial.printf("  mounted      = %u\n",  s_diag_prefs.getUInt("mounted", 0));
  Serial.printf("  override=%u  init=%u  reset=%u\n",
                s_diag_prefs.getUInt("override", 0),
                s_diag_prefs.getUInt("init", 0),
                s_diag_prefs.getUInt("reset", 0));
  Serial.printf("  open=%u  claimed=%u  last_class=0x%02X\n",
                s_diag_prefs.getUInt("open", 0),
                s_diag_prefs.getUInt("claimed", 0),
                s_diag_prefs.getUChar("lastcls", 0xFF));
  Serial.printf("  ep_open_attempts=%u  ep_open_failures=%u\n",
                s_diag_prefs.getUInt("epatts", 0),
                s_diag_prefs.getUInt("epfail", 0));
  Serial.printf("  xfer_arm_attempts=%u\n",
                s_diag_prefs.getUInt("xferarm", 0));
  Serial.printf("  control=%u  last_req=0x%02X\n",
                s_diag_prefs.getUInt("ctrl", 0),
                s_diag_prefs.getUChar("lastreq", 0xFF));
  Serial.printf("  xfer=%u  last_ep=0x%02X\n",
                s_diag_prefs.getUInt("xfer", 0),
                s_diag_prefs.getUChar("lastep", 0xFF));
  Serial.println("[CCID-DIAG-NVS] === end ===");
  s_diag_prefs.end();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void usb_ccid_init() {
  // Dump any leftover diagnostic from the previous run BEFORE we
  // start writing new data into NVS.
  diag_dump_from_nvs();
  Serial.printf("[USBCCID] static-init result: slot=%s code=0x%X\n",
                s_slot_used, (unsigned)s_init_result);
}

bool usb_ccid_is_mounted() {
  return tud_mounted();
}

// Attach state. Tracks whether USB.begin() has run yet — first
// attach() initialises TinyUSB, subsequent attaches just connect
// the device on the bus.
static bool s_usb_ever_started = false;

void usb_ccid_attach() {
  if (!s_usb_ever_started) {
    Serial.println("[USBCCID] first attach — calling USB.begin()");
    USB.productName("Authuino Reader");
    USB.manufacturerName("Authuino");
    USB.begin();
    s_usb_ever_started = true;
    return;  // tud_init() leaves device connected by default
  }
  Serial.println("[USBCCID] re-attach — tud_connect()");
  tud_connect();
}

void usb_ccid_detach() {
  if (!s_usb_ever_started) return;  // never attached, nothing to detach
  Serial.println("[USBCCID] detach — tud_disconnect()");
  tud_disconnect();
}

void usb_ccid_get_stats(UsbCcidLiveStats* out) {
  if (!out) return;
  out->cmd_total          = s_live_cmd_total;
  out->cmd_get_status     = s_live_cmd_get_status;
  out->cmd_power_on       = s_live_cmd_power_on;
  out->cmd_power_off      = s_live_cmd_power_off;
  out->cmd_xfr_block      = s_live_cmd_xfr_block;
  out->cmd_other          = s_live_cmd_other;
  out->last_cmd_type      = s_live_last_cmd_type;
  out->last_status        = s_live_last_status;
  out->last_error         = s_live_last_error;
  out->last_atr_len       = s_live_last_atr_len;
  out->last_apdu_len      = s_live_last_apdu_len;
  out->last_reader_active = s_live_last_reader;
  out->last_card_present  = s_live_last_card;
}

void usb_ccid_tick() {
  // Deferred rx arming: open() asks us (via s_diag_should_arm_rx) to
  // start the OUT-endpoint receive after enumeration is finished
  // rather than from inside open() itself.
  if (s_diag_should_arm_rx && tud_mounted() && s_ep_out) {
    s_diag_should_arm_rx = false;
    s_diag_xfer_arm_attempts++;
    // Use a single-packet buffer length to avoid any RX FIFO budget
    // collision; subsequent packets are received in xfer_cb's re-arm.
    usbd_edpt_xfer(s_rhport, s_ep_out, s_rx_buf, CCID_BULK_PSIZE);
  }

  // Reset notify state when device is unmounted, so the next mount
  // triggers a fresh "first notify" telling the host the current
  // slot state.
  static bool s_was_mounted = false;
  bool mounted = tud_mounted();
  if (mounted && !s_was_mounted) {
    s_first_notify_pending = true;
  }
  s_was_mounted = mounted;

  // Send NotifySlotChange when the visible card state changes. The
  // host then re-runs IccPowerOn instead of using cached state.
  ccid_check_and_notify_slot_change();

  uint32_t now = millis();

  // Frequent NVS save so that if the composite breaks and Serial dies,
  // we still have a recent snapshot to read on the next boot.
  static uint32_t lastSaveMs = 0;
  if (now - lastSaveMs >= 1000) {
    lastSaveMs = now;
    diag_save_to_nvs();
  }

  static uint32_t lastStatusMs = 0;
  if (now - lastStatusMs < 5000) return;
  lastStatusMs = now;

  Serial.println("[USBCCID-DIAG] -----");
  Serial.printf("[USBCCID-DIAG] desc_registered=%d slot=%s init_code=0x%X mounted=%d\n",
                (int)s_desc_built, s_slot_used,
                (unsigned)s_init_result, (int)tud_mounted());
  Serial.printf("[USBCCID-DIAG] flags: OPEN_EPS=%d ARM_RX=%d\n",
                USB_CCID_OPEN_ENDPOINTS, USB_CCID_ARM_RX_XFER);
  Serial.printf("[USBCCID-DIAG] override=%lu init=%lu reset=%lu\n",
                (unsigned long)s_diag_override_calls,
                (unsigned long)s_diag_init_calls,
                (unsigned long)s_diag_reset_calls);
  Serial.printf("[USBCCID-DIAG] open=%lu claimed=%lu last_class=0x%02X\n",
                (unsigned long)s_diag_open_calls,
                (unsigned long)s_diag_open_claimed,
                (unsigned)s_diag_last_open_class);
  Serial.printf("[USBCCID-DIAG] ep_open_attempts=%lu ep_open_failures=%lu\n",
                (unsigned long)s_diag_ep_open_attempts,
                (unsigned long)s_diag_ep_open_failures);
  Serial.printf("[USBCCID-DIAG] xfer_arm_attempts=%lu should_arm_rx=%d\n",
                (unsigned long)s_diag_xfer_arm_attempts,
                (int)s_diag_should_arm_rx);
  Serial.printf("[USBCCID-DIAG] control=%lu last_request=0x%02X\n",
                (unsigned long)s_diag_control_calls,
                (unsigned)s_diag_last_control_req);
  Serial.printf("[USBCCID-DIAG] xfer=%lu last_ep=0x%02X\n",
                (unsigned long)s_diag_xfer_calls,
                (unsigned)s_diag_last_xfer_ep);
  Serial.printf("[USBCCID-DIAG] active itf=%u in=0x%02X out=0x%02X\n",
                (unsigned)s_itf_active,
                (unsigned)s_ep_in,
                (unsigned)s_ep_out);
}

#else  // ARDUINO_USB_MODE == 1 OR USB_CCID_ENABLED == 0

// In recovery mode (USB_CCID_ENABLED=0) we still want to show the
// NVS snapshot from a previous CCID-enabled run, so the user can flash
// a broken CCID build, then flash this recovery build, and see what
// state the broken run left in NVS.

#include <Preferences.h>
static Preferences s_diag_prefs_recovery;

static void diag_dump_from_nvs_recovery() {
  if (!s_diag_prefs_recovery.begin("ccid_diag", true)) {
    Serial.println("[CCID-DIAG-NVS] no namespace yet (no prior CCID run captured)");
    return;
  }
  Serial.println();
  Serial.println("[CCID-DIAG-NVS] === Previous run snapshot ===");
  Serial.printf("  saves        = %u\n",  s_diag_prefs_recovery.getUInt("saves", 0));
  Serial.printf("  uptime_ms    = %u\n",  s_diag_prefs_recovery.getUInt("uptime", 0));
  Serial.printf("  flags        = OPEN_EPS=%u ARM_RX=%u\n",
                s_diag_prefs_recovery.getUInt("opens", 99),
                s_diag_prefs_recovery.getUInt("arms", 99));
  Serial.printf("  desc_built   = %u  slot=%s  init_code=0x%X\n",
                s_diag_prefs_recovery.getUInt("descbuilt", 0),
                s_diag_prefs_recovery.getString("slot", "?").c_str(),
                s_diag_prefs_recovery.getUInt("initcode", 0));
  Serial.printf("  mounted      = %u\n",  s_diag_prefs_recovery.getUInt("mounted", 0));
  Serial.printf("  override=%u  init=%u  reset=%u\n",
                s_diag_prefs_recovery.getUInt("override", 0),
                s_diag_prefs_recovery.getUInt("init", 0),
                s_diag_prefs_recovery.getUInt("reset", 0));
  Serial.printf("  open=%u  claimed=%u  last_class=0x%02X\n",
                s_diag_prefs_recovery.getUInt("open", 0),
                s_diag_prefs_recovery.getUInt("claimed", 0),
                s_diag_prefs_recovery.getUChar("lastcls", 0xFF));
  Serial.printf("  ep_open_attempts=%u  ep_open_failures=%u\n",
                s_diag_prefs_recovery.getUInt("epatts", 0),
                s_diag_prefs_recovery.getUInt("epfail", 0));
  Serial.printf("  xfer_arm_attempts=%u\n",
                s_diag_prefs_recovery.getUInt("xferarm", 0));
  Serial.printf("  control=%u  last_req=0x%02X\n",
                s_diag_prefs_recovery.getUInt("ctrl", 0),
                s_diag_prefs_recovery.getUChar("lastreq", 0xFF));
  Serial.printf("  xfer=%u  last_ep=0x%02X\n",
                s_diag_prefs_recovery.getUInt("xfer", 0),
                s_diag_prefs_recovery.getUChar("lastep", 0xFF));
  Serial.println("[CCID-DIAG-NVS] === end ===");
  Serial.println();
  s_diag_prefs_recovery.end();
}

void usb_ccid_init() {
  Serial.println("[USBCCID] disabled (USB_CCID_ENABLED=0 or non-OTG mode)");
  diag_dump_from_nvs_recovery();
}

bool usb_ccid_is_mounted() { return false; }

void usb_ccid_attach() {}
void usb_ccid_detach() {}

void usb_ccid_get_stats(UsbCcidLiveStats* out) {
  if (out) memset(out, 0, sizeof(*out));
}

void usb_ccid_tick() {
  // Re-print the NVS snapshot every 10 seconds so a serial monitor
  // that opened late still gets to see it.
  static uint32_t lastDumpMs = 0;
  uint32_t now = millis();
  if (now - lastDumpMs >= 10000) {
    lastDumpMs = now;
    diag_dump_from_nvs_recovery();
  }
}

#endif