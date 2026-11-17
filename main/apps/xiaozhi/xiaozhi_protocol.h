#pragma once

// Xiaozhi WebSocket protocol v3 implementation (matches the official xiaozhi-esp32 main/protocols/websocket_protocol.cc)
//
// Threading model (important):
//   * All callbacks of this class fire synchronously in the esp_websocket_client task context.
//   * So callback implementations must "only enqueue, never block", and must never call Stop().
//   * Outbound sends (SendAudio / SendJson) may be called from the recording task; esp_websocket_client
//     uses a reentrant lock internally with a finite timeout (see kSendTimeoutMs), so it cannot deadlock.
//
// Differences from the official implementation (deliberate robustness improvements):
//   * Fragment reassembly: the official code leaves reassembly entirely to the websocket component; here we explicitly
//     reassemble by payload_offset/payload_len, including op_code=0x00 continuation frames, so JSON is not parsed piece by piece
//     when buffer_size is smaller than the message.
//   * Unknown/alert messages are passed to a callback instead of being silently dropped - the screen can tell the user what actually happened.

#include <cstdint>
#include <cstddef>
#include <string>
#include <functional>
#include <atomic>
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

namespace xiaozhi {

enum class State {
    Disconnected,
    Connecting,
    Idle,           // Connected to the Xiaozhi backend, waiting for the user to start a conversation
    Listening,      // User is talking, recording and uploading Opus in real time
    Thinking,       // User stopped recording, cloud STT & LLM are thinking
    Speaking,       // Cloud TTS is streaming down and playing
    Error,
};

// Events posted by the websocket task to the control task.
enum class EvType : uint8_t {
    Connected,
    Disconnected,
    SocketError,
    HelloOk,        // Server hello received, session usable
    SessionInfo,    // session_id changed
    StateChanged,   // Internal protocol state changed (Idle/Listening/Thinking/Speaking...)
    UserText,       // stt (replace semantics)
    AiSentence,     // tts/sentence_start (replace semantics)
    LlmEmotion,     // llm.emotion
    Alert,          // Server alert / unknown message / activation hint
    HandshakeTimeout,
};

constexpr size_t kEventTextCap = 256;
// Text reassembly is session-owned so its 2 KB is returned when Xiaozhi exits.
// Binary packets remain bounded; normal Opus frames are about 300 bytes.
constexpr size_t kMaxTextMessage = 2048;  // MCP initialization + bounded tool arguments
constexpr size_t kMaxBinaryMessage = 512;  // Reassembly limit for a single binary (Opus) frame

struct Event {
    EvType type;
    State state;                 // Valid for StateChanged
    char   text[kEventTextCap];  // NUL-terminated, safely truncated if too long
};

using EventQueue = QueueHandle_t;

class Protocol {
public:
    Protocol();
    ~Protocol();

    // url / token / version are supplied by the upper layer (NVS or OTA).
    bool Start(const std::string &url, const std::string &token, int version);

    // Must be called from the control task; must not be called from a callback (the component rejects it and returns failure).
    void Stop();

    bool IsConnected() const;
    // Whether the websocket client object currently exists. Note it may "exist but not be connected":
    // esp_websocket_client reconnects on its own according to reconnect_timeout_ms,
    // so the upper layer should not keep rebuilding the client during that time.
    bool HasClient() const { return client_ != nullptr; }
    // A clean close/finished worker cannot auto-reconnect even with a live handle.
    bool NeedsRestart() const { return needs_restart_.load(); }
    State GetState() const { return state_.load(); }
    const std::string &session_id() const { return session_id_; }

    void SetEventQueue(EventQueue q) { event_q_ = q; }
    void SetState(State new_state);
    // Connection/state transitions must not compete with display text for the
    // bounded queue. Connection events use a sticky mailbox; state is polled.
    bool PopCriticalEvent(Event *out);

    // Interaction operations (called by the control task)
    bool TriggerStartListen();
    bool TriggerStopListen();
    bool TriggerAbort();
    // Process at most one queued tool request on the control task. NVS work
    // must never run inside a WebSocket callback.
    bool ProcessMcp();

    // Send a compressed Opus audio frame (called by the recording task, bounded timeout internally)
    bool SendAudio(const uint8_t *opus_data, size_t len);

    // Whether the handshake completed before the timeout
    bool HandshakeDone() const { return hello_ok_.load(); }

    // Value of audio_params.sample_rate in the server hello (official server measured at 24000).
    // Playback/decoding must use it, otherwise TTS pitch and speed are wrong.
    int ServerSampleRate() const { return server_sample_rate_; }

    static const char *StateName(State s);

private:
    static void WebsocketEventHandler(void *handler_args, esp_event_base_t base,
                                      int32_t event_id, void *event_data);

    void OnConnected();
    void OnDisconnected();
    void OnSessionEnded();
    void HandleData(const esp_websocket_event_data_t *data);
    void DispatchText();                                  // rx_text_ fully received
    void Publish(EvType type, const char *text, State st = State::Disconnected);
    bool SendJson(const std::string &json);
    void LoadIdentity();
    void ResetRx();
    struct McpRequest { int32_t id; char method[24]; char params[512]; };
    QueueHandle_t mcp_queue_{nullptr};
    // Requests refused before queueing still need an error reply. A burst
    // against a full queue must not overwrite an earlier refused id.
    static constexpr size_t kRejectedSlots = 4;
    std::atomic<int32_t> rejected_mcp_ids_[kRejectedSlots]{{-1}, {-1}, {-1}, {-1}};
    void RejectMcp(int32_t id);
    struct McpReply { int32_t id=-1; uint32_t hash=0; std::string response; };
    McpReply mcp_replies_[4];
    unsigned mcp_reply_cursor_=0;

    esp_websocket_client_handle_t client_{nullptr};
    std::atomic<State> state_{State::Disconnected};
    std::atomic<uint32_t> critical_events_{0};
    EventQueue event_q_{nullptr};

    std::string mac_address_;
    std::string client_id_;
    std::string session_id_;
    std::string url_;
    int version_{3};
    std::atomic<bool> hello_ok_{false};
    std::atomic<bool> needs_restart_{false};
    int server_sample_rate_{16000};

    // Text message reassembly buffer (accessed only by the websocket task)
    char  *rx_text_{nullptr}; // allocated for a session, released after socket join
    size_t rx_text_len_{0};
    size_t rx_text_expect_{0};
    uint8_t rx_opcode_{0};

    // Binary (Opus) message reassembly buffer (accessed only by the websocket task)
    uint8_t rx_bin_[kMaxBinaryMessage];
    size_t  bin_len_{0};
    uint8_t bin_opcode_{0};
};

} // namespace xiaozhi
