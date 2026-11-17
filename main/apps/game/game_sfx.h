// "Why Barking" sound effects (game_sfx): 16kHz square wave, reusing the bsp_audio TX channel.
//
// Why a separate module instead of reusing app_sound from apps/voice:
//   app_sound depends on voice's app_events/app_types and would run bsp_audio_init again
//   + set_format - which races with the mic task opening/closing the codec. This module only does
//   bsp_audio_write (the codec is already opened by the mic task; full duplex, TX/RX are two independent I2S
//   handles); if the codec is not ready it silently skips, and never drives codec state itself.
//
// Cost: one static task with a 2KB stack, alive only while the game is in the foreground; play requests go through task notifications,
// so no queue RAM is used and the LVGL main loop is never blocked (otherwise a 60ms square wave = 2 dropped frames).
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GS_NONE = 0,
    GS_START,   /* start: short rising blip */
    GS_SCORE,   /* passed a pipe: crisp chime */
    GS_DEAD,    /* death: two falling notes */
    GS_BEST,    /* new record: three rising trumpet notes */
    GS_COUNT,
} game_sfx_t;

// Create the playback task (idempotent). Failure only affects sound, not gameplay.
void game_sfx_start(void);

// Delete the playback task (idempotent). Called when leaving the game, to reclaim the stack.
void game_sfx_stop(void);
bool game_sfx_prepare_stop(void);

// Asynchronously request a sound; silently dropped if not started or busy (sound must never slow down the main loop).
void game_sfx_play(game_sfx_t id);

#ifdef __cplusplus
}
#endif
