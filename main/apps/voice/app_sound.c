// main/app_sound.c - prompt tone implementation.
// Each tone is one or more square-wave segments (frequency × duration), played back to back without gaps.
// Everything is 16kHz/16bit/mono - same format as the recording stream, so bsp_audio_set_format is called only once.
#include "app_sound.h"
#include "app_events.h"
#include "bsp_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "app_sound";

#define SAMPLE_RATE 16000
#define CHUNK_SAMPLES 256   // ~16ms per chunk, bounds the temporary buffer on the stack
#define AMPLITUDE 5000      // square-wave amplitude (±), a bit softer than the demo's 6000

// ---- Tone table: each tone = a few {frequency Hz, duration ms} segments ----
typedef struct { uint16_t hz; uint16_t ms; } tone_seg_t;

static const tone_seg_t TONE_TABLE[APP_TONE_COUNT][3] = {
    [APP_TONE_START]    = { { 440, 80 },  { 0, 0 }, { 0, 0 } },  // recording ready
    [APP_TONE_SEND]     = { { 880, 60 },  { 0, 0 }, { 0, 0 } },  // voice sent
    [APP_TONE_APPROVAL] = { { 587, 75 },  { 880, 75 }, { 0, 0 } }, // approval alert (two tones)
    [APP_TONE_SUCCESS]  = { { 660, 100 }, { 880, 100 }, { 0, 0 } }, // done (rising tone)
    [APP_TONE_REJECT]   = { { 220, 120 }, { 0, 0 }, { 0, 0 } },  // reject
    [APP_TONE_ERROR]    = { { 180, 100 }, { 0, 0 }, { 0, 0 } },  // offline / invalid
};

// Play one segment: generate the square wave a chunk at a time + blocking I2S write.
static void play_segment(uint16_t hz, uint16_t ms) {
    int16_t buf[CHUNK_SAMPLES];
    const int period = SAMPLE_RATE / hz;         // samples per period
    const int half = period / 2;
    int total = (int)SAMPLE_RATE * ms / 1000;
    int phase = 0;
    while (total > 0) {
        int n = total < CHUNK_SAMPLES ? total : CHUNK_SAMPLES;
        for (int i = 0; i < n; i++) {
            buf[i] = (phase < half) ? AMPLITUDE : -AMPLITUDE;
            if (++phase >= period) phase = 0;
        }
        if (bsp_audio_write(buf, (size_t)n * sizeof(int16_t)) != ESP_OK) {
            ESP_LOGE(TAG, "bsp_audio_write failed");
            return;
        }
        total -= n;
    }
}

static void play_tone_impl(app_tone_t tone) {
    if (tone >= APP_TONE_COUNT) return;
    const tone_seg_t (*segs)[3] = &TONE_TABLE[tone];
    for (int i = 0; i < 3; i++) {
        if ((*segs)[i].hz == 0) break;
        play_segment((*segs)[i].hz, (*segs)[i].ms);
    }
}

// ---- Async playback: static queue + worker ----
static StaticQueue_t s_queue_struct;
static uint8_t s_queue_storage[APP_TONE_COUNT * sizeof(uint8_t)];
static QueueHandle_t s_queue;

static void sound_worker(void *arg) {
    (void)arg;
    uint8_t tone;
    for (;;) {
        if (xQueueReceive(s_queue, &tone, portMAX_DELAY) == pdTRUE) {
            play_tone_impl((app_tone_t)tone);
            // START tone finished -> tell app_task to start the stream (time-sharing: the beep comes before capture).
            // Other tones do not need this and post no event.
            if (tone == APP_TONE_START) {
                app_event_t ev = { .type = APP_EV_TONE_DONE };
                app_event_post(&ev);
            }
        }
    }
}

esp_err_t app_sound_init(void) {
    esp_err_t e = bsp_audio_init();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "bsp_audio_init failed: %s", esp_err_to_name(e));
        return e;
    }
    e = bsp_audio_set_format(SAMPLE_RATE, 16, 1);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "bsp_audio_set_format failed: %s", esp_err_to_name(e));
        return e;
    }
    bsp_audio_set_volume(80);

    s_queue = xQueueCreateStatic(APP_TONE_COUNT, sizeof(uint8_t),
                                 s_queue_storage, &s_queue_struct);
    if (!s_queue) return ESP_FAIL;
    // Static stack (.bss): 1536B is enough for tones (single-segment 256-sample buf); at the heap limit every byte
    // decides whether the host task (5120B, created dynamically) can be created.
    // At 1536B the high-water mark left only 316B (measured 2026-08-28) -> 2048B.
    static StackType_t s_snd_stack[2048 / sizeof(StackType_t)];
    static StaticTask_t s_snd_tcb;
    // Priority 5 (raised from 3 on 2026-08-29, same level as ble_worker, still below audio_worker 6):
    // the stream only starts after the record beep finishes (TONE_DONE gate); at prio 3 it was queued behind app_task (4) and
    // taskLVGL (4, flushing 240x320 with 2-line buffers) - on real hardware, long-press detection to `capture started` took
    // 578ms while the beep itself is only 80ms. After the raise this dropped back to ~80ms. Tones are only tens of milliseconds of
    // blocking I2S writes (yielding the CPU while waiting on DMA), so they do not starve same- or lower-priority tasks; and they never overlap capture.
    if (!xTaskCreateStatic(sound_worker, "sound_worker", 2048, NULL, 5,
                           s_snd_stack, &s_snd_tcb)) {
        ESP_LOGE(TAG, "sound worker create failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "prompt sounds ready");
    return ESP_OK;
}

bool app_sound_play(app_tone_t tone) {
    if (xQueueSend(s_queue, &tone, 0) != pdTRUE) {
        ESP_LOGW(TAG, "prompt sound queue full, dropping %d", (int)tone);
        return false;
    }
    return true;
}

void app_sound_play_sync(app_tone_t tone) {
    play_tone_impl(tone);
}
