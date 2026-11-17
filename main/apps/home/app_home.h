// Home (personal page): avatar + name/signature + clock + weather + two stat cards (placeholders, always --).
// This is a full app (not part of the Launcher) and follows the registry's "stop deletes the screen"
// convention - the Launcher has freed its UI on entry, and this app likewise deletes all its LVGL objects on exit.
#pragma once

#include "apps/app_base.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const passport_app_t g_home_app;

#ifdef __cplusplus
}
#endif
