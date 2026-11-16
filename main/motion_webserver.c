#include "motion_webserver.h"

#include <inttypes.h>
#include <stdio.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "motion_wifi.h"

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

static httpd_handle_t s_server;
/* Changes on every boot so the browser can tell that sequence numbers restarted. */
static uint32_t s_boot_id;

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

    char body[256];
    const int length = snprintf(
        body, sizeof(body),
        "{\"firmware\":\"paw3395-wifi-monitor\",\"boot_id\":\"%08" PRIx32 "\","
        "\"uptime_ms\":%" PRIu64 ",\"ssid\":\"%s\",\"ip\":\"%s\","
        "\"stations\":%d,\"rssi_dbm\":%d}",
        s_boot_id, (uint64_t)esp_timer_get_time() / 1000U, motion_wifi_ssid(), motion_wifi_ip(),
        stations, rssi);
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

esp_err_t motion_webserver_start(void)
{
    s_boot_id = esp_random();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.max_uri_handlers = 8;
    esp_err_t err = httpd_start(&s_server, &config);
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
    };
    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); ++i) {
        err = httpd_register_uri_handler(s_server, &handlers[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "could not register %s: %s", handlers[i].uri, esp_err_to_name(err));
            return err;
        }
    }
    ESP_LOGI(TAG, "HTTP server ready (boot_id=%08" PRIx32 ")", s_boot_id);
    return ESP_OK;
}
