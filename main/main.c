#include <inttypes.h>
#include <stdio.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "paw3395.h"
#include "raw_web.h"

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
#else
#error "Select a PAW3395 test phase in menuconfig"
#endif
}
