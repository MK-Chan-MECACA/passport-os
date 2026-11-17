// Boot animation: green phosphor style - "PASSPORT OS" typewriter cursor + green dot progress bar.
// Same palette as the home screen (black background / phosphor green), ~0.9s; keeps its own timing, caller must not hold the LVGL lock.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Show the boot animation and block for ~0.9s. Takes and releases the LVGL lock internally.
void boot_splash_run(void);

// Call after the home screen has loaded: deletes the boot screen objects (the animation screen is never shown again).
void boot_splash_end(void);

#ifdef __cplusplus
}
#endif
