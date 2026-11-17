// main/app_sound.h - prompt tone module.
// Square-wave tones output via bsp_audio_write (ported from the play_tone pattern in demo_audio.c).
// Strictly time-shared with recording: sound_worker plays the START tone then posts TONE_DONE, and app_task only starts the stream on receipt
// (S3 event-driven; app_task no longer blocks synchronously); the other tones are also queued asynchronously.
#pragma once

#include "app_types.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Creates the static queue + sound_worker task (prio 3, 2KB stack) and initializes the codec to 16k/16/1.
esp_err_t app_sound_init(void);

// Async play: queued and played by sound_worker; returns whether it was queued.
// After the START tone finishes it posts APP_EV_TONE_DONE (the start-stream signal); on false the caller must provide its own fallback.
bool app_sound_play(app_tone_t tone);

// Sync play: blocks in the caller's context until finished. Only kept for deterministic playback outside S3
// (no callers currently, kept against regressions; START is now async).
void app_sound_play_sync(app_tone_t tone);

#ifdef __cplusplus
}
#endif
