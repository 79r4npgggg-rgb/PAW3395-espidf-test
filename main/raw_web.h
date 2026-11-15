#pragma once

#include "esp_err.h"
#include "paw3395.h"

/** Start the local Wi-Fi access point and RAW-image HTTP viewer. */
esp_err_t raw_web_start(paw3395_t *sensor);
