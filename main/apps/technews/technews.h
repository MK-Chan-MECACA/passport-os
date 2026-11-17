// Tech News for Xiaozhi from Hacker News via the HN Algolia API, fetched by
// the device itself.
//
// The API is HTTPS only. Replies are ~2 KB (title and points only), well
// inside this device's 8 KB TLS receive buffer. Each fetch runs on a
// short-lived task with its own stack, beside the live Xiaozhi session, and
// turns the JSON reply into at most kTextBytes of text.
#pragma once

#include <cstdint>
#include <string>

namespace technews {

enum class Kind : uint8_t { Hot, Latest };
enum class Result : uint8_t { Ok, Empty, NoMemory, Busy, Timeout, Network, Http, RateLimited, Parse };

struct Query {
    Kind kind = Kind::Hot;
    unsigned limit = 0;  // 0: the kind's default
    char text[64] = {};  // Latest: title keywords; empty = "AI"
};

// Blocks the caller for at most timeout_ms; text receives the formatted lines.
Result fetch(const Query &query, std::string *text, uint32_t timeout_ms);

}  // namespace technews
