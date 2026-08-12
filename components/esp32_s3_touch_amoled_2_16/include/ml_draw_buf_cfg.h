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
 * Do NOT move them to PSRAM either: esp_lvgl_adapter bounce-copies PSRAM draw
 * buffers through internal DMA staging on every flush, which collapses to
 * 2-5 fps (§5). Internal, and 32 rows. */
#define ML_DRAW_BUF_HEIGHT      32
#define ML_DRAW_BUF_USE_PSRAM   0
