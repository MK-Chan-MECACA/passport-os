#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "bsp_button.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_ID_LAUNCHER = 0,
    APP_ID_RADIO,
    APP_ID_VOICE,
    APP_ID_XIAOZHI,
    APP_ID_WIFI,
    APP_ID_SETTINGS,
    APP_ID_HOME,
    APP_ID_GAME,
    APP_ID_TETRIS,
    APP_ID_STOPWATCH,
    APP_ID_SOUND_JUMP,
    APP_ID_MUSE,
    APP_ID_POMODORO,
    APP_ID_REMINDERS,
    APP_ID_LISTS,
    APP_ID_COUNT
} passport_app_id_t;

typedef enum {
    BTN_EVT_CLICK = 0,
    BTN_EVT_PRESS,
    BTN_EVT_RELEASE,
    BTN_EVT_LONG,
    BTN_EVT_LONG_UP,
    BTN_EVT_DOUBLE, // resolved by ADC task; reserved for global OK shortcut
} app_btn_event_t;

typedef struct passport_app {
    passport_app_id_t id;
    const char *name;           // Chinese display name
    const char *en_name;        // English label
    const char *desc;           // Short description
    const char *tag;            // Protocol/network feature tag (e.g. "Wi-Fi", "BLE")
    uint32_t theme_color;       // Theme color (RGB888)

    // Lifecycle hooks
    void (*init)(void);         // Initialize at boot
    void (*start)(void);        // Enter foreground and run
    void (*stop)(void);         // Leave foreground (clean up tasks, unregister network, free heap)
    void (*on_key)(uint8_t btn, app_btn_event_t event, uint16_t mv); // Key event dispatch
    // Request cooperative shutdown before releasing the UI or starting another
    // app. Return false while a worker still owns network/audio resources.
#ifdef __cplusplus
    bool (*prepare_stop)(void) = nullptr;
    // Consume long OK to navigate inside the app. False lets the registry exit.
    bool (*on_back)(void) = nullptr;
#else
    bool (*prepare_stop)(void);
    bool (*on_back)(void);
#endif
} passport_app_t;

#ifdef __cplusplus
}
#endif
