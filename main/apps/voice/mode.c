// main/apps/voice/mode.c - link channel implementation
//
// 2026-09-12: the channel moved from BLE to Wi-Fi UDP (contract in docs/archive/zh/voice-udp-channel.md).
// This file went from a "BLE/USB dual-channel abstraction" down to "single-channel forwarding":
//   - the old mode_channel_up(APP_CHAN_USB) was always false (the old mode.c just returned false,
//     and the repo never had a usb_link implementation) -> the USB branch is removed entirely, it never worked;
//   - all old ble_audio_* calls are replaced with udp_audio_*;
//   - the external signatures in mode.h are unchanged - zero changes to the app_state.c state machine and host tests.
#include "mode.h"
#include "audio_streamer.h"
#include "udp_audio.h"

bool mode_link_up(void)
{
    return udp_audio_event_subscribed();
}

bool mode_channel_up(uint8_t chan)
{
    // Single channel: wireless (APP_CHAN_BLE) is the UDP channel. USB has been removed, always false.
    return chan == APP_CHAN_BLE && udp_audio_event_subscribed();
}

int mode_send_event_line(uint8_t chan, const char *line, size_t len)
{
    if (chan != APP_CHAN_BLE) return -1;
    return udp_audio_notify_event(line, len);
}

int mode_send_event_line_important(uint8_t chan, const char *line, size_t len,
                                   uint32_t timeout_ms)
{
    if (chan != APP_CHAN_BLE) return -1;
    return udp_audio_notify_event_blocking(line, len, timeout_ms);
}

const char *mode_audio_format(uint8_t chan)
{
    (void)chan;
    // Over UDP it is still one independent IMA ADPCM block per frame (4:1, 8KB/s): each block carries its own
    // predictor/index header, so losing a block costs only 100ms and never loses sync - a natural fit for UDP's loss model,
    // and better than the intuition "high bandwidth means no compression" (see docs/archive/zh/voice-udp-channel.md §6).
    return "ima_adpcm";
}

void mode_select_audio_sender(uint8_t chan)
{
    (void)chan;
    audio_streamer_set_sender(udp_audio_notify_audio);
    audio_streamer_set_compressed(true);
}

bool mode_wired(void)
{
    return false;   // USB channel has been removed
}
