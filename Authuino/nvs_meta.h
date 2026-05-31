// ===========================================================================
//  nvs_meta — anonymous, persistent credential metadata
//
//  We never store credential names, secrets, or any PII in NVS. The only
//  things persisted here are derived from a SHA-256 hash of the credential
//  name (truncated to 7 bytes), used as the NVS key. The value is a small
//  CredMeta struct (see nvs_meta.cpp) carrying:
//    - add_date    : RTC epoch when this device first saw the credential
//    - custom_pos  : user-assigned display position (0..N-1)
//                    NVS_META_CUSTOM_POS_UNSET if the user has never
//                    reordered the cred.
//
//  Multi-card caveat: hash collisions across cards are negligible (7 bytes
//  ≈ 56 bits), but a credential of the same name on two cards will share
//  an entry. That's acceptable — the metadata is "first time this name
//  was seen on this device".
//
//  Schema version: bumped on incompatible storage changes. Mismatched
//  version on boot triggers a full clear (logged on serial).
// ===========================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

#define NVS_META_CUSTOM_POS_UNSET ((uint16_t)0xFFFF)

// Open the NVS namespace and run any schema migration. Safe to call
// multiple times.
void     nvs_meta_init();

// add_date (RTC epoch) for `name`, or 0 if no entry exists, NVS is
// unavailable, or `name` is null/empty.
uint32_t nvs_meta_get_add_date(const char* name);

// custom_pos for `name`, or NVS_META_CUSTOM_POS_UNSET if not set.
uint16_t nvs_meta_get_custom_pos(const char* name);
void     nvs_meta_set_custom_pos(const char* name, uint16_t pos);

// If `name` is not yet stored AND `now` is non-zero (RTC is set), creates
// an entry with add_date=now and custom_pos=UNSET. Idempotent — no-op
// once the entry exists.
void     nvs_meta_ensure(const char* name, uint32_t now);

// Removes any stored metadata for `name`.
void     nvs_meta_remove(const char* name);

// Reconcile NVS storage with an authoritative list of credential names
// (typically the list returned by the card after a successful PIN auth).
// Removes NVS entries whose hashes don't match any current credential —
// i.e. credentials that were deleted on the card by another reader.
// Existing metadata for kept credentials is preserved as-is.
void     nvs_meta_reconcile(const char* const* names, int count);
