#include "sensor.h"

#include <furi.h>
#include <furi_hal.h>
#include <lib/subghz/devices/cc1101_configs.h>

#define CC1101_REG_MDMCFG4 0x10
#define SWEEP_CHUNK_POINTS 32
#define SWEEP_SETTLE_MS    2
#define SWEEP_RSSI_UNSET   INT8_MIN

typedef struct {
    uint32_t start;
    uint32_t stop;
} SweepBand;

typedef struct {
    const SweepBand* bands;
    size_t band_count;
    uint32_t step;
    /** Receiver bandwidth and data rate exponent, see CC1101 datasheet table 26 */
    uint8_t mdmcfg4;
} SweepConfig;

static const SweepBand radio_bands[] = {
    {300000000, 348000000},
    {387000000, 464000000},
    {779000000, 928000000},
};

static const SweepConfig radio_config = {
    .bands = radio_bands,
    .band_count = COUNT_OF(radio_bands),
    .step = 800000,
    .mdmcfg4 = 0x17, // 650 kHz
};

// DDR2/DDR3 memory clocks that recording cameras radiate (ESauron, USENIX Security 2024)
static const SweepBand chip_bands[] = {
    {398000000, 402000000},
    {794000000, 806000000},
    {844000000, 856000000},
};

static const SweepConfig chip_config = {
    .bands = chip_bands,
    .band_count = COUNT_OF(chip_bands),
    .step = 100000,
    .mdmcfg4 = 0xC7, // 102 kHz
};

static struct {
    const SweepConfig* config;
    uint8_t* preset;
    int8_t* rssi;
    size_t point_count;
    size_t cursor;
} sweep;

static size_t sweep_band_points(const SweepConfig* config, size_t band) {
    return (config->bands[band].stop - config->bands[band].start) / config->step + 1;
}

static uint32_t sweep_point_frequency(size_t point) {
    for(size_t band = 0; band < sweep.config->band_count; band++) {
        size_t points = sweep_band_points(sweep.config, band);
        if(point < points) return sweep.config->bands[band].start + point * sweep.config->step;
        point -= points;
    }
    furi_crash("sweep point out of range");
}

static uint8_t* sweep_preset_alloc(uint8_t mdmcfg4) {
    const uint8_t* base = subghz_device_cc1101_preset_ook_650khz_async_regs;
    size_t registers_size = 0;
    while(base[registers_size])
        registers_size += 2;
    // register pairs, a zero pair terminator, then the 8 byte PA table
    size_t size = registers_size + 2 + 8;

    uint8_t* preset = malloc(size);
    memcpy(preset, base, size);
    for(size_t i = 0; i < registers_size; i += 2) {
        if(preset[i] == CC1101_REG_MDMCFG4) preset[i + 1] = mdmcfg4;
    }
    return preset;
}

static const char* sweep_start(const SweepConfig* config) {
    sweep.config = config;
    sweep.point_count = 0;
    for(size_t band = 0; band < config->band_count; band++) {
        sweep.point_count += sweep_band_points(config, band);
    }
    sweep.rssi = malloc(sweep.point_count);
    memset(sweep.rssi, SWEEP_RSSI_UNSET, sweep.point_count);
    sweep.cursor = 0;
    sweep.preset = sweep_preset_alloc(config->mdmcfg4);

    furi_hal_power_suppress_charge_enter();
    furi_hal_subghz_load_custom_preset(sweep.preset);
    return NULL;
}

static const char* sensor_radio_start(void) {
    return sweep_start(&radio_config);
}

static const char* sensor_chip_start(void) {
    return sweep_start(&chip_config);
}

static void sensor_subghz_stop(void) {
    furi_hal_subghz_idle();
    furi_hal_subghz_sleep();
    furi_hal_power_suppress_charge_exit();
    free(sweep.rssi);
    free(sweep.preset);
    sweep.rssi = NULL;
    sweep.preset = NULL;
}

static bool sensor_subghz_sample(float* value) {
    for(size_t i = 0; i < SWEEP_CHUNK_POINTS; i++) {
        furi_hal_subghz_idle();
        furi_hal_subghz_set_frequency_and_path(sweep_point_frequency(sweep.cursor));
        furi_hal_subghz_rx();
        furi_delay_ms(SWEEP_SETTLE_MS);
        // The HAL can report down to -138 dBm, which does not fit int8_t
        sweep.rssi[sweep.cursor] = (int8_t)MAX(furi_hal_subghz_get_rssi(), SWEEP_RSSI_UNSET + 1);
        sweep.cursor = (sweep.cursor + 1) % sweep.point_count;
    }

    size_t best = 0;
    for(size_t point = 1; point < sweep.point_count; point++) {
        if(sweep.rssi[point] > sweep.rssi[best]) best = point;
    }
    if(sweep.rssi[best] == SWEEP_RSSI_UNSET) return false;

    *value = sweep.rssi[best];
    return true;
}

const Sensor sensor_radio = {
    .title = "Radio",
    .span = 30.0f,
    .start = sensor_radio_start,
    .stop = sensor_subghz_stop,
    .sample = sensor_subghz_sample,
};

const Sensor sensor_chip = {
    .title = "Chip",
    .span = 25.0f,
    .start = sensor_chip_start,
    .stop = sensor_subghz_stop,
    .sample = sensor_subghz_sample,
};
