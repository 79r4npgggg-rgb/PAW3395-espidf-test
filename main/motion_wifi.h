#pragma once

#include <stdint.h>

#include "esp_err.h"

/**
 * Start the phase 5 Wi-Fi access point.  SSID, password, channel, and IPv4
 * address come from menuconfig ("Wireless monitor (phase 5)").
 */
esp_err_t motion_wifi_start(void);

/** Number of associated stations and the RSSI (dBm) of the first one, 0 if none. */
void motion_wifi_get_link(int *station_count, int *rssi_dbm);

/** Configured SSID and IPv4 address strings, for display in the web page. */
const char *motion_wifi_ssid(void);
const char *motion_wifi_ip(void);
