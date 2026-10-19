#include "widgets.h"

#include <math.h>
#include <stdio.h>

static float drag_start; // value when the owning drag began
static int last_tap_id;
static float last_tap_time;

bool w_claim(App *app, int id, float x, float y, float w, float h) {
    Touch *t = &app->touch;
    if (!app->touch_on || !t->down || t->owner) return false;
    if (!ui_hit(t->start_x, t->start_y, x, y, w, h)) return false;
    t->owner = id;
    return true;
}

bool w_owned(App *app, int id) { return app->touch_on && app->touch.owner == id && (app->touch.held || app->touch.down || app->touch.up); }

bool w_tap(App *app, int id, float x, float y, float w, float h) {
    w_claim(app, id, x, y, w, h);
    Touch *t = &app->touch;
    return app->touch_on && t->up && t->owner == id && !t->moved && ui_hit(t->x, t->y, x - 6, y - 6, w + 12, h + 12);
}

bool w_double_tap(App *app, int id, float x, float y, float w, float h) {
    Touch *t = &app->touch;
    if (!app->touch_on || !t->up || t->owner != id || t->moved || !ui_hit(t->x, t->y, x, y, w, h)) return false;
    bool dbl = last_tap_id == id && app->time - last_tap_time < 0.35f;
    last_tap_id = dbl ? 0 : id;
    last_tap_time = app->time;
    return dbl;
}

void w_panel(float x, float y, float w, float h, u32 edge) {
    ui_round_rect(x, y, w, h, 6, edge);
    ui_round_vgrad(x + 1, y + 1, w - 2, h - 2, 5, COL_PANEL2, COL_PANEL);
}

void w_label(App *app, const char *s, float x, float y, u32 c) { ui_text(app, s, x, y, 0.42f, c, ALIGN_LEFT); }

void w_button_draw(App *app, float x, float y, float w, float h, const char *label, Icon icon, u32 c, bool lit) {
    float r = h < 20 ? 4 : 6;
    if (lit) {
        ui_round_rect(x - 1.5f, y - 1.5f, w + 3, h + 3, r + 1, ui_alpha(c, 90)); // glow
        ui_round_rect(x, y, w, h, r, c);
        ui_round_vgrad(x + 2, y + 2, w - 4, h - 4, r - 1, ui_mix(c, RGB(255, 255, 255), 0.15f), ui_mix(c, RGB(0, 0, 0), 0.35f));
    } else {
        ui_round_rect(x, y, w, h, r, ui_mix(c, COL_BG, 0.25f));
        ui_round_vgrad(x + 1.5f, y + 1.5f, w - 3, h - 3, r - 1, RGB(26, 28, 38), RGB(12, 13, 19));
    }
    u32 fg = lit ? RGB(255, 255, 255) : ui_mix(c, RGB(255, 255, 255), 0.25f);
    float size = h < 20 ? 0.38f : h < 26 ? 0.44f : 0.5f;
    if (label && icon == ICON_NONE)
        while (size > 0.3f && ui_text_width(app, label, size) > w - 5) size -= 0.02f; // shrink to fit
    if (icon != ICON_NONE && label && *label) {
        float is = h * 0.5f, tw = ui_text_width(app, label, size), total = is + 4 + tw;
        float x0 = x + (w - total) / 2;
        ui_icon(icon, x0 + is / 2, y + h / 2, is, fg);
        ui_text(app, label, x0 + is + 4, y + (h - ui_line_height(size)) / 2 + 1, size, fg, ALIGN_LEFT);
    } else if (icon != ICON_NONE) {
        ui_icon(icon, x + w / 2, y + h / 2, h * 0.52f, fg);
    } else if (label) {
        ui_text_fit(app, label, x + w / 2, y + (h - ui_line_height(size)) / 2 + 1, size, fg, ALIGN_CENTER, w - 4);
    }
}

bool w_button(App *app, int id, float x, float y, float w, float h, const char *label, Icon icon, u32 c, bool lit) {
    bool fired = w_tap(app, id, x, y, w, h);
    bool pressed = w_owned(app, id) && app->touch.held && ui_hit(app->touch.x, app->touch.y, x, y, w, h);
    w_button_draw(app, x, y, w, h, label, icon, c, lit || pressed);
    return fired;
}

bool w_pad(App *app, int id, float x, float y, float w, float h, const char *label, Icon icon, u32 c, bool lit) {
    bool fired = w_claim(app, id, x, y, w, h);
    bool pressed = w_owned(app, id) && (app->touch.held || app->touch.down);
    w_button_draw(app, x, y, w, h, label, icon, c, lit || pressed);
    return fired;
}

#define KNOB_A0 (0.75f * (float)M_PI)
#define KNOB_SPAN (1.5f * (float)M_PI)

bool w_knob(App *app, int id, float cx, float cy, float r, float *v, float def, u32 c, const char *label) {
    float before = *v;
    if (w_claim(app, id, cx - r - 4, cy - r - 4, 2 * r + 8, 2 * r + 8)) drag_start = *v;
    if (w_owned(app, id)) {
        Touch *t = &app->touch;
        float d = (t->start_y - t->y) / 110.0f + (t->x - t->start_x) / 220.0f;
        *v = drag_start + d;
        if (*v < 0) *v = 0;
        if (*v > 1) *v = 1;
        if (w_double_tap(app, id, cx - r - 4, cy - r - 4, 2 * r + 8, 2 * r + 8)) *v = def;
    }
    // Body, track, value arc, pointer.
    ui_circle(cx, cy, r + 2, RGB(4, 5, 8));
    ui_arc(cx, cy, r + 1.5f, r - 1.5f, KNOB_A0, KNOB_A0 + KNOB_SPAN, COL_FAINT);
    float a = KNOB_A0 + KNOB_SPAN * *v, from = def == 0.5f ? KNOB_A0 + KNOB_SPAN / 2 : KNOB_A0;
    if (fabsf(a - from) > 0.02f) ui_arc(cx, cy, r + 2, r - 2, from, a, c);
    ui_circle(cx, cy, r - 3, RGB(44, 48, 60));
    ui_circle(cx, cy, r - 4.5f, RGB(24, 26, 34));
    ui_line(cx + cosf(a) * r * 0.2f, cy + sinf(a) * r * 0.2f, cx + cosf(a) * (r - 4), cy + sinf(a) * (r - 4), r > 14 ? 3 : 2, RGB(240, 242, 248));
    if (label) ui_text(app, label, cx, cy + r + 2, 0.36f, COL_DIM, ALIGN_CENTER);
    return *v != before;
}

static void fader_cap_v(float cx, float y, float w, u32 c) {
    ui_round_rect(cx - w / 2, y - 5, w, 10, 2, RGB(210, 214, 224));
    ui_rect(cx - w / 2 + 1, y - 1, w - 2, 2, c);
}

bool w_vfader(App *app, int id, float x, float y, float w, float h, float *v, float def, u32 c, bool center) {
    float before = *v, travel = h - 10;
    if (w_claim(app, id, x - 4, y - 4, w + 8, h + 8)) drag_start = *v;
    if (w_owned(app, id)) {
        *v = drag_start + (app->touch.start_y - app->touch.y) / travel;
        if (*v < 0) *v = 0;
        if (*v > 1) *v = 1;
        if (w_double_tap(app, id, x - 4, y - 4, w + 8, h + 8)) *v = def;
    }
    float cx = x + w / 2, top = y + 5, cap = top + travel * (1 - *v);
    for (int i = 0; i <= 10; i++) {
        float ty = top + travel * i / 10;
        float tl = (i % 5 == 0) ? w / 2 : w / 3;
        ui_rect(cx - w / 2 - 1, ty, tl * 0.5f, 1, COL_FAINT);
        ui_rect(cx + w / 2 + 1 - tl * 0.5f, ty, tl * 0.5f, 1, COL_FAINT);
    }
    ui_round_rect(cx - 2, top - 2, 4, travel + 4, 2, RGB(0, 0, 0));
    if (center) {
        float mid = top + travel / 2;
        ui_rect(cx - 1, cap < mid ? cap : mid, 2, fabsf(mid - cap), c);
        ui_rect(cx - w / 2, mid - 0.5f, w, 1, COL_DIM);
    } else {
        ui_rect(cx - 1, cap, 2, top + travel - cap, c);
    }
    fader_cap_v(cx, cap, w - 2, c);
    return *v != before;
}

bool w_hfader(App *app, int id, float x, float y, float w, float h, float *v, float def, u32 c, bool center) {
    float before = *v, travel = w - 12;
    if (w_claim(app, id, x - 4, y - 4, w + 8, h + 8)) drag_start = *v;
    if (w_owned(app, id)) {
        *v = drag_start + (app->touch.x - app->touch.start_x) / travel;
        if (*v < 0) *v = 0;
        if (*v > 1) *v = 1;
        if (w_double_tap(app, id, x - 4, y - 4, w + 8, h + 8)) *v = def;
    }
    float cy = y + h / 2, left = x + 6, cap = left + travel * *v;
    for (int i = 0; i <= 10; i++) {
        float tx = left + travel * i / 10, tl = (i % 5 == 0) ? h / 2 : h / 3.5f;
        ui_rect(tx, cy - h / 2 + 1, 1, tl * 0.5f, COL_FAINT);
        ui_rect(tx, cy + h / 2 - tl * 0.5f, 1, tl * 0.5f, COL_FAINT);
    }
    ui_round_rect(left - 2, cy - 2, travel + 4, 4, 2, RGB(0, 0, 0));
    if (center) {
        float mid = left + travel / 2;
        ui_rect(cap < mid ? cap : mid, cy - 1, fabsf(mid - cap), 2, c);
    } else {
        ui_rect(left, cy - 1, cap - left, 2, c);
    }
    ui_round_rect(cap - 5, cy - h / 2 + 2, 10, h - 4, 2, RGB(210, 214, 224));
    ui_rect(cap - 1, cy - h / 2 + 3, 2, h - 6, c);
    return *v != before;
}

bool w_toggle(App *app, int id, float x, float y, bool *on, u32 c) {
    bool changed = false;
    if (w_tap(app, id, x - 4, y - 4, 42, 26)) {
        *on = !*on;
        changed = true;
    }
    ui_round_rect(x, y, 34, 18, 9, *on ? c : RGB(52, 56, 70));
    ui_circle(*on ? x + 25 : x + 9, y + 9, 7, RGB(245, 246, 250));
    return changed;
}

void w_meter(float x, float y, float w, float h, float level, u32 c) {
    const int segs = 16;
    float sh = h / segs;
    float db = level > 0.0001f ? 20 * log10f(level) : -60; // -36..0 dB over the meter
    int lit = (int)((db + 36) / 36 * segs + 0.5f);
    for (int i = 0; i < segs; i++) {
        float sy = y + h - (i + 1) * sh;
        u32 on = i >= segs - 2 ? RGB(255, 70, 60) : i >= segs - 5 ? ui_mix(c, RGB(255, 255, 255), 0.35f) : c;
        ui_rect(x, sy + 1, w, sh - 1.5f, i < lit ? on : ui_mix(c, COL_BG, 0.85f));
    }
}
