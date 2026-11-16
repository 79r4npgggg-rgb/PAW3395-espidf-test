#pragma once

#include <stddef.h>
#include <stdint.h>

#include "motion_logger.h"

/*
 * Binary WebSocket packet, protocol version 1.  All multi-byte fields are
 * little-endian.  Every packet is one binary WebSocket message:
 *
 * Header (16 bytes)
 *   0  uint8[2]  magic "PW" (0x50 0x57)
 *   2  uint8     protocol version (1)
 *   3  uint8     header size in bytes (16)
 *   4  uint8     sample size in bytes (16)
 *   5  uint8     sample count in this packet (1..64)
 *   6  uint16    reserved (0)
 *   8  uint32    packet sequence (0 for the first packet on each WebSocket connection)
 *  12  uint32    scheduled sample period in microseconds
 *
 * Sample (16 bytes, repeated sample-count times)
 *   0  uint32    sample sequence (since ESP32 boot, +1 per scheduled read)
 *   4  uint32    timestamp in microseconds (low 32 bits of esp_timer; wraps after ~71.6 min)
 *   8  int16     Delta_X counts (PAW3395 register value; same value as BLE 0003)
 *  10  int16     Delta_Y counts (PAW3395 register value; same value as BLE 0004)
 *  12  uint8     SQUAL
 *  13  uint8     Motion register
 *  14  uint8     flags (MOTION_SAMPLE_FLAG_*)
 *  15  uint8     reserved (0)
 *
 * The BLE "graph" characteristics 0006/0007 carry delta + 32768 as uint16;
 * this protocol carries the signed delta itself, with no offset.
 */
#define MOTION_PROTOCOL_VERSION 1
#define MOTION_PROTOCOL_HEADER_SIZE 16
#define MOTION_PROTOCOL_SAMPLE_SIZE 16
#define MOTION_PROTOCOL_MAX_SAMPLES 64
#define MOTION_PROTOCOL_MAX_PACKET_SIZE \
    (MOTION_PROTOCOL_HEADER_SIZE + MOTION_PROTOCOL_MAX_SAMPLES * MOTION_PROTOCOL_SAMPLE_SIZE)

/**
 * Encode one packet.  Returns the number of bytes written, or 0 if the
 * arguments are invalid or the buffer is too small.
 */
size_t motion_protocol_encode(uint8_t *buffer, size_t capacity, uint32_t packet_sequence,
                              uint32_t period_us, const motion_sample_t *samples,
                              size_t sample_count);
