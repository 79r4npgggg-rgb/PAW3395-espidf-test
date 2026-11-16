#include "motion_webserver.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

#include "motion_logger.h"
#include "motion_protocol.h"
#include "motion_wifi.h"

#if CONFIG_PAW3395_TEST_PHASE_WIFI_MONITOR && !CONFIG_HTTPD_WS_SUPPORT
#error "Phase 5 needs CONFIG_HTTPD_WS_SUPPORT (selected automatically by the phase 5 menu entry)"
#endif

/* All sources compile in every phase; WebSocket support is enabled only by phase 5. */
#if CONFIG_HTTPD_WS_SUPPORT

#define SENDER_TASK_PRIORITY 5
#define SENDER_TASK_STACK    6144
#define STATUS_INTERVAL_US   1000000
/* Packets sent back-to-back per wake-up while catching up after a stall. */
#define MAX_PACKETS_PER_WAKE 8

static const char *TAG = "motion_web";

/* Web files embedded by main/CMakeLists.txt (EMBED_FILES). */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");
extern const uint8_t style_css_start[] asm("_binary_style_css_start");
extern const uint8_t style_css_end[] asm("_binary_style_css_end");
extern const uint8_t app_js_start[] asm("_binary_app_js_start");
extern const uint8_t app_js_end[] asm("_binary_app_js_end");

typedef struct {
    const uint8_t *start;
    const uint8_t *end;
    const char *content_type;
} static_file_t;

static const static_file_t k_index_file = {index_html_start, index_html_end, "text/html; charset=utf-8"};
static const static_file_t k_style_file = {style_css_start, style_css_end, "text/css; charset=utf-8"};
static const static_file_t k_app_file = {app_js_start, app_js_end, "text/javascript; charset=utf-8"};

typedef enum {
    STREAM_REQUEST_NONE,
    STREAM_REQUEST_LIVE,
    STREAM_REQUEST_FROM,
} stream_request_t;

/* The single WebSocket client.  A new connection replaces the previous one. */
typedef struct {
    int fd;
    uint32_t generation;
    stream_request_t request;
    uint32_t request_from;
} ws_client_t;

static httpd_handle_t s_server;
static motion_webserver_config_t s_config;
/* Changes on every boot so the browser can tell that sequence numbers restarted. */
static uint32_t s_boot_id;
static portMUX_TYPE s_client_lock = portMUX_INITIALIZER_UNLOCKED;
static ws_client_t s_client = {.fd = -1};
/* SSID as a JSON string body: up to 32 bytes, each at most 6 characters escaped. */
static char s_ssid_json[32 * 6 + 1];

static void json_escape(const char *in, char *out, size_t capacity)
{
    size_t n = 0;
    for (; *in != '\0' && n + 7 < capacity; ++in) {
        const unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') {
            out[n++] = '\\';
            out[n++] = (char)c;
        } else if (c < 0x20) {
            n += (size_t)snprintf(out + n, capacity - n, "\\u%04x", (unsigned int)c);
        } else {
            out[n++] = (char)c;
        }
    }
    out[n] = '\0';
}

static esp_err_t static_file_handler(httpd_req_t *request)
{
    const static_file_t *file = request->user_ctx;
    httpd_resp_set_type(request, file->content_type);
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, (const char *)file->start, file->end - file->start);
}

static esp_err_t info_handler(httpd_req_t *request)
{
    int stations = 0;
    int rssi = 0;
    motion_wifi_get_link(&stations, &rssi);

    char body[512];
    const int length = snprintf(
        body, sizeof(body),
        "{\"firmware\":\"paw3395-wifi-monitor\",\"protocol\":%d,\"boot_id\":\"%08" PRIx32 "\","
        "\"uptime_ms\":%" PRIu64 ",\"ssid\":\"%s\",\"ip\":\"%s\",\"stations\":%d,\"rssi_dbm\":%d,"
        "\"period_us\":%" PRIu32 ",\"synthetic\":%s}",
        MOTION_PROTOCOL_VERSION, s_boot_id, (uint64_t)esp_timer_get_time() / 1000U,
        s_ssid_json, motion_wifi_ip(), stations, rssi,
        s_config.sample_period_ms * 1000U, s_config.synthetic ? "true" : "false");
    if (length < 0 || length >= (int)sizeof(body)) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "info too long");
    }
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, body, length);
}

static esp_err_t no_content_handler(httpd_req_t *request)
{
    httpd_resp_set_status(request, "204 No Content");
    return httpd_resp_send(request, NULL, 0);
}

static void handle_command(int fd, const char *text)
{
    if (strstr(text, "\"cmd\":\"stream\"") == NULL) {
        ESP_LOGW(TAG, "ignored WebSocket command: %s", text);
        return;
    }
    stream_request_t request = STREAM_REQUEST_LIVE;
    uint32_t from = 0;
    const char *from_text = strstr(text, "\"from\":");
    if (from_text != NULL) {
        char *end = NULL;
        const unsigned long value = strtoul(from_text + 7, &end, 10);
        if (end != from_text + 7) {
            request = STREAM_REQUEST_FROM;
            from = (uint32_t)value;
        }
    }

    portENTER_CRITICAL(&s_client_lock);
    if (s_client.fd == fd) {
        s_client.request = request;
        s_client.request_from = from;
    }
    portEXIT_CRITICAL(&s_client_lock);
}

static esp_err_t ws_handler(httpd_req_t *request)
{
    const int fd = httpd_req_to_sockfd(request);
    if (request->method == HTTP_GET) {
        /* Handshake completed: this connection becomes the only streaming client. */
        int previous_fd;
        portENTER_CRITICAL(&s_client_lock);
        previous_fd = s_client.fd;
        s_client.fd = fd;
        ++s_client.generation;
        s_client.request = STREAM_REQUEST_NONE;
        portEXIT_CRITICAL(&s_client_lock);
        if (previous_fd >= 0 && previous_fd != fd) {
            /* Still open (its close callback would have cleared s_client.fd). */
            shutdown(previous_fd, SHUT_RDWR);
        }
        const int no_delay = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay));
        ESP_LOGI(TAG, "WebSocket client connected (fd=%d)", fd);
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {0};
    esp_err_t err = httpd_ws_recv_frame(request, &frame, 0);
    if (err != ESP_OK) {
        return err;
    }
    char text[128];
    if (frame.len >= sizeof(text)) {
        ESP_LOGW(TAG, "WebSocket message too long (%u bytes)", (unsigned int)frame.len);
        return ESP_FAIL;
    }
    frame.payload = (uint8_t *)text;
    if (frame.len > 0) {
        err = httpd_ws_recv_frame(request, &frame, sizeof(text) - 1);
        if (err != ESP_OK) {
            return err;
        }
    }
    text[frame.len] = '\0';
    if (frame.type == HTTPD_WS_TYPE_TEXT) {
        handle_command(fd, text);
    }
    return ESP_OK;
}

static void session_closed(httpd_handle_t server, int fd)
{
    (void)server;
    portENTER_CRITICAL(&s_client_lock);
    if (s_client.fd == fd) {
        s_client.fd = -1;
        s_client.request = STREAM_REQUEST_NONE;
    }
    portEXIT_CRITICAL(&s_client_lock);
    close(fd);
}

/* Identifies one WebSocket connection; lwIP reuses fd numbers after a close. */
typedef struct {
    int fd;
    uint32_t generation;
} ws_target_t;

typedef struct {
    ws_target_t target;
    bool close_session;
    httpd_ws_frame_t frame;
    esp_err_t result;
} ws_job_t;

/* One job at a time, issued only by the sender task. */
static ws_job_t s_job;
static SemaphoreHandle_t s_job_done;

/*
 * Runs in the HTTP server task.  The handshake handler and the close callback
 * run there too, so the client cannot be replaced between this check and the
 * send or close: a frame or close meant for an old connection is never applied
 * to a new connection that happens to get the same fd number.
 */
static void ws_job_run(void *arg)
{
    ws_job_t *job = arg;
    portENTER_CRITICAL(&s_client_lock);
    const bool current = s_client.fd == job->target.fd && s_client.generation == job->target.generation;
    portEXIT_CRITICAL(&s_client_lock);

    if (!current || httpd_ws_get_fd_info(s_server, job->target.fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
        job->result = ESP_ERR_INVALID_STATE;
    } else if (job->close_session) {
        /* Not httpd_sess_trigger_close(): that queues another callback, which could
         * run after this fd has been reused.  The server cleans up on the EOF. */
        job->result = shutdown(job->target.fd, SHUT_RDWR) == 0 ? ESP_OK : ESP_FAIL;
    } else {
        job->result = httpd_ws_send_frame_async(s_server, job->target.fd, &job->frame);
        if (job->result == ESP_OK) {
            /* The browser rarely sends on this socket; keep LRU purge from picking it. */
            httpd_sess_update_lru_counter(s_server, job->target.fd);
        }
    }
    xSemaphoreGive(s_job_done);
}

static esp_err_t run_ws_job(const ws_target_t *target, bool close_session, httpd_ws_type_t type,
                            const void *data, size_t length)
{
    s_job = (ws_job_t){
        .target = *target,
        .close_session = close_session,
        .frame = {.type = type, .payload = (uint8_t *)data, .len = length},
        .result = ESP_FAIL,
    };
    const esp_err_t err = httpd_queue_work(s_server, ws_job_run, &s_job);
    if (err != ESP_OK) {
        return err;
    }
    xSemaphoreTake(s_job_done, portMAX_DELAY);
    return s_job.result;
}

static esp_err_t send_ws(const ws_target_t *target, httpd_ws_type_t type, const void *data, size_t length)
{
    return run_ws_job(target, false, type, data, length);
}

static esp_err_t send_hello(const ws_target_t *target)
{
    motion_logger_stats_t stats;
    motion_logger_get_stats(&stats);
    char json[512];
    const int length = snprintf(
        json, sizeof(json),
        "{\"type\":\"hello\",\"protocol\":%d,\"firmware\":\"paw3395-wifi-monitor\","
        "\"boot_id\":\"%08" PRIx32 "\",\"period_us\":%" PRIu32 ",\"packet_samples\":%" PRIu32 ","
        "\"ring_samples\":%" PRIu32 ",\"synthetic\":%s,\"next_seq\":%" PRIu32 ",\"ssid\":\"%s\"}",
        MOTION_PROTOCOL_VERSION, s_boot_id, s_config.sample_period_ms * 1000U,
        s_config.samples_per_packet, stats.ring_samples, s_config.synthetic ? "true" : "false",
        stats.next_sequence, s_ssid_json);
    if (length < 0 || length >= (int)sizeof(json)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return send_ws(target, HTTPD_WS_TYPE_TEXT, json, (size_t)length);
}

typedef struct {
    uint32_t packets;
    uint32_t send_failures;
    uint32_t ring_lost;
    uint32_t backlog;
} session_counters_t;

static esp_err_t send_status(const ws_target_t *target, const session_counters_t *session)
{
    motion_logger_stats_t stats;
    motion_logger_get_stats(&stats);
    int stations = 0;
    int rssi = 0;
    motion_wifi_get_link(&stations, &rssi);

    char json[640];
    const int length = snprintf(
        json, sizeof(json),
        "{\"type\":\"status\",\"boot_id\":\"%08" PRIx32 "\",\"uptime_ms\":%" PRIu64 ","
        "\"next_seq\":%" PRIu32 ",\"window_samples\":%" PRIu32 ",\"window_us\":%" PRIu32 ","
        "\"motion_samples\":%" PRIu32 ",\"min_period_us\":%" PRIu32 ",\"max_period_us\":%" PRIu32 ","
        "\"max_read_us\":%" PRIu32 ",\"late_reads\":%" PRIu32 ",\"read_errors\":%" PRIu32 ","
        "\"backlog\":%" PRIu32 ",\"ring_lost\":%" PRIu32 ",\"packets\":%" PRIu32 ","
        "\"send_failures\":%" PRIu32 ",\"stations\":%d,\"rssi_dbm\":%d,"
        "\"free_heap\":%u,\"min_free_heap\":%u}",
        s_boot_id, (uint64_t)esp_timer_get_time() / 1000U, stats.next_sequence,
        stats.window_samples, stats.window_us, stats.window_motion_samples,
        stats.window_min_period_us, stats.window_max_period_us, stats.window_max_read_us,
        stats.late_reads, stats.read_errors, session->backlog, session->ring_lost,
        session->packets, session->send_failures, stations, rssi,
        (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned int)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    if (length < 0 || length >= (int)sizeof(json)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return send_ws(target, HTTPD_WS_TYPE_TEXT, json, (size_t)length);
}

static void drop_client(const ws_target_t *target, const char *reason, esp_err_t err)
{
    ESP_LOGW(TAG, "closing WebSocket fd=%d: %s (%s)", target->fd, reason, esp_err_to_name(err));
    run_ws_job(target, true, HTTPD_WS_TYPE_CLOSE, NULL, 0);
}

static void sender_task(void *arg)
{
    (void)arg;
    static uint8_t packet[MOTION_PROTOCOL_MAX_PACKET_SIZE];
    static motion_sample_t samples[MOTION_PROTOCOL_MAX_SAMPLES];
    const uint32_t period_us = s_config.sample_period_ms * 1000U;
    const TickType_t interval = pdMS_TO_TICKS(s_config.sample_period_ms * s_config.samples_per_packet);

    /* Run half a sample period after the sampler so a packet does not race its last sample. */
    vTaskDelay(pdMS_TO_TICKS(s_config.sample_period_ms / 2U));
    TickType_t last_wake = xTaskGetTickCount();

    uint32_t active_generation = 0;
    ws_target_t target = {.fd = -1};
    bool streaming = false;
    bool failed = false;
    uint32_t cursor = 0;
    uint32_t packet_sequence = 0;
    session_counters_t session = {0};
    int64_t next_status_us = 0;

    for (;;) {
        if (xTaskDelayUntil(&last_wake, interval) == pdFALSE) {
            last_wake = xTaskGetTickCount();
        }

        portENTER_CRITICAL(&s_client_lock);
        const ws_client_t client = s_client;
        s_client.request = STREAM_REQUEST_NONE;
        portEXIT_CRITICAL(&s_client_lock);

        if (client.fd < 0) {
            target.fd = -1;
            streaming = false;
            continue;
        }
        if (client.generation != active_generation || client.fd != target.fd) {
            active_generation = client.generation;
            target = (ws_target_t){.fd = client.fd, .generation = client.generation};
            streaming = false;
            failed = false;
            packet_sequence = 0;
            session = (session_counters_t){0};
            const esp_err_t err = send_hello(&target);
            if (err != ESP_OK) {
                failed = true;
                drop_client(&target, "hello failed", err);
                continue;
            }
            next_status_us = esp_timer_get_time();
        }
        if (failed) {
            continue;
        }

        if (client.request == STREAM_REQUEST_LIVE) {
            cursor = motion_logger_next_sequence();
            streaming = true;
        } else if (client.request == STREAM_REQUEST_FROM) {
            /* motion_logger_read() clamps a stale or future cursor and reports skipped samples. */
            cursor = client.request_from;
            streaming = true;
        }

        if (streaming) {
            for (int burst = 0; burst < MAX_PACKETS_PER_WAKE; ++burst) {
                uint32_t lost = 0;
                const size_t count = motion_logger_read(&cursor, samples, MOTION_PROTOCOL_MAX_SAMPLES, &lost);
                session.ring_lost += lost;
                if (count == 0) {
                    break;
                }
                const size_t length = motion_protocol_encode(packet, sizeof(packet), packet_sequence,
                                                             period_us, samples, count);
                const esp_err_t err = send_ws(&target, HTTPD_WS_TYPE_BINARY, packet, length);
                if (err != ESP_OK) {
                    ++session.send_failures;
                    failed = true;
                    drop_client(&target, "send failed", err);
                    break;
                }
                ++packet_sequence;
                ++session.packets;
                if (count < MOTION_PROTOCOL_MAX_SAMPLES) {
                    break;
                }
            }
            session.backlog = motion_logger_next_sequence() - cursor;
        }

        const int64_t now_us = esp_timer_get_time();
        if (!failed && now_us >= next_status_us) {
            next_status_us = now_us + STATUS_INTERVAL_US;
            const esp_err_t err = send_status(&target, &session);
            if (err != ESP_OK) {
                failed = true;
                drop_client(&target, "status failed", err);
            }
        }
    }
}

esp_err_t motion_webserver_start(const motion_webserver_config_t *config)
{
    if (config == NULL || config->sample_period_ms == 0 || config->samples_per_packet == 0 ||
        config->samples_per_packet > MOTION_PROTOCOL_MAX_SAMPLES) {
        return ESP_ERR_INVALID_ARG;
    }
    s_config = *config;
    s_boot_id = esp_random();
    json_escape(motion_wifi_ssid(), s_ssid_json, sizeof(s_ssid_json));
    s_job_done = xSemaphoreCreateBinary();
    if (s_job_done == NULL) {
        return ESP_ERR_NO_MEM;
    }

    httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
    http_config.lru_purge_enable = true;
    http_config.max_uri_handlers = 8;
    http_config.close_fn = session_closed;
    esp_err_t err = httpd_start(&s_server, &http_config);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t handlers[] = {
        {.uri = "/", .method = HTTP_GET, .handler = static_file_handler, .user_ctx = (void *)&k_index_file},
        {.uri = "/index.html", .method = HTTP_GET, .handler = static_file_handler, .user_ctx = (void *)&k_index_file},
        {.uri = "/style.css", .method = HTTP_GET, .handler = static_file_handler, .user_ctx = (void *)&k_style_file},
        {.uri = "/app.js", .method = HTTP_GET, .handler = static_file_handler, .user_ctx = (void *)&k_app_file},
        {.uri = "/api/info", .method = HTTP_GET, .handler = info_handler},
        {.uri = "/favicon.ico", .method = HTTP_GET, .handler = no_content_handler},
        {.uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true},
    };
    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); ++i) {
        err = httpd_register_uri_handler(s_server, &handlers[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "could not register %s: %s", handlers[i].uri, esp_err_to_name(err));
            return err;
        }
    }

    if (xTaskCreate(sender_task, "paw_ws_sender", SENDER_TASK_STACK, NULL, SENDER_TASK_PRIORITY,
                    NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "HTTP/WebSocket server ready (boot_id=%08" PRIx32 ", %u samples per packet)",
             s_boot_id, (unsigned int)s_config.samples_per_packet);
    return ESP_OK;
}

#else

esp_err_t motion_webserver_start(const motion_webserver_config_t *config)
{
    (void)config;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_HTTPD_WS_SUPPORT */
