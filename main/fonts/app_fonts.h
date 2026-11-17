#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Type roles are recorded in DESIGN.md (Typography). Every font here is linked;
// do not add one without a role there.

// Body: Source Han Sans SC 16px, 4bpp, Latin-only (ASCII, Latin-1, dashes, curly
// quotes, ellipsis, arrows, euro). Top-bar app names, body text, lists, key hints
// and all dynamic text (city names, transcripts, replies, stations, headlines).
// Regenerate both fonts with tools/gen_fonts.sh; tests/check_fonts.py rejects
// string literals the font cannot draw.
LV_FONT_DECLARE(buddy_font_16);
// Title: the same face and repertoire at 20px/4bpp for static titles.
LV_FONT_DECLARE(ui_font_20);
// Display readout: Montserrat 32 digits, '.', ':', space and "NET",
// copied from LVGL's built-in font by tools/gen_digit_font.py.
LV_FONT_DECLARE(ui_digits_32);
// Hero readout: digits and colon only; do not link the full
// Montserrat 48px repertoire.
LV_FONT_DECLARE(pomodoro_digits_48);

#ifdef __cplusplus
}
#endif
