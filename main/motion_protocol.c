#include "motion_protocol.h"

static uint8_t *put_u16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    return out + 2;
}

static uint8_t *put_u32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
    return out + 4;
}

size_t motion_protocol_encode(uint8_t *buffer, size_t capacity, uint32_t packet_sequence,
                              uint32_t period_us, const motion_sample_t *samples,
                              size_t sample_count)
{
    if (buffer == NULL || samples == NULL || sample_count == 0 ||
        sample_count > MOTION_PROTOCOL_MAX_SAMPLES) {
        return 0;
    }
    const size_t size = MOTION_PROTOCOL_HEADER_SIZE + sample_count * MOTION_PROTOCOL_SAMPLE_SIZE;
    if (capacity < size) {
        return 0;
    }

    uint8_t *out = buffer;
    *out++ = 'P';
    *out++ = 'W';
    *out++ = MOTION_PROTOCOL_VERSION;
    *out++ = MOTION_PROTOCOL_HEADER_SIZE;
    *out++ = MOTION_PROTOCOL_SAMPLE_SIZE;
    *out++ = (uint8_t)sample_count;
    out = put_u16(out, 0);
    out = put_u32(out, packet_sequence);
    out = put_u32(out, period_us);

    for (size_t i = 0; i < sample_count; ++i) {
        const motion_sample_t *sample = &samples[i];
        out = put_u32(out, sample->sequence);
        out = put_u32(out, sample->timestamp_us);
        out = put_u16(out, (uint16_t)sample->delta_x);
        out = put_u16(out, (uint16_t)sample->delta_y);
        *out++ = sample->squal;
        *out++ = sample->motion;
        *out++ = sample->flags;
        *out++ = 0;
    }
    return size;
}
