#include "sensor.h"

#include <furi.h>
#include <furi_hal_bt.h>
#include <bt/bt_service/bt.h>

#define WIFI_CHANNEL_STEP 2
#define WIFI_POINT_COUNT  (40 / WIFI_CHANNEL_STEP)
#define WIFI_CHUNK_POINTS 5
// The radio refreshes its RSSI value about every 25 ms
#define WIFI_DWELL_MS     30
#define WIFI_RSSI_UNSET   INT8_MIN
#define WIFI_RSSI_NONE    127
#define HCI_OGF_VENDOR    0x3f
#define ACI_HAL_READ_RSSI 0x022

struct hci_request {
    uint16_t ogf;
    uint16_t ocf;
    int event;
    void* cparam;
    int clen;
    void* rparam;
    int rlen;
};

typedef int (*HciSendReq)(struct hci_request* request, uint8_t async);

/**
 * furi_hal_bt_get_rssi uses a command the radio stack no longer has, and the firmware
 * does not export hci_send_req, so the working command is sent through the function's
 * address in known firmware builds. Add a build by reading it from that build's
 * firmware.elf: arm-none-eabi-nm firmware.elf | grep hci_send_req
 */
static const struct {
    const char* firmware_commit;
    uint32_t hci_send_req;
    /** The function's first bytes; a rebuild of the same commit can move it */
    uint8_t signature[16];
} wifi_firmware_builds[] = {
    {
        // official 1.5.1
        "9e810dc3",
        0x0801ae5c,
        {0x2d,
         0xe9,
         0xf7,
         0x43,
         0x2e,
         0x4e,
         0x33,
         0x68,
         0x04,
         0x46,
         0x0b,
         0xb1,
         0x00,
         0x20,
         0x98,
         0x47},
    },
};

static struct {
    HciSendReq hci_send_req;
    Bt* bt;
    bool was_advertising;
    int8_t rssi[WIFI_POINT_COUNT];
    uint8_t cursor;
} wifi;

static HciSendReq wifi_find_hci_send_req(void) {
    const char* commit = version_get_githash(NULL);
    for(size_t i = 0; i < COUNT_OF(wifi_firmware_builds); i++) {
        uint32_t address = wifi_firmware_builds[i].hci_send_req;
        if(strcmp(commit, wifi_firmware_builds[i].firmware_commit) != 0) continue;
        if(memcmp(
               (const void*)address,
               wifi_firmware_builds[i].signature,
               sizeof(wifi_firmware_builds[i].signature)) != 0)
            continue;
        // Bit 0 selects Thumb mode
        return (HciSendReq)(address | 1);
    }
    return NULL;
}

static bool wifi_read_rssi(int8_t* rssi) {
    struct {
        uint8_t status;
        int8_t rssi;
    } response = {.status = 0xFF};
    struct hci_request request = {
        .ogf = HCI_OGF_VENDOR,
        .ocf = ACI_HAL_READ_RSSI,
        .rparam = &response,
        .rlen = sizeof(response),
    };
    if(wifi.hci_send_req(&request, false) < 0) return false;
    if(response.status != 0 || response.rssi == WIFI_RSSI_NONE) return false;
    *rssi = response.rssi;
    return true;
}

static const char* sensor_wifi_start(void) {
    wifi.hci_send_req = wifi_find_hci_send_req();
    if(!wifi.hci_send_req) return "Not set up for this\nfirmware version.\nSee README.";

    wifi.bt = furi_record_open(RECORD_BT);
    // bt_disconnect stops advertising itself, so ask first
    wifi.was_advertising = furi_hal_bt_is_active();
    bt_disconnect(wifi.bt);
    furi_hal_bt_stop_advertising();
    memset(wifi.rssi, WIFI_RSSI_UNSET, sizeof(wifi.rssi));
    wifi.cursor = 0;
    return NULL;
}

static void sensor_wifi_stop(void) {
    if(wifi.was_advertising) furi_hal_bt_start_advertising();
    furi_record_close(RECORD_BT);
    wifi.bt = NULL;
}

static bool sensor_wifi_sample(float* value) {
    for(uint8_t i = 0; i < WIFI_CHUNK_POINTS; i++) {
        int8_t best = WIFI_RSSI_UNSET;
        furi_hal_bt_start_packet_rx(wifi.cursor * WIFI_CHANNEL_STEP, 1);
        uint32_t start = furi_get_tick();
        while(furi_get_tick() - start < furi_ms_to_ticks(WIFI_DWELL_MS)) {
            int8_t rssi;
            if(wifi_read_rssi(&rssi) && rssi > best) best = rssi;
            // Back-to-back reads with no pause crash the radio core
            furi_delay_ms(2);
        }
        furi_hal_bt_stop_packet_test();
        wifi.rssi[wifi.cursor] = best;
        wifi.cursor = (wifi.cursor + 1) % WIFI_POINT_COUNT;
    }

    int8_t best = WIFI_RSSI_UNSET;
    for(uint8_t point = 0; point < WIFI_POINT_COUNT; point++) {
        if(wifi.rssi[point] > best) best = wifi.rssi[point];
    }
    if(best == WIFI_RSSI_UNSET) return false;
    *value = best;
    return true;
}

const Sensor sensor_wifi = {
    .title = "Wi-Fi 2.4",
    .span = 35.0f,
    .start = sensor_wifi_start,
    .stop = sensor_wifi_stop,
    .sample = sensor_wifi_sample,
};
