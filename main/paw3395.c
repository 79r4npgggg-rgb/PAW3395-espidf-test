#include "paw3395.h"

#include <stddef.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Datasheet register addresses in bank 0. */
#define REG_PRODUCT_ID       0x00
#define REG_MOTION           0x02
#define REG_DELTA_X_L        0x03
#define REG_DELTA_X_H        0x04
#define REG_DELTA_Y_L        0x05
#define REG_DELTA_Y_H        0x06
#define REG_SQUAL            0x07
#define REG_POWER_UP_RESET   0x3A
#define REG_PERFORMANCE      0x40
#define REG_RAWDATA_CONTROL  0x50
#define REG_RAWDATA_OPTION   0x55
#define REG_RAWDATA_GRAB     0x58
#define REG_RAWDATA_STATUS   0x59
#define REG_INV_PRODUCT_ID   0x5F
#define REG_BANK_SELECT      0x7F

/*
 * PAW3395 SPI timing, all from PAW3395DM-T6QU datasheet v0.8 table 4.
 * GPIO-driven NCS is intentional: it lets the code hold NCS low across the
 * mandatory 2 us address-to-data wait of a register read.
 */
#define PAW3395_NCS_SETUP_US       1  /* >= tNCS-SCLK: 120 ns */
#define PAW3395_READ_ADDRESS_US    2  /* >= tSRAD: 2 us */
#define PAW3395_AFTER_READ_US      2  /* >= tSRR/tSRW: 2 us */
#define PAW3395_AFTER_WRITE_US     5  /* >= tSWW/tSWR: 5 us */
#define PAW3395_NCS_HOLD_US        1  /* >= tSCLK-NCS/tBEXIT */

typedef struct {
    uint8_t address;
    uint8_t value;
} paw3395_reg_write_t;

/*
 * Exact 137-write table from PAW3395DM-T6QU datasheet v0.8, section 6.2.
 * Do not reorder this table: 0x7F selects the register bank for subsequent
 * accesses.
 */
static const paw3395_reg_write_t k_power_up_settings[] = {
    {0x7F, 0x07}, {0x40, 0x41}, {0x7F, 0x00}, {0x40, 0x80},
    {0x7F, 0x0E}, {0x55, 0x0D}, {0x56, 0x1B}, {0x57, 0xE8},
    {0x58, 0xD5}, {0x7F, 0x14}, {0x42, 0xBC}, {0x43, 0x74},
    {0x4B, 0x20}, {0x4D, 0x00}, {0x53, 0x0E}, {0x7F, 0x05},
    {0x44, 0x04}, {0x4D, 0x06}, {0x51, 0x40}, {0x53, 0x40},
    {0x55, 0xCA}, {0x5A, 0xE8}, {0x5B, 0xEA}, {0x61, 0x31},
    {0x62, 0x64}, {0x6D, 0xB8}, {0x6E, 0x0F}, {0x70, 0x02},
    {0x4A, 0x2A}, {0x60, 0x26}, {0x7F, 0x06}, {0x6D, 0x70},
    {0x6E, 0x60}, {0x6F, 0x04}, {0x53, 0x02}, {0x55, 0x11},
    {0x7A, 0x01}, {0x7D, 0x51}, {0x7F, 0x07}, {0x41, 0x10},
    {0x42, 0x32}, {0x43, 0x00}, {0x7F, 0x08}, {0x71, 0x4F},
    {0x7F, 0x09}, {0x62, 0x1F}, {0x63, 0x1F}, {0x65, 0x03},
    {0x66, 0x03}, {0x67, 0x1F}, {0x68, 0x1F}, {0x69, 0x03},
    {0x6A, 0x03}, {0x6C, 0x1F}, {0x6D, 0x1F}, {0x51, 0x04},
    {0x53, 0x20}, {0x54, 0x20}, {0x71, 0x0C}, {0x72, 0x07},
    {0x73, 0x07}, {0x7F, 0x0A}, {0x4A, 0x14}, {0x4C, 0x14},
    {0x55, 0x19}, {0x7F, 0x14}, {0x4B, 0x30}, {0x4C, 0x03},
    {0x61, 0x0B}, {0x62, 0x0A}, {0x63, 0x02}, {0x7F, 0x15},
    {0x4C, 0x02}, {0x56, 0x02}, {0x41, 0x91}, {0x4D, 0x0A},
    {0x7F, 0x0C}, {0x4A, 0x10}, {0x4B, 0x0C}, {0x4C, 0x40},
    {0x41, 0x25}, {0x55, 0x18}, {0x56, 0x14}, {0x49, 0x0A},
    {0x42, 0x00}, {0x43, 0x2D}, {0x44, 0x0C}, {0x54, 0x1A},
    {0x5A, 0x0D}, {0x5F, 0x1E}, {0x5B, 0x05}, {0x5E, 0x0F},
    {0x7F, 0x0D}, {0x48, 0xDD}, {0x4F, 0x03}, {0x52, 0x49},
    {0x51, 0x00}, {0x54, 0x5B}, {0x53, 0x00}, {0x56, 0x64},
    {0x55, 0x00}, {0x58, 0xA5}, {0x57, 0x02}, {0x5A, 0x29},
    {0x5B, 0x47}, {0x5C, 0x81}, {0x5D, 0x40}, {0x71, 0xDC},
    {0x70, 0x07}, {0x73, 0x00}, {0x72, 0x08}, {0x75, 0xDC},
    {0x74, 0x07}, {0x77, 0x00}, {0x76, 0x08}, {0x7F, 0x10},
    {0x4C, 0xD0}, {0x7F, 0x00}, {0x4F, 0x63}, {0x4E, 0x00},
    {0x52, 0x63}, {0x51, 0x00}, {0x54, 0x54}, {0x5A, 0x10},
    {0x77, 0x4F}, {0x47, 0x01}, {0x5B, 0x40}, {0x64, 0x60},
    {0x65, 0x06}, {0x66, 0x13}, {0x67, 0x0F}, {0x78, 0x01},
    {0x79, 0x9C}, {0x40, 0x00}, {0x55, 0x02}, {0x23, 0x70},
    {0x22, 0x01},
};

static esp_err_t spi_transfer(paw3395_t *sensor, const uint8_t *tx,
                              uint8_t *rx, size_t byte_count)
{
    spi_transaction_t transaction = {0};
    transaction.length = byte_count * 8;
    transaction.tx_buffer = tx;
    transaction.rx_buffer = rx;
    return spi_device_polling_transmit(sensor->spi, &transaction);
}

static void ncs_low(void)
{
    gpio_set_level(PAW3395_PIN_NCS, 0);
    esp_rom_delay_us(PAW3395_NCS_SETUP_US);
}

static void ncs_high(void)
{
    esp_rom_delay_us(PAW3395_NCS_HOLD_US);
    gpio_set_level(PAW3395_PIN_NCS, 1);
}

static void reset_spi_port(void)
{
    gpio_set_level(PAW3395_PIN_NCS, 1);
    esp_rom_delay_us(PAW3395_NCS_SETUP_US);
    gpio_set_level(PAW3395_PIN_NCS, 0);
    esp_rom_delay_us(PAW3395_NCS_SETUP_US);
    gpio_set_level(PAW3395_PIN_NCS, 1);
    esp_rom_delay_us(PAW3395_NCS_HOLD_US);
}

static esp_err_t write_register(paw3395_t *sensor, uint8_t address, uint8_t value)
{
    uint8_t tx[2] = {(uint8_t)(address | 0x80), value};

    ncs_low();
    esp_err_t err = spi_transfer(sensor, tx, NULL, sizeof(tx));
    ncs_high();
    esp_rom_delay_us(PAW3395_AFTER_WRITE_US);
    return err;
}

static esp_err_t read_register(paw3395_t *sensor, uint8_t address, uint8_t *value)
{
    const uint8_t address_byte = address & 0x7F;
    const uint8_t dummy = 0x00;
    uint8_t ignored;
    uint8_t result;

    ncs_low();
    esp_err_t err = spi_transfer(sensor, &address_byte, &ignored, 1);
    if (err == ESP_OK) {
        esp_rom_delay_us(PAW3395_READ_ADDRESS_US);
        err = spi_transfer(sensor, &dummy, &result, 1);
    }
    ncs_high();
    esp_rom_delay_us(PAW3395_AFTER_READ_US);

    if (err == ESP_OK) {
        *value = result;
    }
    return err;
}

static esp_err_t read_startup_status(paw3395_t *sensor, bool *ready)
{
    uint8_t value = 0;
    esp_err_t err = read_register(sensor, 0x6C, &value);
    if (err == ESP_OK) {
        *ready = (value == 0x80);
    }
    return err;
}

static esp_err_t read_and_discard_motion_registers(paw3395_t *sensor)
{
    uint8_t value;
    const uint8_t registers[] = {
        REG_MOTION, REG_DELTA_X_L, REG_DELTA_X_H, REG_DELTA_Y_L, REG_DELTA_Y_H,
    };

    for (size_t i = 0; i < sizeof(registers); ++i) {
        esp_err_t err = read_register(sensor, registers[i], &value);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t paw3395_init_bus(paw3395_t *sensor)
{
    if (sensor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(sensor, 0, sizeof(*sensor));

    const gpio_config_t output_config = {
        .pin_bit_mask = (1ULL << PAW3395_PIN_NCS) | (1ULL << PAW3395_PIN_NRESET),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&output_config);
    if (err != ESP_OK) {
        return err;
    }
    gpio_set_level(PAW3395_PIN_NCS, 1);
    gpio_set_level(PAW3395_PIN_NRESET, 1);  /* NRESET must never float. */

    const spi_bus_config_t bus_config = {
        .miso_io_num = PAW3395_PIN_MISO,
        .mosi_io_num = PAW3395_PIN_MOSI,
        .sclk_io_num = PAW3395_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 16,
    };
    err = spi_bus_initialize(SPI2_HOST, &bus_config, SPI_DMA_DISABLED);
    if (err != ESP_OK) {
        return err;
    }

    const spi_device_interface_config_t device_config = {
        .mode = 0,
        .clock_speed_hz = 1 * 1000 * 1000,
        .spics_io_num = -1,  /* NCS is held manually during tSRAD. */
        .queue_size = 1,
    };
    return spi_bus_add_device(SPI2_HOST, &device_config, &sensor->spi);
}

esp_err_t paw3395_read_identity(paw3395_t *sensor, uint8_t *product_id,
                                uint8_t *inverse_product_id)
{
    if (sensor == NULL || product_id == NULL || inverse_product_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = read_register(sensor, REG_PRODUCT_ID, product_id);
    if (err != ESP_OK) {
        return err;
    }
    return read_register(sensor, REG_INV_PRODUCT_ID, inverse_product_id);
}

esp_err_t paw3395_power_up(paw3395_t *sensor)
{
    if (sensor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Datasheet 6.1 steps 3-5.  The caller has already waited >= 50 ms. */
    reset_spi_port();
    esp_err_t err = write_register(sensor, REG_POWER_UP_RESET, 0x5A);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(5));

    /* Datasheet 6.2 steps 1-137, in the listed order. */
    for (size_t i = 0; i < sizeof(k_power_up_settings) / sizeof(k_power_up_settings[0]); ++i) {
        err = write_register(sensor, k_power_up_settings[i].address,
                             k_power_up_settings[i].value);
        if (err != ESP_OK) {
            return err;
        }
    }

    /* Datasheet 6.2 steps 138-139. */
    esp_rom_delay_us(1000);
    bool startup_ready = false;
    for (int attempt = 0; attempt < 60; ++attempt) {
        err = read_startup_status(sensor, &startup_ready);
        if (err != ESP_OK) {
            return err;
        }
        if (startup_ready) {
            break;
        }
        esp_rom_delay_us(1000);
    }
    if (!startup_ready) {
        const paw3395_reg_write_t recovery[] = {
            {REG_BANK_SELECT, 0x14}, {0x6C, 0x00}, {REG_BANK_SELECT, 0x00},
        };
        for (size_t i = 0; i < sizeof(recovery) / sizeof(recovery[0]); ++i) {
            err = write_register(sensor, recovery[i].address, recovery[i].value);
            if (err != ESP_OK) {
                return err;
            }
        }
    }

    /* Datasheet 6.2 steps 140-144. */
    const paw3395_reg_write_t final_settings[] = {
        {0x22, 0x00}, {0x55, 0x00}, {REG_BANK_SELECT, 0x07},
        {REG_PERFORMANCE, 0x40}, {REG_BANK_SELECT, 0x00},
    };
    for (size_t i = 0; i < sizeof(final_settings) / sizeof(final_settings[0]); ++i) {
        err = write_register(sensor, final_settings[i].address, final_settings[i].value);
        if (err != ESP_OK) {
            return err;
        }
    }

    /* Datasheet 6.1 step 7. */
    err = read_and_discard_motion_registers(sensor);
    if (err != ESP_OK) {
        return err;
    }

    /* SQUAL is specified as valid only in run mode; disable rest mode. */
    return write_register(sensor, REG_PERFORMANCE, 0x80);
}

esp_err_t paw3395_read_motion(paw3395_t *sensor, paw3395_motion_t *sample)
{
    if (sensor == NULL || sample == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t motion;
    uint8_t x_low;
    uint8_t x_high;
    uint8_t y_low;
    uint8_t y_high;
    uint8_t squal;

    /* The datasheet requires this exact Motion, X, Y read order. */
    esp_err_t err = read_register(sensor, REG_MOTION, &motion);
    if (err != ESP_OK) return err;
    err = read_register(sensor, REG_DELTA_X_L, &x_low);
    if (err != ESP_OK) return err;
    err = read_register(sensor, REG_DELTA_X_H, &x_high);
    if (err != ESP_OK) return err;
    err = read_register(sensor, REG_DELTA_Y_L, &y_low);
    if (err != ESP_OK) return err;
    err = read_register(sensor, REG_DELTA_Y_H, &y_high);
    if (err != ESP_OK) return err;
    err = read_register(sensor, REG_SQUAL, &squal);
    if (err != ESP_OK) return err;

    sample->motion = motion;
    sample->squal = squal;
    sample->delta_x = (motion & 0x80) ? (int16_t)((uint16_t)x_low | ((uint16_t)x_high << 8)) : 0;
    sample->delta_y = (motion & 0x80) ? (int16_t)((uint16_t)y_low | ((uint16_t)y_high << 8)) : 0;
    return ESP_OK;
}

static esp_err_t wait_for_register_bits(paw3395_t *sensor, uint8_t address,
                                        uint8_t mask, uint8_t required)
{
    /* The datasheet says to read continuously; the limit only prevents a hung web request. */
    for (int attempt = 0; attempt < 20000; ++attempt) {
        uint8_t value;
        esp_err_t err = read_register(sensor, address, &value);
        if (err != ESP_OK) {
            return err;
        }
        if ((value & mask) == required) {
            return ESP_OK;
        }
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t paw3395_capture_raw(paw3395_t *sensor,
                              uint8_t pixels[PAW3395_RAW_IMAGE_PIXELS])
{
    if (sensor == NULL || pixels == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = ESP_OK;
    uint8_t motion;

    /* PAW3395DM-T6QU datasheet v0.8, section 7.1, steps 2-8. */
    result = write_register(sensor, REG_BANK_SELECT, 0x00);
    if (result != ESP_OK) goto cleanup;
    result = write_register(sensor, REG_PERFORMANCE, 0x80);
    if (result != ESP_OK) goto cleanup;

    /* Wait until the sensor reports run mode (OP_Mode[1:0] == 0). */
    for (int attempt = 0; attempt < 20000; ++attempt) {
        result = read_register(sensor, REG_MOTION, &motion);
        if (result != ESP_OK) goto cleanup;
        if ((motion & 0x03) == 0) break;
        if (attempt == 19999) {
            result = ESP_ERR_TIMEOUT;
            goto cleanup;
        }
    }

    result = write_register(sensor, REG_RAWDATA_CONTROL, 0x01);
    if (result != ESP_OK) goto cleanup;
    result = write_register(sensor, REG_RAWDATA_OPTION, 0x04);
    if (result != ESP_OK) goto cleanup;
    result = write_register(sensor, REG_RAWDATA_GRAB, 0xFF);
    if (result != ESP_OK) goto cleanup;

    /* PG_FIRST and PG_VALID must both be set before reading pixel zero. */
    result = wait_for_register_bits(sensor, REG_RAWDATA_STATUS, 0xC0, 0xC0);
    if (result != ESP_OK) goto cleanup;
    result = read_register(sensor, REG_RAWDATA_GRAB, &pixels[0]);
    if (result != ESP_OK) goto cleanup;
    pixels[0] &= 0x7F;

    /* For each remaining pixel, wait for PG_VALID then read its 7-bit ADC value. */
    for (size_t i = 1; i < PAW3395_RAW_IMAGE_PIXELS; ++i) {
        result = wait_for_register_bits(sensor, REG_RAWDATA_STATUS, 0x80, 0x80);
        if (result != ESP_OK) goto cleanup;
        result = read_register(sensor, REG_RAWDATA_GRAB, &pixels[i]);
        if (result != ESP_OK) goto cleanup;
        pixels[i] &= 0x7F;
    }

cleanup:
    /* Datasheet section 7.1 steps 12-14. Always leave RawData Grab disabled. */
    if (write_register(sensor, REG_PERFORMANCE, 0x00) != ESP_OK && result == ESP_OK) {
        result = ESP_FAIL;
    }
    if (write_register(sensor, REG_RAWDATA_CONTROL, 0x00) != ESP_OK && result == ESP_OK) {
        result = ESP_FAIL;
    }
    if (write_register(sensor, REG_RAWDATA_OPTION, 0x00) != ESP_OK && result == ESP_OK) {
        result = ESP_FAIL;
    }
    return result;
}
