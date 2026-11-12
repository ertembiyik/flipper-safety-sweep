#include <furi.h>
#include <math.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <notification/notification_messages.h>

#include "helpers/level.h"
#include "sensors/sensor.h"
#include "views/meter_view.h"

#define FEEDBACK_PERIOD_MS   50
#define FEEDBACK_VIBRO_FROM  85
#define FEEDBACK_VIBRO_REARM 75
#define WORKER_IDLE_MS       100
#define ZEROING_MS           2000
#define SAMPLE_FAILURES_MAX  10

static const Sensor* const pages[] = {
    &sensor_wifi,
    &sensor_radio,
    &sensor_chip,
};

typedef struct {
    Gui* gui;
    NotificationApp* notifications;
    ViewDispatcher* view_dispatcher;
    MeterView* meter_view;
    FuriThread* worker;
    FuriTimer* feedback_timer;

    volatile bool running;
    volatile uint8_t requested_page;
    volatile bool zero_requested;
    /** Read by the feedback timer; 0 whenever no live reading exists */
    volatile uint8_t percent;
    float click_phase;
    bool vibro_armed;
} SafetySweepApp;

static const NotificationSequence sequence_click = {
    &message_note_c7,
    &message_delay_10,
    &message_sound_off,
    NULL,
};

static void safety_sweep_show_page(SafetySweepApp* app, uint8_t page, const char* reason) {
    with_view_model(
        meter_view_get_view(app->meter_view),
        MeterModel * model,
        {
            model->page = page;
            model->page_count = COUNT_OF(pages);
            model->title = pages[page]->title;
            model->reason = reason;
            model->state = reason ? MeterStateUnavailable : MeterStateZeroing;
            model->percent = 0;
            model->peak = 0;
        },
        true);
}

typedef enum {
    WorkerPhaseUnavailable,
    WorkerPhaseZeroing,
    WorkerPhaseLive,
} WorkerPhase;

static int32_t safety_sweep_worker(void* context) {
    SafetySweepApp* app = context;
    const Sensor* sensor = NULL;
    WorkerPhase phase = WorkerPhaseUnavailable;
    uint8_t page = UINT8_MAX;
    Level level;
    float value;
    uint32_t zeroing_start = 0;
    float zeroing_max = 0.0f;
    uint8_t failed_samples = 0;

    while(app->running) {
        if(page != app->requested_page) {
            app->percent = 0;
            if(phase != WorkerPhaseUnavailable) sensor->stop();
            page = app->requested_page;
            sensor = pages[page];

            safety_sweep_show_page(app, page, NULL);
            const char* reason = sensor->start();
            if(reason) {
                phase = WorkerPhaseUnavailable;
                safety_sweep_show_page(app, page, reason);
            } else {
                level_reset(&level, sensor->span);
                phase = WorkerPhaseZeroing;
                app->zero_requested = true;
            }
        }

        if(phase == WorkerPhaseUnavailable) {
            app->zero_requested = false;
            furi_delay_ms(WORKER_IDLE_MS);
            continue;
        }

        if(app->zero_requested) {
            app->zero_requested = false;
            app->percent = 0;
            phase = WorkerPhaseZeroing;
            zeroing_max = -INFINITY;
            zeroing_start = furi_get_tick();
            failed_samples = 0;
            safety_sweep_show_page(app, page, NULL);
        }

        if(!sensor->sample(&value)) {
            if(++failed_samples < SAMPLE_FAILURES_MAX) {
                furi_delay_ms(WORKER_IDLE_MS);
                continue;
            }
            sensor->stop();
            phase = WorkerPhaseUnavailable;
            app->percent = 0;
            safety_sweep_show_page(app, page, "No reading from\nthis sensor.");
            continue;
        }
        failed_samples = 0;

        if(phase == WorkerPhaseZeroing) {
            if(value > zeroing_max) zeroing_max = value;
            if(furi_get_tick() - zeroing_start < furi_ms_to_ticks(ZEROING_MS)) continue;
            // The loudest thing heard while standing still becomes the room's own level
            level_set_zero(&level, zeroing_max);
            phase = WorkerPhaseLive;
        }

        uint8_t percent = level_update(&level, value, furi_get_tick());
        app->percent = percent;
        with_view_model(
            meter_view_get_view(app->meter_view),
            MeterModel * model,
            {
                model->state = MeterStateLive;
                model->percent = percent;
                model->peak = level.peak;
            },
            true);
    }

    if(phase != WorkerPhaseUnavailable) sensor->stop();
    return 0;
}

static void safety_sweep_feedback(void* context) {
    SafetySweepApp* app = context;
    uint8_t percent = app->percent;

    if(percent < FEEDBACK_VIBRO_REARM) app->vibro_armed = true;
    if(percent >= FEEDBACK_VIBRO_FROM && app->vibro_armed) {
        app->vibro_armed = false;
        notification_message(app->notifications, &sequence_single_vibro);
    }

    if(percent < METER_QUIET_BELOW) {
        app->click_phase = 0.0f;
        return;
    }
    float strength = (float)(percent - METER_QUIET_BELOW) / (100 - METER_QUIET_BELOW);
    float clicks_per_second = 1.0f + strength * strength * 15.0f;
    app->click_phase += clicks_per_second * FEEDBACK_PERIOD_MS / 1000.0f;
    if(app->click_phase >= 1.0f) {
        app->click_phase -= 1.0f;
        notification_message(app->notifications, &sequence_click);
    }
}

static void safety_sweep_action(MeterAction action, void* context) {
    SafetySweepApp* app = context;
    switch(action) {
    case MeterActionPrevious:
        app->requested_page = (app->requested_page + COUNT_OF(pages) - 1) % COUNT_OF(pages);
        break;
    case MeterActionNext:
        app->requested_page = (app->requested_page + 1) % COUNT_OF(pages);
        break;
    case MeterActionZero:
        app->zero_requested = true;
        break;
    }
}

static bool safety_sweep_navigation(void* context) {
    SafetySweepApp* app = context;
    view_dispatcher_stop(app->view_dispatcher);
    return true;
}

int32_t safety_sweep_app(void* p) {
    UNUSED(p);
    SafetySweepApp* app = malloc(sizeof(SafetySweepApp));
    app->gui = furi_record_open(RECORD_GUI);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);

    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, safety_sweep_navigation);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    app->meter_view = meter_view_alloc(safety_sweep_action, app);
    view_dispatcher_add_view(app->view_dispatcher, 0, meter_view_get_view(app->meter_view));
    view_dispatcher_switch_to_view(app->view_dispatcher, 0);

    app->running = true;
    app->worker = furi_thread_alloc_ex("SafetySweepWorker", 2048, safety_sweep_worker, app);
    furi_thread_start(app->worker);
    app->feedback_timer = furi_timer_alloc(safety_sweep_feedback, FuriTimerTypePeriodic, app);
    furi_timer_start(app->feedback_timer, furi_ms_to_ticks(FEEDBACK_PERIOD_MS));
    notification_message(app->notifications, &sequence_display_backlight_enforce_on);

    view_dispatcher_run(app->view_dispatcher);

    notification_message(app->notifications, &sequence_display_backlight_enforce_auto);
    furi_timer_stop(app->feedback_timer);
    furi_timer_free(app->feedback_timer);
    app->running = false;
    furi_thread_join(app->worker);
    furi_thread_free(app->worker);

    view_dispatcher_remove_view(app->view_dispatcher, 0);
    meter_view_free(app->meter_view);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}
