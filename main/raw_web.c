#include "raw_web.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#define RAW_WEB_AP_SSID "PAW3395-RAW"
#define RAW_WEB_AP_CHANNEL 1
#define RAW_WEB_MAX_CLIENTS 4

static const char *TAG = "raw_web";
static SemaphoreHandle_t s_capture_lock;

static const char k_index_html[] =
    "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>PAW3395 RAW</title><style>body{background:#111;color:#eee;font:16px system-ui;text-align:center;margin:2rem}"
    "button{font:inherit;padding:.7rem 1rem;margin:1rem}canvas{image-rendering:pixelated;width:min(90vw,576px);"
    "height:min(90vw,576px);background:#000;border:1px solid #555}p{color:#bbb}</style></head><body>"
    "<h1>PAW3395 RAW image</h1><button id=capture>Capture (keep sensor still)</button><p id=status>Ready</p>"
    "<canvas id=image width=36 height=36></canvas><p>36 x 36 pixels, 7-bit grayscale. Brightness comes from the tracking surface.</p>"
    "<script>const c=document.querySelector('#image'),x=c.getContext('2d'),s=document.querySelector('#status');"
    "async function capture(){s.textContent='Capturing... keep still';try{const r=await fetch('/api/raw.bin?'+Date.now());"
    "if(!r.ok)throw Error(await r.text());const a=new Uint8Array(await r.arrayBuffer()),im=x.createImageData(36,36);"
    "for(let i=0;i<a.length;i++){const v=a[i]<<1,j=i*4;im.data[j]=im.data[j+1]=im.data[j+2]=v;im.data[j+3]=255}"
    "x.putImageData(im,0,0);s.textContent='Captured '+new Date().toLocaleTimeString()}catch(e){s.textContent='Capture failed: '+e.message}}"
    "document.querySelector('#capture').onclick=capture;</script></body></html>";

static esp_err_t index_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    return httpd_resp_send(request, k_index_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t raw_handler(httpd_req_t *request)
{
    paw3395_t *sensor = request->user_ctx;
    uint8_t pixels[PAW3395_RAW_IMAGE_PIXELS];

    if (xSemaphoreTake(s_capture_lock, pdMS_TO_TICKS(3000)) != pdTRUE) {
        /* ESP-IDF v6.0.2 has no HTTPD_503_* enum; set this standard status directly. */
        httpd_resp_set_status(request, "503 Service Unavailable");
        httpd_resp_sendstr(request, "Sensor is busy");
        return ESP_FAIL;
    }
    esp_err_t err = paw3395_capture_raw(sensor, pixels);
    xSemaphoreGive(s_capture_lock);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RAW capture failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "RAW capture failed");
        return err;
    }

    httpd_resp_set_type(request, "application/octet-stream");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, (const char *)pixels, sizeof(pixels));
}

static esp_err_t start_wifi_access_point(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));

    wifi_config_t config = {0};
    const size_t password_length = strlen(CONFIG_PAW3395_RAW_WEB_AP_PASSWORD);
    if (password_length < 8 || password_length >= sizeof(config.ap.password)) {
        ESP_LOGE(TAG, "Wi-Fi password must contain 8 to %u characters",
                 (unsigned int)(sizeof(config.ap.password) - 1));
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(config.ap.ssid, RAW_WEB_AP_SSID, sizeof(RAW_WEB_AP_SSID));
    memcpy(config.ap.password, CONFIG_PAW3395_RAW_WEB_AP_PASSWORD, password_length + 1);
    config.ap.ssid_len = strlen(RAW_WEB_AP_SSID);
    config.ap.channel = RAW_WEB_AP_CHANNEL;
    config.ap.max_connection = RAW_WEB_MAX_CLIENTS;
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    config.ap.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &config));
    return esp_wifi_start();
}

esp_err_t raw_web_start(paw3395_t *sensor)
{
    if (sensor == NULL) return ESP_ERR_INVALID_ARG;
    s_capture_lock = xSemaphoreCreateMutex();
    if (s_capture_lock == NULL) return ESP_ERR_NO_MEM;

    esp_err_t err = start_wifi_access_point();
    if (err != ESP_OK) return err;

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    err = httpd_start(&server, &config);
    if (err != ESP_OK) return err;

    const httpd_uri_t index = {
        .uri = "/", .method = HTTP_GET, .handler = index_handler, .user_ctx = sensor,
    };
    const httpd_uri_t raw = {
        .uri = "/api/raw.bin", .method = HTTP_GET, .handler = raw_handler, .user_ctx = sensor,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &index));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &raw));

    ESP_LOGI(TAG, "Join Wi-Fi '%s' and open http://192.168.4.1/", RAW_WEB_AP_SSID);
    return ESP_OK;
}
