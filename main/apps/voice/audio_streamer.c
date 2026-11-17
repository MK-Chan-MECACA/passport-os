// main/audio_streamer.c - streaming audio pipeline implementation.
#include "audio_streamer.h"
#include "app_events.h"
#include "bsp_audio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "adpcm.h"
#include <stdlib.h>

static const char *TAG = "audio";

// 100ms @ 16kHz/16bit/mono = 3200B (ble_audio.h AUDIO_FRAME_BYTES and
// AUDIO_FRAME_BYTES in companion/relay.py - all three must match)
#define CHUNK_BYTES  3200
// Ring item = [uint32 session token][3200B PCM] (3204B, 4B-aligned, 4×801).
// The token is incremented by start()/cancel() and written into the item header; ble_worker only processes
// items whose token == the current session; leftovers from an old session (unconsumed in ring / in flight / late chunks) are invalidated
// by token and silently dropped - no "drain" gate is needed after cancel/disconnect, and start never refuses because of leftovers
// (task 08-27-ring-desync-diagnose, conclusion A1: a leftover gate is structurally fragile).
#define CHUNK_ITEM_BYTES  (4 + CHUNK_BYTES)
// BLE channel compression: IMA ADPCM 4:1 (3200B PCM -> 804B block, 8KB/s).
// Why not Opus: the real BLE bottleneck is the mbuf pool (4×256B) + first fragment fails -> whole frame dropped,
// not air bandwidth; with the bottleneck fixed 8KB/s needs only ~0.7 packets per connection event, and Opus's extra compression
// has nothing to buy, yet costs 38.5KB of static state + 16KB of stack, exactly the heap the bottleneck fix needs (see task
// design.md §3). ADPCM state is 4 bytes on the stack: no heap, no alloca, no new component.
// USB has bandwidth to spare, so no compression (lossy is unnecessary); the s_compressed switch selects between them.
static volatile bool s_compressed = false;    // compression switch (set by mode.c per channel)
// Session token: start() increments it to open a new session, cancel() increments it to invalidate late chunks (see the audio_worker
// snapshot semantics). The worker writes the snapshot into each item header; ble_worker only processes items matching the current value.
static volatile uint32_t s_session_token = 0;
static volatile uint32_t s_last_encoded_token = 0;  // session already ADPCM-encoded (token changed = first chunk of a new session)
static adpcm_state_t s_adpcm;                 // accessed only from ble_worker context (4 bytes)
// Ring buffer: NOSPLIT, two slots (200ms).
// ⚠ BYTEBUF cannot be used: in ESP-IDF v5.5, prvGetItemByteBuf only returns the segment up to the end of the buffer when data
// wraps past the tail, so xRingbufferReceiveUpTo returns far less than requested (measured on real hardware:
// 16B shift per round -> returned as 16+2544 in two parts). The compressed path needs "whole 3200B chunk" semantics; a short read
// would encode misaligned audio; NOSPLIT + xRingbufferReceive guarantees intact item boundaries.
// Two slots leave 100ms of buffer for BLE jitter (the old version held only one chunk, so a single notify stall dropped frames).
#define RING_SLOTS   2
// NOSPLIT adds an 8B header per item with 4B alignment; xMaxItemSize = ALIGN(xSize/2)-8,
// so fitting 3204 needs xSize/2 ≥ 3212 -> 6424; 6432 leaves 8B of wrap margin.
#define RING_BYTES   6432
// Semaphore wait limit (F1 event-driven, replaces polling): in-flight notify ≤50ms. Timeout semantics: stop/cancel
// just return on timeout (the tone may overlap the tail, not a critical path; cancel's leftovers are covered by token invalidation).
// The wait is not a single take: the semaphore only speeds up wakeup; wait_worker_exit loops re-checking the worker
// state for the final decision (a stale give only causes one useless wakeup). Worst-case wait ≤200ms, typical <30ms.
#ifndef WORKER_EXIT_TIMEOUT_MS
#define WORKER_EXIT_TIMEOUT_MS 200   // wait for the capture worker to leave its blocking read (formerly 30×5ms polling)
#endif

// Session-owned storage: inactive Voice must not take SRAM from Xiaozhi/Muse.
typedef struct {
    StaticRingbuffer_t ring;
    StaticTask_t audio_tcb, send_tcb;
    StackType_t *audio_stack;
    StackType_t *send_stack;
    uint8_t *ring_storage;
    uint8_t *chunk;
} stream_memory_t;
static stream_memory_t *s_memory;
// Keep the proven stack and buffer sizes, but do not require one ~18 KiB hole.
// Every allocation is session-owned and is released only after both tasks stop.
static void stream_memory_free(void) {
    if (!s_memory) return;
    free(s_memory->chunk);
    free(s_memory->ring_storage);
    free(s_memory->send_stack);
    free(s_memory->audio_stack);
    free(s_memory);
    s_memory = NULL;
}
static bool stream_memory_alloc(void) {
    s_memory = calloc(1, sizeof(*s_memory));
    if (!s_memory) return false;
    s_memory->audio_stack = malloc(3072);
    s_memory->send_stack = malloc(4096);
    s_memory->ring_storage = malloc(RING_BYTES);
    s_memory->chunk = malloc(CHUNK_ITEM_BYTES);
    if (!s_memory->audio_stack || !s_memory->send_stack ||
        !s_memory->ring_storage || !s_memory->chunk) {
        stream_memory_free();
        return false;
    }
    return true;
}
#define s_ring_struct (s_memory->ring)
#define s_ring_storage (s_memory->ring_storage)
#define s_chunk (s_memory->chunk)
#define s_audio_stack (s_memory->audio_stack)
#define s_ble_stack (s_memory->send_stack)
#define s_audio_tcb (s_memory->audio_tcb)
#define s_ble_tcb (s_memory->send_tcb)
static volatile bool s_quit, s_audio_done, s_send_done;
static RingbufHandle_t s_ring;
static SemaphoreHandle_t s_sem;          // binary: the shared wait signal for both workers while idle
// F1 event-driven waits (replace polling; cancel/wind-down no longer busy-wait):
// s_worker_exit_sem - given when audio_worker leaves its blocking read (stop/cancel wait on it);
// s_ring_empty_sem  - given when the ring is empty after ble_worker returns a chunk (cancel/start/drain wait on it).
// Binary semaphores remember state: a give before take makes take return immediately; multiple gives do not count, no side effects.
// s_worker_exit_sem's consumer is wait_worker_exit: the give only speeds up wakeup; whether the worker exited
// is decided by s_worker_busy - a stale give (from a previous exit, never consumed) cannot cause a false result.
static SemaphoreHandle_t s_worker_exit_sem;
static SemaphoreHandle_t s_ring_empty_sem;
static volatile bool s_active = false;
static volatile bool s_worker_busy = false;  // whether audio_worker is still in its blocking read (for stop/cancel to wait on)
static volatile uint16_t s_peak = 0;
static bool s_drop_active = false;           // shared drop flag: capture and send sides share one edge
// The drop count is ++'d by both workers (capture ring full / send failure) and taken by the app task - ++ is not atomic even on a single core,
// so it is protected by a critical section (microseconds, does not hurt real-time behavior).
static portMUX_TYPE s_drop_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_drop_count = 0;            // this session's drop count (cleared on start; taken and reported after voice.end)

// Rate-limit send-failure logs: under congestion (no subscription / flow-control timeout) a Warning per frame would add load.
// The first frame logs immediately, then one aggregated line per second (count = drops in that 1s window; the cumulative total is in s_drop_count).
static uint32_t s_fail_log_n = 0;
static TickType_t s_fail_log_at = 0;

static TaskHandle_t s_audio_task = NULL;
static TaskHandle_t s_ble_task = NULL;
// Static-task APIs use the session allocation above. Workers stay resident
// across PTT turns, then cooperatively stop before that allocation is freed.
// Set only when init fully succeeds: if any creation fails the public API is a no-op (see each entry check) and never touches
// a NULL ring/sem - the main flow (mode switch, etc.) does not crash because the audio pipeline failed to init; audio
// degrades gracefully (voice.start proceeds as usual with no data stream, review P2).
static volatile bool s_ready = false;

// Audio frame send function (channel-agnostic, see audio_streamer.h). Registered from main.c at boot / mode switch.
// Defaults to NULL: sends always count as failed (drop count), the pipeline stays healthy (round 6/7 semantics preserved).
static audio_send_fn_t s_send_fn = NULL;

void audio_streamer_set_sender(audio_send_fn_t fn) { s_send_fn = fn; }

void audio_streamer_set_compressed(bool on) { s_compressed = on; }

// Chunks in the ring (pending + in-flight chunks taken by ble_worker but not yet returned). ++ after send, -- after return;
// zero ⇔ no data in the ring and nothing in flight - exactly the "drained" semantics cancel/start/drain need.
// Not using xRingbufferGetCurFreeSize to test emptiness: under NOSPLIT it returns "the largest item that fits right now"
// (varies with the read/write pointer positions), so it does not return to a fixed value even when empty.
static volatile uint32_t s_ring_items = 0;
static portMUX_TYPE s_ring_mux = portMUX_INITIALIZER_UNLOCKED;

static uint32_t ring_items(void) {
    portENTER_CRITICAL(&s_ring_mux);
    uint32_t n = s_ring_items;
    portEXIT_CRITICAL(&s_ring_mux);
    return n;
}

static void ring_items_inc(void) {
    portENTER_CRITICAL(&s_ring_mux);
    s_ring_items++;
    portEXIT_CRITICAL(&s_ring_mux);
}

static void drop_inc(void) {
    portENTER_CRITICAL(&s_drop_mux);
    s_drop_count++;
    portEXIT_CRITICAL(&s_drop_mux);
}

// Called after ble_worker returns a chunk: when the ring is empty (no leftovers and nothing in flight) give the semaphore,
// waking the drain waiter (voice.end frame order). Returning a chunk is the only operation that frees the ring, so this is the one correct check point.
// Return a chunk: count--, give when it reaches zero (wakes the drain waiter).
static void ring_item_returned(void) {
    bool empty;
    portENTER_CRITICAL(&s_ring_mux);
    if (s_ring_items > 0) s_ring_items--;
    empty = (s_ring_items == 0);
    portEXIT_CRITICAL(&s_ring_mux);
    if (empty) xSemaphoreGive(s_ring_empty_sem);
}

// Wait for the ring to empty (up to ms milliseconds). The semaphore only speeds up wakeup; the ring state makes the final decision:
// (1) check the ring first; empty -> done immediately (otherwise, with no event to wait for, we would sit out the timeout);
// (2) not empty -> wait for a return give (≤50ms typical). A stale give (semaphore left set by an earlier empty ring)
// only causes one useless wakeup and the loop re-checks the ring - a stale event can never be mistaken for drained.
// Returns true ⇔ the ring really was empty when checked. Wrap-safe: the deadline difference is cast to int32, negative means expired.
static bool wait_ring_empty(uint32_t ms) {
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(ms);
    for (;;) {
        if (ring_items() == 0) return true;
        TickType_t remain = (TickType_t)(deadline - xTaskGetTickCount());
        if ((int32_t)remain <= 0) return false;
        xSemaphoreTake(s_ring_empty_sem, remain);
    }
}

// Wait for the capture worker to leave its blocking read (up to ms milliseconds). The semaphore only speeds up wakeup; worker state makes
// the final decision (same pattern as wait_ring_empty): loop re-checking s_worker_busy until it has really exited.
// A stale give (semaphore left set by a previous worker exit that nobody consumed) only causes one useless
// wakeup and the loop re-checks the state - a stale token can never be mistaken for an exit (if the worker is still in its blocking read,
// returning early from stop would overlap the tone with the capture tail). Returns true ⇔ the worker really had exited when checked.
static bool wait_worker_exit(uint32_t ms) {
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(ms);
    while (s_worker_busy) {
        TickType_t remain = (TickType_t)(deadline - xTaskGetTickCount());
        if ((int32_t)remain <= 0) return false;
        xSemaphoreTake(s_worker_exit_sem, remain);
    }
    return true;
}

static void audio_worker(void *arg);
static void ble_worker(void *arg);

esp_err_t audio_streamer_init(void) {
    // Idempotent: re-entry after success returns immediately (re-review R1) - rebuilding would overwrite the global handles, and the old worker
    // (reading the global s_ring/s_sem pointers) would switch to the new resources -> two workers running concurrently / mixed-up resources.
    // Retry after failure: the all-or-nothing guarantee means no worker survives a failure and the global handles were not overwritten (initially
    // NULL or valid leftovers from last time), so a retry rebuilds from a clean state.
    if (s_ready) return ESP_OK;
    if (s_memory) return ESP_ERR_INVALID_STATE;
    if (!stream_memory_alloc()) return ESP_ERR_NO_MEM;
    s_quit = s_audio_done = s_send_done = false;
    s_ready = false;   // reset first: the failure path keeps it false, public API is a no-op (review P2)
    // All-or-nothing: global handles are set only once every resource is created - the failure path never overwrites old handles
    // (on repeated init, concurrent readers / leftover threads still see valid handles and partial failure never exposes NULL;
    // if the first init fails on real firmware the global handles stay at their initial NULL, guarded by s_ready no-op).
    // Failure paths release semaphores, the ring and its session allocation.
    RingbufHandle_t ring = xRingbufferCreateStatic(RING_BYTES, RINGBUF_TYPE_NOSPLIT,
                                                   s_ring_storage, &s_ring_struct);
    if (!ring) { stream_memory_free(); return ESP_FAIL; }
    SemaphoreHandle_t sem = xSemaphoreCreateBinary();
    if (!sem) { vRingbufferDelete(ring); stream_memory_free(); return ESP_FAIL; }
    SemaphoreHandle_t exit_sem = xSemaphoreCreateBinary();
    if (!exit_sem) {
        vSemaphoreDelete(sem);
        vRingbufferDelete(ring); stream_memory_free();
        return ESP_FAIL;
    }
    SemaphoreHandle_t empty_sem = xSemaphoreCreateBinary();
    if (!empty_sem) {
        vSemaphoreDelete(sem);
        vSemaphoreDelete(exit_sem);
        vRingbufferDelete(ring); stream_memory_free();
        return ESP_FAIL;
    }
    s_ring = ring;
    s_ring_items = 0;
    s_sem = sem;
    s_worker_exit_sem = exit_sem;
    s_ring_empty_sem = empty_sem;

    // Audio first: capture (6) > send (5), so a slow send never disturbs the microphone capture cadence.
    // Stacks: audio 3072 bytes (deep path bsp_audio_read->esp_codec_dev_read, driver ~1-1.5KB);
    // ble 4096 - ADPCM encoding is just table lookups + shifts, the largest local is the 804B output buffer (see ble_worker),
    // plenty of margin for the notify path; the real HWM is printed in the stop log, tighten from that.
    // Check creation failures: a silent failure on low memory would leave nobody consuming after start (semaphore given for nothing, pipeline falsely alive).
    // All-or-nothing: any failure rolls back the tasks already created - never allow "capture task running, send task missing"
    // as a lopsided state (the ring buffer keeps piling up, review P2-6). On failure the pointers stay NULL/deleted; stop's NULL
    // pointer guards already exist; the explicit error is handled by the caller degrading gracefully.
    // Task creation failure also frees the semaphores already created (re-review R1): the workers were not created / were deleted, so no task references them.
    TaskHandle_t audio_h = xTaskCreateStatic(audio_worker, "audio_worker", 3072, NULL, 6,
                                             s_audio_stack, &s_audio_tcb);
    if (!audio_h) {
        ESP_LOGE(TAG, "audio worker create failed");
        vSemaphoreDelete(sem);
        vSemaphoreDelete(exit_sem);
        vSemaphoreDelete(empty_sem);
        vRingbufferDelete(ring); stream_memory_free();
        s_ring = NULL; s_sem = s_worker_exit_sem = s_ring_empty_sem = NULL;
        return ESP_FAIL;
    }
    s_audio_task = audio_h;   // static task handle: used for HWM logging and rollback deletion
    // The handle must be kept: uxTaskGetStackHighWaterMark(NULL) reports the caller's (app_task's)
    // headroom, not ble_worker's - the old code discarded the return value, so the ble headroom in the stop log was bogus.
    TaskHandle_t ble_h = xTaskCreateStatic(ble_worker, "ble_worker", 4096, NULL, 5,
                                           s_ble_stack, &s_ble_tcb);
    if (!ble_h) {
        ESP_LOGE(TAG, "ble worker create failed, rolling back audio worker");
        vTaskDelete(audio_h);   // audio worker is blocked waiting, safe to delete (holds no resources)
        s_audio_task = NULL;
        vSemaphoreDelete(sem);
        vSemaphoreDelete(exit_sem);
        vSemaphoreDelete(empty_sem);
        vRingbufferDelete(ring); stream_memory_free();
        s_ring = NULL; s_sem = s_worker_exit_sem = s_ring_empty_sem = NULL;
        return ESP_FAIL;
    }
    s_ble_task = ble_h;
    s_ready = true;   // all resources ready: only from here may the public API touch ring/sem/worker
    ESP_LOGI(TAG, "stream pipeline ready (static ring %d B / %d slots, block %d B)",
             RING_BYTES, RING_SLOTS, CHUNK_BYTES);
    return ESP_OK;
}

bool audio_streamer_deinit(void) {
    if (!s_memory) return true;
    audio_streamer_cancel();
    s_ready = false;
    s_active = false;
    s_quit = true;
    xSemaphoreGive(s_sem);
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
    while (!s_audio_done || !s_send_done) {
        if ((int32_t)(deadline - xTaskGetTickCount()) <= 0) {
            ESP_LOGE(TAG, "Voice pipeline stop timed out; retaining live buffers");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    // Workers suspend only after leaving codec/network IO and returning items.
    vTaskDelete(s_audio_task); s_audio_task = NULL;
    vTaskDelete(s_ble_task); s_ble_task = NULL;
    vRingbufferDelete(s_ring); s_ring = NULL;
    vSemaphoreDelete(s_sem); s_sem = NULL;
    vSemaphoreDelete(s_worker_exit_sem); s_worker_exit_sem = NULL;
    vSemaphoreDelete(s_ring_empty_sem); s_ring_empty_sem = NULL;
    stream_memory_free();
    s_ring_items = 0; s_peak = 0;
    ESP_LOGI(TAG, "Voice pipeline released: %u bytes", (unsigned)(sizeof(stream_memory_t) + 3072 + 4096 + RING_BYTES + CHUNK_ITEM_BYTES));
    return true;
}

esp_err_t audio_streamer_start(void) {
    if (!s_ready) return ESP_ERR_INVALID_STATE;   // init failed: start refused (no-op, never touches a NULL ring/sem)
    if (s_active) return ESP_OK;                  // idempotent: already in a session
    // Session token: every new session takes a new value; leftover/in-flight items in the ring (old token) are dropped silently in ble_worker
    // - leftovers never leak into a new session, and start never refuses because of leftovers (no drain gate,
    // see the CHUNK_ITEM_BYTES comment; after cancel it can reopen immediately without waiting for the ring to empty).
    s_session_token++;
    s_active = true;
    s_peak = 0;
    s_drop_active = false;
    s_drop_count = 0;
    // The ADPCM adaptive reset is consumed by ble_worker (token-change detection; this task does not touch the encoder
    // state): reset before a new session's first chunk, so the previous session's silence/noise adaptation is not carried over.
    xSemaphoreGive(s_sem);
    ESP_LOGI(TAG, "capture started (%s, session %u)", s_compressed ? "ADPCM 4:1" : "PCM",
             (unsigned)s_session_token);
    return ESP_OK;
}

void audio_streamer_stop(void) {
    if (!s_active) return;
    s_active = false;             // audio worker exits on its next loop
    // Wait for the worker to leave its blocking read (semaphore wakeup + state decision, see wait_worker_exit),
    // so the SEND tone does not overlap the capture tail and write to the codec.
    wait_worker_exit(WORKER_EXIT_TIMEOUT_MS);
    if (s_audio_task) {   // the deep path is reached after the first session; shrink the stack on real hardware per the HWM (see the init comment)
        ESP_LOGI(TAG, "stack headroom: audio %u/3072, ble %u/4096",
                 (unsigned)uxTaskGetStackHighWaterMark(s_audio_task),
                 (unsigned)uxTaskGetStackHighWaterMark(s_ble_task));
    }
    ESP_LOGI(TAG, "capture stopped");
}

void audio_streamer_cancel(void) {
    if (!s_ready) return;   // init failed: idempotent no-op (never touches a NULL ring, review P2)
    // Idempotent: s_active is not checked - capture may already be stopped (disconnect after STOP), and ring leftovers still need invalidating.
    // Semantics: (1) invalidate the session token: the worker snapshotted the old token while blocked in read, so its late chunk
    // (sent after read returns) carries the old value and is dropped silently by ble_worker - leftover and in-flight
    // frames never leak into the next session, and no drain wait is needed (cancel returns fast).
    // (2) stop capture (no new writes to the ring) (3) wait for audio_worker to leave its blocking read (the ring is stable after that).
    // No ring reset API (ESP-IDF has no xRingbufferReset; and any "clear" implementation would
    // conflict with in-flight chunks ble_worker has taken but not returned) - token invalidation is equivalent to clearing, at zero cost.
    s_session_token++;
    s_active = false;
    wait_worker_exit(WORKER_EXIT_TIMEOUT_MS);
    ESP_LOGI(TAG, "capture cancelled (leftovers dropped by token invalidation)");
}

bool audio_streamer_active(void) { return s_active; }

void audio_streamer_release_audio(void) {
    // Only the voice owner calls this after stop/cancel. Never delete a codec
    // that the worker is still reading when its bounded stop wait timed out.
    if (!s_active && !s_worker_busy) bsp_audio_deinit();
    else ESP_LOGW(TAG, "Audio still busy; defer codec release");
}

uint16_t audio_streamer_peak(void) { return s_peak; }

uint32_t audio_streamer_take_drops(void) {
    portENTER_CRITICAL(&s_drop_mux);
    uint32_t d = s_drop_count;
    s_drop_count = 0;
    portEXIT_CRITICAL(&s_drop_mux);
    return d;
}

// Wait for the ring to empty (up to ms milliseconds), to guarantee frame order before voice.end.
// Only polls, never consumes: a full ring still has data (including in-flight chunks ble_worker took but has not returned); taking without sending = dropping the tail.
void audio_streamer_drain(uint32_t ms) {
    if (!s_ready) return;   // init failed: no-op (never touches a NULL ring)
    // Wait for the ring to empty (semaphore event-driven + ring state decision, see wait_ring_empty). A timeout is only logged
    // - the caller guarantees voice.end frame order (send end after drain); if a stray frame is still in flight, end is still sent,
    // and the status reconciliation frame covers it.
    if (!wait_ring_empty(ms)) {
        ESP_LOGW(TAG, "drain timed out, frames may still be in flight");
    }
}

static void audio_worker(void *arg) {
    (void)arg;
    while (!s_quit) {
        xSemaphoreTake(s_sem, portMAX_DELAY);    // wait for the start signal
        if (s_quit) break;
        s_worker_busy = true;
        while (s_active) {
            // The session token this chunk belongs to (snapshotted on entering the loop): if cancel happens while read is blocked,
            // the late chunk (this one) still carries the old token and is dropped silently by ble_worker.
            uint32_t my_token = s_session_token;
            // +4: the item header holds the token; overall 4B-aligned, so the DMA target (+4) stays on a 4B boundary
            esp_err_t e = bsp_audio_read(s_chunk + 4, CHUNK_BYTES);
            if (e != ESP_OK) {
                ESP_LOGE(TAG, "bsp_audio_read failed (%s), stopping stream",
                         esp_err_to_name(e));
                app_event_t ev = { .type = APP_EV_AUDIO_ERROR };  // dedicated hardware-error event
                app_event_post(&ev);
                s_active = false;
                break;
            }
            // Peak sample (UI level meter). Widen to int32 before negating:
            // negating int16 -32768 directly is signed overflow (UB).
            const int16_t *p = (const int16_t *)(s_chunk + 4);
            uint16_t peak = 0;
            for (size_t i = 0; i < CHUNK_BYTES / 2; i++) {
                int32_t v = p[i];
                uint16_t a = (uint16_t)(v < 0 ? -v : v);
                if (a > peak) peak = a;
            }
            s_peak = peak;

            // Stamp the session token (item header) and push it into the ring together with the chunk
            *(uint32_t *)s_chunk = my_token;
            if (xRingbufferSend(s_ring, s_chunk, CHUNK_ITEM_BYTES, 0) == pdTRUE) {
                ring_items_inc();
            } else {
                // Ring full: drop at the source immediately (non-blocking - blocking for space would delay the next capture,
                // breaking the 100ms capture cadence; the microphone keeps running and the I2S driver drops samples itself), report BLE BUSY
                drop_inc();
                if (!s_drop_active) {
                    s_drop_active = true;
                    app_event_t ev = { .type = APP_EV_AUDIO_DROP_START };
                    app_event_post(&ev);
                }
            }
        }
        s_worker_busy = false;
        xSemaphoreGive(s_worker_exit_sem);   // F1: notify the stop/cancel waiters here
    }
    s_audio_done = true;
    vTaskSuspend(NULL);
}

static void ble_worker(void *arg) {
    (void)arg;
    while (!s_quit) {
        size_t len = 0;
        // NOSPLIT: whole-item semantics, len always equals the written length (no short reads like BYTEBUF)
        void *item = xRingbufferReceive(s_ring, &len, pdMS_TO_TICKS(50));
        if (!item) {
            continue;   // ring empty: normal state
        }
        int rc = 0;
        if (len != CHUNK_ITEM_BYTES) {
            // Should not happen (NOSPLIT guarantees intact items); if it does, drop this chunk and leave evidence,
            // never interpret the buffer at a fixed length (that is exactly how the old BYTEBUF path read out of bounds).
            ESP_LOGW(TAG, "bad ring block length %u (expected %d), dropping",
                     (unsigned)len, CHUNK_ITEM_BYTES);
            rc = -1;
        } else if (*(const uint32_t *)item != s_session_token) {
            // Session gate: leftover/late chunks from an old session (normal length but token mismatch) are dropped silently
            // - this is what invalidates ring leftovers after cancel/disconnect, so start can reopen immediately without draining.
            // Silent: part of the normal lifecycle (happens on every cancel), so no count, no warning, no event.
            vRingbufferReturnItem(s_ring, item);
            ring_item_returned();
            continue;
        } else if (s_compressed) {
            // BLE compressed path: whole 3200B PCM chunk -> one 804B ADPCM block sent in one go.
            // Each block carries its own predictor/index header and decodes alone - losing one costs only that 100ms.
            if (s_last_encoded_token != s_session_token) {
                // First chunk of a new session (token-change detection): reset adaptation, do not carry the previous session's
                // silence/noise adaptation
                adpcm_state_reset(&s_adpcm);
                s_last_encoded_token = s_session_token;
            }
            uint8_t block[ADPCM_BLOCK_BYTES];
            size_t n = adpcm_encode_block(&s_adpcm, (const int16_t *)(item + 4),
                                          CHUNK_BYTES / 2, block, sizeof(block));
            if (n == 0) {   // argument/capacity mismatch (compile-time constants, unreachable in theory): drop the chunk and leave evidence
                ESP_LOGW(TAG, "ADPCM encode failed (block %u B), dropping", (unsigned)len);
                rc = -1;
            } else {
                // an unregistered channel counts as a send failure (same as the old semantics)
                rc = s_send_fn ? s_send_fn(block, n) : -1;
            }
        } else {
            // Uncompressed path (USB): send the PCM chunk as is (the 4B token is ring-internal metadata,
            // never goes on the link; the 3200B payload matches the old protocol)
            rc = s_send_fn ? s_send_fn((const uint8_t *)item + 4, CHUNK_BYTES) : -1;
        }
        vRingbufferReturnItem(s_ring, item);
        ring_item_returned();
        if (rc == 0) {
            // Clear BLE BUSY only once sends recover and the ring has room again (no backlog) -
            // send success alone is not enough: if sends are slow but always succeed, drops caused by a full ring would never clear
            if (s_drop_active && ring_items() < RING_SLOTS) {
                s_drop_active = false;
                app_event_t ev = { .type = APP_EV_AUDIO_DROP_END };
                app_event_post(&ev);
            }
        } else {
            // no subscription / disconnected / encode failure / bad chunk length: drop the whole chunk and count it (reported in the status frame after voice.end)
            drop_inc();
            if (!s_drop_active) {
                s_drop_active = true;
                app_event_t ev = { .type = APP_EV_AUDIO_DROP_START };
                app_event_post(&ev);
            }
            s_fail_log_n++;
            if (s_fail_log_at == 0 ||   // first failure logs immediately (then aggregated per 1s window)
                xTaskGetTickCount() - s_fail_log_at >= pdMS_TO_TICKS(1000)) {
                ESP_LOGW(TAG, "audio frame send failed, dropping (%u frames/window)", s_fail_log_n);
                s_fail_log_n = 0;
                s_fail_log_at = xTaskGetTickCount();
            }
        }
    }
    s_send_done = true;
    vTaskSuspend(NULL);
}
