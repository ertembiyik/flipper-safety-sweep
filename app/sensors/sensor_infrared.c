#include "sensor.h"

#include <furi.h>
#include <furi_hal_infrared.h>
#include <math.h>

#define INFRARED_WINDOW_MS 100

static volatile uint32_t infrared_edges;

static void sensor_infrared_capture(void* context, bool level, uint32_t duration) {
    UNUSED(context);
    UNUSED(level);
    UNUSED(duration);
    infrared_edges++;
}

static const char* sensor_infrared_start(void) {
    if(furi_hal_infrared_is_busy()) return "Infrared is in use\nby another app.";
    infrared_edges = 0;
    furi_hal_infrared_async_rx_set_capture_isr_callback(sensor_infrared_capture, NULL);
    furi_hal_infrared_async_rx_start();
    return NULL;
}

static void sensor_infrared_stop(void) {
    furi_hal_infrared_async_rx_stop();
    furi_hal_infrared_async_rx_set_capture_isr_callback(NULL, NULL);
}

static bool sensor_infrared_sample(float* value) {
    furi_delay_ms(INFRARED_WINDOW_MS);
    FURI_CRITICAL_ENTER();
    uint32_t edges = infrared_edges;
    infrared_edges = 0;
    FURI_CRITICAL_EXIT();

    uint32_t edges_per_second = edges * 1000 / INFRARED_WINDOW_MS;
    *value = log2f(1.0f + edges_per_second);
    return true;
}

const Sensor sensor_infrared = {
    .title = "Infrared",
    .span = 12.0f,
    .start = sensor_infrared_start,
    .stop = sensor_infrared_stop,
    .sample = sensor_infrared_sample,
};
