// main/apps/voice/udp_audio.c - Wi-Fi UDP audio/event channel implementation.
// Protocol contract in docs/archive/zh/voice-udp-channel.md (same structure as companion/udp_transport.py).
#include "udp_audio.h"

#include "app_protocol.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "udp_audio";

// An audio block is always 804B (4B block header + 800B of 4-bit data), see apps/voice/adpcm.h
#define ADPCM_BLOCK_MAX 804
// Uplink buffer: [type 1B][payload]; maximum payload = 2B frame header + 804B block
#define TX_BUF_MAX (1 + 2 + ADPCM_BLOCK_MAX)
// Downlink receive buffer: maximum payload = 513B (EVENT), with headroom to detect oversized packets
#define RX_BUF_MAX 640
// Threshold for deciding the peer "went away and came back". Note: the *1000 here is a ms->us conversion, so the result is
// the link timeout itself (3000ms = 3e6us), not the "3×" the old comment claimed. Semantically that is exactly right:
// with beacons normal, one arrives every second (~1s gap < 3s, does not trigger); a gap of 3s or more means the link was down,
// so the next beacon resends device.hello and companion learns about us again.
#define PEER_STALE_US ((int64_t)VOICE_UDP_PEER_TIMEOUT_MS * 1000)
#define MY_IP_STR(buf) ((buf)[0] ? (buf) : "0.0.0.0")

static int s_sock = -1;
static volatile bool s_active = false;
static TaskHandle_t s_rx_task = NULL;
static SemaphoreHandle_t s_tx_mux = NULL;
static StaticSemaphore_t s_tx_mux_storage;

// Learned peer address (written only by the RX task on receiving a beacon; protected by s_peer_mux)
static struct sockaddr_in s_peer;
static volatile bool s_peer_known = false;
static volatile int64_t s_peer_last_us = 0;
static portMUX_TYPE s_peer_mux = portMUX_INITIALIZER_UNLOCKED;

// Uplink buffer: allocated in init, freed in deinit, only accessed while holding s_tx_mux
static uint8_t *s_tx_buf = NULL;
static uint8_t s_audio_seq = 0;

static portMUX_TYPE s_stat_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_audio_drops = 0;
static uint32_t s_event_drops = 0;
static volatile uint32_t s_beacon_cnt = 0;   // link diagnostics: total beacons received
static volatile uint32_t s_ping_cnt = 0;     // link diagnostics: total pings sent

// ---- Internal helpers ----

static void drop_inc(uint32_t *counter)
{
    portENTER_CRITICAL(&s_stat_mux);
    (*counter)++;
    portEXIT_CRITICAL(&s_stat_mux);
}

static uint32_t drop_get(uint32_t *counter)
{
    portENTER_CRITICAL(&s_stat_mux);
    uint32_t v = *counter;
    portEXIT_CRITICAL(&s_stat_mux);
    return v;
}

// Snapshot the peer address. Returns false = peer not learned yet (link not up).
static bool peer_snapshot(struct sockaddr_in *out)
{
    bool known;
    portENTER_CRITICAL(&s_peer_mux);
    known = s_peer_known;
    if (known) *out = s_peer;
    portEXIT_CRITICAL(&s_peer_mux);
    return known;
}

// Whether the link is fresh: peer known and a packet from it received within VOICE_UDP_PEER_TIMEOUT_MS.
static bool peer_fresh(void)
{
    int64_t now = esp_timer_get_time();
    bool fresh = false;
    portENTER_CRITICAL(&s_peer_mux);
    fresh = s_peer_known && s_peer_last_us != 0 &&
            (now - s_peer_last_us) < (int64_t)VOICE_UDP_PEER_TIMEOUT_MS * 1000;
    portEXIT_CRITICAL(&s_peer_mux);
    return fresh;
}

// Send one unicast. Returns -1 if the peer is unknown / the socket is not ready / the send failed.
// s_tx_buf is a shared buffer serialized by s_tx_mux (the audio worker and the app task are two different tasks).
// head is an optional payload prefix (the audio frame header), assembled straight into s_tx_buf, avoiding an extra buffer.
static int tx_send_parts(uint8_t type, const uint8_t *head, size_t head_len,
                         const void *payload, size_t len)
{
    if (s_sock < 0 || head_len + len + 1 > TX_BUF_MAX || s_tx_mux == NULL) {
        return -1;
    }
    if (xSemaphoreTake(s_tx_mux, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "TX mutex wait timed out, dropping frame");
        return -1;
    }
    int rc = -1;
    struct sockaddr_in peer;
    if (s_tx_buf != NULL && peer_snapshot(&peer)) {
        s_tx_buf[0] = type;
        if (head_len > 0) memcpy(s_tx_buf + 1, head, head_len);
        if (len > 0) memcpy(s_tx_buf + 1 + head_len, payload, len);
        len += head_len;
        ssize_t n = sendto(s_sock, s_tx_buf, len + 1, 0,
                           (const struct sockaddr *)&peer, sizeof(peer));
        if (n == (ssize_t)(len + 1)) {
            rc = 0;
        } else {
            ESP_LOGW(TAG, "sendto failed: rc=%d errno=%d", (int)n, errno);
        }
    }
    xSemaphoreGive(s_tx_mux);
    return rc;
}

static int tx_send(uint8_t type, const void *payload, size_t len)
{
    return tx_send_parts(type, NULL, 0, payload, len);
}

static void send_device_hello(void)
{
    char buf[128];
    size_t n = app_protocol_device_hello(buf, sizeof(buf), 1);
    if (n > 0) {
        udp_audio_notify_event(buf, n);
    }
}

// Liveness: send companion an empty-payload PING every VOICE_UDP_PING_MS.
// An idle device produces no packets, so companion could not tell "device still here" from "device left the voice app"
// - this ping is the only thing its 6s-silence-means-down check relies on. Not sent while the link is down (no point).
// Not counted in drop stats: a failed liveness ping is naturally covered by beacons / the next packet; it is not an audio/event loss.
static void send_liveness_ping(void)
{
    if (!peer_fresh()) return;
    (void)tx_send(VOICE_UDP_T_PING, NULL, 0);
    s_ping_cnt++;
}

// beacon: establish or refresh the peer. Returns true = must reply with a device.hello.
static bool peer_accept_beacon(const struct sockaddr_in *src)
{
    int64_t now = esp_timer_get_time();
    bool need_hello = false;
    portENTER_CRITICAL(&s_peer_mux);
    bool same = s_peer_known && s_peer.sin_addr.s_addr == src->sin_addr.s_addr;
    if (!same) {
        s_peer.sin_family = AF_INET;
        s_peer.sin_addr   = src->sin_addr;
        s_peer.sin_port   = htons(VOICE_UDP_PORT);
        s_peer_known      = true;
        need_hello        = true;
    } else if (s_peer_last_us == 0 || now - s_peer_last_us > PEER_STALE_US) {
        need_hello = true;   // the peer went away and came back: let companion learn about us again
    }
    s_peer_last_us = now;
    portEXIT_CRITICAL(&s_peer_mux);
    return need_hello;
}

// Non-beacon downlink: only extends the link when it comes from the established peer (stops third parties keeping the link alive)
static void peer_refresh(const struct sockaddr_in *src)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_peer_mux);
    if (s_peer_known && s_peer.sin_addr.s_addr == src->sin_addr.s_addr) {
        s_peer_last_us = now;
    }
    portEXIT_CRITICAL(&s_peer_mux);
}

static void rx_task(void *arg)
{
    // The receive buffer is allocated by init and freed when this task exits (the task may still be using it after a deinit timeout).
    // 1 extra byte for an in-place '\0', so event JSON is parsed directly in rx.
    uint8_t *rx = arg;
    int64_t last_ping_us = 0;
    ESP_LOGI(TAG, "UDP RX task started (port %d)", VOICE_UDP_PORT);

    while (s_active) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        ssize_t n = recvfrom(s_sock, rx, RX_BUF_MAX, 0,
                             (struct sockaddr *)&src, &slen);
        // n <= 0 means SO_RCVTIMEO (200ms) expired, the normal path -> fall through to the liveness tick below
        if (n > 0) {
            const uint8_t type = rx[0];
            if (type == VOICE_UDP_T_BEACON) {
                s_beacon_cnt++;
                if (peer_accept_beacon(&src)) {
                    ESP_LOGI(TAG, "companion beacon from %s → link up",
                             inet_ntoa(src.sin_addr));
                    send_device_hello();
                }
            } else if (type == VOICE_UDP_T_EVENT) {
                struct sockaddr_in peer;
                if (!peer_snapshot(&peer) || peer.sin_addr.s_addr != src.sin_addr.s_addr ||
                    src.sin_port != peer.sin_port) continue;
                peer_refresh(&src);
                size_t plen = (size_t)n - 1;   // <= RX_BUF_MAX - 1 < APP_PROTO_RX_CAP
                char *json = (char *)rx + 1;
                json[plen] = '\0';
                app_event_t ev;
                if (app_protocol_parse(json, plen, &ev)) {
                    app_protocol_dispatch_event(&ev);   // approvals take the important path, others are zero-blocking
                } else {
                    ESP_LOGW(TAG, "downlink JSON rejected: %.*s",
                             (int)(plen > 80 ? 80 : plen), json);
                }
            } else {
                ESP_LOGW(TAG, "unknown packet type 0x%02x (%d B), ignored", type, (int)n);
            }
        }

        // Liveness tick: decoupled from receiving (runs whether or not a packet arrived this round)
        int64_t now_us = esp_timer_get_time();
        if (last_ping_us == 0 ||
            now_us - last_ping_us >= (int64_t)VOICE_UDP_PING_MS * 1000) {
            last_ping_us = now_us;
            send_liveness_ping();
        }
        // Link diagnostics (every 5s): the side whose beacon/ping count stops rising is the break point
        static int64_t last_diag_us = 0;
        if (last_diag_us == 0 ||
            now_us - last_diag_us >= 5000000) {
            last_diag_us = now_us;
            bool known;
            int64_t age_ms;
            portENTER_CRITICAL(&s_peer_mux);
            known = s_peer_known;
            age_ms = s_peer_last_us ? (now_us - s_peer_last_us) / 1000 : -1;
            portEXIT_CRITICAL(&s_peer_mux);
            ESP_LOGI(TAG, "link diag: beacons=%u pings=%u peer_known=%d peer_age_ms=%ld",
                     (unsigned)s_beacon_cnt, (unsigned)s_ping_cnt,
                     (int)known, (long)age_ms);
        }
    }
    ESP_LOGI(TAG, "UDP RX task exiting");
    free(rx);
    s_rx_task = NULL;
    vTaskDelete(NULL);
}

// ---- Public API ----

int udp_audio_init(void)
{
    if (s_sock >= 0) return 0;   // idempotent

    if (s_tx_mux == NULL) {
        s_tx_mux = xSemaphoreCreateMutexStatic(&s_tx_mux_storage);
        if (s_tx_mux == NULL) {
            ESP_LOGE(TAG, "TX mutex create failed");
            return -1;
        }
    }

    if (s_tx_buf == NULL) {
        uint8_t *tx = malloc(TX_BUF_MAX);
        if (tx == NULL) {
            ESP_LOGE(TAG, "TX buffer alloc failed");
            return -1;
        }
        xSemaphoreTake(s_tx_mux, portMAX_DELAY);
        s_tx_buf = tx;
        xSemaphoreGive(s_tx_mux);
    }
    uint8_t *rx = malloc(RX_BUF_MAX + 1);
    if (rx == NULL) {
        ESP_LOGE(TAG, "RX buffer alloc failed");
        return -1;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket create failed: errno=%d", errno);
        free(rx);
        return -1;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(VOICE_UDP_PORT);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind %d failed: errno=%d", VOICE_UDP_PORT, errno);
        close(sock);
        free(rx);
        return -1;
    }
    // Receive timeout: lets the RX task re-check the stop flag periodically (so deinit can exit cleanly)
    struct timeval tv;
    tv.tv_sec  = 0;
    tv.tv_usec = 200 * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    // Allow sending to the broadcast address (the device does not broadcast now; kept for future active probing)
    int bcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));

    s_sock = sock;
    s_active = true;
    if (xTaskCreate(rx_task, "udp_rx", 4096, rx, 4, &s_rx_task) != pdPASS) {
        ESP_LOGE(TAG, "RX task create failed");
        s_active = false;
        free(rx);
        close(sock);
        s_sock = -1;
        return -1;
    }
    ESP_LOGI(TAG, "UDP channel ready (port %d), waiting for companion beacon", VOICE_UDP_PORT);
    return 0;
}

int udp_audio_deinit(void)
{
    if (s_sock < 0 && s_rx_task == NULL && s_tx_buf == NULL) return 0;   // idempotent

    // 1. Set the stop flag first: recvfrom's 200ms timeout brings the task back to the loop top where it sees it
    s_active = false;
    // 2. Wait for the task to exit on its own before closing the socket - close racing a blocked recvfrom is a race condition
    for (int i = 0; i < 50 && s_rx_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_rx_task != NULL) {
        ESP_LOGW(TAG, "RX task did not exit within 500 ms (closing anyway; it will wind down itself)");
    }
    // 3. Close the socket
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
    // 4. Clear peer state: rediscover on next entry
    portENTER_CRITICAL(&s_peer_mux);
    s_peer_known  = false;
    s_peer_last_us = 0;
    memset(&s_peer, 0, sizeof(s_peer));
    portEXIT_CRITICAL(&s_peer_mux);
    s_audio_seq = 0;
    // 5. Free the TX buffer (under the lock, mutually exclusive with senders still winding down); the RX buffer is freed by the task itself
    if (s_tx_mux != NULL) {
        xSemaphoreTake(s_tx_mux, portMAX_DELAY);
        free(s_tx_buf);
        s_tx_buf = NULL;
        xSemaphoreGive(s_tx_mux);
    }
    ESP_LOGI(TAG, "UDP channel closed");
    return 0;
}

int udp_audio_notify_audio(const uint8_t *frame, size_t len)
{
    if (frame == NULL || len == 0 || len > ADPCM_BLOCK_MAX) return -1;
    // Over UDP each block is always a single, last fragment -> frame header [seq][0x80], exactly matching what companion's
    // reassemble_adpcm() expects: "new seq + is_last".
    const uint8_t head[2] = {s_audio_seq++, 0x80};
    int rc = tx_send_parts(VOICE_UDP_T_AUDIO, head, sizeof(head), frame, len);
    if (rc != 0) drop_inc(&s_audio_drops);
    return rc;
}

int udp_audio_notify_event(const char *line, size_t len)
{
    if (line == NULL || len == 0) return -1;
    int rc = tx_send(VOICE_UDP_T_EVENT, line, len);
    if (rc != 0) drop_inc(&s_event_drops);
    return rc;
}

int udp_audio_notify_event_blocking(const char *line, size_t len, uint32_t timeout_ms)
{
    (void)timeout_ms;   // UDP has no send queue or backpressure, no wait semantics
    return udp_audio_notify_event(line, len);
}

bool udp_audio_connected(void)        { return peer_fresh(); }
bool udp_audio_event_subscribed(void) { return peer_fresh(); }

uint32_t udp_audio_audio_drops(void) { return drop_get(&s_audio_drops); }
uint32_t udp_audio_event_drops(void) { return drop_get(&s_event_drops); }

const char *udp_audio_peer_ip(void)
{
    static char buf[16];
    struct sockaddr_in peer;
    if (!peer_snapshot(&peer)) {
        return "0.0.0.0";
    }
    const char *s = inet_ntoa(peer.sin_addr);
    strncpy(buf, s ? s : "0.0.0.0", sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    return MY_IP_STR(buf);
}
