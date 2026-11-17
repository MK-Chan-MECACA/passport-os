#include "apps/technews/technews.h"

#include "apps/technews/technews_format.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <atomic>
#include <cstdio>
#include <new>

namespace {
// RFC 3986 unreserved characters pass through; everything else is %XX.
bool url_encode(const char *in, char *out, size_t capacity) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    size_t n = 0;
    for (; *in; ++in) {
        const unsigned char c = static_cast<unsigned char>(*in);
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                           c == '-' || c == '_' || c == '.' || c == '~';
        if (n + (plain ? 1 : 3) >= capacity) return false;
        if (plain) out[n++] = static_cast<char>(c);
        else { out[n++] = '%'; out[n++] = kHex[c >> 4]; out[n++] = kHex[c & 15]; }
    }
    if (!capacity) return false;
    out[n] = 0;
    return true;
}
}  // namespace

namespace technews {
namespace {

constexpr char kTag[] = "technews";
// TLS handshake (certificate verify) plus the scanner and collector frames.
constexpr uint32_t kTaskStack = 7168;
// The Xiaozhi session keeps ~72 KiB free (largest ~61 KiB, measured): a second
// TLS session needs its 8 KiB record buffer contiguous plus handshake scratch.
constexpr size_t kNeedFree = 40 * 1024;
constexpr size_t kNeedLargest = 16 * 1024;
constexpr int64_t kRequestDeadlineUs = 9 * 1000 * 1000;
// Only the fields the reply uses, no highlight markup: keeps replies ~2 KB.
constexpr char kFields[] = "attributesToRetrieve=title,points&attributesToHighlight=none";

std::atomic<bool> s_busy{false};
std::atomic<bool> s_cancel{false};

// Shared by caller and task; whichever finishes second frees it.
struct Job {
    Query query;
    char text[kTextBytes] = {};
    Result result = Result::Network;
    std::atomic<bool> done{false};
    std::atomic<bool> released{false};
};

void release(Job *job) {
    if (job->released.exchange(true, std::memory_order_acq_rel)) delete job;
}

unsigned limit_of(const Query &q) { return q.limit ? q.limit : q.kind == Kind::Hot ? kMaxHot : 5; }

bool url(const Query &q, char *out, size_t capacity) {
    int n = 0;
    if (q.kind == Kind::Hot) {
        n = std::snprintf(out, capacity, "https://hn.algolia.com/api/v1/search?tags=front_page&hitsPerPage=%u&%s",
                          limit_of(q), kFields);
    } else {
        // Newest stories with some traction whose title has the keywords as
        // whole words: without the title restriction and prefixNone, "AI"
        // would also match "Airlines".
        char keywords[3 * 63 + 1];
        if (!url_encode(q.text[0] ? q.text : "AI", keywords, sizeof(keywords))) return false;
        n = std::snprintf(out, capacity,
                          "https://hn.algolia.com/api/v1/search_by_date?query=%s&tags=story&numericFilters=points%%3E50"
                          "&restrictSearchableAttributes=title&queryType=prefixNone&typoTolerance=false&hitsPerPage=%u&%s",
                          keywords, limit_of(q), kFields);
    }
    return n > 0 && static_cast<size_t>(n) < capacity;
}

Result get(Job &job) {
    const Query &q = job.query;
    char address[512];
    if (!url(q, address, sizeof(address))) return Result::Parse;
    Writer out{job.text, sizeof(job.text)};
    HitCollector hits{out, limit_of(q)};
    JsonScanner scanner(HitCollector::sink, &hits);
    esp_http_client_config_t cfg{};
    cfg.url = address;
    cfg.timeout_ms = 6000;
    cfg.buffer_size = 1024;
    cfg.buffer_size_tx = 768;  // the request line (up to 512 bytes) must fit whole
    cfg.user_agent = "PassportOS-MY";
    cfg.disable_auto_redirect = true;
    cfg.keep_alive_enable = false;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return Result::NoMemory;
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    Result result = Result::Network;
    const int64_t deadline = esp_timer_get_time() + kRequestDeadlineUs;
    do {
        if (esp_http_client_open(client, 0) != ESP_OK || esp_http_client_fetch_headers(client) < 0) break;
        const int status = esp_http_client_get_status_code(client);
        if (status != 200) {
            ESP_LOGW(kTag, "HTTP %d", status);
            result = status == 429 ? Result::RateLimited : Result::Http;
            break;
        }
        char chunk[512];
        for (;;) {
            if (s_cancel.load(std::memory_order_acquire) || esp_timer_get_time() > deadline) { result = Result::Timeout; break; }
            const int n = esp_http_client_read(client, chunk, sizeof(chunk));
            if (n < 0) break;
            if (n == 0) {
                result = esp_http_client_is_complete_data_received(client) && scanner.complete() ? Result::Ok
                         : scanner.failed() ? Result::Parse : Result::Network;
                break;
            }
            if (!scanner.feed(chunk, static_cast<size_t>(n))) { result = Result::Parse; break; }
        }
    } while (false);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (result == Result::Ok && !out.entries) result = Result::Empty;
    return result;
}

void task(void *arg) {
    auto *job = static_cast<Job *>(arg);
    job->result = get(*job);
    job->done.store(true, std::memory_order_release);
    release(job);
    s_busy.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
}

}  // namespace

Result fetch(const Query &query, std::string *text, uint32_t timeout_ms) {
    text->clear();
    if (esp_get_free_heap_size() < kNeedFree || heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < kNeedLargest) {
        ESP_LOGW(kTag, "Skip fetch: free=%u largest=%u", (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        return Result::NoMemory;
    }
    bool idle = false;
    if (!s_busy.compare_exchange_strong(idle, true)) return Result::Busy;
    auto *job = new (std::nothrow) Job;
    if (!job) { s_busy.store(false); return Result::NoMemory; }
    job->query = query;
    s_cancel.store(false, std::memory_order_release);
    if (xTaskCreate(task, "technews", kTaskStack, job, 4, nullptr) != pdPASS) {
        delete job;
        s_busy.store(false);
        return Result::NoMemory;
    }
    const int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000;
    while (!job->done.load(std::memory_order_acquire) && esp_timer_get_time() < deadline) vTaskDelay(pdMS_TO_TICKS(20));
    Result result = Result::Timeout;
    if (job->done.load(std::memory_order_acquire)) {
        result = job->result;
        if (result == Result::Ok) text->assign(job->text);
    } else {
        s_cancel.store(true, std::memory_order_release);  // the task ends its request and frees the job
        ESP_LOGW(kTag, "Fetch timed out after %u ms", (unsigned)timeout_ms);
    }
    release(job);
    return result;
}

}  // namespace technews
