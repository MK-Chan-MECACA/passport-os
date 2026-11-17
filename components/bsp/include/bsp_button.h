// components/bsp/include/bsp_button.h
// Three buttons share one ADC pin, distinguished by divider resistors. See bsp_pins.h for the voltage windows.
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Button index. The count is BSP_BTN_COUNT in bsp_pins.h (a hardware property, owned by the pin table);
// no trailing count entry is defined here, to avoid two near-identical names BSP_BTN_COUNT / BSP_BTN_COUNT_.
typedef enum {
    BSP_BTN_UP = 0,
    BSP_BTN_DOWN,
    BSP_BTN_OK,
} bsp_btn_t;

typedef enum {
    BSP_BTN_PRESS = 0,   // moment of press (low latency, for game-style instant response)
    BSP_BTN_RELEASE,     // moment of release (Push-to-Talk's "release to end" relies on it)
    BSP_BTN_CLICK,       // single click (press and release)
    BSP_BTN_DOUBLE,      // double click
    BSP_BTN_LONG,        // long press (the moment the threshold is reached; UP held 0.5s = start talking)
    BSP_BTN_LONG_UP,     // release after a long press (RELEASE is not reported in long-press state; UP = "stop talking")
} bsp_btn_ev_t;

// Button event callback. Runs in the button component's timer task; do not block or do heavy work in it.
typedef void (*bsp_btn_cb_t)(bsp_btn_t btn, bsp_btn_ev_t ev, void *user);

esp_err_t bsp_button_init(bsp_btn_cb_t cb, void *user);

// Read the current raw ADC voltage (mV). About 3300 when released; while a button is held, about that button's divider voltage.
// ★ After changing divider/pull-up resistors, use this to measure your three levels, then update BSP_BTN_MV_TABLE in bsp_pins.h.
// Returns -1 on read failure.
int bsp_button_read_mv(void);

// Panel power-up transient suppression: called at the entry of bsp_display_power(true). SLPOUT+DISPON+LVGL
// refresh SPI activity couples into ADC1_CH0, ADC reads ~0mV continuously → fake UP press → fake PTT
// (captured on a real device 2026-08-28). Within the window button checks just report "not pressed".
void bsp_button_suppress_panel_glitch(void);

#ifdef __cplusplus
}
#endif
