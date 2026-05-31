// ===========================================================================
//  nvs_meta.cpp — see nvs_meta.h for design notes
// ===========================================================================
#include "nvs_meta.h"
#include <Arduino.h>
#include <Preferences.h>
#include <mbedtls/sha256.h>
#include <string.h>

// Bump on incompatible value-format changes. Mismatch triggers a full
// clear of the namespace on boot.
#define META_VERSION   2
#define META_VER_KEY   "ver"

// Per-credential value. Keep packed/aligned so the on-disk size is
// stable across compilers — NVS stores the raw bytes.
typedef struct {
  uint32_t add_date;     // RTC epoch when first seen
  uint16_t custom_pos;   // NVS_META_CUSTOM_POS_UNSET if unset
  uint16_t reserved;     // pad to 8 bytes; reserved for future flags
} __attribute__((packed)) CredMeta;

static_assert(sizeof(CredMeta) == 8, "CredMeta layout drifted");

static Preferences _prefs;
static bool        _ready = false;

// ---------------------------------------------------------------------------
// SHA-256 of the name, truncated to 7 bytes.
// ---------------------------------------------------------------------------
static void hash7(const char* name, uint8_t out[7]) {
  uint8_t full[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);                 // 0 = SHA-256 (not 224)
  mbedtls_sha256_update(&ctx, (const uint8_t*)name, strlen(name));
  mbedtls_sha256_finish(&ctx, full);
  mbedtls_sha256_free(&ctx);
  memcpy(out, full, 7);
}

// ---------------------------------------------------------------------------
// Build a 15-char NVS key (NVS_KEY_NAME_MAX_SIZE - 1 = 15) from a name.
// Format: 'm' + 14 hex chars from the 7-byte hash.
// The 'm' prefix segregates per-credential keys from bookkeeping keys
// like META_VER_KEY (which is never a hex-only string).
// ---------------------------------------------------------------------------
static void name_to_key(const char* name, char keyOut[16]) {
  static const char hex[] = "0123456789abcdef";
  uint8_t h[7];
  hash7(name, h);
  keyOut[0] = 'm';
  for (int i = 0; i < 7; i++) {
    keyOut[1 + i*2    ] = hex[(h[i] >> 4) & 0x0F];
    keyOut[1 + i*2 + 1] = hex[ h[i]       & 0x0F];
  }
  keyOut[15] = '\0';
}

static bool read_meta(const char* key, CredMeta* out) {
  size_t got = _prefs.getBytes(key, out, sizeof(CredMeta));
  return got == sizeof(CredMeta);
}

static void write_meta(const char* key, const CredMeta* in) {
  _prefs.putBytes(key, in, sizeof(CredMeta));
}

// ---------------------------------------------------------------------------
void nvs_meta_init() {
  if (_ready) return;
  if (!_prefs.begin("authuino", /*readOnly=*/false)) {
    Serial.println("[NVS] meta open FAILED — date sort and reorder will be unavailable");
    return;
  }
  _ready = true;

  uint8_t ver = _prefs.getUChar(META_VER_KEY, 0);
  if (ver != META_VERSION) {
    Serial.printf("[NVS] meta: schema v%u -> v%u, clearing existing entries\n",
                  ver, META_VERSION);
    _prefs.clear();
    _prefs.putUChar(META_VER_KEY, META_VERSION);
  }
  Serial.printf("[NVS] meta open v%u (free entries: %u)\n",
                (unsigned)META_VERSION, (unsigned)_prefs.freeEntries());
}

uint32_t nvs_meta_get_add_date(const char* name) {
  if (!_ready || !name || !*name) return 0;
  char key[16];
  name_to_key(name, key);
  CredMeta m;
  if (!read_meta(key, &m)) return 0;
  return m.add_date;
}

uint16_t nvs_meta_get_custom_pos(const char* name) {
  if (!_ready || !name || !*name) return NVS_META_CUSTOM_POS_UNSET;
  char key[16];
  name_to_key(name, key);
  CredMeta m;
  if (!read_meta(key, &m)) return NVS_META_CUSTOM_POS_UNSET;
  return m.custom_pos;
}

void nvs_meta_set_custom_pos(const char* name, uint16_t pos) {
  if (!_ready || !name || !*name) return;
  char key[16];
  name_to_key(name, key);
  CredMeta m = { 0, NVS_META_CUSTOM_POS_UNSET, 0 };
  read_meta(key, &m);                     // preserve add_date if present
  if (m.custom_pos == pos) return;        // no-op write avoidance
  m.custom_pos = pos;
  write_meta(key, &m);
}

void nvs_meta_ensure(const char* name, uint32_t now) {
  if (!_ready || !name || !*name) return;
  if (now == 0) return;                            // RTC not set yet
  char key[16];
  name_to_key(name, key);
  CredMeta m;
  if (read_meta(key, &m)) return;                  // already exists
  m.add_date   = now;
  m.custom_pos = NVS_META_CUSTOM_POS_UNSET;
  m.reserved   = 0;
  write_meta(key, &m);
  Serial.printf("[NVS] meta + %s @%lu\n", name, (unsigned long)now);
}

void nvs_meta_remove(const char* name) {
  if (!_ready || !name || !*name) return;
  char key[16];
  name_to_key(name, key);
  if (_prefs.isKey(key)) {
    _prefs.remove(key);
    Serial.printf("[NVS] meta - %s\n", name);
  }
}

// ---------------------------------------------------------------------------
// Reconcile
//
// We keep a "manifest" blob — a packed list of every per-credential hash
// we've ever stored — so we don't have to iterate NVS keys (which would
// require pulling in IDF version-specific headers). Each reconcile diffs
// the manifest against the authoritative current set, removes orphans,
// and rewrites the manifest.
//
// Manifest layout (key "mlist"):
//   uint8_t count
//   count × 7-byte hash
// ---------------------------------------------------------------------------
#define MANIFEST_KEY        "mlist"
#define MANIFEST_MAX        64        // upper bound on tracked credentials

void nvs_meta_reconcile(const char* const* names, int count) {
  if (!_ready) return;
  if (count > MANIFEST_MAX) count = MANIFEST_MAX;

  // Build the new manifest = current set of name hashes.
  uint8_t cur[MANIFEST_MAX][7];
  for (int i = 0; i < count; i++) {
    if (names[i]) hash7(names[i], cur[i]);
    else          memset(cur[i], 0, 7);
  }

  // Read the previous manifest, if any.
  uint8_t old_buf[1 + MANIFEST_MAX * 7];
  size_t  old_n = _prefs.getBytes(MANIFEST_KEY, old_buf, sizeof(old_buf));

  int orphans = 0;
  if (old_n >= 1) {
    int old_count = old_buf[0];
    if (old_count > MANIFEST_MAX) old_count = MANIFEST_MAX;
    if ((size_t)(1 + old_count * 7) <= old_n) {
      static const char hex[] = "0123456789abcdef";
      for (int i = 0; i < old_count; i++) {
        const uint8_t* h = old_buf + 1 + i * 7;
        bool kept = false;
        for (int j = 0; j < count; j++) {
          if (memcmp(h, cur[j], 7) == 0) { kept = true; break; }
        }
        if (kept) continue;

        // Orphan — rebuild the per-cred key from the hash and drop it.
        char key[16];
        key[0] = 'm';
        for (int k = 0; k < 7; k++) {
          key[1 + k*2    ] = hex[(h[k] >> 4) & 0x0F];
          key[1 + k*2 + 1] = hex[ h[k]       & 0x0F];
        }
        key[15] = '\0';
        if (_prefs.isKey(key)) {
          _prefs.remove(key);
          orphans++;
        }
      }
    }
  }

  // Rewrite the manifest to match the current set.
  uint8_t new_buf[1 + MANIFEST_MAX * 7];
  new_buf[0] = (uint8_t)count;
  if (count > 0) memcpy(new_buf + 1, cur, count * 7);
  _prefs.putBytes(MANIFEST_KEY, new_buf, 1 + count * 7);

  Serial.printf("[NVS] reconcile: %d kept, %d orphan(s) removed\n",
                count, orphans);
}
