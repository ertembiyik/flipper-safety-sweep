#pragma once

#include <gui/view.h>

/** Below this the dial reads as the room's own noise: no verdict, no clicks */
#define METER_QUIET_BELOW 15

typedef enum {
    MeterStateZeroing,
    MeterStateLive,
    MeterStateUnavailable,
} MeterState;

typedef struct {
    MeterState state;
    const char* title;
    /** Shown in MeterStateUnavailable */
    const char* reason;
    uint8_t page;
    uint8_t page_count;
    uint8_t percent;
    uint8_t peak;
    /** Where the needle is drawn; it chases `percent` like a sprung pointer */
    float needle;
    float needle_velocity;
} MeterModel;

typedef enum {
    MeterActionPrevious,
    MeterActionNext,
    MeterActionZero,
} MeterAction;

typedef void (*MeterActionCallback)(MeterAction action, void* context);

typedef struct MeterView MeterView;

MeterView* meter_view_alloc(MeterActionCallback callback, void* context);
void meter_view_free(MeterView* meter_view);
View* meter_view_get_view(MeterView* meter_view);
