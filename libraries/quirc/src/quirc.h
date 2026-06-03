#pragma once
/*
 * Arduino include shim for quirc.
 *
 * quirc's real public header lives in the pinned upstream submodule at
 * upstream/lib/quirc.h. This forwarding header places `quirc.h` on this
 * Arduino library's src/ include path, so the firmware can do
 * `#include "quirc.h"` (or <quirc.h>) and arduino-cli associates it with
 * this library.
 */
#include "../upstream/lib/quirc.h"
