#pragma once
#include "radio_player.h"

// Shared presets: one catalog failure must not leave the radio with one station.
inline constexpr RadioStation kRadioPresets[] = {
#ifdef PASSPORT_RADIO_HLS_TEST
    {"Voice of Economy", "HLS test · TS/AAC", "http://ngcdn002.cnr.cn/live/jjzs/index.m3u8", 0},
    {"Chinese Hits", "HLS test · AAC segments", "http://ls.qingting.fm/live/3412131.m3u8?bitrate=64", 0},
    {"Phoenix News", "HLS test · TS/AAC", "http://playtv-live.ifeng.com/live/06OLEEWQKN4_audio.m3u8", 0},
    {"SomaFM", "US · MP3 test", "https://ice6.somafm.com/groovesalad-128-mp3", 0},
    {"MANGORADIO", "Germany · MP3 test", "https://mangoradio.stream.laut.fm/mangoradio", 0},
    {"Voice of China", "MP3 regression test", "http://lhttp.qingting.fm/live/15318317/64k.mp3", 0},
#else
    // MP3 (96 kbps) only: AAC needs ~23 KiB of decoder heap, and presets are the
    // fallback when everything else failed. Frequencies differ by region, so 0.
    {"Traxx FM", "Featured · English music", "http://playerservices.streamtheworld.com/api/livestream-redirect/TRAXX_FM.mp3", 0},
    {"BFM 89.9", "Featured · Business news", "http://playerservices.streamtheworld.com/api/livestream-redirect/BFM.mp3", 0},
    {"Nasional FM", "Featured · Malay", "http://playerservices.streamtheworld.com/api/livestream-redirect/NASIONAL_FM.mp3", 0},
    {"Klasik FM", "Featured · Malay classics", "http://playerservices.streamtheworld.com/api/livestream-redirect/RADIO_KLASIK.mp3", 0},
    {"ai FM", "Featured · Chinese", "http://playerservices.streamtheworld.com/api/livestream-redirect/AI_FM.mp3", 0},
    {"988 FM", "Featured · Chinese", "http://playerservices.streamtheworld.com/api/livestream-redirect/988_FM.mp3", 0},
#endif
};
