// Boot easter egg alongside the home profile: retro 8-bit boot sound (E5-G5-B5-E6 square-wave arpeggio).
// Synthesized in code, no audio assets; plays in a dedicated short-lived task and calls bsp_audio_deinit right after
// to return codec/DMA memory to the system - boot borrows it, never keeps it.
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Create a one-shot task to play the boot sound (async, does not block boot). Calling it again is harmless.
void boot_chime_start(void);
bool boot_chime_busy(void);

#ifdef __cplusplus
}
#endif
