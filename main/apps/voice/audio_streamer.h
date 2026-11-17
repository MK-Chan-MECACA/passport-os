// main/audio_streamer.h - streaming audio pipeline.
// Session-owned ring buffer (NOSPLIT, two slots) + two workers: audio capture -> ring -> channel send.
// init allocates the buffer and worker stacks once, nothing is allocated per recording; deinit frees them after the tasks have stopped.
// On congestion the source drops frames and reports AUDIO_DROP_* events; the drop count feeds the status frame reconciliation after voice.end.
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Audio frame send function (channel-agnostic): BLE -> ble_audio_notify_audio; USB -> usb_link_send_audio.
// Payload: in compressed mode (ble) an 804B IMA ADPCM block (4B header + 800B data); uncompressed
// (usb) a raw 3200B PCM chunk. The 4B session token on ring items is ring-internal metadata
// and never goes on the link. The App parses according to the audio field of voice.start.
// Returns 0 = sent; non-zero = send failed (drop-count path). When not registered (NULL) it always counts as failed.
typedef int (*audio_send_fn_t)(const uint8_t *data, size_t len);

// Register the send function. Call sites: main.c at boot and after a mode switch, for the current channel; before a switch the link is down and
// the pipeline stopped, so the pointer is never changed concurrently (design in mode.c).
void audio_streamer_set_sender(audio_send_fn_t fn);
// Compression switch (mode-driven, BLE only): true -> each 3200B PCM chunk is encoded into one 804B ADPCM
// block and sent (4:1, 8KB/s); false -> sent as is (USB has bandwidth to spare, no compression).
void audio_streamer_set_compressed(bool on);

esp_err_t audio_streamer_init(void);   // create the ring buffer and two workers (waiting idle)
bool audio_streamer_deinit(void);      // wait for both workers to stop, then return all session memory
// Start capture and sending (clears the session drop count). Each session is identified by an increasing token; ring item headers carry
// the snapshotted token; leftovers from an old session (in ring / in flight / late chunks) are invalidated by token in ble_worker and silently
// dropped - start never refuses because of leftovers and can reopen immediately without draining.
// Returns: ESP_OK on success; ESP_ERR_INVALID_STATE if the pipeline is not ready. The caller must check the return value:
// on failure recording did not start and the UI must not show recording (REVIEW P2-C: back to READY + toast).
esp_err_t audio_streamer_start(void);
void audio_streamer_stop(void);        // stop capture (the send worker keeps draining; drain before voice.end)
// Cancel/disconnect: invalidate the session token + stop capture; leftover/in-flight/late frames in the ring are dropped silently by
// ble_worker on token mismatch - returns fast (does not wait for the ring to empty, no drain gate).
// Idempotent: also valid when capture has already stopped (disconnect after STOP) - the leftover token is already invalidated.
void audio_streamer_cancel(void);
// Wait for leftover ring data to be sent (up to ms). Call before voice.end to guarantee frame order.
void audio_streamer_drain(uint32_t ms);
bool audio_streamer_active(void);
void audio_streamer_release_audio(void);
uint16_t audio_streamer_peak(void);    // peak sample of the latest chunk (for the UI level meter)
// Read and reset this session's (since the last start) cumulative drop count. main.c takes it after the drain in SEND_VOICE_END
// (capture stopped + ring drained, so the count is stable) and reports it to the Mac in the status frame after voice.end (dropped-frame reconciliation).
uint32_t audio_streamer_take_drops(void);

#ifdef __cplusplus
}
#endif
