// Home weather: Open-Meteo (no API key) current temperature + WMO weather code, mapped to a Chinese phrase.
// Coordinates are geocoded in the browser by the upload page and stored in the profile; the device makes only one small request.
// The last successful result is cached in NVS so the latest weather shows even when booting offline.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool valid;
    int16_t temp_c;    /* Celsius, rounded */
    uint16_t code;     /* WMO weather code */
    uint8_t humidity;  /* Relative humidity % */
    uint8_t wind_kmh;  /* Wind speed at 10 m, km/h (rounded) */
    uint16_t wind_dir; /* Wind direction at 10 m, degrees (0=north, clockwise) */
    uint32_t updated_at; /* UTC seconds of the successful fetch; 0 = unknown (clock not synced then) */
} home_weather_t;

// Open-Meteo/CAMS model concentration, not a Chinese AQI or indoor measurement.
// Volatile only: reuse the same city's sample for 30 minutes across app visits.
// Failed requests, city changes, expiry and device restarts invalidate it.
typedef struct {
    bool valid;
    uint32_t pm25_x10; /* tenths of micrograms per cubic metre */
} home_air_quality_t;

bool home_air_quality_get(home_air_quality_t *out);
esp_err_t home_air_quality_refresh(const char *lat, const char *lon);
void home_air_quality_label(bool connected, char *out, size_t capacity);
// Cancel bounded asynchronous HTTPS work before the Home task is stopped.
void home_weather_allow_requests(void);
void home_weather_cancel_requests(void);

// Load the NVS cache so the UI has "last weather" to show immediately.
void home_weather_init(void);

bool home_weather_get(home_weather_t *out);

// WMO code → Chinese phrase (all glyphs are in the buddy font).
const char *home_weather_code_text(uint16_t code);

// Beaufort scale: km/h → 0..12.
int home_weather_beaufort(uint8_t wind_kmh);

// Wind direction → Chinese compass label ("north wind"/"southeast wind"; calm returns "no wind"). All glyphs are in the buddy font.
const char *home_weather_wind_dir_text(uint16_t deg);

// Blocking fetch (lat/lon as decimal strings). Call from the Home background task without holding the LVGL lock.
esp_err_t home_weather_refresh(const char *lat, const char *lon);

// Data age phrase: "just now"/"25 min ago"/"3 h ago"/"2 days ago"; when updated_at or now is unknown
// returns "offline data". Pure function, easy to host-test.
void home_weather_age_text(uint32_t updated_at, int64_t now, char *out, size_t capacity);

// Incremented on every successful refresh; the UI uses it to redraw the weather row.
uint32_t home_weather_version(void);

#ifdef __cplusplus
}
#endif
