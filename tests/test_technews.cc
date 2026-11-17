// Tech News reply formatting on real HN Algolia responses (tests/fixtures/technews).
#include "apps/technews/technews_format.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <string>

using namespace technews;

namespace {

std::string read(const char *name) {
    std::string path = std::string("tests/fixtures/technews/") + name, body;
    FILE *f = std::fopen(path.c_str(), "rb");
    assert(f);
    char chunk[333];
    for (size_t got; (got = std::fread(chunk, 1, sizeof(chunk), f)) > 0;) body.append(chunk, got);
    std::fclose(f);
    return body;
}

std::string run(const char *fixture, HitCollector &collector, size_t step = 97) {
    JsonScanner scanner(HitCollector::sink, &collector);
    const std::string body = read(fixture);
    for (size_t i = 0; i < body.size(); i += step) assert(scanner.feed(body.data() + i, std::min(step, body.size() - i)));
    assert(scanner.complete());
    return collector.out.text;
}

}  // namespace

int main() {
    char text[kTextBytes];
    char c[10];
    clip(c, sizeof(c), "Nvidia GPUs"); assert(!std::strcmp(c, "Nvidia…"));  // 9 bytes: 6 chars + ellipsis
    clip(c, sizeof(c), "abc"); assert(!std::strcmp(c, "abc"));

    {
        Writer out{text, sizeof(text)};
        HitCollector hits{out, kMaxHot};
        const std::string s = run("front_page.json", hits);
        assert(out.entries == 10 && s.size() < kTextBytes);
        assert(s.rfind("1. Carnegie Mellon is putting computer science courses on YouTube for free (1719 points)\n", 0) == 0);
        // – and \" escapes decode to UTF-8 and a plain quote.
        assert(s.find("\n5. Frame – 31.5\" color ePaper display (455 points)\n") != std::string::npos);
        assert(s.find("\n10. OpenCode Browse (294 points)\n") != std::string::npos);
    }
    {
        Writer out{text, sizeof(text)};
        HitCollector hits{out, 3};
        const std::string s = run("front_page.json", hits, 7);  // tiny chunks: values split across feeds
        assert(out.entries == 3 && s.find("\n3. ") != std::string::npos && s.find("\n4. ") == std::string::npos);
    }
    {
        Writer out{text, sizeof(text)};
        HitCollector hits{out, kMaxLatest};
        const std::string s = run("latest_ai.json", hits);
        assert(out.entries == 8);
        assert(s.rfind("1. Ask HN: If AI tools are so good, why isn't your bug backlog empty? (68 points)\n", 0) == 0);
        assert(s.find("All AI-generated code should be Open Source (1273 points)") != std::string::npos);
    }
    {
        // A small reply buffer keeps whole lines only.
        char small[200];
        Writer out{small, sizeof(small)};
        HitCollector hits{out, kMaxHot};
        const std::string s = run("front_page.json", hits);
        assert(out.full && out.entries == 2 && s.size() < sizeof(small) && s.back() == '\n');
    }
    std::puts("Tech News: front page, latest search, escapes, split feeds, whole-line budget PASS");
}
