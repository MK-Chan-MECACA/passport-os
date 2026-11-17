// main/time_sync.c - wall-clock time sync (the only time source is the PC client).
// Single entry point for three channels: BLE protocol line time.set, USB SYS command time set, manual REPL.
// State: not synced after reboot (valid=false); once set_epoch sets it, it stays valid for this session.
// Time zone: minute offset s_tz_min. NVS key tz_min (set automatically from the weather API) takes priority, otherwise the old
// tz_hour (int8, default +8, ±12 check). No TZ env.
#include "time_sync.h"
#include "nvs_settings.h"
#include "esp_log.h"
#include <time.h>
#include <sys/time.h>  // settimeofday
#include <stdio.h>

static const char *TAG = "time_sync";

#define TZ_DEFAULT_HOUR 8
#define TZ_MAX_HOUR     12
#define TZ_MIN_MINUTES  (-720)
#define TZ_MAX_MINUTES  840   /* UTC+14 (Kiribati) */

// s_epoch is the sync baseline (single write point: set_epoch); settimeofday syncs the system clock, which keeps
// running - the device display reads time(NULL) (see format_local), otherwise it would stay frozen at the sync
// moment (seen on real hardware: stuck at 13:46). Host tests have no system clock, so they format from s_epoch directly.
static int64_t s_epoch = 0;
static bool s_valid = false;
static int s_tz_min = TZ_DEFAULT_HOUR * 60;

esp_err_t time_sync_init(void)
{
    int16_t minutes = 0;
    if (nvs_settings_get_tz_min(&minutes) == ESP_OK) {
        s_tz_min = minutes;
    } else {
        int8_t tz = 0;
        nvs_settings_get_tz_hour(&tz);   // already has a fallback inside: cannot open / corrupt / missing -> 8
        s_tz_min = ((tz >= -TZ_MAX_HOUR && tz <= TZ_MAX_HOUR) ? tz : TZ_DEFAULT_HOUR) * 60;
    }
    s_valid = false;
    return ESP_OK;
}

void time_sync_set_epoch(int64_t epoch_sec)
{
    s_epoch = epoch_sec;
    s_valid = true;
#ifdef ESP_PLATFORM
    struct timeval tv = { .tv_sec = (time_t)epoch_sec, .tv_usec = 0 };
    settimeofday(&tv, NULL);   // failure ignored: the UI uses s_epoch, system time is only a backup for log timestamps
#endif
}

bool time_sync_valid(void) { return s_valid; }
int64_t time_sync_last_epoch(void) { return s_epoch; }

void passport_phone_time(int64_t epoch, int minutes) {
    if (epoch < 1700000000LL || epoch > 4102444800LL ||
        minutes < -720 || minutes > 840 || minutes % 15) return;
    if (time(NULL) < 1700000000LL) time_sync_set_epoch(epoch);
    time_sync_set_tz_offset_min(minutes);
}

int time_sync_tz_hour(void) { return s_tz_min / 60; }

int time_sync_tz_offset_sec(void) { return s_tz_min * 60; }

esp_err_t time_sync_set_tz_offset_min(int minutes)
{
    if (minutes < TZ_MIN_MINUTES || minutes > TZ_MAX_MINUTES) return ESP_ERR_INVALID_ARG;
    if (minutes == s_tz_min) return ESP_OK;   // called on every weather refresh, so do not rewrite flash needlessly
    ESP_LOGI(TAG, "time zone auto-updated %+d -> %+d min", s_tz_min, minutes);
    s_tz_min = minutes;
    return nvs_settings_set_tz_min((int16_t)minutes);
}

esp_err_t time_sync_set_tz(int hour)
{
    if (hour < -TZ_MAX_HOUR || hour > TZ_MAX_HOUR) {
        ESP_LOGW(TAG, "time zone offset out of range %d (±%d)", hour, TZ_MAX_HOUR);
        return ESP_ERR_INVALID_ARG;
    }
    s_tz_min = hour * 60;
    nvs_settings_set_tz_min((int16_t)s_tz_min);   // manual setting overrides the automatic value
    return nvs_settings_set_tz_hour((int8_t)hour);
}

// Local time = current system clock + tz*3600, read as UTC (manual offset with gmtime_r, no dependence on
// TZ env). On the device set_epoch already called settimeofday so the clock keeps running; host tests
// have no system clock and format from the s_epoch baseline (deterministic reference). Returns the length written
// (fixed "--:--", 5 characters, when not synced).
int time_sync_format_local(char *buf, size_t cap)
{
    if (!s_valid) return snprintf(buf, cap, "--:--");
    time_t t;
#ifdef ESP_PLATFORM
    time_t now = time(NULL);
    if (now < s_epoch) now = (time_t)s_epoch;   // fallback if settimeofday did not take effect: freeze at the baseline
    t = now + (int64_t)s_tz_min * 60;
#else
    t = (time_t)(s_epoch + (int64_t)s_tz_min * 60);
#endif
    struct tm tm;
    gmtime_r(&t, &tm);
    return snprintf(buf, cap, "%02d:%02d", tm.tm_hour, tm.tm_min);
}
