// ===========================================================================
//  nvs_aid.cpp — see nvs_aid.h for design notes
// ===========================================================================
#include "nvs_aid.h"
#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

#define AIDLIST_KEY  "aidlist"

static Preferences _prefs;
static bool        _ready = false;

#define BUF_BYTES (1 + NVS_AID_MAX_COUNT * sizeof(AidEntry))

// Read the full blob; returns the byte count actually read (0 if absent
// or NVS unavailable).
static size_t read_blob(uint8_t* buf) {
  if (!_ready) return 0;
  return _prefs.getBytes(AIDLIST_KEY, buf, BUF_BYTES);
}

void nvs_aid_init() {
  if (_ready) return;
  if (_prefs.begin("authuino", /*readOnly=*/false)) {
    _ready = true;
    int n = nvs_aid_count();
    Serial.printf("[AID] init: %d stored AID(s)\n", n);
  } else {
    Serial.println("[AID] init FAILED");
  }
}

int nvs_aid_count() {
  if (!_ready) return 0;
  uint8_t buf[BUF_BYTES];
  size_t n = read_blob(buf);
  if (n < 1) return 0;
  int count = buf[0];
  if (count < 0 || count > NVS_AID_MAX_COUNT) count = 0;
  return count;
}

bool nvs_aid_get(int idx, AidEntry* out) {
  if (!_ready || !out) return false;
  uint8_t buf[BUF_BYTES];
  size_t n = read_blob(buf);
  if (n < 1) return false;
  int count = buf[0];
  if (count > NVS_AID_MAX_COUNT) count = NVS_AID_MAX_COUNT;
  if (idx < 0 || idx >= count) return false;
  size_t off = 1 + (size_t)idx * sizeof(AidEntry);
  if (off + sizeof(AidEntry) > n) return false;
  memcpy(out, buf + off, sizeof(AidEntry));
  // Defensive: ensure name is null-terminated even if storage was truncated.
  out->name[NVS_AID_MAX_NAME - 1] = '\0';
  return true;
}

bool nvs_aid_add(const uint8_t* aid, uint8_t aid_len, const char* name) {
  if (!_ready)                                   return false;
  if (!aid || aid_len == 0 || aid_len > NVS_AID_MAX_BYTES) return false;
  if (!name || !*name)                           return false;

  int count = nvs_aid_count();
  if (count >= NVS_AID_MAX_COUNT) {
    Serial.println("[AID] add: list is full");
    return false;
  }

  uint8_t buf[BUF_BYTES];
  memset(buf, 0, sizeof(buf));
  if (count > 0) read_blob(buf);

  // Shift existing entries down by one slot — new entry goes to index 0
  // so newest-added has highest priority during oath_select().
  if (count > 0) {
    memmove(buf + 1 + sizeof(AidEntry),
            buf + 1,
            (size_t)count * sizeof(AidEntry));
  }

  AidEntry* e = (AidEntry*)(buf + 1);
  e->aid_len = aid_len;
  memset(e->aid, 0, sizeof(e->aid));
  memcpy(e->aid, aid, aid_len);
  strncpy(e->name, name, NVS_AID_MAX_NAME - 1);
  e->name[NVS_AID_MAX_NAME - 1] = '\0';

  buf[0] = (uint8_t)(count + 1);
  size_t bytes = 1 + (size_t)(count + 1) * sizeof(AidEntry);
  size_t wrote = _prefs.putBytes(AIDLIST_KEY, buf, bytes);
  if (wrote != bytes) {
    Serial.printf("[AID] add: putBytes wrote %u / %u\n",
                  (unsigned)wrote, (unsigned)bytes);
    return false;
  }

  Serial.printf("[AID] + %s (%u-byte AID) -> priority 0, total %d\n",
                name, aid_len, count + 1);
  return true;
}

bool nvs_aid_remove(int idx) {
  if (!_ready) return false;
  int count = nvs_aid_count();
  if (idx < 0 || idx >= count) return false;

  uint8_t buf[BUF_BYTES];
  size_t n = read_blob(buf);
  if (n < 1) return false;

  // Shift entries after idx down by one slot.
  if (idx < count - 1) {
    size_t move_count = (size_t)(count - 1 - idx);
    memmove(buf + 1 + (size_t)idx       * sizeof(AidEntry),
            buf + 1 + (size_t)(idx + 1) * sizeof(AidEntry),
            move_count * sizeof(AidEntry));
  }

  int new_count = count - 1;
  buf[0] = (uint8_t)new_count;

  if (new_count == 0) {
    _prefs.remove(AIDLIST_KEY);
  } else {
    size_t bytes = 1 + (size_t)new_count * sizeof(AidEntry);
    _prefs.putBytes(AIDLIST_KEY, buf, bytes);
  }

  Serial.printf("[AID] - idx %d (remaining %d)\n", idx, new_count);
  return true;
}
