// components/bsp/include/bsp_audio.h
// ES8311 audio codec: I2C for control (reuses bsp_i2c's shared bus), I2S for full-duplex data.
#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the codec and I2S. Calls bsp_i2c_init() internally (idempotent); no need to call it first.
esp_err_t bsp_audio_init(void);

// RX-only, compact DMA ring for 16 kHz microphone streaming with TLS.
// Deinit before changing profiles; bsp_audio_init retains duplex compatibility.
esp_err_t bsp_audio_init_capture(void);
// TX-only, normal playback DMA ring. No microphone channel is allocated.
esp_err_t bsp_audio_init_playback(void);

// Short system feedback may initialize only an idle codec. It must not reuse
// another owner's device. The registry must await its shutdown before an app starts.
esp_err_t bsp_audio_init_feedback(void);

// Close and unload codec/I2S, returning the DMA and codec heap (for one-off uses such as the boot sound).
// A later bsp_audio_init() can fully rebuild it. Running apps must not call this (it tears down their audio).
void bsp_audio_deinit(void);
// Final power-off step only: close codec and retain its control lock until sleep.
void bsp_audio_prepare_sleep(void);

// Set the sample format. Repeated calls with the same format are cheap (the already-open codec is reused).
//
// ⚠ A pitfall that must be avoided: when the codec is ALREADY OPEN, esp_codec_dev_open() returns OK right away and
//     DOES NOT RECONFIGURE THE SAMPLE RATE. Without closing first, playing 8kHz after 16kHz goes out on the 16k clock -
//     twice the pitch and speed. So this function closes and then opens when the format changes.
esp_err_t bsp_audio_set_format(uint32_t hz, uint8_t bits, uint8_t ch);

// Playback / recording. bytes is a byte count (for 16-bit mono = sample count x 2).
esp_err_t bsp_audio_write(const void *pcm, size_t bytes);
esp_err_t bsp_audio_read(void *pcm, size_t bytes);

// Output volume 0..100 (%).
void bsp_audio_set_volume(uint8_t percent);
// Only valid during bsp_audio_init_feedback(): Pomodoro/reminders use a fixed output volume that is not
// scaled down by master volume (it still sounds at master volume 0); normal scaling resumes after deinit or set_volume.
void bsp_audio_set_alert_volume(uint8_t percent);

// Device-wide master volume 0..100 (%). Every app's requested volume is scaled by it, so an app
// cannot overwrite the user's device-wide setting with its own default at startup.
void bsp_audio_set_master_volume(uint8_t percent);
uint8_t bsp_audio_get_master_volume(void);

#ifdef __cplusplus
}
#endif
