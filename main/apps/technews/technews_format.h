// HN Algolia search replies -> short English text for Xiaozhi.
//
// Replies are ~2 KB JSON (only title and points are requested); the assistant
// needs a few titles to read aloud. The collector runs on the streaming
// scanner and writes straight into a bounded buffer, so memory stays fixed.
// Pure code: the host tests feed real replies.
#pragma once

#include "json_scanner.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace technews {

using json::JsonScanner;

constexpr size_t kTextBytes = 2048;  // whole tool reply text
constexpr size_t kTitleBytes = 150;
constexpr unsigned kMaxHot = 10, kMaxLatest = 8;

// Length of the longest prefix of text within limit bytes that does not split
// a UTF-8 character.
inline size_t utf8_fit(const char *text, size_t limit) {
    size_t n = std::strlen(text);
    if (n <= limit) return n;
    n = limit;
    while (n && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80) --n;
    return n;
}

// Copies text, trimmed at a character boundary with an ellipsis when cut.
inline void clip(char *out, size_t capacity, const char *text) {
    static constexpr char kEllipsis[] = "…";
    const size_t whole = std::strlen(text);
    size_t n = utf8_fit(text, capacity - 1);
    const bool cut = n < whole;
    if (cut) n = utf8_fit(text, capacity - sizeof(kEllipsis));
    std::memcpy(out, text, n);
    if (cut) std::memcpy(out + n, kEllipsis, sizeof(kEllipsis) - 1), n += sizeof(kEllipsis) - 1;
    out[n] = 0;
}

// Appends whole lines only: a line that does not fit ends the reply, so the
// assistant never reads half an entry.
struct Writer {
    char *text;
    size_t capacity;
    size_t length = 0;
    bool full = false;
    unsigned entries = 0;

    bool line(const char *value) {
        const size_t n = std::strlen(value);
        if (full || length + n + 2 > capacity) { full = true; return false; }
        std::memcpy(text + length, value, n);
        length += n;
        text[length++] = '\n';
        text[length] = 0;
        return true;
    }
};

inline bool is(const char *a, const char *b) { return !std::strcmp(a, b); }

// A string longer than the scanner buffer arrives as ValuePart chunks and a
// final Value with its tail. Only the leading chunk is wanted: the clip
// already marks the cut, and later chunks must not replace or repeat it.
struct LeadingChunk {
    bool tail = false;
    bool take(JsonScanner::Event event) {
        if (tail) {
            if (event == JsonScanner::Event::Value) tail = false;
            return false;
        }
        if (event == JsonScanner::Event::ValuePart) tail = true;
        return true;
    }
};

// GET /api/v1/search and /api/v1/search_by_date: hits[].title, hits[].points.
struct HitCollector {
    Writer &out;
    unsigned limit;
    char title[kTitleBytes] = {};
    unsigned points = 0;
    LeadingChunk chunk{};

    static void sink(void *opaque, JsonScanner::Event event, const char *path, const char *value, size_t) {
        auto &c = *static_cast<HitCollector *>(opaque);
        if (event == JsonScanner::Event::ObjectEnd) {
            if (!is(path, "hits[]")) return;
            if (c.title[0] && c.out.entries < c.limit) {
                char line[kTitleBytes + 32];
                std::snprintf(line, sizeof(line), "%u. %s (%u point%s)", c.out.entries + 1, c.title, c.points,
                              c.points == 1 ? "" : "s");
                if (c.out.line(line)) ++c.out.entries;
            }
            c.title[0] = 0;
            c.points = 0;
            return;
        }
        if (!c.chunk.take(event)) return;
        if (is(path, "hits[].title")) clip(c.title, sizeof(c.title), value);
        else if (is(path, "hits[].points")) c.points = static_cast<unsigned>(std::strtoul(value, nullptr, 10));
    }
};

}  // namespace technews
