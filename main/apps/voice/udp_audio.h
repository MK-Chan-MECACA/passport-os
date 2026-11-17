// main/apps/voice/udp_audio.h - Wi-Fi UDP audio/event channel (replaces BLE).
//
// Contract in docs/archive/zh/voice-udp-channel.md - both the device and companion follow it; change one side
// and you must change the other. Key points:
//
//   port UDP 33333 (same port both ways)
//   companion = server: bind 33333, broadcasts a beacon to 255.255.255.255:33333 every 1000ms
//   device    = client: bind 33333 (receives beacons), unicasts to <pc_ip>:33333 once it learns the PC's IP
//
//   packet = [1B type][payload]
//     0x01 AUDIO  dev→PC   [seq][0x80] + 804B IMA ADPCM block  = 807B
//     0x02 EVENT  both     UTF-8 JSON line (with trailing '\n')  ≤ 513B
//     0x03 BEACON PC→dev   {"v":1,"role":"pc","port":33333}     ~40B
//     0x04 PING   dev→PC   empty payload, sent every 2s, liveness only = 1B
//
// Why PING exists: when the device is idle (not recording, no events) it sends nothing, so companion
// could not tell "device still in the voice app" from "device left the voice app". One every 2s lets
// companion's link liveness check (6s of silence = down) work properly.
//
// AUDIO keeps the BLE-era 2B fragment header ([block seq][fragment seq|0x80 last]): over UDP it is always
// "single fragment + last" -> [seq][0x80]. It is kept so companion's reassemble_adpcm()
// needs no changes at all; seq is ++ mod 256 per block, and duplicate/late blocks are dropped by companion's existing last_seq logic.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VOICE_UDP_PORT      33333
#define VOICE_UDP_T_AUDIO   0x01
#define VOICE_UDP_T_EVENT   0x02
#define VOICE_UDP_T_BEACON  0x03
#define VOICE_UDP_T_PING    0x04   // device->PC liveness (companion does not dispatch it, only refreshes the link timestamp)
#define VOICE_UDP_PING_MS   2000u  // liveness period (companion side: 6s of silence = down)

// How long the peer must be silent before the link counts as down (3:1 redundancy over the 1000ms beacon period)
#define VOICE_UDP_PEER_TIMEOUT_MS 3000u

// ---- Lifecycle ----
// Create the UDP socket, bind 33333 and start the RX task. Idempotent: returns 0 if already initialized.
// Returns 0 = success; non-zero = socket/bind/task creation failed.
int udp_audio_init(void);
// Stop the RX task and close the socket (set the stop flag and wait for the task to exit before closing, to avoid a recvfrom race).
// Idempotent: no side effects if not initialized.
int udp_audio_deinit(void);

// ---- Uplink (maps one-to-one onto the old ble_audio_* semantics) ----
// Audio block: frame = the 804B IMA ADPCM block produced by audio_streamer (not 3200B PCM).
// Builds [0x01][seq][0x80][block] and unicasts it. Returns non-zero if the peer is unknown / send failed (caller counts a drop).
int udp_audio_notify_audio(const uint8_t *frame, size_t len);
// Event line (≤512B, including the trailing '\n'): builds [0x02][line] and unicasts it. Non-blocking.
int udp_audio_notify_event(const char *line, size_t len);
// UDP has no send queue or backpressure, so this is equivalent to udp_audio_notify_event - the signature is kept only so
// the session boundary frame (voice.start/end) call sites in app_voice.cc need no changes.
int udp_audio_notify_event_blocking(const char *line, size_t len, uint32_t timeout_ms);

// ---- Status queries ----
bool udp_audio_connected(void);       // peer known and not timed out
bool udp_audio_event_subscribed(void); // basis for link_up (same as connected; UDP has no notion of subscription)
uint32_t udp_audio_audio_drops(void);  // cumulative audio frame drops (peer unknown / send failed)
uint32_t udp_audio_event_drops(void);  // cumulative event line drops
// Dotted-decimal IP string of the learned peer; returns "0.0.0.0" if none learned yet (for diagnostics/console)
const char *udp_audio_peer_ip(void);

#ifdef __cplusplus
}
#endif
