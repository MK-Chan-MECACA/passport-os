// Voice-control audio capture: reads the mic at 16kHz/16bit/mono and outputs a smoothed loudness of 0..4095.
// Small standalone task (3KB stack); only one atomic variable is shared with LVGL - the game loop never locks or waits.
//
// 2026-09-13 optimization:
//   * Added game_mic_ready() - distinguishes "room is quiet" from "mic not warmed up yet", so the upper layer can
//     decide whether to show a hint instead of misreading the warm-up 0 as "player isn't barking";
//   * game_mic_start() is now safely re-entrant: when re-entering the app right after leaving, if the old task is still
//     finishing (s_task non-null) it first waits for it to exit before creating a new one; before, it silently skipped creating the task -
//     which left that round with no voice control at all (this happened before).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Start the capture task (idempotent; if the previous task has not exited yet, wait for it first).
// Internally bsp_audio_init/set_format(16k) use the same config as the voice app;
// apps run mutually exclusively, so there is no contention for I2S.
void game_mic_start(void);

// Request exit and wait for the task to finish (each read takes at most 16ms; usually returns in <50ms).
void game_mic_stop(void);
bool game_mic_prepare_stop(void);

// Smoothed loudness (mean rectified × EMA). A quiet room is a few hundred, normal speech a few thousand.
// Always 0 while not ready (warm-up).
uint16_t game_mic_level(void);

// Whether the mic is past its power-up transient and the output is trustworthy. Warm-up is about 160ms.
bool game_mic_ready(void);

#ifdef __cplusplus
}
#endif
