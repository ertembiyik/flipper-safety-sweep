#pragma once

#include <stdint.h>

/** Turns raw sensor values into a steady 0..100 rise above a zero point, with a held peak */
typedef struct {
    float span;
    float zero;
    float shown;
    uint8_t peak;
    uint32_t peak_tick;
} Level;

/** `span` is the rise above zero that fills the scale */
void level_reset(Level* level, float span);
void level_set_zero(Level* level, float value);
uint8_t level_update(Level* level, float value, uint32_t tick_ms);
