#pragma once

#include <stdint.h>

#include "esp_err.h"

#include "paw3395.h"

/*
 * Primary service:       8f3c0001-8c18-4f6f-a69c-0c3a1df3e001
 * Motion characteristic: 8f3c0002-8c18-4f6f-a69c-0c3a1df3e001 (READ, NOTIFY)
 * Delta X characteristic: 8f3c0003-8c18-4f6f-a69c-0c3a1df3e001 (READ, NOTIFY)
 * Delta Y characteristic: 8f3c0004-8c18-4f6f-a69c-0c3a1df3e001 (READ, NOTIFY)
 * SQUAL characteristic:   8f3c0005-8c18-4f6f-a69c-0c3a1df3e001 (READ, NOTIFY)
 *
 * Motion values and notifications are exactly 10 bytes in little-endian order:
 * elapsed_ms (uint32), delta_x (int16), delta_y (int16), squal (uint8), and
 * motion (uint8). Delta X and Delta Y values are separate 2-byte int16
 * little-endian values; SQUAL is a separate 1-byte uint8 value.
 * Graph Delta X:         8f3c0006-8c18-4f6f-a69c-0c3a1df3e001 (READ, NOTIFY)
 * Graph Delta Y:         8f3c0007-8c18-4f6f-a69c-0c3a1df3e001 (READ, NOTIFY)
 */
esp_err_t motion_ble_start(void);

/** Store one PAW3395 sample and notify each independently enabled characteristic. */
void motion_ble_publish(uint32_t elapsed_ms, const paw3395_motion_t *sample);
