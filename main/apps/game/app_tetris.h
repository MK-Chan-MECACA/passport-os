// Tetris app.
// Memory strategy (same discipline as the voice-controlled bird game):
//   1. The only large object is the board canvas 80x160 RGB565 = 25.6KB, heap-allocated in start()/freed in stop()
//      (largest free block before entry ~106KB, 4x headroom); zero heap allocation during play;
//   2. Game loop = 50ms lv_timer, no extra task; logic lives in tetris_core (pure integer);
//   3. The result panel is shown/hidden by moving it off-screen - the HIDDEN flag path has a hang bug in LVGL 9.3 (see app_game);
//   4. After stop() the heap returns to its pre-entry level.
#pragma once

#include "apps/app_base.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const passport_app_t g_tetris_app;

#ifdef __cplusplus
}
#endif
