#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint32_t sample_period_ms;   /* motion_logger read period */
    uint32_t samples_per_packet; /* nominal samples per WebSocket packet */
    bool synthetic;              /* motion_logger is generating test data */
} motion_webserver_config_t;

/**
 * Start the phase 5 HTTP server.  It serves the embedded monitor page
 * (index.html, style.css, app.js), GET /api/info, and the WebSocket endpoint
 * /ws that streams motion_logger samples (see motion_protocol.h).
 */
esp_err_t motion_webserver_start(const motion_webserver_config_t *config);
