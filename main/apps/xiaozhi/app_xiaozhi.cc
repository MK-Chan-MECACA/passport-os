#include "apps/xiaozhi/app_xiaozhi.h"
#include "apps/xiaozhi/xiaozhi_protocol.h"
#include "apps/xiaozhi/xiaozhi_audio.h"
#include "apps/xiaozhi/xiaozhi_ui.h"
#include "apps/xiaozhi/xiaozhi_ota.h"
#include "apps/xiaozhi/xiaozhi_store.h"
#include "wifi_manager.h"
#include "bsp_audio.h"
#include "launcher/settings.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <atomic>
#include <freertos/semphr.h>
#include <cstring>
#include <string>

static const char *TAG = "app_xiaozhi";

// ---------------------------------------------------------------------------
// Architecture: single-owner control task (xzg_ctl)
//
// The real failure before the fix: multiple tasks operated on the same audio state at once, with no locks at all -
//   * websocket task: cloud audio received → StartPlayback()/PlayAudioFrame()
//   * input task: button press → StopPlayback()/StartRecording()
// "Press OK to interrupt while the AI is speaking" made the two run concurrently: the input task freed playback_buf_
// and closed opus_dec_handle_ while the websocket task was decoding with them → use-after-free
// → panic → reboot. Reproduced on device (the log stops at "Opus decoder closed" right after
// "Starting audio playback", and the device reboots immediately).
//
// Division of responsibilities after the fix:
//   * Only the control task may call Protocol::Start/Stop/Trigger*, AudioService
//     Start*/Stop*, and all ui_* functions.
//   * websocket callbacks only write the event queue + push Opus into the ring buffer, then return immediately.
//   * Button callbacks only post commands, then return immediately.
//   * The Protocol instance is a "never destroyed" function-local static, which eliminates callbacks touching a destroyed object at the root.
// ---------------------------------------------------------------------------

namespace {

using xiaozhi::EvType;
using xiaozhi::Event;
using xiaozhi::OtaResult;
using xiaozhi::State;

constexpr int kProtoQueueDepth = 8;  // text only; critical transitions use mailbox
constexpr int kCmdQueueDepth = 8;
// The control task only does the state machine / UI / small message sends (ssl_write needs about 2-3KB of stack).
// The two really stack-hungry jobs were moved out: OTA's mbedTLS handshake → temporary xzg_ota task;
// Opus encode/decode → resident shared xzg_audio task (24KB, see the notes in xiaozhi_audio.cc).
// MCP tool calls run here (JSON, NVS, device control). 2026-11-07 device probe:
// 1364 B left of 6144 after an in-session music search; heap-allocated per session.
constexpr size_t kControlTaskStack = 7168;
constexpr uint32_t kLoopTickMs = 20;

// Timers (milliseconds)
constexpr int64_t kConnectTimeoutMs = 12000;    // Max wait for hello after starting a connection
constexpr int64_t kThinkingTimeoutMs = 50000;   // Leaves response time on weak networks while still preventing a permanent hang
constexpr int64_t kListeningTimeoutMs = 30000;
constexpr int64_t kSpeakingStallTimeoutMs = 30000; // no audio/PCM progress, not reply duration
constexpr int64_t kReconnectDelayMs = 6000;     // Reconnect backoff after disconnect
constexpr int64_t kActivationRepollMs = 30000;  // Re-query interval while waiting for the user to bind the device
constexpr int64_t kWifiRetryMs = 1000;
constexpr int64_t kOtaRefreshMs = 10 * 60 * 1000;  // Connection parameter cache lifetime (10 minutes)
constexpr int kOtaRetryAfterFailures = 3;          // Re-fetch config after this many consecutive failures

enum class Cmd : uint8_t {
    Connect = 0,
    StartListen,
    StopListen,
    Abort,
    VolUp,
    VolDown,
    ToggleListen,
    Stop,
};

QueueHandle_t s_proto_q = nullptr;
QueueHandle_t s_cmd_q = nullptr;
StaticQueue_t s_proto_queue_storage{}, s_cmd_queue_storage{};
uint8_t s_proto_items[kProtoQueueDepth * sizeof(Event)];
uint8_t s_cmd_items[kCmdQueueDepth * sizeof(Cmd)];
StaticSemaphore_t s_stopped_storage{};
TaskHandle_t s_ctl_task = nullptr;
std::atomic<bool> s_control_running{false};
bool s_start_failed = false;
SemaphoreHandle_t s_stopped_sem = nullptr;
std::atomic<bool> s_app_active{false};
// Separate fallback flag for the Stop command: see the post_cmd() comment (must still stop when the queue is full).
std::atomic<bool> s_stop_requested{false};
// Whether the protocol singleton has been constructed. Constructing it writes NVS (including SPI-Flash erase/write, a deep call chain),
// which may only happen in the control task with its large stack; the button task (3072-byte stack) must never trigger it.
std::atomic<bool> s_protocol_ready{false};
// Separate mailbox: a full volume/key queue cannot lose a double-click's
// conversation request while OTA/TLS connection is still in progress.
std::atomic<bool> s_conversation_requested{false};

// Control task private state
State s_cur_state = State::Disconnected;
bool s_connect_pending = false;
int64_t s_connect_started_ms = 0;
int64_t s_thinking_since_ms = 0;
int64_t s_speaking_since_ms = 0;
uint32_t s_playback_progress = 0;
int64_t s_reconnect_at_ms = 0;
int64_t s_reconnect_delay_ms = kReconnectDelayMs;
int64_t s_listening_since_ms = 0;
int64_t s_next_connect_attempt_ms = 0;
bool s_activation_pending = false;
std::string s_activation_code;
bool s_audio_ready = false;
bool s_session_ready = false;
bool s_audio_recovery_required = false;
std::string s_retry_reason;
// From when the user presses OK until the user presses OK again or it aborts. Only when this flag is true does it automatically
// return to the next listening turn after TTS finishes; avoids state events reopening the mic after the user deliberately interrupted.
bool s_conversation_active = false;
// A user's OK while disconnected resumes listening only after the new hello
// and audio reservation succeed. Background reconnects never open the mic.
bool s_listen_after_connect = false;

// Connection parameter cache: the OTA request runs one HTTPS request (a few hundred ms + about 30KB of heap),
// so it can't be repeated on every reconnect. Only re-fetch on first use / expiry / consecutive failures.
bool s_cfg_valid = false;
std::string s_cfg_url;
std::string s_cfg_token;
int s_cfg_version = 3;
int64_t s_cfg_fetched_ms = 0;
// Reuse the last OTA result across app switches: a successful OTA already wrote url/token/version to NVS,
// so on exit we only free the heap strings and record this flag; on re-entry, if not expired, connect WS directly with the NVS config,
// saving one HTTPS/TLS handshake. If connecting with it fails once, go back to re-fetching OTA.
bool s_cfg_carried = false;
int s_connect_failures = 0;

int64_t now_ms() { return esp_timer_get_time() / 1000; }

void post_cmd(Cmd c) {
    if (s_cmd_q == nullptr) return;
    // 2026-09-13 fix (A4): Stop must never be dropped. Previously everything was posted non-blocking, so while the control task
    // was stuck in connect_xiaozhi() (OTA/websocket, up to 20s), a full queue dropped Stop,
    // and stop() timed out while the task was still alive; then start(), checking only s_app_active, created another
    // control task → two tasks operating on audio concurrently = the UAF documented in the file header comes back.
    // Now: Stop uses a 500ms blocking post + a separate s_stop_requested flag as a double safeguard;
    // even if the post fails, the control task sees the flag on its next loop and exits.
    if (c == Cmd::Stop) {
        s_stop_requested = true;
        xiaozhi::AudioService::RequestStop();
    }
    const TickType_t wait = (c == Cmd::Stop) ? pdMS_TO_TICKS(500) : 0;
    if (xQueueSend(s_cmd_q, &c, wait) != pdTRUE) {
        ESP_LOGW(TAG, "Command queue full, dropped cmd=%d", (int)c);
    }
}

// Never-destroyed protocol instance: websocket callbacks/the recording task may reference it, so static lifetime is safest.
xiaozhi::Protocol &proto() {
    static xiaozhi::Protocol inst;
    return inst;
}

void schedule_reconnect(int64_t delay_ms) {
    if (s_reconnect_at_ms != 0) return; // error/disconnect can describe the same failure
    if (delay_ms == kReconnectDelayMs) {
        delay_ms = s_reconnect_delay_ms;
        s_reconnect_delay_ms = delay_ms >= 30000 ? 60000 : delay_ms * 2;
    }
    s_reconnect_at_ms = now_ms() + delay_ms;
    s_connect_pending = false;
}

void clear_timers() {
    s_listening_since_ms = 0;
    s_connect_pending = false;
    s_thinking_since_ms = 0;
    s_speaking_since_ms = 0;
}

void on_state_changed(State st) {
    State old = s_cur_state;
    s_cur_state = st;
    // TTS stop can arrive with seconds of buffered speech still playing. Keep
    // the speaking face/waveform until the existing bounded drain finishes.
    if (old == State::Speaking && st != State::Speaking) {
        xiaozhi::AudioService::GetInstance().StopPlayback();
    }
    if (st == State::Listening && old != st) s_listening_since_ms = now_ms();
    if (st != State::Listening) s_listening_since_ms = 0;
    xiaozhi::ui_set_state(st);
    if (!s_retry_reason.empty() &&
        (st == State::Disconnected || st == State::Error)) {
        xiaozhi::ui_set_status_text(s_retry_reason);
    }

    if (st == State::Thinking) {
        s_thinking_since_ms = now_ms();
        s_speaking_since_ms = 0;
        // In auto mode the server VAD has finished segmenting the sentence. First wait for the recording worker to fully exit, so
        // later TTS never runs alongside the mic, and the C3 doesn't need to carry the memory cost of AEC/full-duplex.
        xiaozhi::AudioService::GetInstance().StopRecording();
    } else if (st == State::Speaking) {
        s_thinking_since_ms = 0;
        s_speaking_since_ms = now_ms();
        s_playback_progress = xiaozhi::AudioService::GetInstance().PlaybackProgress();
        // Start playback as soon as we enter "replying". Without this, audio already pushed by the cloud just piles up in
        // the ring buffer until the tick fallback notices it - which shows up as dropped opening frames + about 1 second of reply delay.
        if (!xiaozhi::AudioService::GetInstance().IsPlaying()) {
            if (!s_session_ready || !xiaozhi::AudioService::GetInstance().StartPlayback()) {
                s_session_ready = false;
                s_audio_recovery_required = true;
                s_conversation_active = false;
                proto().Stop();
                s_retry_reason = "Audio failed, retrying";
                schedule_reconnect(kReconnectDelayMs);
            }
        }
    } else {
        s_thinking_since_ms = 0;
        s_speaking_since_ms = 0;
    }

    // Leaving "replying": play out the remaining audio in the ring buffer before finishing (the tail is not cut off)
    if (old == State::Speaking && st != State::Speaking) {
        // Conversation mode: start the next recording turn only after the previous reply has fully played and the speaker is muted.
        // Done through the command queue, to avoid recursively switching the audio lifecycle inside the protocol event handler stack.
        if (st == State::Idle && s_conversation_active && s_session_ready &&
            proto().HandshakeDone() && proto().IsConnected()) {
            post_cmd(Cmd::StartListen);
        }
    }
}

// The OTA HTTPS request runs a full mbedTLS handshake - the most stack-hungry call chain in this firmware
// (measured on device: a 6144-byte stack is not enough and overflows into a reboot). But it only happens when "fetching config", and once done
// the stack should be given back. So it runs in a temporary task that is deleted after use, and the control task keeps a small stack.
struct OtaJob {
    xiaozhi::OtaOutcome out;
    std::atomic<unsigned> refs{2};  // controller + worker
    std::atomic<bool> done{false};
};

OtaJob *s_ota_job = nullptr;  // only controller owns this pointer
std::atomic<bool> s_ota_worker_running{false};

void release_ota_job(OtaJob *job) {
    if (job->refs.fetch_sub(1) == 1) delete job;
}

void ota_worker(void *arg) {
    auto *job = static_cast<OtaJob *>(arg);
    job->out = xiaozhi::ota_fetch_config();
    job->done.store(true);  // publishes out before controller copies it
    release_ota_job(job);
    s_ota_worker_running.store(false);
    vTaskDelete(nullptr);
}

// Returns false if no result was obtained this time (including out of memory/timeout).
bool fetch_ota_config(xiaozhi::OtaOutcome *out) {
    // Keep a timed-out worker's owner until completion. No forced task deletion,
    // leaked semaphore, concurrent use of OTA's static response, or permanent
    // failure latch. A late result is consumed on the next retry/app opening.
    const bool fresh = s_ota_job == nullptr;
    if (fresh) {
        if (s_ota_worker_running.load()) {
            out->detail = "previous OTA request is finishing";
            return false;
        }
        s_ota_job = new (std::nothrow) OtaJob{};
        if (s_ota_job == nullptr) {
            out->detail = "no memory for OTA job";
            return false;
        }
        s_ota_worker_running.store(true);
        if (xTaskCreate(ota_worker, "xzg_ota", 12288, s_ota_job, 4, nullptr) != pdPASS) {
            s_ota_worker_running.store(false);
            delete s_ota_job;
            s_ota_job = nullptr;
            out->detail = "task create failed";
            return false;
        }
    }
    const int64_t started = now_ms();
    while (!s_ota_job->done.load()) {
        if (!fresh || s_stop_requested || now_ms() - started >= 20000) {
            out->detail = "OTA request still running; will retry after completion";
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    *out = s_ota_job->out;
    release_ota_job(s_ota_job);
    s_ota_job = nullptr;
    // Force a yield to the FreeRTOS Idle task: the 12KB stack and TCB of the self-deleted ota_worker are reclaimed by the Idle task.
    // control_task has priority 5, so it must delay briefly to let Idle run, reclaim and merge heap blocks,
    // so the later 22KB stack allocation for audio init doesn't hit out-of-memory because the old stack hasn't been freed yet.
    vTaskDelay(pdMS_TO_TICKS(60));
    return true;
}

void do_connect() {
    if (proto().NeedsRestart() && proto().HasClient()) {
        s_conversation_active = false;
        s_session_ready = false;
        clear_timers();
        xiaozhi::AudioService::GetInstance().StopRecording();
        xiaozhi::AudioService::GetInstance().AbortPlayback();
        if (!xiaozhi::AudioService::GetInstance().ReleaseSessionResources()) {
            s_audio_recovery_required = true;
        }
        // Only the controller may join/destroy the websocket worker.
        proto().Stop();
    }
    if (proto().IsConnected()) return;

    // The client object still exists → esp_websocket_client is reconnecting per its own reconnect_timeout_ms,
    // so don't tear it down and rebuild yet (otherwise the two interrupt each other and recovery gets slower). Only rebuild after repeated failures.
    if (proto().HasClient() && s_connect_failures < kOtaRetryAfterFailures) {
        ESP_LOGI(TAG, "WebSocket client still present, letting it auto-reconnect");
        s_next_connect_attempt_ms = now_ms() + 2000;
        return;
    }

    if (!WifiManager::GetInstance().IsConnected()) {
        xiaozhi::ui_set_state(State::Disconnected);
        xiaozhi::ui_set_status_text("Network lost");
        s_next_connect_attempt_ms = now_ms() + kWifiRetryMs;
        return;
    }

    // Rebuilding after repeated failures must stop the old auto-reconnecting
    // TLS client before running another HTTPS handshake in this small heap.
    if (proto().HasClient()) proto().Stop();
    if (s_audio_recovery_required && s_audio_ready) {
        if (!xiaozhi::AudioService::GetInstance().Deinit()) {
            schedule_reconnect(kReconnectDelayMs);
            return;
        }
        s_audio_ready = false;
        s_audio_recovery_required = false;
    }
    s_retry_reason.clear();

    const int64_t t = now_ms();
    const bool carried = s_cfg_carried && s_connect_failures == 0;
    const bool cfg_stale = (!s_cfg_valid && !carried) || (t - s_cfg_fetched_ms > kOtaRefreshMs) ||
                           (s_connect_failures >= kOtaRetryAfterFailures);

    // 1. If needed, first exchange the websocket config with the backend. The official backend requires this step:
    //    only then does it return websocket{url,token,version}, or, when the device is not bound, return
    //    activation{code,message} asking the user to bind it first.
    if (cfg_stale) {
        xiaozhi::ui_set_status_text("Preparing to connect");
        s_cfg_carried = false;
        xiaozhi::OtaOutcome ota;
        if (!fetch_ota_config(&ota)) {
            s_retry_reason = "Can't reach server";
            xiaozhi::ui_set_status_text(s_retry_reason);
            schedule_reconnect(kReconnectDelayMs);
            return;
        }

        if (ota.result == OtaResult::NeedsActivation) {
            s_activation_pending = true;
            s_activation_code = ota.activation_code;
            if (!s_activation_code.empty()) {
                ESP_LOGW(TAG, "Activation required, code=%s", s_activation_code.c_str());
                // The verification code must be fully visible in the message area (the status pill is too narrow and truncates it), and we give
                // regular users the real binding path - direct MAC binding is a developer backdoor that users don't have.
                xiaozhi::ui_set_status_text("Activate: " + s_activation_code);
                xiaozhi::ui_set_ai_text(
                    "Device not linked. On your phone, open xiaozhi.me, sign in, "
                    "tap Add Device and enter code " + s_activation_code +
                    ". It connects about 30 s after linking.");
                xiaozhi::ui_set_hint("Connects once linked");
            } else {
                ESP_LOGW(TAG, "Activation required (no code returned)");
                xiaozhi::ui_set_status_text("Waiting for link");
                xiaozhi::ui_set_ai_text(ota.activation_message.empty()
                                            ? "Device not activated. Open xiaozhi.me and add this device."
                                            : ota.activation_message);
            }
            s_next_connect_attempt_ms = now_ms() + kActivationRepollMs;
            return;
        }
        s_activation_pending = false;

        if (ota.result == OtaResult::Ok) {
            s_cfg_url = ota.url;
            s_cfg_token = ota.token;
            s_cfg_version = (ota.version > 0) ? ota.version : 3;
            s_cfg_valid = true;
            s_cfg_fetched_ms = t;
            s_connect_failures = 0;
            ESP_LOGI(TAG, "OTA config accepted: url=%s version=%d token=%s",
                     s_cfg_url.c_str(), s_cfg_version, s_cfg_token.empty() ? "(none)" : "(set)");
        } else {
            ESP_LOGW(TAG, "OTA gave no websocket config (%s); falling back to NVS/defaults",
                     ota.detail.c_str());
        }
    }

    // 2. Decide the final connection parameters: OTA/cache > this app's NVS > official same-name NVS > built-in default
    std::string url = s_cfg_url;
    std::string token = s_cfg_token;
    int version = s_cfg_version;

    if (url.empty()) url = store_get_string(kNsXiaozhi, "ws_url", "");
    if (url.empty()) url = store_get_string(kNsWebsocket, "url", "");
    if (url.empty()) url = "wss://api.tenclass.net/xiaozhi/v1/";

    if (token.empty()) token = store_get_string(kNsXiaozhi, "token", "");
    if (token.empty()) token = store_get_string(kNsWebsocket, "token", "");
    // Note: no longer falls back to the literal "test-token". With no token we simply don't send the Authorization header,
    // letting the server treat it as anonymous/unactivated and saying so clearly on screen, instead of failing silently with a fake token.

    if (version <= 0) version = store_get_int(kNsXiaozhi, "version", 0);
    if (version <= 0) version = store_get_int(kNsWebsocket, "version", 0);
    if (version <= 0) version = 3;

    ESP_LOGI(TAG, "Connecting: url=%s version=%d token=%s (free=%u largest=%u)",
             url.c_str(), version, token.empty() ? "(none)" : "(set)",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    // The audio subsystem is initialized only "after the OTA temporary task's 12KB stack has been returned":
    // the resident recording task needs a 16KB contiguous stack, and allocating it earlier, while the heap is more intact, is more reliable.
    if (!s_audio_ready) {
        if (xiaozhi::AudioService::GetInstance().Init() != ESP_OK) {
            ESP_LOGE(TAG, "Audio init failed; Xiaozhi stays unusable");
            s_retry_reason = "Audio failed, retrying";
            xiaozhi::ui_set_status_text(s_retry_reason);
            schedule_reconnect(kReconnectDelayMs);
            return;
        }
        s_audio_ready = true;
    }

    xiaozhi::ui_set_status_text("Connecting to Xiaozhi");
    if (!proto().Start(url, token, version)) {
        s_retry_reason = "Can't reach server";
        xiaozhi::ui_set_status_text(s_retry_reason);
        s_connect_failures++;
        schedule_reconnect(kReconnectDelayMs);
        return;
    }
    s_connect_pending = true;
    s_connect_started_ms = now_ms();
}

void do_start_listen() {
    if (s_stop_requested || !s_app_active) return;
    if (proto().NeedsRestart() || !proto().IsConnected()) {
        post_cmd(Cmd::Connect);
        return;
    }
    if (!s_session_ready || !proto().HandshakeDone()) {
        xiaozhi::ui_set_status_text("Getting ready…");
        return;
    }
    // Use the protocol layer's live state, not this task's cached s_cur_state (which can lag by one loop)
    const State st = proto().GetState();
    if (st == State::Listening || st == State::Thinking) return;

    xiaozhi::ui_clear_text();

    if (!proto().TriggerStartListen()) {
        xiaozhi::ui_set_status_text("Send failed, try again");
        return;
    }
    s_conversation_active = true;
    s_listening_since_ms = now_ms();

    // The recording task only captures + encodes; frames are sent by this task (drain_audio_tx)
    if (!xiaozhi::AudioService::GetInstance().StartRecording()) {
        // The server already got listen/start, so we must send an abort to close it out, otherwise the session is left hanging
        ESP_LOGE(TAG, "StartRecording failed; aborting session");
        s_conversation_active = false;
        s_session_ready = false;
        s_audio_recovery_required = true;
        proto().TriggerAbort();
        proto().Stop();
        s_retry_reason = "Mic failed, retrying";
        xiaozhi::ui_set_status_text(s_retry_reason);
        schedule_reconnect(kReconnectDelayMs);
    }
}

// Send the Opus frames encoded by the recording task; returns the number of frames sent this time.
// Only called from the control task - so mbedTLS send stack usage lands on the 10KB control task
// instead of pushing the recording task to ~14KB (on device, 10KB overflows in ssl_write and reboots).
unsigned s_tx_frames = 0;

int drain_audio_tx() {
    static uint8_t frame[xiaozhi::kOpusFrameMaxBytes];
    size_t len = 0;
    int sent = 0;
    while (xiaozhi::AudioService::GetInstance().PopEncodedFrame(frame, sizeof(frame), &len)) {
        if (!proto().SendAudio(frame, len)) {
            ESP_LOGW(TAG, "SendAudio failed, dropping frame (%u bytes)", (unsigned)len);
            break;
        }
        s_tx_frames++;
        if (s_tx_frames % 50 == 0) {
            ESP_LOGI(TAG, "Sent %u audio frames so far (%u bytes last)", (unsigned)s_tx_frames,
                     (unsigned)len);
        }
        if (++sent >= 8) break;   // Send at most 8 frames per call to avoid holding the control loop too long
    }
    return sent;
}

void do_stop_listen() {
    if (s_cur_state != State::Listening) return;
    // In auto conversation mode, OK ends the whole conversation rather than submitting the current sentence (sentence segmentation is done by the server
    // VAD). Stop recording completely first, then abort, so no audio keeps uploading after exit.
    s_conversation_active = false;
    xiaozhi::AudioService::GetInstance().StopRecording();
    proto().TriggerAbort();
}

void do_abort() {
    // Cut the sound first, then notify the server: it feels like "instantly quiet"
    s_conversation_active = false;
    xiaozhi::AudioService::GetInstance().AbortPlayback();
    if (proto().IsConnected()) {
        proto().TriggerAbort();
    } else {
        on_state_changed(State::Idle);
    }
}

void do_stop_app() {
    ESP_LOGI(TAG, "Stopping Xiaozhi app: free heap=%u largest=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    s_conversation_active = false;
    s_listen_after_connect = false;
    s_conversation_requested = false;
    s_session_ready = false;
    // The worker owns a second reference and completes without this task. Keep
    // a global running flag so the next app cannot reuse OTA's response buffer.
    if (s_ota_job != nullptr) {
        release_ota_job(s_ota_job);
        s_ota_job = nullptr;
    }
    xiaozhi::AudioService::GetInstance().StopRecording();
    xiaozhi::AudioService::GetInstance().AbortPlayback();
    if (s_protocol_ready) {
        proto().Stop();
    }
    s_protocol_ready = false;
    // Reconnects within this app retain the cache; leaving returns all strings
    // allocated during OTA/TLS so they cannot split the next app's codec heap.
    if (s_cfg_valid) s_cfg_carried = true;
    s_cfg_valid = false;
    std::string{}.swap(s_cfg_url);
    std::string{}.swap(s_cfg_token);
    std::string{}.swap(s_activation_code);
    std::string{}.swap(s_retry_reason);
    // Return the resident recording task and PCM buffers to the heap too: after switching to another app, Xiaozhi shouldn't hold memory
    if (s_audio_ready) {
        if (xiaozhi::AudioService::GetInstance().Deinit()) {
            s_audio_ready = false;
        } else {
            ESP_LOGE(TAG, "Audio teardown incomplete; retained resources for safety");
        }
    }
    // The registry keeps the last UI alive until its safe screen handoff.
    clear_timers();
    s_activation_pending = false;

    ESP_LOGI(TAG, "Xiaozhi app stopped cleanly. free heap=%u largest=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

void set_volume(int delta) {
    int vol = (int)bsp_audio_get_master_volume() + delta;
    if (vol > 100) vol = 100;
    if (vol < 0) vol = 0;
    settings_set_volume((uint8_t)vol);
    xiaozhi::ui_set_volume((unsigned)vol);
    ESP_LOGI(TAG, "Volume -> %d%%", vol);
}

void handle_proto_event(const Event &ev) {
    switch (ev.type) {
    case EvType::Connected:
        if (proto().HandshakeDone()) break;  // stale connected before queued hello
        s_connect_pending = true;
        s_connect_started_ms = now_ms();
        ESP_LOGI(TAG, "Socket connected, waiting for server hello...");
        xiaozhi::ui_set_status_text("Connecting");
        break;

    case EvType::HelloOk:
        if (!proto().HandshakeDone() || !proto().IsConnected()) break;
        s_connect_pending = false;
        s_reconnect_at_ms = 0;
        s_connect_failures = 0;
        // Playback sample rate follows the server (official server measured sending 24000, local default 16000)
        if (proto().ServerSampleRate() > 0) {
            xiaozhi::AudioService::GetInstance().SetPlaybackSampleRate(proto().ServerSampleRate());
        }
        if (!xiaozhi::AudioService::GetInstance().PrepareSessionResources()) {
            ESP_LOGE(TAG, "Server hello succeeded but post-hello audio reservation failed");
            s_session_ready = false;
            s_audio_recovery_required = true;
            s_retry_reason = "Low memory, retrying";
            s_conversation_active = false;
            proto().Stop();
            schedule_reconnect(kReconnectDelayMs);
            xiaozhi::ui_set_status_text(s_retry_reason);
            break;
        }
        s_session_ready = true;
        s_reconnect_delay_ms = kReconnectDelayMs;
        s_retry_reason.clear();
        xiaozhi::ui_set_status_text("Ready");
        if (s_listen_after_connect) {
            s_listen_after_connect = false;
            do_start_listen();
        }
        break;

    case EvType::StateChanged:
        on_state_changed(ev.state);
        break;

    case EvType::Disconnected:
        s_retry_reason = "Network lost";
        xiaozhi::ui_set_status_text(s_retry_reason);
        if (s_connect_pending) s_connect_failures++;
        s_connect_pending = false;
        clear_timers();
        s_conversation_active = false;
        s_session_ready = false;
        // Clean up audio on disconnect, so no leftover task holds on to the codec
        xiaozhi::AudioService::GetInstance().StopRecording();
        xiaozhi::AudioService::GetInstance().AbortPlayback();
        // Playback resources stay resident within one websocket session, but the reconnect TLS handshake
        // needs this short-lived contiguous memory. Only freed at the disconnect boundary, so multi-turn conversations are unaffected.
        if (!xiaozhi::AudioService::GetInstance().ReleaseSessionResources()) {
            s_audio_recovery_required = true;
        }
        schedule_reconnect(kReconnectDelayMs);
        break;

    case EvType::SocketError:
        s_connect_failures++;
        s_connect_pending = false;
        clear_timers();
        s_conversation_active = false;
        s_session_ready = false;
        xiaozhi::AudioService::GetInstance().StopRecording();
        xiaozhi::AudioService::GetInstance().AbortPlayback();
        if (!xiaozhi::AudioService::GetInstance().ReleaseSessionResources()) {
            s_audio_recovery_required = true;
        }
        s_retry_reason = "Can't reach server";
        xiaozhi::ui_set_status_text(s_retry_reason);
        schedule_reconnect(kReconnectDelayMs);
        break;

    case EvType::UserText:
        xiaozhi::ui_set_user_text(ev.text);
        break;

    case EvType::AiSentence:
        xiaozhi::ui_set_ai_text(ev.text);
        break;

    case EvType::LlmEmotion:
        xiaozhi::ui_set_emotion(ev.text);
        break;

    case EvType::Alert:
        ESP_LOGW(TAG, "Server alert: %s", ev.text);
        xiaozhi::ui_set_status_text(ev.text);
        break;

    case EvType::SessionInfo:
    case EvType::HandshakeTimeout:
        break;
    }
}

void handle_cmd(Cmd c) {
    switch (c) {
    case Cmd::Connect:
        s_reconnect_at_ms = 0;
        do_connect();
        break;
    case Cmd::StartListen: do_start_listen(); break;
    case Cmd::StopListen:  do_stop_listen();  break;
    case Cmd::Abort:       do_abort();        break;
    case Cmd::VolUp:       set_volume(+10);   break;
    case Cmd::VolDown:     set_volume(-10);   break;
    case Cmd::ToggleListen:
        // The input task never dereferences a client that the controller may
        // concurrently stop/destroy. Resolve the button against live state here.
        if (proto().NeedsRestart() || !proto().IsConnected() ||
            !proto().HandshakeDone() || !s_session_ready) {
            s_listen_after_connect = true;
            do_connect();
        }
        else if (proto().GetState() == State::Speaking) do_abort();
        else if (proto().GetState() == State::Listening) do_stop_listen();
        else if (proto().GetState() != State::Thinking) do_start_listen();
        break;
    case Cmd::Stop:        do_stop_app();     break;
    }
}

void process_conversation_request() {
    if (!s_conversation_requested.exchange(false) || s_stop_requested || !s_app_active) return;
    if (proto().NeedsRestart() || !proto().IsConnected() ||
        !proto().HandshakeDone() || !s_session_ready) {
        s_listen_after_connect = true;
        do_connect();
        return;
    }
    // A second double-click is an explicit start request, not ToggleListen:
    // never stop a recording or interrupt an ongoing answer/thinking turn.
    if (proto().GetState() == State::Idle) do_start_listen();
}

void tick() {
    const int64_t t = now_ms();

    // 1. Wi-Fi connected / reconnect time reached → start connecting
    if (!proto().IsConnected() && s_reconnect_at_ms != 0 && t >= s_reconnect_at_ms) {
        s_reconnect_at_ms = 0;
        post_cmd(Cmd::Connect);
    }
    if (s_next_connect_attempt_ms != 0 && t >= s_next_connect_attempt_ms) {
        s_next_connect_attempt_ms = 0;
        post_cmd(Cmd::Connect);
    }

    // 2. Handshake timeout: connected but the server never sends hello
    if (s_connect_pending && t - s_connect_started_ms > kConnectTimeoutMs) {
        ESP_LOGW(TAG, "Server hello timeout, retrying");
        s_connect_pending = false;
        s_connect_failures++;
        proto().Stop();
        s_retry_reason = "Server not responding";
        xiaozhi::ui_set_status_text(s_retry_reason);
        schedule_reconnect(kReconnectDelayMs);
    }

    if (proto().GetState() == State::Listening && s_listening_since_ms != 0 &&
        t - s_listening_since_ms >= kListeningTimeoutMs) {
        s_listening_since_ms = 0;
        s_conversation_active = false;
        xiaozhi::AudioService::GetInstance().StopRecording();
        proto().TriggerAbort();
        xiaozhi::ui_set_ai_text("Recording stopped. Press OK to talk again.");
    }

    // 3. No response in thinking state → abort proactively, so the UI doesn't stay stuck on "thinking" with OK unresponsive
    if (proto().GetState() == State::Thinking && s_thinking_since_ms != 0 &&
        t - s_thinking_since_ms > kThinkingTimeoutMs) {
        ESP_LOGW(TAG, "Thinking timeout, aborting this turn");
        s_thinking_since_ms = 0;
        s_conversation_active = false;
        proto().TriggerAbort();
        xiaozhi::ui_set_ai_text("No reply in time. Please say it again.");
    }

    // Continuous stories/music may last arbitrarily long. Only lack of both
    // accepted network audio and successful PCM writes counts as a stall.
    if (proto().GetState() == State::Speaking) {
        const uint32_t progress = xiaozhi::AudioService::GetInstance().PlaybackProgress();
        if (progress != s_playback_progress) {
            s_playback_progress = progress;
            s_speaking_since_ms = t;
        }
        if (s_speaking_since_ms != 0 && t - s_speaking_since_ms > kSpeakingStallTimeoutMs) {
            ESP_LOGW(TAG, "Playback stalled without audio/PCM progress; aborting");
            do_abort();
        }
    }

    // 5. Fallback: cloud audio is already in the ring buffer but the playback task hasn't started (e.g. the server didn't send
    //    tts/start and pushed audio directly). Normally tts start arrives first; this is just a safeguard.
    if (s_session_ready && proto().GetState() == State::Speaking &&
        !xiaozhi::AudioService::GetInstance().IsPlaying() &&
        xiaozhi::AudioService::GetInstance().HasPendingPlayback()) {
        ESP_LOGI(TAG, "Audio queued without playback task; starting playback");
        xiaozhi::AudioService::GetInstance().StartPlayback();
    }
}

void control_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "Control task started (stack=%u, free=%u largest=%u)",
             (unsigned)kControlTaskStack, (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    // The audio subsystem is not initialized here: wait until the OTA temporary task has returned its 12KB stack and the heap is most intact
    // before creating the resident recording task (see do_connect).
    proto().SetEventQueue(s_proto_q);
    s_protocol_ready = true;   // Only after this may the button task access the protocol singleton
    xiaozhi::ui_init();
    xiaozhi::ui_show();

    post_cmd(Cmd::Connect);

    Event pev;
    Cmd cmd;

    while (true) {
        bool did_work = false;

        while (proto().PopCriticalEvent(&pev)) {
            handle_proto_event(pev);
            did_work = true;
        }
        if (proto().GetState() != s_cur_state) {
            on_state_changed(proto().GetState());
            did_work = true;
        }

        while (xQueueReceive(s_proto_q, &pev, 0) == pdTRUE) {
            handle_proto_event(pev);
            did_work = true;
        }

        bool stop_requested = false;
        // A4 fallback: if the Stop command was dropped because the queue was full, the flag is still set, and we exit here.
        if (s_stop_requested) stop_requested = true;
        while (xQueueReceive(s_cmd_q, &cmd, 0) == pdTRUE) {
            if (cmd == Cmd::Stop) {
                stop_requested = true;
                break;
            }
            handle_cmd(cmd);
            did_work = true;
        }

        if (stop_requested) {
            do_stop_app();
            break;
        }

        process_conversation_request();

        // Recording frames are sent here (not by the recording task); see the notes on drain_audio_tx
        if (proto().ProcessMcp()) did_work = true;
        if (drain_audio_tx() > 0) did_work = true;

        tick();

        // Loop again right away when there is work; 20ms ticks when idle (imperceptible for button response)
        if (!did_work) vTaskDelay(pdMS_TO_TICKS(kLoopTickMs));
    }

    s_app_active = false;
    s_ctl_task = nullptr;
    if (s_stopped_sem) xSemaphoreGive(s_stopped_sem);
    ESP_LOGI(TAG, "Control task exiting");
    s_control_running.store(false);
    vTaskDelete(NULL);
}

} // namespace

// ---------------------------------------------------------------------------
// passport_app_t interface
// ---------------------------------------------------------------------------

void xiaozhi_app_init(void) {
    // Heavy init (audio buffers/codec) is done in the control task; see control_task().
    ESP_LOGI(TAG, "Xiaozhi app init (deferred to control task)");
}

void xiaozhi_app_start(void) {
    s_start_failed = false;
    // 2026-09-13 fix (A4): the old logic only checked s_app_active. If the previous stop() failed to reap
    // the control task (Stop dropped / stuck in connecting), this would create another xzg_ctl - two tasks
    // operating on proto() and the audio service at once, inevitably hitting the use-after-free documented in the file header.
    // Now: as long as the old task handle still exists, refuse to start.
    if (s_ctl_task != nullptr) {
        ESP_LOGE(TAG, "Previous control task still alive; refusing to start a second one");
        return;
    }
    if (s_app_active) {
        ESP_LOGW(TAG, "Xiaozhi app already active");
        return;
    }

    // A previous bounded stop may have retained a slow worker to avoid UAF.
    // Give it one more cleanup opportunity before creating a new owner.
    if (s_audio_ready) {
        if (!xiaozhi::AudioService::GetInstance().Deinit()) {
            ESP_LOGE(TAG, "Previous audio worker is still alive; refusing a second owner");
            return;
        }
        s_audio_ready = false;
    }

    if (s_proto_q == nullptr) s_proto_q = xQueueCreateStatic(kProtoQueueDepth, sizeof(Event), s_proto_items, &s_proto_queue_storage);
    if (s_cmd_q == nullptr) s_cmd_q = xQueueCreateStatic(kCmdQueueDepth, sizeof(Cmd), s_cmd_items, &s_cmd_queue_storage);
    if (s_stopped_sem == nullptr) s_stopped_sem = xSemaphoreCreateBinaryStatic(&s_stopped_storage);

    if (s_proto_q == nullptr || s_cmd_q == nullptr || s_stopped_sem == nullptr) {
        ESP_LOGE(TAG, "Failed to create queues/semaphore (free=%u)",
                 (unsigned)esp_get_free_heap_size());
        s_start_failed = true;
        xiaozhi::ui_init();
        xiaozhi::ui_show();
        xiaozhi::ui_set_status_text("Start failed, try again");
        xiaozhi::ui_set_ai_text("Low memory. OK to retry, or Hold OK to go back.");
        return;
    }

    // Clear any leftover events/commands
    xQueueReset(s_proto_q);
    xQueueReset(s_cmd_q);
    while (xSemaphoreTake(s_stopped_sem, 0) == pdTRUE) { }

    s_app_active = true;
    s_stop_requested = false;
    s_conversation_requested = false;
    s_cur_state = State::Disconnected;
    s_listen_after_connect = false;
    clear_timers();
    s_reconnect_at_ms = 0;
    s_next_connect_attempt_ms = 0;
    s_activation_pending = false;
    s_conversation_active = false;
    s_session_ready = false;
    s_audio_recovery_required = false;
    s_retry_reason.clear();
    s_connect_failures = 0;

    ESP_LOGI(TAG, "Starting Xiaozhi app...");
    s_control_running.store(true);
    BaseType_t ok = xTaskCreate(control_task, "xzg_ctl", kControlTaskStack, nullptr, 5, &s_ctl_task);
    if (ok != pdPASS) {
        s_control_running.store(false);
        ESP_LOGE(TAG, "Failed to create control task (free=%u largest=%u)",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        s_app_active = false;
        s_start_failed = true;
        xiaozhi::ui_init();
        xiaozhi::ui_show();
        xiaozhi::ui_set_status_text("Start failed, try again");
        xiaozhi::ui_set_ai_text("Low memory. OK to retry, or Hold OK to go back.");
        return;
    }
}

void xiaozhi_app_request_conversation(void) {
    if (s_app_active && !s_stop_requested) s_conversation_requested = true;
}

bool xiaozhi_app_prepare_stop(void) {
    s_conversation_requested = false;
    if (s_app_active.exchange(false)) post_cmd(Cmd::Stop);
    if (!s_control_running.load() && s_audio_ready) {
        if (xiaozhi::AudioService::GetInstance().Deinit()) s_audio_ready = false;
    }
    // OTA owns its own stack and HTTPS client. It must also finish before a
    // different app allocates its audio/TLS working set.
    return !s_control_running.load() && !s_ota_worker_running.load() && !s_audio_ready;
}

void xiaozhi_app_stop(void) {
    ESP_LOGI(TAG, "Requesting Xiaozhi app stop");
    s_app_active = false;

    if (s_ctl_task == nullptr) {
        // The control task has already exited: still clean up the protocol stack and audio here, so this is idempotent
        do_stop_app();
        xiaozhi::ui_hide();
        return;
    }

    // Ask the control task to wrap up, and wait until it actually ends. Never vTaskDelete a running task
    // (the old code deleted the wifi task that might be stuck in connect_xiaozhi(), the classic
    //  "task deleted while holding the heap lock/in an intermediate state" → heap corruption → random reboots).
    post_cmd(Cmd::Stop);
    if (xSemaphoreTake(s_stopped_sem, pdMS_TO_TICKS(6000)) != pdTRUE) {
        ESP_LOGE(TAG, "Control task did not stop in time; leaving it to self-exit");
    } else {
        xiaozhi::ui_hide();
    }
}

void xiaozhi_app_on_key(uint8_t btn, app_btn_event_t event, uint16_t mv) {
    (void)mv;
    if (s_start_failed && btn == BSP_BTN_OK && event == BTN_EVT_CLICK) {
        xiaozhi_app_start();
        return;
    }
    if (!s_app_active) return;
    // Don't touch the protocol singleton before it is constructed: the button task has only a 3072-byte stack, and constructing Protocol
    // goes through NVS writes (SPI-Flash erase/write), a very deep call chain.
    if (!s_protocol_ready) return;
    if (event != BTN_EVT_CLICK) return;

    if (btn == BSP_BTN_OK) {
        post_cmd(Cmd::ToggleListen);
    } else if (btn == BSP_BTN_UP) {
        post_cmd(Cmd::VolUp);
    } else if (btn == BSP_BTN_DOWN) {
        post_cmd(Cmd::VolDown);
    }
}

const passport_app_t g_xiaozhi_app = {
    .id = APP_ID_XIAOZHI,
    .name = "Xiaozhi AI",
    .en_name = "XIAOZHI",
    .desc = "Hands-free chat with Xiaozhi\nOnline, auto turn-taking",
    .tag = "Direct",
    .theme_color = 0x38BDF8,
    .init = xiaozhi_app_init,
    .start = xiaozhi_app_start,
    .stop = xiaozhi_app_stop,
    .on_key = xiaozhi_app_on_key,
    .prepare_stop = xiaozhi_app_prepare_stop,
};
