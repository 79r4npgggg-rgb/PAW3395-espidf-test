#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "paw3395.h"

/* Per-sample flags (also sent unchanged in the WebSocket protocol). */
#define MOTION_SAMPLE_FLAG_READ_ERROR 0x01 /* SPI read failed; motion/squal/delta are 0 */
#define MOTION_SAMPLE_FLAG_SYNTHETIC  0x02 /* generated test data, not read from PAW3395 */
#define MOTION_SAMPLE_FLAG_LATE       0x04 /* this read started after its scheduled tick */

/**
 * One PAW3395 read.  delta_x/delta_y are the signed 16-bit Delta_X/Delta_Y
 * counts exactly as returned by paw3395_read_motion(): the motion accumulated
 * by the sensor since the previous read (0 when Motion bit 7 is clear).
 */
typedef struct {
    uint32_t sequence;     /* increments by 1 per scheduled read since boot */
    uint32_t timestamp_us; /* low 32 bits of esp_timer_get_time() when the read started */
    int16_t delta_x;
    int16_t delta_y;
    uint8_t squal;
    uint8_t motion; /* raw Motion register */
    uint8_t flags;  /* MOTION_SAMPLE_FLAG_* */
} motion_sample_t;

typedef struct {
    uint32_t period_ms;   /* scheduled read period */
    size_t ring_samples;  /* history kept for the sender and for reconnects */
    bool synthetic;       /* generate test data instead of reading PAW3395 */
} motion_logger_config_t;

typedef struct {
    uint32_t next_sequence; /* sequence the next sample will receive */
    uint32_t ring_samples;  /* samples retained for the sender / reconnects */
    uint32_t read_errors;   /* since boot */
    uint32_t late_reads;    /* since boot */
    /* Last completed window of about one second (ESP32 clock). */
    uint32_t window_samples;
    uint32_t window_us;
    uint32_t window_motion_samples; /* samples with Motion bit 7 set */
    uint32_t window_min_period_us;
    uint32_t window_max_period_us;
    uint32_t window_max_read_us;
} motion_logger_stats_t;

/** Start the sampling task.  The sensor must already be powered up. */
esp_err_t motion_logger_start(paw3395_t *sensor, const motion_logger_config_t *config);

/** Sequence number the next sample will receive (i.e. "live" read position). */
uint32_t motion_logger_next_sequence(void);

/**
 * Copy up to max_samples samples starting at *cursor and advance *cursor.
 * If samples at *cursor were already overwritten, *cursor jumps to the oldest
 * retained sample and *lost receives the number of skipped samples.  A cursor
 * ahead of the newest sample (e.g. from before a reboot) is moved to "live".
 */
size_t motion_logger_read(uint32_t *cursor, motion_sample_t *samples, size_t max_samples,
                          uint32_t *lost);

void motion_logger_get_stats(motion_logger_stats_t *stats);
