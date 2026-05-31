// ===========================================================================
//  nvs_aid — persistent list of OATH-applet AIDs to try on card insert
//
//  The compiled-in default AID (APEX_TOTP_AID in sc_interface.h) is always
//  attempted as a fallback. Stored AIDs are tried first, in stored order;
//  if any succeed, the matching applet is used. The user can manage this
//  list via the Manage tile (UI) or the AID serial commands (sketch).
//
//  Storage: single packed blob in the "authuino" NVS namespace under key
//  "aidlist": { uint8_t count, count × AidEntry }.
// ===========================================================================
#pragma once

#include <stdint.h>

#define NVS_AID_MAX_COUNT  8
#define NVS_AID_MAX_BYTES 16
#define NVS_AID_MAX_NAME  32

typedef struct {
  uint8_t aid_len;                       // 5..NVS_AID_MAX_BYTES
  uint8_t aid[NVS_AID_MAX_BYTES];        // zero-padded if shorter
  char    name[NVS_AID_MAX_NAME];        // null-terminated, UTF-8
} __attribute__((packed)) AidEntry;

void nvs_aid_init();
int  nvs_aid_count();                                  // 0..NVS_AID_MAX_COUNT
bool nvs_aid_get(int idx, AidEntry* out);
bool nvs_aid_add(const uint8_t* aid, uint8_t aid_len, const char* name);
bool nvs_aid_remove(int idx);
