#pragma once

#include <stdint.h>

#include "esp_err.h"

#include "paw3395.h"

/*
 * Primary service:       8f3c0001-8c18-4f6f-a69c-0c3a1df3e001
 * Motion characteristic: 8f3c0002-8c18-4f6f-a69c-0c3a1df3e001 (READ, NOTIFY)
 *
 * Each characteristic value and notification is exactly 10 bytes in little
 * endian order: elapsed_ms (uint32), delta_x (int16), delta_y (int16),
 * squal (uint8), motion (uint8).
 */
esp_err_t motion_ble_start(void);

/** Store the current sample and notify an enabled central, if connected. */
void motion_ble_publish(uint32_t elapsed_ms, const paw3395_motion_t *sample);
