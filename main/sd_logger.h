#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"
#include "sdmmc_cmd.h"

#include "paw3395.h"

/* The logger keeps its file open and buffers rows between periodic flushes. */
typedef struct {
    bool mounted;
    sdmmc_card_t *card;
    FILE *file;
    uint32_t next_mount_attempt_ms;
    uint32_t last_flush_ms;
    uint32_t rows_written;
    char file_buffer[512];
} sd_logger_t;

/** Reset state without accessing the card. */
void sd_logger_init(sd_logger_t *logger);

/** Try to mount an already FAT-formatted card and open /sdcard/paw3395.csv. */
esp_err_t sd_logger_start(sd_logger_t *logger);

/**
 * Append one sample when a card is mounted.  If no card is available, this is
 * non-blocking from the caller's point of view and retries mounting later.
 */
void sd_logger_record(sd_logger_t *logger, uint32_t elapsed_ms,
                      uint64_t elapsed_us, const paw3395_motion_t *sample);
