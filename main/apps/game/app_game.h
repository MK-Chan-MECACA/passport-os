// "Why Barking": a quirky voice-controlled app.
// Memory strategy (hard requirements for release):
//   1. No large buffers - no canvas/framebuffer; the bird and pipes are a resident pool of small objects (about 40
//      lv_obj, ~5KB), zero heap allocation during play;
//   2. No separate render task - the game loop is a 33ms lv_timer running in the LVGL task;
//      the only extra task is mic capture (3KB stack, computes as soon as a read fills; only one atomic variable between it and the UI);
//   3. stop() reclaims everything: delete the timer first, then tear down the screen; the mic task is joined before returning, heap back to its pre-entry level.
#pragma once

#include "apps/app_base.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const passport_app_t g_game_app;

#ifdef __cplusplus
}
#endif
