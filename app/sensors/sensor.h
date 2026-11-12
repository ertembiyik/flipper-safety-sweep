#pragma once

#include <stdbool.h>

typedef struct {
    const char* title;
    /** Rise above the room's own level that fills the scale, in the sensor's unit */
    float span;
    /** NULL on success, otherwise a short reason shown instead of a reading */
    const char* (*start)(void);
    void (*stop)(void);
    /** Blocks for one measurement step, at most ~250 ms */
    bool (*sample)(float* value);
} Sensor;

extern const Sensor sensor_wifi;
extern const Sensor sensor_radio;
extern const Sensor sensor_chip;
extern const Sensor sensor_infrared;
