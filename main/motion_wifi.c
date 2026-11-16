#include "motion_wifi.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

/* All sources compile in every phase, but these options are visible only in phase 5. */
#ifndef CONFIG_PAW3395_MONITOR_AP_SSID
#define CONFIG_PAW3395_MONITOR_AP_SSID "PAW3395-MONITOR"
#endif
#ifndef CONFIG_PAW3395_MONITOR_AP_PASSWORD
#define CONFIG_PAW3395_MONITOR_AP_PASSWORD "paw3395monitor"
#endif
#ifndef CONFIG_PAW3395_MONITOR_AP_CHANNEL
#define CONFIG_PAW3395_MONITOR_AP_CHANNEL 1
#endif
#ifndef CONFIG_PAW3395_MONITOR_AP_IP
#define CONFIG_PAW3395_MONITOR_AP_IP "192.168.4.1"
#endif
#ifndef CONFIG_PAW3395_MONITOR_AP_MAX_CLIENTS
#define CONFIG_PAW3395_MONITOR_AP_MAX_CLIENTS 2
#endif

static const char *TAG = "motion_wifi";

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        const wifi_event_ap_staconnected_t *event = data;
        ESP_LOGI(TAG, "station " MACSTR " joined (aid=%d)", MAC2STR(event->mac), event->aid);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        const wifi_event_ap_stadisconnected_t *event = data;
        ESP_LOGI(TAG, "station " MACSTR " left (reason=%d)", MAC2STR(event->mac), event->reason);
    }
}

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    return err;
}

static esp_err_t configure_ap_address(esp_netif_t *netif)
{
    esp_netif_ip_info_t ip_info = {0};
    esp_err_t err = esp_netif_str_to_ip4(CONFIG_PAW3395_MONITOR_AP_IP, &ip_info.ip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "invalid access point IPv4 address '%s'", CONFIG_PAW3395_MONITOR_AP_IP);
        return ESP_ERR_INVALID_ARG;
    }
    ip_info.gw = ip_info.ip;
    esp_netif_set_ip4_addr(&ip_info.netmask, 255, 255, 255, 0);

    err = esp_netif_dhcps_stop(netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        return err;
    }
    err = esp_netif_set_ip_info(netif, &ip_info);
    if (err != ESP_OK) {
        return err;
    }
    return esp_netif_dhcps_start(netif);
}

esp_err_t motion_wifi_start(void)
{
    const size_t ssid_length = strlen(CONFIG_PAW3395_MONITOR_AP_SSID);
    const size_t password_length = strlen(CONFIG_PAW3395_MONITOR_AP_PASSWORD);
    wifi_config_t config = {0};
    if (ssid_length == 0 || ssid_length > sizeof(config.ap.ssid)) {
        ESP_LOGE(TAG, "SSID must contain 1 to %u characters", (unsigned int)sizeof(config.ap.ssid));
        return ESP_ERR_INVALID_ARG;
    }
    if (password_length < 8 || password_length >= sizeof(config.ap.password)) {
        ESP_LOGE(TAG, "Wi-Fi password must contain 8 to %u characters",
                 (unsigned int)(sizeof(config.ap.password) - 1));
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = init_nvs();
    if (err != ESP_OK) return err;
    err = esp_netif_init();
    if (err != ESP_OK) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    esp_netif_t *netif = esp_netif_create_default_wifi_ap();
    if (netif == NULL) return ESP_FAIL;
    err = configure_ap_address(netif);
    if (err != ESP_OK) return err;

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_config);
    if (err != ESP_OK) return err;
    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    if (err != ESP_OK) return err;

    memcpy(config.ap.ssid, CONFIG_PAW3395_MONITOR_AP_SSID, ssid_length);
    memcpy(config.ap.password, CONFIG_PAW3395_MONITOR_AP_PASSWORD, password_length + 1);
    config.ap.ssid_len = ssid_length;
    config.ap.channel = CONFIG_PAW3395_MONITOR_AP_CHANNEL;
    config.ap.max_connection = CONFIG_PAW3395_MONITOR_AP_MAX_CLIENTS;
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    config.ap.pmf_cfg.required = false;

    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_config(WIFI_IF_AP, &config);
    if (err != ESP_OK) return err;
    err = esp_wifi_start();
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "Join Wi-Fi '%s' and open http://%s/",
             CONFIG_PAW3395_MONITOR_AP_SSID, CONFIG_PAW3395_MONITOR_AP_IP);
    return ESP_OK;
}

void motion_wifi_get_link(int *station_count, int *rssi_dbm)
{
    wifi_sta_list_t stations = {0};
    int count = 0;
    int rssi = 0;
    if (esp_wifi_ap_get_sta_list(&stations) == ESP_OK) {
        count = stations.num;
        if (count > 0) {
            rssi = stations.sta[0].rssi;
        }
    }
    if (station_count != NULL) *station_count = count;
    if (rssi_dbm != NULL) *rssi_dbm = rssi;
}

const char *motion_wifi_ssid(void)
{
    return CONFIG_PAW3395_MONITOR_AP_SSID;
}

const char *motion_wifi_ip(void)
{
    return CONFIG_PAW3395_MONITOR_AP_IP;
}
