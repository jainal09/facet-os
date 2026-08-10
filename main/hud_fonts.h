#pragma once
#include "lvgl.h"

/* Orbitron (SIL OFL), converted with lv_font_conv.
 * hud_clock_76: digits and colon only — a full ASCII range at this size would
 * cost ~10x the flash for glyphs a clock never draws. */
LV_FONT_DECLARE(hud_clock_76);
LV_FONT_DECLARE(hud_text_18);

/* Material Icons, four glyphs only — converting the whole set would cost
 * megabytes for icons we never draw. An icon FONT beats PNGs here: sharp at
 * any size, no decode, no cache, ~34 KB of flash and zero RAM. The SD card
 * stays for photographic assets, where its size actually buys something. */
LV_FONT_DECLARE(app_icons_64);
#define ICON_DASHBOARD  "\xEE\xA1\xB1"   /* U+E871 */
#define ICON_PETS       "\xEE\xA4\x9D"   /* U+E91D */
#define ICON_WIFI       "\xEE\x98\xBE"   /* U+E63E */
#define ICON_MEMORY     "\xEE\x8C\xA2"   /* U+E322 */
