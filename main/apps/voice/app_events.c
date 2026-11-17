// main/app_events.c - application event queue implementation.
#include "app_events.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static const char *TAG = "app_evt";

#define APP_EVENT_QUEUE_DEPTH 8

// Static queue: 8×240B ≈ 1.9KB statically allocated, removing the boot heap peak and the creation-failure branch (M2).
// Depth 16->8 (performance pass, saves 1856B): consuming takes ~µs per event; backlog only builds during app_task's long-action
// windows (voice.end drain ≤500ms / cancel ≤200ms), which are orthogonal in time to high-burst sources (transcript/CTRL lines);
// drop-when-full + logging is unchanged, and critical wind-down events use the blocking post_important
// so they are not lost; the worst that gets dropped is a display-only transcript line (overwritten line by line, self-healing, visible in logs).
// Note: after TIME_SET added an int64 the struct is 240B (depth 8 = 1920B); the analysis above is unchanged (independent of size).
static StaticQueue_t s_queue_struct;
static uint8_t s_queue_storage[APP_EVENT_QUEUE_DEPTH * sizeof(app_event_t)];
static QueueHandle_t s_queue;

esp_err_t app_events_init(void) {
    s_queue = xQueueCreateStatic(APP_EVENT_QUEUE_DEPTH, sizeof(app_event_t),
                                 s_queue_storage, &s_queue_struct);
    if (!s_queue) {   // static creation only fails on a storage alignment fault; kept as a defensive check
        ESP_LOGE(TAG, "event queue create failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

void app_event_post(const app_event_t *ev) {
    if (!s_queue || !ev) return;
    if (xQueueSend(s_queue, ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "event queue full, dropping type=%d", (int)ev->type);
    }
}

esp_err_t app_event_post_important(const app_event_t *ev, uint32_t timeout_ms) {
    if (!s_queue || !ev) return ESP_ERR_INVALID_STATE;
    if (xQueueSend(s_queue, ev, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGW(TAG, "important event post timed out (%u ms), type=%d",
                 (unsigned)timeout_ms, (int)ev->type);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

QueueHandle_t app_events_queue(void) { return s_queue; }
