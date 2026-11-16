#pragma once

#include "esp_err.h"

/**
 * Start the phase 5 HTTP server.  It serves the embedded monitor page
 * (index.html, style.css, app.js) and GET /api/info.
 */
esp_err_t motion_webserver_start(void);
