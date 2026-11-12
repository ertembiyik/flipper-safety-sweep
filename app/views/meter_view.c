#include "meter_view.h"

#include <furi.h>
#include <gui/elements.h>
#include <math.h>

#define DIAL_X          38
#define DIAL_Y          59
#define DIAL_RADIUS     36
#define DIAL_HALF_SWEEP 60.0f
#define DIAL_HOT_FROM   60
#define DIAL_TICKS      10
#define COLUMN_X        80

#define NEEDLE_FRAME_MS  33
#define NEEDLE_STIFFNESS 140.0f
// Slightly under critical damping: the needle overshoots a touch, as a real one does
#define NEEDLE_DAMPING   17.0f
#define NEEDLE_REST      0.05f

struct MeterView {
    View* view;
    FuriTimer* needle_timer;
    MeterActionCallback callback;
    void* context;
};

typedef struct {
    const char* line1;
    const char* line2;
} MeterText;

static MeterText meter_text(const MeterModel* model) {
    if(model->state == MeterStateZeroing) return (MeterText){"Hold still", "learning"};
    if(model->percent < METER_QUIET_BELOW) return (MeterText){"Nothing", "unusual"};
    if(model->percent < DIAL_HOT_FROM) return (MeterText){"Something", "is nearby"};
    return (MeterText){"Very close", "look here"};
}

typedef struct {
    int32_t x;
    int32_t y;
} DialPoint;

static DialPoint dial_point(float percent, float radius) {
    float angle = (percent / 50.0f - 1.0f) * DIAL_HALF_SWEEP * (float)M_PI / 180.0f;
    return (DialPoint){
        .x = DIAL_X + lroundf(radius * sinf(angle)),
        .y = DIAL_Y - lroundf(radius * cosf(angle)),
    };
}

static void dial_draw_arc(Canvas* canvas, uint8_t from, uint8_t to, float radius) {
    DialPoint previous = dial_point(from, radius);
    for(uint8_t percent = from + 2; percent <= to; percent += 2) {
        DialPoint next = dial_point(percent, radius);
        canvas_draw_line(canvas, previous.x, previous.y, next.x, next.y);
        previous = next;
    }
}

static void meter_draw_dial(Canvas* canvas, const MeterModel* model) {
    dial_draw_arc(canvas, 0, 100, DIAL_RADIUS);
    dial_draw_arc(canvas, DIAL_HOT_FROM, 100, DIAL_RADIUS - 1);
    dial_draw_arc(canvas, DIAL_HOT_FROM, 100, DIAL_RADIUS - 2);

    for(uint8_t tick = 0; tick <= DIAL_TICKS; tick++) {
        float at = tick * 100.0f / DIAL_TICKS;
        DialPoint inner = dial_point(at, DIAL_RADIUS);
        DialPoint outer = dial_point(at, DIAL_RADIUS + (tick % 5 == 0 ? 5 : 3));
        canvas_draw_line(canvas, inner.x, inner.y, outer.x, outer.y);
    }

    float needle = CLAMP(model->needle, 104.0f, -4.0f);
    DialPoint tip = dial_point(needle, DIAL_RADIUS - 4);
    canvas_draw_line(canvas, DIAL_X, DIAL_Y, tip.x, tip.y);
    canvas_draw_disc(canvas, DIAL_X, DIAL_Y, 2);

    if(model->peak > model->percent) {
        DialPoint mark = dial_point(model->peak, DIAL_RADIUS + 8);
        canvas_draw_disc(canvas, mark.x, mark.y, 1);
    }
}

static void meter_draw_pages(Canvas* canvas, const MeterModel* model) {
    for(uint8_t page = 0; page < model->page_count; page++) {
        int32_t x = COLUMN_X + page * 5;
        if(page == model->page) {
            canvas_draw_box(canvas, x, 59, 3, 3);
        } else {
            canvas_draw_dot(canvas, x + 1, 60);
        }
    }
}

static void meter_view_draw(Canvas* canvas, void* context) {
    const MeterModel* model = context;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, COLUMN_X, 9, model->title);
    meter_draw_pages(canvas, model);

    if(model->state == MeterStateUnavailable) {
        canvas_set_font(canvas, FontSecondary);
        elements_multiline_text_aligned(canvas, 2, 22, AlignLeft, AlignTop, model->reason);
        return;
    }

    meter_draw_dial(canvas, model);
    if(model->state == MeterStateLive) {
        char number[4];
        snprintf(number, sizeof(number), "%u", model->percent);
        canvas_set_font(canvas, FontBigNumbers);
        canvas_draw_str(canvas, COLUMN_X, 31, number);
    }
    MeterText text = meter_text(model);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, COLUMN_X, 43, text.line1);
    canvas_draw_str(canvas, COLUMN_X, 52, text.line2);
}

static void meter_view_needle_tick(void* context) {
    MeterView* meter_view = context;
    const float dt = NEEDLE_FRAME_MS / 1000.0f;
    bool moved = false;

    MeterModel* model = view_get_model(meter_view->view);
    float target = model->state == MeterStateLive ? model->percent : 0.0f;
    float offset = target - model->needle;
    if(fabsf(offset) > NEEDLE_REST || fabsf(model->needle_velocity) > NEEDLE_REST) {
        model->needle_velocity +=
            (NEEDLE_STIFFNESS * offset - NEEDLE_DAMPING * model->needle_velocity) * dt;
        model->needle += model->needle_velocity * dt;
        moved = true;
    } else if(model->needle != target) {
        model->needle = target;
        model->needle_velocity = 0.0f;
        moved = true;
    }
    view_commit_model(meter_view->view, moved);
}

static void meter_view_enter(void* context) {
    MeterView* meter_view = context;
    furi_timer_start(meter_view->needle_timer, furi_ms_to_ticks(NEEDLE_FRAME_MS));
}

static void meter_view_exit(void* context) {
    MeterView* meter_view = context;
    furi_timer_stop(meter_view->needle_timer);
}

static bool meter_view_input(InputEvent* event, void* context) {
    MeterView* meter_view = context;
    if(event->type != InputTypeShort) return false;

    switch(event->key) {
    case InputKeyLeft:
        meter_view->callback(MeterActionPrevious, meter_view->context);
        return true;
    case InputKeyRight:
        meter_view->callback(MeterActionNext, meter_view->context);
        return true;
    case InputKeyOk:
        meter_view->callback(MeterActionZero, meter_view->context);
        return true;
    default:
        return false;
    }
}

MeterView* meter_view_alloc(MeterActionCallback callback, void* context) {
    MeterView* meter_view = malloc(sizeof(MeterView));
    meter_view->callback = callback;
    meter_view->context = context;
    meter_view->view = view_alloc();
    view_allocate_model(meter_view->view, ViewModelTypeLocking, sizeof(MeterModel));
    view_set_context(meter_view->view, meter_view);
    view_set_draw_callback(meter_view->view, meter_view_draw);
    view_set_input_callback(meter_view->view, meter_view_input);
    view_set_enter_callback(meter_view->view, meter_view_enter);
    view_set_exit_callback(meter_view->view, meter_view_exit);
    meter_view->needle_timer =
        furi_timer_alloc(meter_view_needle_tick, FuriTimerTypePeriodic, meter_view);
    return meter_view;
}

void meter_view_free(MeterView* meter_view) {
    furi_timer_stop(meter_view->needle_timer);
    furi_timer_free(meter_view->needle_timer);
    view_free(meter_view->view);
    free(meter_view);
}

View* meter_view_get_view(MeterView* meter_view) {
    return meter_view->view;
}
