/*
 * quirc lib/quirc.c, compiled as part of this Arduino library.
 *
 * quirc's sources live in the pinned upstream submodule (upstream/lib/),
 * which is outside this library's src/ folder, so arduino-cli does not
 * compile them directly. Each upstream .c is pulled in here as its own
 * translation unit (one shim per file) — identical to quirc's normal
 * multi-TU build, which avoids any cross-file `static` symbol clashes.
 */
#include "../upstream/lib/quirc.c"
