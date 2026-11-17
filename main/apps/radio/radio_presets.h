#pragma once
#include "radio_player.h"

// Shared presets: one catalog failure must not leave the radio with one station.
inline constexpr RadioStation kRadioPresets[] = {
#ifdef PASSPORT_RADIO_HLS_TEST
    {"经济之声", "HLS测试 · TS/AAC", "http://ngcdn002.cnr.cn/live/jjzs/index.m3u8", 0},
    {"华语金曲", "HLS测试 · AAC分片", "http://ls.qingting.fm/live/3412131.m3u8?bitrate=64", 0},
    {"凤凰资讯", "HLS测试 · TS/AAC", "http://playtv-live.ifeng.com/live/06OLEEWQKN4_audio.m3u8", 0},
    {"SomaFM", "美国 · MP3测试", "https://ice6.somafm.com/groovesalad-128-mp3", 0},
    {"MANGORADIO", "德国 · MP3测试", "https://mangoradio.stream.laut.fm/mangoradio", 0},
    {"中国之声", "MP3回归测试", "http://lhttp.qingting.fm/live/15318317/64k.mp3", 0},
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
