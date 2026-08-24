#pragma once

/*
 * Draw-buffer ramp experiment knobs — the single place to edit per step.
 * Buffer bytes = 480 * ML_DRAW_BUF_HEIGHT * 2 (RGB565), x2 when double buffered.
 * 48 rows = 1/10 of the 480-line panel = 46,080 bytes per buffer.
 */
/* 32 rows. Tried 64 on 2026-08-12 and reverted it: fps was identical (14.3 peak,
 * same interaction) while runtime heap fell 133,787 -> 61,995. Halving the number
 * of render passes does not change the total pixels pushed over QSPI, so it only
 * saves per-transfer overhead — and that is not the bottleneck here. Do not
 * re-raise this without a measurement showing a frame-rate gain.
 *
 * Measured 2026-08-23 under a CONTROL scroll, which settles what the bottleneck
 * actually is and confirms the paragraph above for the right reason:
 *
 *     render 54.0 ms   submit 0.5 ms/flush (5.5 ms/frame)   wait 0.0 ms
 *     11 flushes/redraw, 157k px/frame, 17.5 fps
 *
 * So the frame is 94% software rendering, the QSPI bus is ~10% and never
 * stalls, and a fit of render time against pixel count across four samples is a
 * straight line through the origin -- 2.77 Mpx/s, intercept -2.1 ms, residuals
 * under 0.75 ms. **87 CPU cycles per pixel.** There is no fixed per-frame cost
 * hiding anywhere, so nothing about buffer geometry can help: bigger buffers
 * cut per-transfer overhead that was already only half a millisecond, and the
 * pixels still have to be rendered one at a time either way. The lever is
 * making a pixel cheaper (SIMD blending, fewer anti-aliased masks), not moving
 * where they are staged.
 *
 * Do NOT move them to PSRAM either: esp_lvgl_adapter bounce-copies PSRAM draw
 * buffers through internal DMA staging on every flush, which collapses to
 * 2-5 fps (§5). Internal, and 32 rows. */
#define ML_DRAW_BUF_HEIGHT      32
#define ML_DRAW_BUF_USE_PSRAM   0
