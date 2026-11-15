#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "driver/spi_master.h"

/* Wiring for Seeed Studio XIAO ESP32-S3. */
#define PAW3395_PIN_SCLK   GPIO_NUM_7   /* XIAO D8  */
#define PAW3395_PIN_MOSI   GPIO_NUM_9   /* XIAO D10 */
#define PAW3395_PIN_MISO   GPIO_NUM_8   /* XIAO D9  */
#define PAW3395_PIN_NCS    GPIO_NUM_1   /* XIAO D0  */
#define PAW3395_PIN_NRESET GPIO_NUM_2   /* XIAO D1  */

#define PAW3395_PRODUCT_ID     0x51
#define PAW3395_INV_PRODUCT_ID 0xAE
#define PAW3395_RAW_IMAGE_WIDTH  36
#define PAW3395_RAW_IMAGE_HEIGHT 36
#define PAW3395_RAW_IMAGE_PIXELS (PAW3395_RAW_IMAGE_WIDTH * PAW3395_RAW_IMAGE_HEIGHT)

typedef struct {
    spi_device_handle_t spi;
} paw3395_t;

typedef struct {
    uint8_t motion;
    uint8_t squal;
    int16_t delta_x;
    int16_t delta_y;
} paw3395_motion_t;

/** Configure the manual NCS/NRESET GPIOs and a Mode-0, 1 MHz SPI bus. */
esp_err_t paw3395_init_bus(paw3395_t *sensor);

/** Read the two fixed identity registers.  This does not change sensor state. */
esp_err_t paw3395_read_identity(paw3395_t *sensor, uint8_t *product_id,
                                uint8_t *inverse_product_id);

/**
 * Perform the complete PAW3395DM-T6QU v0.8 power-up sequence.
 * Call only after a successful identity check and at least 50 ms after power.
 */
esp_err_t paw3395_power_up(paw3395_t *sensor);

/** Read a coherent Motion, Delta_X, Delta_Y, and SQUAL sample. */
esp_err_t paw3395_read_motion(paw3395_t *sensor, paw3395_motion_t *sample);

/**
 * Capture the 36 x 36, 7-bit RawData array according to datasheet section 7.1.
 * The sensor must remain stationary while this function runs.
 */
esp_err_t paw3395_capture_raw(paw3395_t *sensor,
                              uint8_t pixels[PAW3395_RAW_IMAGE_PIXELS]);
