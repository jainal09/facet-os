#pragma once

/*
 * Draw-buffer ramp experiment knobs — the single place to edit per step.
 * Buffer bytes = 480 * ML_DRAW_BUF_HEIGHT * 2 (RGB565), x2 when double buffered.
 * 48 rows = 1/10 of the 480-line panel = 46,080 bytes per buffer.
 */
#define ML_DRAW_BUF_HEIGHT      32
#define ML_DRAW_BUF_USE_PSRAM   0
