#pragma once

#include "radio_player.h"

#include <cstddef>

struct RadioLocation {
    char country_code[4];
    char region[48];
    char city[48];
    char search_city[48];
};

using RadioCatalogCancelled = bool (*)();

// Locates the radio from its public network address and loads playable MP3
// and AAC stations for the detected city, topped up from the surrounding state
// and then the whole country.
std::size_t radio_catalog_discover(RadioStation *stations, std::size_t capacity,
                                   RadioLocation *location,
                                   RadioCatalogCancelled cancelled = nullptr);

// True for a Malaysian place or state name in the offline catalog (case and
// punctuation ignored); used to validate voice requests.
bool radio_catalog_known_place(const char *name);

// Loads the most popular playable stations for a user-selected Malaysian city.
// This skips IP geolocation and is used by the on-device city selector.
std::size_t radio_catalog_discover_city(const char *city, RadioStation *stations,
                                        std::size_t capacity,
                                        RadioLocation *location);

// Preferred profile location skips IP geolocation; empty city uses IP instead.
// Sparse local results are topped up with state, country and curated presets.
// Voice requests may pass station keywords (including "" for any local station):
// stop directory lookups once that station is found, then add presets locally.
std::size_t radio_catalog_discover_location(const RadioLocation &preferred,
                                           RadioStation *stations, std::size_t capacity,
                                           RadioLocation *location,
                                           RadioCatalogCancelled cancelled = nullptr,
                                           const char *voice_station = nullptr);
