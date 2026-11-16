#include "motion_logger.h"

#include <math.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Above lwIP (18) and the HTTP server (5); Wi-Fi itself runs on core 0. */
#define SAMPLER_TASK_PRIORITY 20
#define SAMPLER_TASK_STACK    4096
#if CONFIG_FREERTOS_UNICORE
#define SAMPLER_TASK_CORE 0
#else
#define SAMPLER_TASK_CORE 1
#endif
#define STATS_WINDOW_US 1000000U
#define MIN_RING_SAMPLES 64U

static const char *TAG = "motion_logger";

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static motion_sample_t *s_ring;
static uint32_t s_ring_size; /* power of two */
static uint32_t s_ring_mask;
static uint32_t s_next_sequence;       /* guarded by s_lock */
static motion_logger_stats_t s_stats;  /* guarded by s_lock */
static paw3395_t *s_sensor;
static motion_logger_config_t s_config;
static bool s_started;

typedef struct {
    bool started;
    uint32_t start_us;
    uint32_t samples;
    uint32_t motion_samples;
    uint32_t min_period_us;
    uint32_t max_period_us;
    uint32_t max_read_us;
} stats_window_t;

static void synthesize_reading(int64_t now_us, paw3395_motion_t *reading)
{
    /* Smooth, clearly artificial pattern for checking the link without moving the car. */
    const float t = (float)now_us * 1e-6f;
    const float two_pi = 6.2831853f;
    reading->delta_x = (int16_t)lroundf(120.0f * sinf(two_pi * t / 4.0f));
    reading->delta_y = (int16_t)lroundf(40.0f * sinf(two_pi * t / 1.5f));
    reading->squal = (uint8_t)lroundf(100.0f + 20.0f * sinf(two_pi * t / 10.0f));
    reading->motion = (reading->delta_x != 0 || reading->delta_y != 0) ? 0x80 : 0x00;
}

static void publish_window(const stats_window_t *window, uint32_t end_us)
{
    portENTER_CRITICAL(&s_lock);
    s_stats.window_samples = window->samples;
    s_stats.window_us = end_us - window->start_us;
    s_stats.window_motion_samples = window->motion_samples;
    s_stats.window_min_period_us = window->min_period_us == UINT32_MAX ? 0 : window->min_period_us;
    s_stats.window_max_period_us = window->max_period_us;
    s_stats.window_max_read_us = window->max_read_us;
    portEXIT_CRITICAL(&s_lock);
}

static void sampler_task(void *arg)
{
    (void)arg;
    const TickType_t period_ticks = pdMS_TO_TICKS(s_config.period_ms);
    TickType_t last_wake = xTaskGetTickCount();
    stats_window_t window = {0};
    uint32_t sequence = 0;
    uint32_t previous_start_us = 0;
    bool have_previous = false;
    uint32_t read_errors = 0;
    uint32_t late_reads = 0;

    for (;;) {
        uint8_t flags = 0;
        if (xTaskDelayUntil(&last_wake, period_ticks) == pdFALSE) {
            /* The scheduled tick has already passed.  Restart the schedule
             * instead of issuing a burst of back-to-back catch-up reads. */
            flags |= MOTION_SAMPLE_FLAG_LATE;
            ++late_reads;
            last_wake = xTaskGetTickCount();
        }

        const int64_t start = esp_timer_get_time();
        paw3395_motion_t reading = {0};
        if (s_config.synthetic) {
            synthesize_reading(start, &reading);
            flags |= MOTION_SAMPLE_FLAG_SYNTHETIC;
        } else if (paw3395_read_motion(s_sensor, &reading) != ESP_OK) {
            memset(&reading, 0, sizeof(reading));
            flags |= MOTION_SAMPLE_FLAG_READ_ERROR;
            ++read_errors;
        }
        const int64_t end = esp_timer_get_time();

        const motion_sample_t sample = {
            .sequence = sequence,
            .timestamp_us = (uint32_t)start,
            .delta_x = reading.delta_x,
            .delta_y = reading.delta_y,
            .squal = reading.squal,
            .motion = reading.motion,
            .flags = flags,
        };

        const uint32_t start_us = (uint32_t)start;
        if (!window.started || (uint32_t)(start_us - window.start_us) >= STATS_WINDOW_US) {
            if (window.started) {
                publish_window(&window, start_us);
            }
            window = (stats_window_t){
                .started = true, .start_us = start_us, .min_period_us = UINT32_MAX,
            };
        }
        ++window.samples;
        if (reading.motion & 0x80) {
            ++window.motion_samples;
        }
        if (have_previous) {
            const uint32_t period_us = start_us - previous_start_us;
            if (period_us < window.min_period_us) window.min_period_us = period_us;
            if (period_us > window.max_period_us) window.max_period_us = period_us;
        }
        const uint32_t read_us = (uint32_t)(end - start);
        if (read_us > window.max_read_us) window.max_read_us = read_us;
        previous_start_us = start_us;
        have_previous = true;

        portENTER_CRITICAL(&s_lock);
        s_ring[sequence & s_ring_mask] = sample;
        s_next_sequence = sequence + 1;
        s_stats.read_errors = read_errors;
        s_stats.late_reads = late_reads;
        portEXIT_CRITICAL(&s_lock);
        ++sequence;
    }
}

esp_err_t motion_logger_start(paw3395_t *sensor, const motion_logger_config_t *config)
{
    if (config == NULL || (sensor == NULL && !config->synthetic)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    const TickType_t period_ticks = pdMS_TO_TICKS(config->period_ms);
    if (period_ticks == 0 || period_ticks * portTICK_PERIOD_MS != config->period_ms) {
        ESP_LOGE(TAG, "%u ms is not a whole number of FreeRTOS ticks (%u ms); set CONFIG_FREERTOS_HZ=1000",
                 (unsigned int)config->period_ms, (unsigned int)portTICK_PERIOD_MS);
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t ring_size = MIN_RING_SAMPLES;
    while (ring_size < config->ring_samples && ring_size < (1U << 20)) {
        ring_size <<= 1;
    }
    s_ring = heap_caps_calloc(ring_size, sizeof(motion_sample_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_ring == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_ring_size = ring_size;
    s_ring_mask = ring_size - 1;
    s_sensor = sensor;
    s_config = *config;
    s_started = true;

    if (xTaskCreatePinnedToCore(sampler_task, "paw_sampler", SAMPLER_TASK_STACK, NULL,
                                SAMPLER_TASK_PRIORITY, NULL, SAMPLER_TASK_CORE) != pdPASS) {
        heap_caps_free(s_ring);
        s_ring = NULL;
        s_started = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "sampling every %u ms%s; ring holds %u samples (%u ms)",
             (unsigned int)config->period_ms, config->synthetic ? " (SYNTHETIC TEST DATA)" : "",
             (unsigned int)ring_size, (unsigned int)(ring_size * config->period_ms));
    return ESP_OK;
}

uint32_t motion_logger_next_sequence(void)
{
    portENTER_CRITICAL(&s_lock);
    const uint32_t next = s_next_sequence;
    portEXIT_CRITICAL(&s_lock);
    return next;
}

size_t motion_logger_read(uint32_t *cursor, motion_sample_t *samples, size_t max_samples,
                          uint32_t *lost)
{
    if (lost != NULL) {
        *lost = 0;
    }
    if (cursor == NULL || samples == NULL || max_samples == 0 || s_ring == NULL) {
        return 0;
    }

    uint32_t skipped = 0;
    uint32_t position = *cursor;
    size_t count;

    portENTER_CRITICAL(&s_lock);
    const uint32_t next = s_next_sequence;
    const uint32_t retained = next < s_ring_size ? next : s_ring_size;
    if ((int32_t)(next - position) < 0) {
        position = next;
    } else if (next - position > retained) {
        const uint32_t oldest = next - retained;
        skipped = oldest - position;
        position = oldest;
    }
    const uint32_t available = next - position;
    count = available < max_samples ? available : max_samples;
    for (size_t i = 0; i < count; ++i) {
        samples[i] = s_ring[(position + i) & s_ring_mask];
    }
    portEXIT_CRITICAL(&s_lock);

    *cursor = position + (uint32_t)count;
    if (lost != NULL) {
        *lost = skipped;
    }
    return count;
}

void motion_logger_get_stats(motion_logger_stats_t *stats)
{
    if (stats == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    *stats = s_stats;
    stats->next_sequence = s_next_sequence;
    stats->ring_samples = s_ring_size;
    portEXIT_CRITICAL(&s_lock);
}
