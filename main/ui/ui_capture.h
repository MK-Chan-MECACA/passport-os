#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * On-device UI screenshot (for debugging).
 *
 * The C3 internal heap cannot hold a full 240x320 RGB565 frame (153600 bytes), so this **renders in horizontal bands**:
 * it manipulates the LVGL layer directly (the same mechanism as lv_snapshot), restricting the layer's buf_area /
 * _clip_area to a 240 x N band so the software renderer only draws that band, then immediately dumps
 * the pixels as hex text over the console before rendering the next band.
 * Peak memory is only 240 * N * 2 bytes (15KB at N=32) instead of 150KB.
 *
 * Hex (rather than raw binary) is used because Wi-Fi / battery and other tasks keep logging during capture,
 * which would break a raw binary stream beyond recovery; each line carries an `H:<row>:` prefix and the host only accepts that prefix,
 * so interleaved log lines are dropped naturally and missing rows can be detected.
 *
 * The caller must **already hold** bsp_lvgl_lock (lv_obj_redraw must run in LVGL context).
 */
void ui_capture_dump_hex(void);

#ifdef __cplusplus
}
#endif
