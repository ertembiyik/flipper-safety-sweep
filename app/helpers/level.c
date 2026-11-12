#include "level.h"

#define LEVEL_RELEASE      0.35f
#define LEVEL_PEAK_HOLD_MS 4000

void level_reset(Level* level, float span) {
    *level = (Level){.span = span};
}

void level_set_zero(Level* level, float value) {
    level->zero = value;
    level->shown = 0.0f;
    level->peak = 0;
}

uint8_t level_update(Level* level, float value, uint32_t tick_ms) {
    float target = (value - level->zero) / level->span;
    if(target < 0.0f) target = 0.0f;
    if(target > 1.0f) target = 1.0f;

    // Rise at once, fall slowly: bursty transmitters stay readable
    if(target > level->shown) {
        level->shown = target;
    } else {
        level->shown += (target - level->shown) * LEVEL_RELEASE;
    }

    uint8_t percent = (uint8_t)(level->shown * 100.0f + 0.5f);
    if(percent >= level->peak || tick_ms - level->peak_tick > LEVEL_PEAK_HOLD_MS) {
        level->peak = percent;
        level->peak_tick = tick_ms;
    }
    return percent;
}
