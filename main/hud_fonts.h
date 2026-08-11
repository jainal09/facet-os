#pragma once
#include "lvgl.h"

/* Orbitron (SIL OFL), converted with lv_font_conv.
 * hud_clock_76: digits and colon only — a full ASCII range at this size would
 * cost ~10x the flash for glyphs a clock never draws. */
LV_FONT_DECLARE(hud_clock_76);
/* Same face at 48 px, for the lock screen in music mode: the now-playing panel
 * needs the height more than the clock needs to be enormous. Weight 700 to match
 * hud_clock_76 — the two are never on screen together, so a weight mismatch would
 * read as a different typeface rather than as a size change. */
LV_FONT_DECLARE(hud_clock_48);
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
#define ICON_TARGET     "\xEE\x8E\x9E"   /* U+E39E "adjust" — a bullseye, for FOCUS */
#define ICON_MUSIC      "\xEE\x90\x85"   /* U+E405 music_note */

/* A second, small icon face. app_icons_64 is sized for drawer tiles; a 64 px
 * glyph in a 56 px button is unusable. Five glyphs, ~12 KB of flash. */
LV_FONT_DECLARE(hud_icons_30);
/* U+E32D "speaker" — a single cabinet, the closest Material glyph to the icon
 * Spotify itself uses for Connect. A music note read as "play" and a plain
 * volume symbol now means volume, so both were wrong for this button. */
#define ICON_DEVICES    "\xEE\x8C\xAD"
#define ICON_VOL_UP     "\xEE\x81\x90"   /* U+E050 volume_up                 */
#define ICON_VOL_MUTE   "\xEE\x81\x8F"   /* U+E04F volume_off                */
#define ICON_HEART      "\xEE\xA1\xBD"   /* U+E87D favorite, filled          */
#define ICON_HEART_OPEN "\xEE\xA1\xBE"   /* U+E87E favorite_border           */
