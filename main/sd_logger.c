#include "sd_logger.h"

#include <inttypes.h>
#include <string.h>

#include "driver/sdspi_host.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "sdkconfig.h"

#define SD_LOGGER_MOUNT_POINT "/sdcard"
#define SD_LOGGER_FILE_PATH SD_LOGGER_MOUNT_POINT "/paw3395.csv"
#define SD_LOGGER_SPI_HOST SPI2_HOST

/*
 * All source files compile in every test phase, while these Kconfig options
 * are visible only in Phase 4.  Keep harmless defaults for Phase 1-3 builds.
 */
#ifndef CONFIG_PAW3395_SD_CS_GPIO
#define CONFIG_PAW3395_SD_CS_GPIO 3
#endif
#ifndef CONFIG_PAW3395_SD_FLUSH_PERIOD_MS
#define CONFIG_PAW3395_SD_FLUSH_PERIOD_MS 1000
#endif
#ifndef CONFIG_PAW3395_SD_RETRY_PERIOD_MS
#define CONFIG_PAW3395_SD_RETRY_PERIOD_MS 5000
#endif

static const char *TAG = "sd_logger";

static uint32_t elapsed_ms_now(void)
{
    return (uint32_t)((uint64_t)esp_timer_get_time() / 1000U);
}

static bool time_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static bool sd_cs_conflicts_with_paw3395(void)
{
    const int cs = CONFIG_PAW3395_SD_CS_GPIO;
    return cs == PAW3395_PIN_NCS || cs == PAW3395_PIN_NRESET ||
           cs == PAW3395_PIN_SCLK || cs == PAW3395_PIN_MOSI ||
           cs == PAW3395_PIN_MISO;
}

static void sd_logger_close_and_unmount(sd_logger_t *logger)
{
    if (logger->file != NULL) {
        fclose(logger->file);
        logger->file = NULL;
    }
    if (logger->mounted && logger->card != NULL) {
        esp_err_t err = esp_vfs_fat_sdcard_unmount(SD_LOGGER_MOUNT_POINT, logger->card);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "SD unmount failed: %s", esp_err_to_name(err));
        }
    }
    logger->mounted = false;
    logger->card = NULL;
}

static void sd_logger_mark_unavailable(sd_logger_t *logger, const char *reason)
{
    ESP_LOGW(TAG, "SD logging paused: %s", reason);
    sd_logger_close_and_unmount(logger);
    logger->next_mount_attempt_ms = elapsed_ms_now() + CONFIG_PAW3395_SD_RETRY_PERIOD_MS;
}

static esp_err_t sd_logger_mount(sd_logger_t *logger)
{
    if (sd_cs_conflicts_with_paw3395()) {
        ESP_LOGE(TAG, "GPIO%d cannot be used for SD CS: it conflicts with PAW3395 wiring",
                 CONFIG_PAW3395_SD_CS_GPIO);
        return ESP_ERR_INVALID_ARG;
    }

    /* SPI2 has already been initialized by paw3395_init_bus().  This call only
     * attaches the SD card as a second SPI device, so PAW3395 NCS stays GPIO1.
     */
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_LOGGER_SPI_HOST;
    sdspi_device_config_t slot_config = {
        .host_id = SD_LOGGER_SPI_HOST,
        .gpio_cs = CONFIG_PAW3395_SD_CS_GPIO,
        .gpio_cd = SDSPI_SLOT_NO_CD,
        .gpio_wp = SDSPI_SLOT_NO_WP,
        .gpio_int = SDSPI_SLOT_NO_INT,
    };
    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 2,
        .allocation_unit_size = 0,
        .disk_status_check_enable = true,
        .use_one_fat = false,
    };

    esp_err_t err = esp_vfs_fat_sdspi_mount(SD_LOGGER_MOUNT_POINT, &host,
                                             &slot_config, &mount_config,
                                             &logger->card);
    if (err != ESP_OK) {
        logger->card = NULL;
        return err;
    }
    logger->mounted = true;

    logger->file = fopen(SD_LOGGER_FILE_PATH, "a+");
    if (logger->file == NULL) {
        ESP_LOGE(TAG, "could not open %s", SD_LOGGER_FILE_PATH);
        sd_logger_close_and_unmount(logger);
        return ESP_FAIL;
    }
    setvbuf(logger->file, logger->file_buffer, _IOFBF, sizeof(logger->file_buffer));

    if (fseek(logger->file, 0, SEEK_END) != 0) {
        sd_logger_close_and_unmount(logger);
        return ESP_FAIL;
    }
    const long file_size = ftell(logger->file);
    if (file_size < 0) {
        sd_logger_close_and_unmount(logger);
        return ESP_FAIL;
    }
    if (file_size == 0) {
        /* Both time columns are monotonic time since this firmware boot. */
        if (fprintf(logger->file, "elapsed_ms,elapsed_us,delta_x,delta_y,squal,motion\n") < 0 ||
            fflush(logger->file) != 0) {
            sd_logger_close_and_unmount(logger);
            return ESP_FAIL;
        }
    }

    logger->last_flush_ms = elapsed_ms_now();
    logger->rows_written = 0;
    ESP_LOGI(TAG, "mounted %s; logging to %s", SD_LOGGER_MOUNT_POINT, SD_LOGGER_FILE_PATH);
    sdmmc_card_print_info(stdout, logger->card);
    return ESP_OK;
}

void sd_logger_init(sd_logger_t *logger)
{
    if (logger != NULL) {
        memset(logger, 0, sizeof(*logger));
    }
}

esp_err_t sd_logger_start(sd_logger_t *logger)
{
    if (logger == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    sd_logger_close_and_unmount(logger);
    const esp_err_t err = sd_logger_mount(logger);
    if (err != ESP_OK) {
        logger->next_mount_attempt_ms = elapsed_ms_now() + CONFIG_PAW3395_SD_RETRY_PERIOD_MS;
    }
    return err;
}

void sd_logger_record(sd_logger_t *logger, uint32_t elapsed_ms,
                      uint64_t elapsed_us, const paw3395_motion_t *sample)
{
    if (logger == NULL || sample == NULL) {
        return;
    }

    if (!logger->mounted) {
        if (time_reached(elapsed_ms, logger->next_mount_attempt_ms)) {
            const esp_err_t err = sd_logger_mount(logger);
            if (err != ESP_OK) {
                logger->next_mount_attempt_ms = elapsed_ms + CONFIG_PAW3395_SD_RETRY_PERIOD_MS;
            }
        }
        if (!logger->mounted) {
            return;
        }
    }

    if (fprintf(logger->file, "%" PRIu32 ",%" PRIu64 ",%d,%d,%u,%u\n",
                elapsed_ms, elapsed_us, sample->delta_x, sample->delta_y,
                (unsigned int)sample->squal, (unsigned int)sample->motion) < 0) {
        sd_logger_mark_unavailable(logger, "CSV write failed");
        return;
    }
    ++logger->rows_written;

    if (time_reached(elapsed_ms, logger->last_flush_ms + CONFIG_PAW3395_SD_FLUSH_PERIOD_MS)) {
        if (fflush(logger->file) != 0) {
            sd_logger_mark_unavailable(logger, "CSV flush failed (card may have been removed)");
            return;
        }
        logger->last_flush_ms = elapsed_ms;
    }
}
