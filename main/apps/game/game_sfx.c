// main/apps/game/game_sfx.c - see the design notes in game_sfx.h.
#include "apps/game/game_sfx.h"

#include "bsp_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>

static const char *TAG = "game_sfx";

#define SFX_RATE  16000  /* same format as the mic: bsp_audio_set_format(16000,16,1) */
#define SFX_CHUNK 128    /* 8ms per chunk; 256B on the stack */
#define SFX_AMP   4200   /* square-wave amplitude (±), a bit softer than the voice prompt tones to avoid harshness */

/* Startup race: game_start() starts the sfx task first, but the mic task needs another ~200ms before
   bsp_audio_set_format() opens the output (measured: write requested at 14473, open only at 14667).
   The old code "muted permanently after one failed write" → the start sound always failed → no sound for the whole round.
   Now a bounded retry: on write failure wait for the codec to be ready, and only declare it unavailable after a timeout (600ms). */
#define SFX_READY_WAIT_MS 600
#define SFX_READY_STEP_MS 20

typedef struct {
    uint16_t hz;
    uint16_t ms;
} sfx_seg_t;

/* Tone table: up to 3 segments per sound, played back to back. hz==0 marks the end. */
static const sfx_seg_t kTone[GS_COUNT][3] = {
    [GS_START] = { { 740, 45 },  { 0, 0 },    { 0, 0 } },
    [GS_SCORE] = { { 1046, 55 }, { 0, 0 },    { 0, 0 } },
    [GS_DEAD]  = { { 330, 80 },  { 196, 140 },{ 0, 0 } },
    [GS_BEST]  = { { 659, 70 },  { 880, 70 }, { 1175, 140 } },
};

static TaskHandle_t s_task;
static portMUX_TYPE s_task_mux = portMUX_INITIALIZER_UNLOCKED;
static atomic_bool s_quit;
static atomic_bool s_running;
static atomic_int s_pending;     /* 0 = no request; only the latest one is kept, no queueing */
static bool s_muted;             /* set true once the codec is really unavailable; no more retries/log spam */

/* Write one PCM chunk; on failure wait (bounded) for the codec to be ready and retry.
   Returns false = still could not write after waiting SFX_READY_WAIT_MS (codec really not open). */
static bool write_blocking(const int16_t *buf, size_t bytes) {
    for (int waited = 0; waited < SFX_READY_WAIT_MS; waited += SFX_READY_STEP_MS) {
        if (atomic_load(&s_quit)) return false;
        if (bsp_audio_write(buf, bytes) == ESP_OK) return true;
        vTaskDelay(pdMS_TO_TICKS(SFX_READY_STEP_MS));
    }
    return false;
}

/* Generate and blocking-write one square-wave segment. I2S writes wait for DMA and yield the CPU, so LVGL is not starved. */
static void play_seg(uint16_t hz, uint16_t ms) {
    if (hz == 0 || ms == 0) return;
    int16_t buf[SFX_CHUNK];
    const int period = SFX_RATE / hz;   /* samples per period, guaranteed >1 */
    const int half = period > 1 ? period / 2 : 1;
    int left = (int)SFX_RATE * ms / 1000;
    int phase = 0;
    while (left > 0 && !atomic_load(&s_quit)) {
        const int n = left < SFX_CHUNK ? left : SFX_CHUNK;
        for (int i = 0; i < n; ++i) {
            buf[i] = (phase < half) ? (int16_t)SFX_AMP : (int16_t)-SFX_AMP;
            if (++phase >= period) phase = 0;
        }
        if (!write_blocking(buf, (size_t)n * sizeof(int16_t))) {
            if (!s_muted) {
                s_muted = true;  /* still could not write after 600ms - report only once, don't spam the log */
                ESP_LOGW(TAG, "codec output not ready (waited %dms), sound effects off this round", SFX_READY_WAIT_MS);
            }
            return;
        }
        left -= n;
    }
}

static void sfx_task(void *arg) {
    (void)arg;
    for (;;) {
        /* Wait for a notification: no queue RAM and no extra semaphore needed. */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (atomic_load(&s_quit)) break;
        const int id = atomic_exchange(&s_pending, 0);
        if (id <= GS_NONE || id >= GS_COUNT || s_muted) continue;
        for (int i = 0; i < 3; ++i) {
            const sfx_seg_t *seg = &kTone[id][i];
            if (seg->hz == 0) break;
            play_seg(seg->hz, seg->ms);
        }
    }
    taskENTER_CRITICAL(&s_task_mux);
    s_task = NULL;
    taskEXIT_CRITICAL(&s_task_mux);
    atomic_store(&s_running, false);
    vTaskDelete(NULL);
}

void game_sfx_start(void) {
    if (atomic_load(&s_running)) return;
    s_pending = 0;
    s_muted = false;
    atomic_store(&s_quit, false);
    atomic_store(&s_running, true);
    /* Priority 5 (above LVGL's 4): tones are only tens of ms long and I2S writes yield the CPU while blocking,
       so rendering is not slowed; any lower and the sound lags visibly behind the picture. */
    if (xTaskCreate(sfx_task, "game_sfx", 2048, NULL, 5, &s_task) != pdPASS) {
        ESP_LOGW(TAG, "sound effect task creation failed, no sound this round");
        s_task = NULL;
        atomic_store(&s_running, false);
    }
}

void game_sfx_stop(void) {
    game_sfx_prepare_stop();
    s_pending = 0;
}

bool game_sfx_prepare_stop(void) {
    atomic_store(&s_quit, true);
    // A worker cannot clear/delete its handle between this check and notify.
    taskENTER_CRITICAL(&s_task_mux);
    if (s_task) xTaskNotifyGive(s_task);
    taskEXIT_CRITICAL(&s_task_mux);
    return !atomic_load(&s_running);
}

void game_sfx_play(game_sfx_t id) {
    if (id <= GS_NONE || id >= GS_COUNT) return;
    taskENTER_CRITICAL(&s_task_mux);
    if (s_task && !atomic_load(&s_quit)) {
        atomic_store(&s_pending, (int)id);
        xTaskNotifyGive(s_task);
    }
    taskEXIT_CRITICAL(&s_task_mux);
}
