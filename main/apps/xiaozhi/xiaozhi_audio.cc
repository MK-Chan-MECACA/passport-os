#include "apps/xiaozhi/xiaozhi_audio.h"
#include "apps/xiaozhi/opus_decoder_storage.h"
#include "apps/xiaozhi/xiaozhi_level.h"
#include "bsp_audio.h"
#include "audio_workspace.h"
#include "encoder/impl/esp_opus_enc.h"
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <cstring>
#include <cstdlib>

static const char *TAG = "xiaozhi_audio";

namespace xiaozhi {

namespace {
std::atomic<bool> s_cancel_requested{false};
// One atomic snapshot: low byte = level, upper 24 bits = FreeRTOS tick timestamp.
// Wrap-safe expiry also handles starvation and blocked I2S without UI callbacks.
std::atomic<uint32_t> s_visual_level{0};
uint32_t visual_time() { return xTaskGetTickCount() & 0xFFFFFFu; }
void publish_level(const uint8_t *pcm, size_t bytes, unsigned gain = 100) {
    const uint8_t level = face::pcm_level(reinterpret_cast<const int16_t *>(pcm), bytes / 2, gain);
    s_visual_level.store((visual_time() << 8) | level, std::memory_order_relaxed);
}
constexpr int kEncodeRate = 16000;      // Encoding fixed at 16kHz (same as the official firmware)
constexpr int kChannels = 1;
constexpr int kBitsPerSample = 16;

// Upper bounds for bounded waits: cleanup must finish even if something is stuck; these are fallbacks, not the expected path.
constexpr uint32_t kWorkIdleTimeoutMs = 1500;
constexpr uint32_t kPlaybackDrainTimeoutMs = 2000;
constexpr uint32_t kPlaybackAbortTimeoutMs = 800;
constexpr uint32_t kPlaybackEnqueueWaitMs = 300;

// Xiaozhi TTS packets arrive in WebSocket bursts. Without pre-buffering, any jitter in the network task means that after 60ms
// of PCM plays out I2S briefly starves, which sounds like continuous electrical popping. Accumulate 3 frames (about 180ms) first
// before starting playback; if it runs dry midway, re-accumulate. A very short pause is better than feeding choppy noise to the amp.
constexpr size_t kPlaybackPrebufferFrames = 3;

// TTS follows the user's master volume without a hidden 70% ceiling. Recording
// and idle paths still request 0%, which the BSP now applies as a real mute.
constexpr uint8_t kXiaozhiPlaybackVolume = 100;

constexpr size_t kRingHdr = 2;          // 2-byte length prefix per record

// Half-duplex codecs reuse the fixed auxiliary area. Their large states never
// depend on session heap fragmentation; only the small ESP encoder wrapper
// remains allocated while recording. Capacity is checked by the factory.

// Resident worker thread stack. The Opus codecs put their temporary buffers on the stack: encoding (clt_mdct_forward_c /
// silk_NSQ_c) measured at ~19KB, decoding (quant_all_bands) >6KB. The 20KB version dropped to about 0.8KB free in continuous
// conversation on device; 22KB keeps about 2.8KB of safety margin while lowering the contiguous-block requirement.
// The stack comes from the fixed work area shared with the radio, not allocated on the heap, which fragments after app switches.
// The next app may use that area only after the worker task has exited and been deleted by its owner.
constexpr size_t kWorkerStack = 22 * 1024;
static_assert(kWorkerStack <= audio_workspace::kRealtimeBytes);
} // namespace

// ---------------------------------------------------------------- ring buffer

void AudioService::RingInit(Ring &r, uint8_t *buf, size_t cap, SemaphoreHandle_t lock,
                            const char *name) {
    r.buf = buf;
    r.cap = cap;
    r.head = 0;
    r.tail = 0;
    r.records = 0;
    r.lock = lock;
    r.name = name;
}

size_t AudioService::RingUsed(const Ring &r) {
    return (r.head + r.cap - r.tail) % r.cap;
}

size_t AudioService::RingFree(const Ring &r) {
    return r.cap - RingUsed(r) - 1;   // Keep 1 byte to tell full from empty
}

size_t AudioService::RingCount(const Ring &r) {
    if (r.lock == nullptr) return 0;
    size_t count = 0;
    if (xSemaphoreTake(r.lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        count = r.records;
        xSemaphoreGive(r.lock);
    }
    return count;
}

bool AudioService::RingPush(Ring &r, const uint8_t *data, size_t len) {
    if (r.lock == nullptr || len == 0 || len > 0xFFFF) return false;

    bool ok = false;
    if (xSemaphoreTake(r.lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        const size_t need = kRingHdr + len;
        if (r.buf != nullptr && RingFree(r) >= need) {
            if (r.head + need <= r.cap) {
                r.buf[r.head] = (uint8_t)(len & 0xFF);
                r.buf[r.head + 1] = (uint8_t)((len >> 8) & 0xFF);
                std::memcpy(r.buf + r.head + kRingHdr, data, len);
            } else {
                for (size_t i = 0; i < need; ++i) {
                    uint8_t byte;
                    if (i == 0) byte = (uint8_t)(len & 0xFF);
                    else if (i == 1) byte = (uint8_t)((len >> 8) & 0xFF);
                    else byte = data[i - kRingHdr];
                    r.buf[(r.head + i) % r.cap] = byte;
                }
            }
            r.head = (r.head + need) % r.cap;
            r.records++;
            ok = true;
        }
        xSemaphoreGive(r.lock);
    }
    return ok;
}

size_t AudioService::RingPop(Ring &r, uint8_t *out, size_t out_cap) {
    if (r.lock == nullptr) return 0;
    size_t result = 0;
    if (xSemaphoreTake(r.lock, pdMS_TO_TICKS(20)) != pdTRUE) return 0;

    if (r.buf != nullptr && RingUsed(r) >= kRingHdr) {
        const size_t h = r.tail;
        const size_t len = (size_t)r.buf[h] | ((size_t)r.buf[(h + 1) % r.cap] << 8);
        if (len > 0 && len <= out_cap && RingUsed(r) >= kRingHdr + len) {
            for (size_t i = 0; i < len; ++i) {
                out[i] = r.buf[(h + kRingHdr + i) % r.cap];
            }
            r.tail = (h + kRingHdr + len) % r.cap;
            if (r.records > 0) r.records--;
            result = len;
        } else if (len > out_cap) {
            ESP_LOGW(TAG, "%s: record len=%u > cap=%u, dropped", r.name, (unsigned)len,
                     (unsigned)out_cap);
            r.tail = (h + kRingHdr + len) % r.cap;
            if (r.records > 0) r.records--;
        }
    }
    xSemaphoreGive(r.lock);
    return result;
}

void AudioService::RingReset(Ring &r) {
    if (r.lock == nullptr) return;
    if (xSemaphoreTake(r.lock, portMAX_DELAY) == pdTRUE) {
        r.head = 0;
        r.tail = 0;
        r.records = 0;
        xSemaphoreGive(r.lock);
    }
}

void AudioService::RingRelease(Ring &r, uint8_t *&owned_buffer) {
    if (r.lock == nullptr) return;
    xSemaphoreTake(r.lock, portMAX_DELAY);
    r.buf = nullptr;
    r.head = r.tail = r.records = 0;
    if (owned_buffer != nullptr) free(owned_buffer);
    owned_buffer = nullptr;
    xSemaphoreGive(r.lock);
}

// ------------------------------------------------------------------- lifecycle

AudioService &AudioService::GetInstance() {
    static AudioService s_instance;
    return s_instance;
}

void AudioService::RequestStop() {
    s_cancel_requested.store(true);
    s_visual_level.store(0, std::memory_order_relaxed);
}

uint8_t AudioService::VisualLevel() {
    if (s_cancel_requested.load()) return 0;
    const uint32_t snapshot = s_visual_level.load(std::memory_order_relaxed);
    return ((visual_time() - (snapshot >> 8)) & 0xFFFFFFu) <= pdMS_TO_TICKS(180) ? snapshot & 0xFFu : 0;
}

AudioService::AudioService() {
    lifecycle_lock_ = xSemaphoreCreateRecursiveMutexStatic(&sync_storage_[0]);
    RingInit(play_ring_, nullptr, kPlayRingCap, xSemaphoreCreateMutexStatic(&sync_storage_[1]), "play_ring");
    RingInit(tx_ring_, nullptr, kTxRingCap, xSemaphoreCreateMutexStatic(&sync_storage_[2]), "tx_ring");
    worker_stopped_ = xSemaphoreCreateBinaryStatic(&sync_storage_[3]);
}

AudioService::~AudioService() {
    if (opus_enc_handle_) esp_opus_enc_close(opus_enc_handle_);
    opus_dec_handle_ = nullptr; // fixed storage is owned by the foreground workspace
}

esp_err_t AudioService::Init() {
    // A transient allocation failure while constructing the singleton must be
    // retryable too; no protocol callback exists before Init succeeds.
    if (lifecycle_lock_ == nullptr) lifecycle_lock_ = xSemaphoreCreateRecursiveMutexStatic(&sync_storage_[0]);
    if (play_ring_.lock == nullptr) play_ring_.lock = xSemaphoreCreateMutexStatic(&sync_storage_[1]);
    if (tx_ring_.lock == nullptr) tx_ring_.lock = xSemaphoreCreateMutexStatic(&sync_storage_[2]);
    if (worker_stopped_ == nullptr) worker_stopped_ = xSemaphoreCreateBinaryStatic(&sync_storage_[3]);
    if (lifecycle_lock_ == nullptr || play_ring_.lock == nullptr || tx_ring_.lock == nullptr ||
        worker_stopped_ == nullptr) return ESP_ERR_NO_MEM;
    if (worker_task_ != nullptr && bsp_owned_) return ESP_OK;

    if (!audio_workspace::acquire(audio_workspace::Owner::Xiaozhi)) {
        ESP_LOGE(TAG, "Audio workspace still owned by another app");
        return ESP_ERR_NO_MEM;
    }
    worker_stack_ = reinterpret_cast<StackType_t *>(audio_workspace::storage);
    esp_err_t ret = bsp_audio_init();
    if (ret != ESP_OK) {
        worker_stack_ = nullptr;
        audio_workspace::release(audio_workspace::Owner::Xiaozhi);
        return ret;
    }
    bsp_owned_ = true;
    ret = bsp_audio_set_format(kEncodeRate, kBitsPerSample, kChannels);
    if (ret != ESP_OK) {
        bsp_audio_deinit();
        bsp_owned_ = false;
        worker_stack_ = nullptr;
        audio_workspace::release(audio_workspace::Owner::Xiaozhi);
        return ret;
    }
    bsp_audio_set_volume(0);

    // The shared workspace guarantees a full 22 KiB stack and a codec area.
    // Validate both phases before small PCM buffers and TLS are allocated.
    if (work_signal_ == nullptr) work_signal_ = xSemaphoreCreateBinaryStatic(&sync_storage_[4]);
    if (work_idle_ == nullptr) work_idle_ = xSemaphoreCreateBinaryStatic(&sync_storage_[5]);
    if (work_signal_ == nullptr || work_idle_ == nullptr) {
        bsp_audio_deinit();
        bsp_owned_ = false;
        worker_stack_ = nullptr;
        audio_workspace::release(audio_workspace::Owner::Xiaozhi);
        return ESP_ERR_NO_MEM;
    }
    auto fail_init = [this]() {
        if (pcm_scratch_) { free(pcm_scratch_); pcm_scratch_ = nullptr; }
        if (opus_scratch_) { free(opus_scratch_); opus_scratch_ = nullptr; }
        if (opus_dec_handle_) { /* caller-owned decoder state; no heap free */ opus_dec_handle_ = nullptr; opus_dec_rate_ = 0; }
        if (opus_enc_handle_) { esp_opus_enc_close(opus_enc_handle_); opus_enc_handle_ = nullptr; }
        worker_stack_ = nullptr;
        bsp_audio_deinit(); bsp_owned_ = false;
        audio_workspace::release(audio_workspace::Owner::Xiaozhi);
        return ESP_ERR_NO_MEM;
    };
    if (!EnsureEncoder()) return fail_init();
    // Initialize the caller-owned decoder area before starting the worker.
    // Validate both codec modes; the decoder remains initialized for hello.
    // Server hello may change its sample rate before any audio is accepted.
    if (!OpenDecoder()) return fail_init();
    pcm_scratch_ = static_cast<uint8_t *>(malloc(kMaxPcmFrameBytes));
    opus_scratch_ = static_cast<uint8_t *>(malloc(kOpusFrameMaxBytes));
    if (pcm_scratch_ == nullptr || opus_scratch_ == nullptr) return fail_init();
    RingReset(play_ring_);
    RingReset(tx_ring_);
    while (xSemaphoreTake(work_signal_, 0) == pdTRUE) { }
    while (xSemaphoreTake(work_idle_, 0) == pdTRUE) { }
    while (xSemaphoreTake(worker_stopped_, 0) == pdTRUE) { }
    worker_quit_ = false;
    s_cancel_requested.store(false);
    work_ = Work::Idle;
    // Only the small rings remain deferred until TLS hello. PCM/Opus scratch
    // stays shared and the proven 22 KiB audio stack is unchanged.
    worker_task_ = xTaskCreateStatic(WorkerEntry, "xzg_audio", kWorkerStack, this, 5,
                                      worker_stack_, &worker_tcb_);
    if (worker_task_ == nullptr) return fail_init();
    ESP_LOGI(TAG, "Audio service ready: shared scratch, stack=%u free=%u largest=%u",
             (unsigned)kWorkerStack, (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return ESP_OK;
}

bool AudioService::Deinit() {
    StopRecording();
    AbortPlayback();

    if (worker_task_ != nullptr) {
        worker_quit_ = true;
        xSemaphoreGive(work_signal_);
        if (xSemaphoreTake(worker_stopped_, pdMS_TO_TICKS(4000)) != pdTRUE) {
            // The worker can still be inside I2S/Opus. Freeing its buffers or
            // codec here would turn a slow stop into a deterministic UAF.
            ESP_LOGE(TAG, "Audio worker did not exit; keeping all owned resources intact");
            return false;
        }
        // Worker has reported completion and suspended itself. Delete it from
        // the owner task, then the shared workspace can change ownership.
        vTaskDelete(worker_task_);
        worker_task_ = nullptr;
    }
    if (worker_stack_ != nullptr) {
        worker_stack_ = nullptr;
        audio_workspace::release(audio_workspace::Owner::Xiaozhi);
    }

    // The codecs must be closed explicitly here: the encoder is now resident (StopRecording no longer closes it),
    // Close the small ESP wrapper before returning the shared codec area.
    if (opus_enc_handle_ != nullptr) {
        esp_opus_enc_close(opus_enc_handle_);
        opus_enc_handle_ = nullptr;
    }
    if (opus_dec_handle_ != nullptr) {
        /* caller-owned decoder state; no heap free */
        opus_dec_handle_ = nullptr;
        opus_dec_rate_ = 0;
    }
    if (pcm_scratch_) { free(pcm_scratch_); pcm_scratch_ = nullptr; }
    if (opus_scratch_) { free(opus_scratch_); opus_scratch_ = nullptr; }

    RingRelease(play_ring_, play_ring_buf_);
    RingRelease(tx_ring_, tx_ring_buf_);
    if (bsp_owned_) {
        bsp_audio_deinit();
        bsp_owned_ = false;
    }
    ESP_LOGI(TAG, "Audio service deinit. free=%u largest=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return true;
}

bool AudioService::WaitWorkIdle(uint32_t timeout_ms, const char *name) {
    if (worker_task_ == nullptr || work_ == Work::Idle) return true;
    const TickType_t started = xTaskGetTickCount();
    const TickType_t limit = pdMS_TO_TICKS(timeout_ms);
    while (work_ != Work::Idle) {
        const TickType_t elapsed = xTaskGetTickCount() - started;
        if (elapsed >= limit) break;
        xSemaphoreTake(work_idle_, limit - elapsed);
    }
    const bool ok = work_ == Work::Idle;
    if (!ok) ESP_LOGE(TAG, "%s: audio worker did not report idle in %ums", name, timeout_ms);
    return ok;
}

// ------------------------------------------------------------------- recording

bool AudioService::EnsureEncoder() {
    if (opus_enc_handle_ != nullptr) return true;
    // The factory initializes caller storage, so only the small ESP wrapper
    // uses heap. Never overwrite the decoder while the worker is using it.
    if (work_ != Work::Idle) return false;
    opus_dec_handle_ = nullptr;
    opus_dec_rate_ = 0;

    // CELT (APPLICATION_AUDIO): matches the official xiaozhi-esp32 voice pipeline, and is also the config that worked on the
    // official backend before the rework (trying SILK/20ms made the server not respond at all, so it was reverted).
    esp_opus_enc_config_t enc_cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
    enc_cfg.sample_rate = kEncodeRate;
    enc_cfg.channel = ESP_AUDIO_MONO;
    enc_cfg.bits_per_sample = kBitsPerSample;
    enc_cfg.bitrate = 32000;
    enc_cfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS;
    enc_cfg.application_mode = ESP_OPUS_ENC_APPLICATION_AUDIO;
    enc_cfg.complexity = 0;      // C3 single core at 160MHz; complexity 0 keeps it real-time
    enc_cfg.enable_fec = false;
    // DTX off: silence suppression only hurts push-to-talk - once it decides it's silence it sends no packets at all,
    // the server gets no audio and simply doesn't respond, which looks exactly like "no reply after talking".
    enc_cfg.enable_dtx = false;
    enc_cfg.enable_vbr = true;

    const esp_audio_err_t err = esp_opus_enc_open(&enc_cfg, sizeof(enc_cfg), &opus_enc_handle_);
    if (err != ESP_AUDIO_ERR_OK || opus_enc_handle_ == nullptr) {
        ESP_LOGE(TAG, "Opus encoder open failed: %d (free=%u largest=%u)", (int)err,
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        opus_enc_handle_ = nullptr;
        return false;
    }
    ESP_LOGI(TAG, "Opus encoder resident. free=%u largest=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return true;
}

bool AudioService::PrepareSessionResources() {
    if (lifecycle_lock_ == nullptr) return false;
    xSemaphoreTakeRecursive(lifecycle_lock_, portMAX_DELAY);

    if (worker_task_ == nullptr || !bsp_owned_ || work_ != Work::Idle) {
        ESP_LOGE(TAG, "Cannot prepare session resources before Init()");
        xSemaphoreGiveRecursive(lifecycle_lock_);
        return false;
    }

    // A reconnect can theoretically negotiate another rate. Recreate only at
    // that session boundary, never between ordinary TTS/recording turns.
    if (opus_dec_handle_ != nullptr && opus_dec_rate_ != playback_rate_) {
        /* caller-owned decoder state; no heap free */
        opus_dec_handle_ = nullptr;
        opus_dec_rate_ = 0;
    }

    // Normally the decoder is already reserved by Init(). A negotiated rate
    // change reinitializes the same fixed area before allocating small rings.
    if (opus_dec_handle_ == nullptr && !OpenDecoder()) {
        ESP_LOGE(TAG, "Post-hello decoder reservation failed (free=%u largest=%u)",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
        xSemaphoreGiveRecursive(lifecycle_lock_);
        return false;
    }
    if (play_ring_buf_ == nullptr) play_ring_buf_ = static_cast<uint8_t *>(malloc(kPlayRingCap));
    if (tx_ring_buf_ == nullptr) tx_ring_buf_ = static_cast<uint8_t *>(malloc(kTxRingCap));

    play_ring_.buf = play_ring_buf_;
    tx_ring_.buf = tx_ring_buf_;
    const bool ready = pcm_scratch_ != nullptr && play_ring_buf_ != nullptr && tx_ring_buf_ != nullptr;
    if (ready) {
        RingReset(play_ring_);
        RingReset(tx_ring_);
        ESP_LOGI(TAG, "Post-hello audio resources ready. free=%u largest=%u",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    } else {
        ESP_LOGE(TAG, "Post-hello audio buffers failed (pcm=%p play=%p tx=%p free=%u largest=%u)",
                 pcm_scratch_, play_ring_buf_, tx_ring_buf_, (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }

    xSemaphoreGiveRecursive(lifecycle_lock_);
    return ready;
}

bool AudioService::ReleaseSessionResources() {
    if (lifecycle_lock_ == nullptr) return false;
    xSemaphoreTakeRecursive(lifecycle_lock_, portMAX_DELAY);
    if (worker_task_ != nullptr && work_ != Work::Idle) {
        ESP_LOGE(TAG, "Retaining session resources: audio worker is still active");
        xSemaphoreGiveRecursive(lifecycle_lock_);
        return false;
    }

    // Keep the current codec state and the worker stack through reconnects. Releasing
    // the decoder here lets the next TLS handshake fragment its reserved block.
    // Return the small rings; Deinit() still returns every app-owned allocation.
    RingReset(play_ring_);
    RingReset(tx_ring_);
    RingRelease(play_ring_, play_ring_buf_);
    RingRelease(tx_ring_, tx_ring_buf_);

    ESP_LOGI(TAG, "Session audio released for reconnect. free=%u largest=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    xSemaphoreGiveRecursive(lifecycle_lock_);
    return true;
}

bool AudioService::StartRecording() {
    if (lifecycle_lock_ == nullptr || s_cancel_requested.load()) return false;
    xSemaphoreTakeRecursive(lifecycle_lock_, portMAX_DELAY);

    if (is_recording_) {
        xSemaphoreGiveRecursive(lifecycle_lock_);
        return true;
    }

    // Half-duplex: playback must stop before recording (lifecycle_lock_ is a recursive lock, so calling directly from the same task is safe)
    if (is_playing_ || work_ == Work::Play) {
        AbortPlayback();
    }

    if (work_ != Work::Idle) {
        ESP_LOGE(TAG, "Recorder deferred: previous audio work has not stopped");
        xSemaphoreGiveRecursive(lifecycle_lock_);
        return false;
    }
    if (pcm_scratch_ == nullptr || opus_scratch_ == nullptr || tx_ring_buf_ == nullptr) {
        ESP_LOGE(TAG, "Record buffers missing; Init() not done?");
        xSemaphoreGiveRecursive(lifecycle_lock_);
        return false;
    }
    if (worker_task_ == nullptr) {
        ESP_LOGE(TAG, "Audio worker not running; Init() failed earlier?");
        xSemaphoreGiveRecursive(lifecycle_lock_);
        return false;
    }

    RingReset(tx_ring_);
    bsp_audio_set_volume(0);
    if (bsp_audio_set_format(kEncodeRate, kBitsPerSample, kChannels) != ESP_OK) {
        ESP_LOGE(TAG, "Recording codec format switch failed");
        xSemaphoreGiveRecursive(lifecycle_lock_);
        return false;
    }

    // Reinitialize the fixed codec area when changing from playback to record.
    if (!EnsureEncoder()) {
        ESP_LOGE(TAG, "StartRecording: encoder unavailable (free=%u largest=%u)",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        xSemaphoreGiveRecursive(lifecycle_lock_);
        return false;
    }

    while (xSemaphoreTake(work_idle_, 0) == pdTRUE) { }
    is_recording_ = true;
    s_visual_level.store(0, std::memory_order_relaxed);
    work_ = Work::Record;
    xSemaphoreGive(work_signal_);

    ESP_LOGI(TAG, "Recording started. free=%u largest=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    xSemaphoreGiveRecursive(lifecycle_lock_);
    return true;
}

void AudioService::StopRecording() {
    if (lifecycle_lock_ == nullptr) return;
    xSemaphoreTakeRecursive(lifecycle_lock_, portMAX_DELAY);

    if (!is_recording_ && opus_enc_handle_ == nullptr) {
        xSemaphoreGiveRecursive(lifecycle_lock_);
        return;
    }

    is_recording_ = false;                 // The only exit condition of the recording loop
    s_visual_level.store(0, std::memory_order_relaxed);
    const bool idle = (work_ != Work::Record) || WaitWorkIdle(kWorkIdleTimeoutMs, "StopRecording");

    // Keep the encoder for another recording turn. Playback can close its small
    // wrapper and reinitialize the same fixed state once this worker is idle.
    // The user has stopped speaking; no need to send the half frame left in the queue
    RingReset(tx_ring_);

    if (worker_task_ != nullptr) {
        ESP_LOGI(TAG, "Recording stopped (idle=%d, worker stack free %u bytes). free=%u", (int)idle,
                 (unsigned)(uxTaskGetStackHighWaterMark(worker_task_) * sizeof(StackType_t)),
                 (unsigned)esp_get_free_heap_size());
    } else {
        ESP_LOGI(TAG, "Recording stopped. free=%u", (unsigned)esp_get_free_heap_size());
    }
    xSemaphoreGiveRecursive(lifecycle_lock_);
}

void AudioService::RecordSection() {
    while (is_recording_ && opus_enc_handle_ != nullptr && !s_cancel_requested.load()) {
        // bsp_audio_read is a portMAX_DELAY blocking read (see bsp_audio.h); normally a 60ms block
        // always returns, so is_recording_ is checked often enough.
        if (bsp_audio_read(pcm_scratch_, kEncPcmFrameBytes) != ESP_OK) {
            s_visual_level.store(0, std::memory_order_relaxed);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (!is_recording_ || opus_enc_handle_ == nullptr) break;
        publish_level(pcm_scratch_, kEncPcmFrameBytes);

        esp_audio_enc_in_frame_t in_f = {};
        in_f.buffer = pcm_scratch_;
        in_f.len = (uint32_t)kEncPcmFrameBytes;

        esp_audio_enc_out_frame_t out_f = {};
        out_f.buffer = opus_scratch_;
        out_f.len = (uint32_t)kOpusFrameMaxBytes;
        out_f.encoded_bytes = 0;

        const esp_audio_err_t res = esp_opus_enc_process(opus_enc_handle_, &in_f, &out_f);
        if (res == ESP_AUDIO_ERR_OK && out_f.encoded_bytes > 0) {
            // Enqueue only, don't send: network I/O is left to the control task
            if (!RingPush(tx_ring_, opus_scratch_, out_f.encoded_bytes)) {
                ESP_LOGW(TAG, "TX ring full, dropping a frame");
            }
        }
    }
}

bool AudioService::PopEncodedFrame(uint8_t *out, size_t out_cap, size_t *out_len) {
    const size_t n = RingPop(tx_ring_, out, out_cap);
    if (out_len) *out_len = n;
    return n > 0;
}

bool AudioService::HasPendingEncoded() const {
    return RingCount(tx_ring_) > 0;
}

void AudioService::ResetEncodedQueue() {
    RingReset(tx_ring_);
}

// -------------------------------------------------------------------- playback

void AudioService::SetPlaybackSampleRate(int hz) {
    if (hz < 8000 || hz > 48000) {
        ESP_LOGW(TAG, "Ignoring odd playback sample rate %d", hz);
        return;
    }
    if (hz != playback_rate_) {
        ESP_LOGI(TAG, "Playback sample rate -> %d Hz", hz);
        playback_rate_ = hz;
    }
}

bool AudioService::OpenDecoder() {
    if (audio_workspace::owner.load() != audio_workspace::Owner::Xiaozhi) return false;
    if (work_ != Work::Idle) return false;
    const int bytes = opus_decoder_get_size(1);
    if (bytes <= 0 || static_cast<size_t>(bytes) > audio_workspace::kAuxBytes) {
        ESP_LOGE(TAG, "Opus decoder state does not fit: need=%d capacity=%u", bytes,
                 (unsigned)audio_workspace::kAuxBytes);
        return false;
    }
    // Half-duplex handoff: the old worker is idle and its codec wrapper must
    // be closed before the shared state is initialized for the other mode.
    if (opus_enc_handle_) {
        esp_opus_enc_close(opus_enc_handle_);
        opus_enc_handle_ = nullptr;
    }
    auto *state = reinterpret_cast<OpusDecoder *>(audio_workspace::aux());
    const int err = opus_decoder_init(state, playback_rate_, 1);
    if (err != 0) {
        ESP_LOGE(TAG, "Opus fixed decoder init failed: %d", err);
        return false;
    }
    opus_dec_handle_ = state;
    opus_dec_rate_ = playback_rate_;
    ESP_LOGI(TAG, "Opus decoder in shared storage: %d bytes", bytes);
    return true;
}

bool AudioService::StartPlayback() {
    if (lifecycle_lock_ == nullptr || s_cancel_requested.load()) return false;
    xSemaphoreTakeRecursive(lifecycle_lock_, portMAX_DELAY);

    if (is_playing_ && work_ == Work::Play) {
        xSemaphoreGiveRecursive(lifecycle_lock_);
        return true;
    }

    // Half-duplex: recording must stop before playback (recursive lock, so calling directly from the same task is safe).
    // An idle encoder alone is not a reason to discard its outgoing queue.
    if (is_recording_) {
        StopRecording();
    }

    if (work_ != Work::Idle) {
        ESP_LOGE(TAG, "Playback deferred: recorder has not stopped");
        xSemaphoreGiveRecursive(lifecycle_lock_);
        return false;
    }
    if (opus_scratch_ == nullptr || play_ring_buf_ == nullptr) {
        ESP_LOGE(TAG, "Playback buffer missing; Init() not done?");
        xSemaphoreGiveRecursive(lifecycle_lock_);
        return false;
    }
    if (worker_task_ == nullptr) {
        ESP_LOGE(TAG, "Audio worker not running");
        xSemaphoreGiveRecursive(lifecycle_lock_);
        return false;
    }

    // If the server sends a new sample rate, replace the old decoder (recording is stopped and the worker is idle at this point)
    if (opus_dec_handle_ != nullptr && opus_dec_rate_ != playback_rate_) {
        ESP_LOGI(TAG, "Decoder sample rate %d -> %d; reopening", opus_dec_rate_, playback_rate_);
        /* caller-owned decoder state; no heap free */
        opus_dec_handle_ = nullptr;
        opus_dec_rate_ = 0;
    }

    if (opus_dec_handle_ == nullptr && !OpenDecoder()) {
        // Initialization failure must prevent playback from using stale state.
        ESP_LOGE(TAG, "Opus decoder unavailable; preserving recorder (free=%u largest=%u)",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        xSemaphoreGiveRecursive(lifecycle_lock_);
        return false;
    }

    // Strict half-duplex: by the time we get here StopRecording() has already waited for the recording worker to go idle;
    // codec state shares the fixed area, and the codec switches from 16kHz recording to the server's playback sample rate.
    // Mute before switching format so codec close/open transients don't reach the amp. Each app must still declare
    // its own logical volume; playback volume is controlled by the system setting, with no hidden 70% cap on top.
    bsp_audio_set_volume(0);
    // Matches the official folo-ai-passport-xiaozhi: drive the ES8311 directly at the native sample rate (24000Hz),
    // no more 48kHz upsampling, which removes clock and interpolation distortion and frees 2.9KB of heap.
    if (bsp_audio_set_format((uint32_t)playback_rate_, kBitsPerSample, kChannels) != ESP_OK) {
        ESP_LOGE(TAG, "Playback codec format switch failed");
        xSemaphoreGiveRecursive(lifecycle_lock_);
        return false;
    }
    bsp_audio_set_volume(kXiaozhiPlaybackVolume);

    while (xSemaphoreTake(work_idle_, 0) == pdTRUE) { }
    work_ = Work::Play;
    play_stop_ = false;
    play_drain_ = false;
    is_playing_ = true;
    s_visual_level.store(0, std::memory_order_relaxed);
    xSemaphoreGive(work_signal_);

    ESP_LOGI(TAG, "Playback started: Opus %dHz (native). free=%u", playback_rate_,
             (unsigned)esp_get_free_heap_size());
    xSemaphoreGiveRecursive(lifecycle_lock_);
    return true;
}

void AudioService::StopPlayback() {
    if (lifecycle_lock_ == nullptr) return;
    xSemaphoreTakeRecursive(lifecycle_lock_, portMAX_DELAY);
    if (work_ != Work::Play) {
        is_playing_ = false;
        RingReset(play_ring_);
        xSemaphoreGiveRecursive(lifecycle_lock_);
        return;
    }

    // Soft stop: play out the remaining buffered audio, then exit (normal tts stop, the tail is not cut off).
    // is_playing_ is deliberately not set to false here - the playback loop uses it to decide whether to keep writing to the codec.
    play_drain_ = true;
    xSemaphoreGive(work_signal_);

    // Drain by progress rather than wall time. Even a low-bitrate ring covering
    // several seconds must play fully; only a stuck decoder/I2S is force-stopped.
    uint32_t progress = playback_progress_.load();
    TickType_t progressed_at = xTaskGetTickCount();
    while (work_ != Work::Idle && !s_cancel_requested.load()) {
        const uint32_t current = playback_progress_.load();
        if (current != progress) {
            progress = current;
            progressed_at = xTaskGetTickCount();
        }
        if (xTaskGetTickCount() - progressed_at >= pdMS_TO_TICKS(kPlaybackDrainTimeoutMs)) break;
        xSemaphoreTake(work_idle_, pdMS_TO_TICKS(50));
    }
    bool idle = work_ == Work::Idle;
    if (!idle) {
        play_stop_ = true;
        xSemaphoreGive(work_signal_);
        idle = WaitWorkIdle(kPlaybackAbortTimeoutMs, "StopPlayback(force)");
    }
    is_playing_ = false;
    bsp_audio_set_volume(0);
    RingReset(play_ring_);

    // Decoder + PCM remain resident for the whole Xiaozhi session. This makes
    // every subsequent conversation allocation-free and prevents heap churn.

    ESP_LOGI(TAG, "Playback stopped (drained=%d). free=%u", (int)idle,
             (unsigned)esp_get_free_heap_size());
    xSemaphoreGiveRecursive(lifecycle_lock_);
}

void AudioService::AbortPlayback() {
    s_visual_level.store(0, std::memory_order_relaxed);
    if (lifecycle_lock_ == nullptr) return;
    xSemaphoreTakeRecursive(lifecycle_lock_, portMAX_DELAY);
    if (work_ != Work::Play) {
        is_playing_ = false;
        RingReset(play_ring_);
        bsp_audio_set_volume(0);
        xSemaphoreGiveRecursive(lifecycle_lock_);
        return;
    }

    is_playing_ = false;
    play_stop_ = true;          // Exit immediately, don't wait for the buffer
    play_drain_ = false;
    bsp_audio_set_volume(0);
    RingReset(play_ring_);
    xSemaphoreGive(work_signal_);

    const bool idle = WaitWorkIdle(kPlaybackAbortTimeoutMs, "AbortPlayback");
    // Keep decoder, encoder and PCM resident until Deinit().

    ESP_LOGI(TAG, "Playback aborted (idle=%d). free=%u", (int)idle,
             (unsigned)esp_get_free_heap_size());
    xSemaphoreGiveRecursive(lifecycle_lock_);
}

void AudioService::PlaySection() {
    bool primed = false;
    bool format_logged = false;
    uint32_t starve_ticks = 0;
    while (!play_stop_ && !s_cancel_requested.load()) {
        if (!primed) {
            const size_t queued = RingCount(play_ring_);
            if (queued == 0 && play_drain_) break;
            if (!play_drain_ && queued < kPlaybackPrebufferFrames) {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            primed = queued > 0;
            if (primed) {
                ESP_LOGI(TAG, "Playback buffer primed with %u Opus frames",
                         (unsigned)queued);
            }
        }

        const size_t got = RingPop(play_ring_, opus_scratch_, kOpusFrameMaxBytes);
        if (got == 0) {
            // Soft stop received and the buffer is drained → normal finish (the tail has all been written to the codec)
            if (play_drain_ && !HasPendingPlayback()) break;
            vTaskDelay(pdMS_TO_TICKS(5));
            starve_ticks += 5;
            // Only mark as not pre-buffered again on a severe dropout (no data for more than 200ms),
            // so everyday network jitter doesn't keep resetting pre-buffering and make the underlying DMA run dry and pop.
            if (starve_ticks >= 200) {
                primed = false;
                starve_ticks = 0;
            }
            continue;
        }
        starve_ticks = 0;

        const int frames = opus_decode(static_cast<OpusDecoder *>(opus_dec_handle_),
            opus_scratch_, static_cast<int32_t>(got), reinterpret_cast<int16_t *>(pcm_scratch_),
            kMaxPcmFrameBytes / sizeof(int16_t), 0);
        if (frames > 0 && !play_stop_) {
            const size_t bytes = static_cast<size_t>(frames) * sizeof(int16_t);
            if (!format_logged) {
                ESP_LOGI(TAG, "Decoded PCM frame=%u bytes, I2S direct write", (unsigned)bytes);
                format_logged = true;
            }
            if (bsp_audio_write(pcm_scratch_, bytes) != ESP_OK) {
                s_visual_level.store(0, std::memory_order_relaxed);
                ESP_LOGE(TAG, "I2S playback write failed (%u bytes)", (unsigned)bytes);
                break;
            }
            playback_progress_.fetch_add(1);
            if (!play_stop_ && !s_cancel_requested.load())
                publish_level(pcm_scratch_, bytes, bsp_audio_get_master_volume());
        } else if (frames < 0) {
            ESP_LOGW(TAG, "Opus decode failed: %d (%u-byte frame)", frames, (unsigned)got);
        }
    }
}

void AudioService::PushIncomingAudio(const uint8_t *opus_data, size_t len) {
    if (opus_data == nullptr || len == 0 || len > kOpusFrameMaxBytes) return;

    // The TTS server may burst audio faster than real-time playback. The old logic dropped Opus frames as soon as the 2KB queue
    // was full; device logs confirmed consecutive drops on long replies, which is exactly the hissing/crackling heard.
    // Now a larger queue absorbs bursts first; if still full, wait briefly on the websocket receive task and use
    // TCP backpressure to slow the sender. 300ms is below keepalive/protocol timeout scale and won't stall the callback for long.
    const TickType_t wait_started = xTaskGetTickCount();
    const TickType_t wait_ticks = pdMS_TO_TICKS(kPlaybackEnqueueWaitMs);
    while (!RingPush(play_ring_, opus_data, len)) {
        if (play_stop_ || xTaskGetTickCount() - wait_started >= wait_ticks) {
            ESP_LOGW(TAG, "Playback ring full after %ums wait (%u/%u), frame dropped",
                     (unsigned)kPlaybackEnqueueWaitMs, (unsigned)RingUsed(play_ring_),
                     (unsigned)kPlayRingCap);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    playback_progress_.fetch_add(1);
    // Wake the worker thread immediately (a semaphore has no handle, so there is no "task deletes itself right after its handle was read" race)
    if (work_signal_ != nullptr) xSemaphoreGive(work_signal_);
}

bool AudioService::HasPendingPlayback() const {
    return RingCount(play_ring_) > 0;
}

void AudioService::WorkerEntry(void *arg) {
    static_cast<AudioService *>(arg)->WorkerLoop();
}

// Resident: normally parked on work_signal_ using no CPU; works in the requested mode only when signaled.
void AudioService::WorkerLoop() {
    ESP_LOGI(TAG, "Audio worker running (stack %u bytes)", (unsigned)kWorkerStack);

    while (!worker_quit_) {
        if (xSemaphoreTake(work_signal_, pdMS_TO_TICKS(500)) != pdTRUE) continue;
        if (worker_quit_) break;

        if (work_ == Work::Record) {
            RecordSection();
        } else if (work_ == Work::Play) {
            PlaySection();
        }
        // Whichever mode ended, report "stopped" so the control task can close handles safely
        s_visual_level.store(0, std::memory_order_relaxed);
        is_recording_ = false;
        is_playing_ = false;
        work_ = Work::Idle;
        xSemaphoreGive(work_idle_);
    }

    ESP_LOGI(TAG, "Audio worker exiting (stack free %u bytes)",
             (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
    if (worker_stopped_ != nullptr) xSemaphoreGive(worker_stopped_);
    // The owner deletes this static task after receiving worker_stopped_.
    // Suspending here prevents any return through a freed stack.
    vTaskSuspend(NULL);
}

void AudioService::Reset() {
    StopRecording();
    AbortPlayback();
}

} // namespace xiaozhi
