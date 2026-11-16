#include <inttypes.h>
#include <stdio.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "motion_ble.h"
#include "motion_logger.h"
#include "motion_webserver.h"
#include "motion_wifi.h"
#include "paw3395.h"
#include "raw_web.h"
#include "sd_logger.h"

static const char *TAG = "paw3395_test";

static void wait_forever(void)
{
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}

void app_main(void)
{
    paw3395_t sensor;
    ESP_ERROR_CHECK(paw3395_init_bus(&sensor));

    /* Datasheet 6.1 requires >=50 ms after stable power before a reset/init. */
    vTaskDelay(pdMS_TO_TICKS(60));

    uint8_t product_id = 0;
    uint8_t inverse_product_id = 0;
    ESP_ERROR_CHECK(paw3395_read_identity(&sensor, &product_id, &inverse_product_id));

    ESP_LOGI(TAG, "Product_ID=0x%02X (expected 0x%02X), Inv_Product_ID=0x%02X (expected 0x%02X)",
             product_id, PAW3395_PRODUCT_ID, inverse_product_id, PAW3395_INV_PRODUCT_ID);

    if (product_id != PAW3395_PRODUCT_ID || inverse_product_id != PAW3395_INV_PRODUCT_ID) {
        ESP_LOGE(TAG, "SPI probe failed: do not enable full initialization yet.");
        wait_forever();
    }

#if CONFIG_PAW3395_TEST_PHASE_SPI_PROBE
    ESP_LOGI(TAG, "SPI probe passed. Select phase 2 in menuconfig before the next flash.");
    wait_forever();
#elif CONFIG_PAW3395_TEST_PHASE_FULL_INIT
    ESP_LOGI(TAG, "SPI probe passed; performing the PAW3395 datasheet power-up sequence.");
    ESP_ERROR_CHECK(paw3395_power_up(&sensor));
    ESP_LOGI(TAG, "Initialization passed. The following rows are: SQUAL,Delta_X,Delta_Y");
    ESP_LOGI(TAG, "Open a serial plotter now; all subsequent rows are numeric CSV.");

    for (;;) {
        paw3395_motion_t sample;
        esp_err_t err = paw3395_read_motion(&sensor, &sample);
        if (err == ESP_OK) {
            /* Numeric-only CSV: suitable for Arduino Serial Plotter and similar tools. */
            printf("%u,%d,%d\n", sample.squal, sample.delta_x, sample.delta_y);
            fflush(stdout);
        } else {
            ESP_LOGE(TAG, "sample read failed: %s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(CONFIG_PAW3395_SAMPLE_PERIOD_MS));
    }
#elif CONFIG_PAW3395_TEST_PHASE_RAW_WEB
    ESP_LOGI(TAG, "SPI probe passed; initializing the PAW3395 RAW image viewer.");
    ESP_ERROR_CHECK(paw3395_power_up(&sensor));
    ESP_ERROR_CHECK(raw_web_start(&sensor));
    wait_forever();
#elif CONFIG_PAW3395_TEST_PHASE_LOGGER
    ESP_LOGI(TAG, "SPI probe passed; starting microSD + BLE motion logger.");
    ESP_ERROR_CHECK(paw3395_power_up(&sensor));

    sd_logger_t logger;
    sd_logger_init(&logger);
    esp_err_t err = sd_logger_start(&logger);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "microSD unavailable (%s); BLE monitoring continues and SD mounting will retry",
                 esp_err_to_name(err));
    }

    err = motion_ble_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BLE did not start: %s; microSD logging continues", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "Logger samples: elapsed_ms,delta_x,delta_y,squal,motion");
    for (;;) {
        paw3395_motion_t sample;
        const uint64_t elapsed_us = (uint64_t)esp_timer_get_time();
        const uint32_t elapsed_ms = (uint32_t)(elapsed_us / 1000U);

        err = paw3395_read_motion(&sensor, &sample);
        if (err == ESP_OK) {
            sd_logger_record(&logger, elapsed_ms, elapsed_us, &sample);
            motion_ble_publish(elapsed_ms, &sample);
        } else {
            ESP_LOGE(TAG, "sample read failed: %s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(CONFIG_PAW3395_SAMPLE_PERIOD_MS));
    }
#elif CONFIG_PAW3395_TEST_PHASE_WIFI_MONITOR
    ESP_LOGI(TAG, "SPI probe passed; starting the PAW3395 Wi-Fi real-time monitor.");
    ESP_ERROR_CHECK(paw3395_power_up(&sensor));

    /* Only the sampling task touches the PAW3395 from here on; Wi-Fi and
     * WebSocket work happens in other tasks and reads from its ring buffer. */
    const motion_logger_config_t logger_config = {
        .period_ms = CONFIG_PAW3395_MONITOR_SAMPLE_PERIOD_MS,
        .ring_samples = CONFIG_PAW3395_MONITOR_RING_SAMPLES,
#if CONFIG_PAW3395_MONITOR_SYNTHETIC_DATA
        .synthetic = true,
#else
        .synthetic = false,
#endif
    };
    ESP_ERROR_CHECK(motion_logger_start(&sensor, &logger_config));
    ESP_ERROR_CHECK(motion_wifi_start());
    const motion_webserver_config_t web_config = {
        .sample_period_ms = logger_config.period_ms,
        .samples_per_packet = CONFIG_PAW3395_MONITOR_SAMPLES_PER_PACKET,
        .synthetic = logger_config.synthetic,
    };
    ESP_ERROR_CHECK(motion_webserver_start(&web_config));
    wait_forever();
#else
#error "Select a PAW3395 test phase in menuconfig"
#endif
}
