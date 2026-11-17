// Home clock: once Wi-Fi is up, SNTP sets the system clock; the UI formats the date from system time + local
// time zone offset. The time zone reuses the time_sync (voice app) NVS setting, consistent system-wide.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Idempotently start SNTP (network must be ready). Repeated calls are harmless.
void home_clock_start(void);

// Stop SNTP and release the network dependency (call when leaving Home). An already-synced system clock keeps running.
void home_clock_stop(void);

// Whether the system clock is synced (valid if after 2023; set by either PC sync or SNTP).
bool home_clock_valid(void);

// "12:34" / "--:--"
void home_clock_format_time(char *buf, size_t cap);

// "12 Sep"
void home_clock_format_date(char *buf, size_t cap);

// "Sun".."Sat"
void home_clock_format_weekday(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
