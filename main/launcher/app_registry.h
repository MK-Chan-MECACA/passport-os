#pragma once

#include "apps/app_base.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the app registry
void app_registry_init(void);

// Get the number of apps (excluding the Launcher)
size_t app_registry_get_count(void);

// Get the app descriptor at the given index
const passport_app_t *app_registry_get_by_index(size_t index);

// Get the app descriptor for the given ID
const passport_app_t *app_registry_get_by_id(passport_app_id_t id);

// Get the ID of the app currently active in the foreground
passport_app_id_t app_registry_get_active(void);
// True while another navigation is already retiring or starting an app.
bool app_registry_switch_pending(void);

// Call once at boot: if the last reboot was a soft reboot to enter a heavy app, returns that app ID (otherwise
// APP_ID_COUNT). The record is cleared once read.
passport_app_id_t app_registry_take_clean_launch(void);

// Switch the foreground app (passing APP_ID_LAUNCHER returns to the main menu)
void app_registry_switch_to(passport_app_id_t id);
// Run from the input task's full-size stack, never from sys_mon or LVGL.
void app_registry_process_pending_switch(void);
// Phone setup's background completion must not override a user shortcut.
void app_registry_complete_setup(void);

// Global unified key dispatch (double-press OK opens Xiaozhi, long-press back, and screen-off wake guard)
void app_registry_dispatch_key(uint8_t btn, app_btn_event_t event, uint16_t mv);

// Called by the system heartbeat; handles cross-app screen-off in one place without creating another resident task.
void app_registry_tick(void);
void app_registry_set_screen_timeout(uint16_t seconds);
uint16_t app_registry_get_screen_timeout(void);
bool app_registry_screen_is_on(void);
// Screen off and the foreground app is not streaming (radio/music/voice/Xiaozhi/
// Muse): background polling may slow down without anyone noticing.
bool app_registry_background_idle(void);
void app_registry_log_display_state(void);

// Activity (app activity) keep-alive: apps not driven by key presses (e.g. voice-controlled games) call this while running
// to hold off the screen-off timer. Only refreshes the timer; it never lights up a screen that is already off.
void app_registry_keep_awake(void);
// Caller holds LVGL lock. Wake without switching apps; false during handoff.
bool app_registry_wake_for_notice(void);
// Caller holds LVGL lock. Never wait for the lifecycle lock or seize audio.
bool app_registry_sound_notice(void);

#ifdef __cplusplus
}
#endif
