#pragma once

// Xiaozhi backend OTA / device activation client.
//
// Why this layer is required: the official backend (api.tenclass.net) does not hand out usable
// WebSocket credentials for free. The device must first call the OTA endpoint with Device-Id/Client-Id, and only then does the server return
//
//   * websocket{url, token, version}  - the actual conversation address and token
//   * activation{code, message}       - the device is not bound yet; the user must bind this MAC in the backend
//
// The official firmware (xiaozhi-esp32 ota.cc / application.cc) does exactly this. The previous code directly
// hardcoded wss://api.tenclass.net/xiaozhi/v1/ + "test-token" and skipped the whole activation flow,
// which is the root cause of "connects but the conversation gives no result" on the official backend.
//
// Self-hosted servers (xiaozhi-esp32-server) usually provide the same OTA path, so this flow
// works for self-hosted backends too.

#include <string>

namespace xiaozhi {

enum class OtaResult {
    Ok,               // Got the websocket config
    NeedsActivation,  // Server requires binding the device first (with activation code)
    NoConfig,         // Response OK but no websocket section (continue with local NVS / defaults)
    NetworkError,     // Request failed (no network, TLS, timeout)
    ParseError,       // Response could not be parsed
};

struct OtaOutcome {
    OtaResult result{OtaResult::NetworkError};
    std::string url;                  // websocket url
    std::string token;                // websocket token
    int version{0};                   // Protocol version (0 = not specified by server)
    std::string activation_code;      // Activation code (for NeedsActivation)
    std::string activation_message;   // Hint text from the server
    std::string detail;               // Diagnostics / HTTP status code
};

// Exchange /xiaozhi/ota/ for config. On success also writes url/token/version to the NVS namespace
// "websocket" (same layout as the official firmware) and exposes them via the "xiaozhi" namespace of xiaozhi_store.
//
// Must be called from the control task (makes one HTTPS request with TLS internally, takes a few hundred ms).
OtaOutcome ota_fetch_config();

// HTTP status code returned by the server (for diagnostics; 0 means no response was received)
int ota_last_http_status();

} // namespace xiaozhi
