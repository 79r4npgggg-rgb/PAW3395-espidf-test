#include "motion_ble.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "sdkconfig.h"

static const char *TAG = "motion_ble";

/* This file also compiles for Phase 1-3, where the Phase 4 menu is hidden. */
#ifndef CONFIG_PAW3395_BLE_DEVICE_NAME
#define CONFIG_PAW3395_BLE_DEVICE_NAME "PAW3395-Logger"
#endif

/* BLE_UUID128_INIT stores bytes in over-the-air (little-endian UUID) order. */
static const ble_uuid128_t s_service_uuid =
    BLE_UUID128_INIT(0x01, 0xe0, 0xf3, 0x1d, 0x3a, 0x0c, 0x9c, 0xa6,
                     0x6f, 0x4f, 0x18, 0x8c, 0x01, 0x00, 0x3c, 0x8f);
static const ble_uuid128_t s_motion_uuid =
    BLE_UUID128_INIT(0x01, 0xe0, 0xf3, 0x1d, 0x3a, 0x0c, 0x9c, 0xa6,
                     0x6f, 0x4f, 0x18, 0x8c, 0x02, 0x00, 0x3c, 0x8f);
static const ble_uuid128_t s_delta_x_uuid =
    BLE_UUID128_INIT(0x01, 0xe0, 0xf3, 0x1d, 0x3a, 0x0c, 0x9c, 0xa6,
                     0x6f, 0x4f, 0x18, 0x8c, 0x03, 0x00, 0x3c, 0x8f);
static const ble_uuid128_t s_delta_y_uuid =
    BLE_UUID128_INIT(0x01, 0xe0, 0xf3, 0x1d, 0x3a, 0x0c, 0x9c, 0xa6,
                     0x6f, 0x4f, 0x18, 0x8c, 0x04, 0x00, 0x3c, 0x8f);
static const ble_uuid128_t s_squal_uuid =
    BLE_UUID128_INIT(0x01, 0xe0, 0xf3, 0x1d, 0x3a, 0x0c, 0x9c, 0xa6,
                     0x6f, 0x4f, 0x18, 0x8c, 0x05, 0x00, 0x3c, 0x8f);

typedef struct __attribute__((packed)) {
    uint32_t elapsed_ms;
    int16_t delta_x;
    int16_t delta_y;
    uint8_t squal;
    uint8_t motion;
} motion_ble_packet_t;

_Static_assert(sizeof(motion_ble_packet_t) == 10, "BLE packet must stay 10 bytes");

static portMUX_TYPE s_sample_lock = portMUX_INITIALIZER_UNLOCKED;
static motion_ble_packet_t s_latest_sample;
static uint16_t s_motion_value_handle;
static uint16_t s_delta_x_value_handle;
static uint16_t s_delta_y_value_handle;
static uint16_t s_squal_value_handle;
static uint16_t s_connection_handle = BLE_HS_CONN_HANDLE_NONE;
static bool s_motion_notifications_enabled;
static bool s_delta_x_notifications_enabled;
static bool s_delta_y_notifications_enabled;
static bool s_squal_notifications_enabled;
static bool s_started;

static void motion_ble_advertise(void);

static void motion_ble_clear_subscriptions(void)
{
    s_motion_notifications_enabled = false;
    s_delta_x_notifications_enabled = false;
    s_delta_y_notifications_enabled = false;
    s_squal_notifications_enabled = false;
}

static void motion_ble_snapshot(motion_ble_packet_t *snapshot)
{
    portENTER_CRITICAL(&s_sample_lock);
    *snapshot = s_latest_sample;
    portEXIT_CRITICAL(&s_sample_lock);
}

static void motion_ble_encode_int16_le(int16_t value, uint8_t encoded[2])
{
    const uint16_t unsigned_value = (uint16_t)value;
    encoded[0] = (uint8_t)unsigned_value;
    encoded[1] = (uint8_t)(unsigned_value >> 8);
}

static int motion_access(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    motion_ble_packet_t snapshot;
    motion_ble_snapshot(&snapshot);

    if (attr_handle == s_motion_value_handle) {
        return os_mbuf_append(ctxt->om, &snapshot, sizeof(snapshot)) == 0 ?
            0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    if (attr_handle == s_delta_x_value_handle || attr_handle == s_delta_y_value_handle) {
        const int16_t value = attr_handle == s_delta_x_value_handle ?
            snapshot.delta_x : snapshot.delta_y;
        uint8_t encoded[2];
        motion_ble_encode_int16_le(value, encoded);
        return os_mbuf_append(ctxt->om, encoded, sizeof(encoded)) == 0 ?
            0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    if (attr_handle == s_squal_value_handle) {
        return os_mbuf_append(ctxt->om, &snapshot.squal, sizeof(snapshot.squal)) == 0 ?
            0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def s_gatt_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_motion_uuid.u,
                .access_cb = motion_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_motion_value_handle,
            },
            {
                .uuid = &s_delta_x_uuid.u,
                .access_cb = motion_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_delta_x_value_handle,
            },
            {
                .uuid = &s_delta_y_uuid.u,
                .access_cb = motion_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_delta_y_value_handle,
            },
            {
                .uuid = &s_squal_uuid.u,
                .access_cb = motion_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_squal_value_handle,
            },
            {0},
        },
    },
    {0},
};

static int motion_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_connection_handle = event->connect.conn_handle;
            motion_ble_clear_subscriptions();
            ESP_LOGI(TAG, "central connected (handle=%u)", s_connection_handle);
        } else {
            ESP_LOGW(TAG, "BLE connection failed: rc=%d", event->connect.status);
            motion_ble_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "central disconnected (reason=%d)", event->disconnect.reason);
        s_connection_handle = BLE_HS_CONN_HANDLE_NONE;
        motion_ble_clear_subscriptions();
        motion_ble_advertise();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_motion_value_handle) {
            s_motion_notifications_enabled = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "motion notifications %s",
                     s_motion_notifications_enabled ? "enabled" : "disabled");
        } else if (event->subscribe.attr_handle == s_delta_x_value_handle) {
            s_delta_x_notifications_enabled = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "delta X notifications %s",
                     s_delta_x_notifications_enabled ? "enabled" : "disabled");
        } else if (event->subscribe.attr_handle == s_delta_y_value_handle) {
            s_delta_y_notifications_enabled = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "delta Y notifications %s",
                     s_delta_y_notifications_enabled ? "enabled" : "disabled");
        } else if (event->subscribe.attr_handle == s_squal_value_handle) {
            s_squal_notifications_enabled = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "SQUAL notifications %s",
                     s_squal_notifications_enabled ? "enabled" : "disabled");
        }
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        motion_ble_advertise();
        return 0;

    default:
        return 0;
    }
}

static void motion_ble_advertise(void)
{
    uint8_t own_addr_type;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "could not infer BLE address type: rc=%d", rc);
        return;
    }

    struct ble_hs_adv_fields adv_fields = {0};
    const char *name = ble_svc_gap_device_name();
    adv_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv_fields.name = (const uint8_t *)name;
    adv_fields.name_len = strlen(name);
    adv_fields.name_is_complete = 1;
    rc = ble_gap_adv_set_fields(&adv_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "could not set BLE advertisement: rc=%d", rc);
        return;
    }

    /* Keep the name in the 31-byte advertisement and put our 128-bit service
     * UUID in the scan response, where nRF Connect can still discover it.
     */
    struct ble_hs_adv_fields scan_response = {0};
    scan_response.uuids128 = &s_service_uuid;
    scan_response.num_uuids128 = 1;
    scan_response.uuids128_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&scan_response);
    if (rc != 0) {
        ESP_LOGE(TAG, "could not set BLE scan response: rc=%d", rc);
        return;
    }

    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &params,
                           motion_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "could not start advertising: rc=%d", rc);
        return;
    }
    ESP_LOGI(TAG, "advertising as %s", name);
}

static void motion_ble_on_sync(void)
{
    const int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "could not ensure BLE address: rc=%d", rc);
        return;
    }
    motion_ble_advertise();
}

static void motion_ble_on_reset(int reason)
{
    s_connection_handle = BLE_HS_CONN_HANDLE_NONE;
    motion_ble_clear_subscriptions();
    ESP_LOGW(TAG, "NimBLE reset: reason=%d", reason);
}

static void motion_ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t motion_ble_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    if (err != ESP_OK) {
        return err;
    }

    err = nimble_port_init();
    if (err != ESP_OK) {
        return err;
    }

    ble_hs_cfg.reset_cb = motion_ble_on_reset;
    ble_hs_cfg.sync_cb = motion_ble_on_sync;
    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_svc_gap_device_name_set(CONFIG_PAW3395_BLE_DEVICE_NAME);
    if (rc != 0) {
        ESP_LOGE(TAG, "could not set BLE device name: rc=%d", rc);
        nimble_port_deinit();
        return ESP_FAIL;
    }
    rc = ble_gatts_count_cfg(s_gatt_services);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(s_gatt_services);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "could not register GATT service: rc=%d", rc);
        nimble_port_deinit();
        return ESP_FAIL;
    }

    s_started = true;
    nimble_port_freertos_init(motion_ble_host_task);
    return ESP_OK;
}

static void motion_ble_notify(uint16_t value_handle, const void *data, size_t length,
                              const char *characteristic_name)
{
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, length);
    if (om == NULL) {
        ESP_LOGW(TAG, "%s notification dropped: out of mbufs", characteristic_name);
        return;
    }

    const int rc = ble_gatts_notify_custom(s_connection_handle, value_handle, om);
    if (rc != 0 && rc != BLE_HS_ENOTCONN) {
        ESP_LOGW(TAG, "%s notification dropped: rc=%d", characteristic_name, rc);
    }
}

void motion_ble_publish(uint32_t elapsed_ms, const paw3395_motion_t *sample)
{
    if (sample == NULL || !s_started) {
        return;
    }

    motion_ble_packet_t packet = {
        .elapsed_ms = elapsed_ms,
        .delta_x = sample->delta_x,
        .delta_y = sample->delta_y,
        .squal = sample->squal,
        .motion = sample->motion,
    };
    portENTER_CRITICAL(&s_sample_lock);
    s_latest_sample = packet;
    portEXIT_CRITICAL(&s_sample_lock);

    if (s_connection_handle == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }

    if (s_motion_notifications_enabled) {
        motion_ble_notify(s_motion_value_handle, &packet, sizeof(packet), "motion");
    }

    if (s_delta_x_notifications_enabled) {
        uint8_t encoded[2];
        motion_ble_encode_int16_le(packet.delta_x, encoded);
        motion_ble_notify(s_delta_x_value_handle, encoded, sizeof(encoded), "delta X");
    }

    if (s_delta_y_notifications_enabled) {
        uint8_t encoded[2];
        motion_ble_encode_int16_le(packet.delta_y, encoded);
        motion_ble_notify(s_delta_y_value_handle, encoded, sizeof(encoded), "delta Y");
    }

    if (s_squal_notifications_enabled) {
        motion_ble_notify(s_squal_value_handle, &packet.squal, sizeof(packet.squal),
                          "SQUAL");
    }
}
