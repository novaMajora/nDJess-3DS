// The panel view: header, page tabs and the Mixer, EQ, Effects, Sampler and Record pages.
// (Library and Settings live in browser.c and settings.c.)
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "app.h"
#include "widgets.h"

#define ID(i) WID(0x200000, i)
#define WHITE RGB(255, 255, 255)
#define Y0 46 // top of the page content

// ---------------------------------------------------------------------------------------------
// Header: logo, date, time, battery (no Wi-Fi)

static float clock_draw(App *app, float right, float y, float size, u32 color) {
    static const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    float x = right, bh = 10, bw = 20, by = y + ui_line_height(size) / 2 - bh / 2;
    static u8 level = 100;
    static u64 last_read;
    u64 now_ms = osGetTime();
    if (!last_read || now_ms - last_read > 5000) { // the battery changes slowly
        last_read = now_ms;
        u8 l = 0;
        if (R_FAILED(MCUHWC_GetBatteryLevel(&l)) || l == 0) {
            u8 lv = 0;
            if (R_SUCCEEDED(PTMU_GetBatteryLevel(&lv))) l = lv * 20;
        }
        level = l ? l : 100;
    }
    x -= bw + 3;
    ui_round_frame(x, by, bw, bh, 2, 1.5f, color);
    ui_rect(x + bw, by + 3, 2, bh - 6, color);
    u32 fill = level > 20 ? RGB(60, 220, 255) : RGB(255, 70, 60);
    ui_rect(x + 2.5f, by + 2.5f, (bw - 5) * level / 100.0f, bh - 5, fill);
    x -= 9;
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    if (tm) {
        char buf[32];
        int h12 = tm->tm_hour % 12 ? tm->tm_hour % 12 : 12;
        snprintf(buf, sizeof buf, "%d:%02d", h12, tm->tm_min);
        x -= ui_text(app, buf, x, y, size, color, ALIGN_RIGHT) + 10;
        snprintf(buf, sizeof buf, "%d/%d (%s)", tm->tm_mon + 1, tm->tm_mday, days[tm->tm_wday % 7]);
        x -= ui_text(app, buf, x, y, size, color, ALIGN_RIGHT);
    }
    return x;
}

void header_draw(App *app, float W) {
    ui_vgrad(0, 0, W, 20, RGB(20, 22, 34), RGB(8, 9, 14));
    ui_hgrad(0, 20, W / 2, 1, ui_alpha(deck_color(app, 0), 200), ui_alpha(deck_color(app, 0), 40));
    ui_hgrad(W / 2, 20, W / 2, 1, ui_alpha(deck_color(app, 1), 40), ui_alpha(deck_color(app, 1), 200));
    // Equalizer bars that dance with the master level.
    float l, r;
    engine_meters(&l, &r);
    for (int i = 0; i < 8; i++) {
        float lev = i < 4 ? l : r;
        float h = 3 + 11 * fminf(1, lev * (0.6f + 0.4f * fabsf(sinf(app->time * (3 + i) + i * 1.7f))));
        ui_rect(5 + i * 3.5f, 17 - h, 2.5f, h, ui_mix(deck_color(app, 0), deck_color(app, 1), i / 7.0f));
    }
    float x = 36;
    x += ui_text(app, "nDJ", x, 1, 0.6f, deck_color(app, 0), ALIGN_LEFT);
    ui_text(app, "ess", x, 1, 0.6f, deck_color(app, 1), ALIGN_LEFT);
    clock_draw(app, W - 4, 3, 0.42f, COL_TEXT);
}

// ---------------------------------------------------------------------------------------------
// Tabs

static const struct { const char *name; Icon icon; } TABS[PAGE_COUNT] = {
    {"Music", ICON_NOTE}, {"Mixer", ICON_MIXER}, {"EQ", ICON_EQ}, {"FX", ICON_FX},
    {"Pads", ICON_PADS}, {"Rec", ICON_RECORD}, {NULL, ICON_SETTINGS},
};

static void tabs_draw(App *app, float W) {
    float y = 23, h = 20, gap = 2, last = 26;
    float tw = (W - 4 - last - gap * (PAGE_COUNT - 1)) / (PAGE_COUNT - 1);
    float x = 2;
    for (int p = 0; p < PAGE_COUNT; p++) {
        float w = TABS[p].name ? tw : last;
        u32 c = p == PAGE_RECORD && engine_recording(NULL) ? RGB(255, 50, 50) : deck_color(app, 0);
        if (w_button(app, ID(p), x, y, w, h, TABS[p].name, TABS[p].icon, c, app->cfg.page == p)) app_set_page(app, p);
        x += w + gap;
    }
}

// ---------------------------------------------------------------------------------------------
// Mixer

static void crossfader(App *app, float W, float y) {
    MixControls *m = engine_mix();
    float fw = W * 0.46f, fx = W / 2 - fw / 2;
    ui_text(app, "A", fx - 9, y + 3, 0.42f, deck_color(app, 0), ALIGN_CENTER);
    ui_text(app, "B", fx + fw + 9, y + 3, 0.42f, deck_color(app, 1), ALIGN_CENTER);
    w_hfader(app, ID(0), fx, y, fw, 24, &m->crossfader, 0.5f, WHITE, true);
}

static void page_mixer(App *app, float W) {
    float s = W / 320.0f;
    for (int d = 0; d < DECKS; d++) {
        u32 c = deck_color(app, d);
        DeckControls *ctl = engine_controls(d);
#define MX(xa) (d == 0 ? (xa) * s : W - (xa) * s)
        ui_text(app, d == 0 ? "Deck A" : "Deck B", MX(8), Y0, 0.46f, c, d == 0 ? ALIGN_LEFT : ALIGN_RIGHT);
        float f = (ctl->filter + 1) / 2;
        if (w_knob(app, ID(d), MX(36), Y0 + 62, 20, &f, 0.5f, c, "Filter")) ctl->filter = f * 2 - 1;
        if (w_pad(app, ID(d), MX(36) - 20, Y0 + 106, 40, 24, NULL, ICON_HEADPHONES, c, ctl->pfl)) ctl->pfl = !ctl->pfl;
        static const char *bands[3] = {"High", "Mid", "Low"};
        for (int b = 0; b < 3; b++) w_knob(app, ID(d * 4 + b), MX(88), Y0 + 26 + b * 46, 13, &ctl->eq[2 - b], 0.5f, c, bands[b]);
        w_vfader(app, ID(d), MX(122) - 9, Y0 + 14, 18, 128, &ctl->volume, 0.85f, c, false);
        w_meter(MX(140) - 3, Y0 + 16, 6, 124, app->deck[d].peak, c);
#undef MX
    }
    // Master meters and volume.
    float l, r;
    engine_meters(&l, &r);
    ui_text(app, "Master", W / 2, Y0, 0.42f, COL_TEXT, ALIGN_CENTER);
    w_meter(W / 2 - 11, Y0 + 18, 8, 100, l, deck_color(app, 0));
    w_meter(W / 2 + 3, Y0 + 18, 8, 100, r, deck_color(app, 1));
    if (w_knob(app, ID(0), W / 2, Y0 + 136, 12, &app->cfg.master, 0.85f, WHITE, NULL)) app_config_changed(app);
    crossfader(app, W, 212);
}

// ---------------------------------------------------------------------------------------------
// EQ: big band faders with kills, gain and filter

static void page_eq(App *app, float W) {
    float half = W / 2;
    static const char *bands[3] = {"LOW", "MID", "HIGH"};
    for (int d = 0; d < DECKS; d++) {
        u32 c = deck_color(app, d);
        DeckControls *ctl = engine_controls(d);
        float hx = d * half, s = half / 160.0f;
        ui_text(app, d == 0 ? "Deck A" : "Deck B", hx + 8, Y0, 0.46f, c, ALIGN_LEFT);
        if (engine_mix()->auto_gain) ui_text(app, "AUTO", hx + half - 8, Y0 + 2, 0.34f, COL_DIM, ALIGN_RIGHT);
        w_knob(app, ID(d), hx + 26 * s, Y0 + 44, 15, &ctl->trim, 0.5f, c, "Gain");
        w_meter(hx + 22 * s, Y0 + 80, 8, 80, app->deck[d].peak, c);
        for (int b = 0; b < 3; b++) {
            float fx = hx + (56 + b * 34) * s;
            w_vfader(app, ID(d * 4 + b), fx, Y0 + 16, 16, 112, &ctl->eq[b], 0.5f, ctl->kill[b] ? COL_FAINT : c, true);
            ui_text(app, bands[b], fx + 8, Y0 + 130, 0.34f, COL_DIM, ALIGN_CENTER);
            if (w_pad(app, ID(d * 4 + b), fx - 7, Y0 + 146, 30, 20, "KILL", ICON_NONE, c, ctl->kill[b])) ctl->kill[b] = !ctl->kill[b];
        }
        ui_text(app, "Filter", hx + 8, Y0 + 172, 0.34f, COL_DIM, ALIGN_LEFT);
        float f = (ctl->filter + 1) / 2;
        if (w_hfader(app, ID(d), hx + 40 * s, Y0 + 168, half - 50 * s, 20, &f, 0.5f, c, true)) ctl->filter = f * 2 - 1;
    }
    ui_rect(half - 0.5f, Y0 + 4, 1, 186, COL_EDGE);
}

// ---------------------------------------------------------------------------------------------
// Effects: zoomed waveforms, hot cues, loops and FX

static const char *FX_NAMES[FX_COUNT] = {"Echo", "Flanger", "Crush", "Gate"};

static void stepper(App *app, int id, float x, float y, float w, const char *value, int *dir, u32 c) {
    *dir = 0;
    if (w_pad(app, id, x, y, 22, 24, "<", ICON_NONE, c, false)) *dir = -1;
    if (w_pad(app, id + 1, x + w - 22, y, 22, 24, ">", ICON_NONE, c, false)) *dir = 1;
    ui_round_rect(x + 24, y, w - 48, 24, 4, RGB(8, 9, 14));
    ui_text_fit(app, value, x + w / 2, y + 4, 0.44f, COL_TEXT, ALIGN_CENTER, w - 52);
}

static void page_fx(App *app, float W) {
    // Waveforms with zoom buttons.
    if (w_pad(app, ID(0), 2, Y0, 20, 50, "-", ICON_NONE, COL_DIM, false) && app->wave_zoom < 16) app->wave_zoom *= 1.5f;
    if (w_pad(app, ID(0), W - 22, Y0, 20, 50, "+", ICON_NONE, COL_DIM, false) && app->wave_zoom > 1.5f) app->wave_zoom /= 1.5f;
    for (int d = 0; d < DECKS; d++) deck_wave_h(app, d, 25, Y0 + d * 26, W - 50, 24, app->wave_zoom);

    int d = app->fx_deck;
    u32 c = deck_color(app, d);
    for (int i = 0; i < DECKS; i++)
        if (w_button(app, ID(i), 4 + i * (W / 2 - 2), Y0 + 54, W / 2 - 6, 18, i == 0 ? "Deck A" : "Deck B", ICON_NONE, deck_color(app, i), d == i)) app->fx_deck = i;
    const DeckState *s = &app->deck[d];
    DeckControls *ctl = engine_controls(d);
    float top = Y0 + 76, col = (W - 16) / 3;

    // Hot cues: tap to set or jump, hold to clear.
    float x = 4;
    w_label(app, "HOT CUE", x, top, COL_DIM);
    for (int i = 0; i < HOTCUES; i++) {
        float bw = (col - 4) / 2, bx = x + (i % 2) * (bw + 4), by = top + 16 + (i / 2) * 40;
        char lbl[4] = {(char)('1' + i), 0};
        int id = ID(i);
        if (w_pad(app, id, bx, by, bw, 36, lbl, ICON_NONE, s->hotcue_set[i] ? RGB(255, 190, 40) : c, s->hotcue_set[i])) engine_hotcue(d, i);
        static bool cleared[HOTCUES];
        if (w_owned(app, id) && app->touch.held_time > 0.8f && !cleared[i] && s->hotcue_set[i]) {
            engine_hotcue_clear(d, i);
            cleared[i] = true;
            app_status(app, "Hot cue cleared");
        }
        if (!w_owned(app, id)) cleared[i] = false;
    }
    ui_text(app, "hold to clear", x + col / 2, top + 98, 0.32f, COL_FAINT, ALIGN_CENTER);

    // Loop.
    x = 8 + col;
    w_label(app, "LOOP", x, top, COL_DIM);
    char buf[16];
    float beats = engine_loop_beats(s->loop_size);
    if (beats < 1) snprintf(buf, sizeof buf, "1/%d", (int)roundf(1 / beats));
    else snprintf(buf, sizeof buf, "%d", (int)beats);
    int dir;
    stepper(app, ID(0), x, top + 16, col, buf, &dir, c);
    if (dir) engine_loop_resize(d, dir);
    if (w_pad(app, ID(0), x, top + 44, col, 30, s->loop_on ? "EXIT LOOP" : "LOOP", ICON_NONE, c, s->loop_on)) engine_loop_toggle(d);
    if (w_pad(app, ID(0), x, top + 78, col, 20, "SLIP", ICON_NONE, c, s->slip)) engine_slip_toggle(d);

    // FX.
    x = 12 + 2 * col;
    w_label(app, "FX", x, top, COL_DIM);
    stepper(app, ID(0), x, top + 16, col, FX_NAMES[ctl->fx], &dir, c);
    if (dir) ctl->fx = (ctl->fx + dir + FX_COUNT) % FX_COUNT;
    if (w_pad(app, ID(0), x, top + 44, col, 30, ctl->fx_on ? "FX ON" : "FX OFF", ICON_FX, c, ctl->fx_on)) ctl->fx_on = !ctl->fx_on;
    w_hfader(app, ID(0), x, top + 76, col, 20, &ctl->fx_amount, 0.5f, c, false);
    ui_text(app, "amount", x + col / 2, top + 98, 0.32f, COL_FAINT, ALIGN_CENTER);
}

// ---------------------------------------------------------------------------------------------
// Sampler

static void page_sampler(App *app, float W) {
    u32 colors[PADS] = {deck_color(app, 0), deck_color(app, 1), ui_mix(deck_color(app, 0), deck_color(app, 1), 0.5f), RGB(255, 186, 40)};
    static const char *keys[PADS] = {"A", "B", "X", "Y"};
    float pw = (W - 70) / 2, ph = 88;
    for (int p = 0; p < PADS; p++) {
        float x = 8 + (p % 2) * (pw + 6), y = Y0 + 2 + (p / 2) * (ph + 6);
        bool on = engine_pad_active(p);
        if (w_pad(app, ID(p), x, y, pw, ph, NULL, ICON_NONE, colors[p], on)) engine_pad(p);
        char big[2] = {(char)('A' + p), 0};
        ui_text(app, big, x + pw / 2, y + 14, 1.0f, on ? WHITE : colors[p], ALIGN_CENTER);
        ui_text(app, engine_pad_name(p), x + pw / 2, y + 50, 0.42f, on ? WHITE : COL_DIM, ALIGN_CENTER);
        char hint[16];
        snprintf(hint, sizeof hint, "%s button", keys[p]);
        ui_text(app, hint, x + pw / 2, y + 66, 0.32f, COL_FAINT, ALIGN_CENTER);
    }
    float vx = W - 46;
    ui_text(app, "Vol", vx + 19, Y0, 0.4f, COL_DIM, ALIGN_CENTER);
    if (w_vfader(app, ID(0), vx + 10, Y0 + 18, 18, 164, &app->cfg.sampler_vol, 0.8f, colors[2], false)) app_config_changed(app);
}

// ---------------------------------------------------------------------------------------------
// Record

static char rec_name[64];

static void record_toggle(App *app) {
    char msg[160];
    if (engine_recording(NULL)) {
        engine_record_stop();
        snprintf(msg, sizeof msg, "Saved %s", rec_name);
        app_status(app, msg);
        return;
    }
    mkdir("sdmc:/3ds", 0777);
    mkdir(CONFIG_DIR, 0777);
    mkdir(RECORD_DIR, 0777);
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    if (tm) strftime(rec_name, sizeof rec_name, "mix_%Y-%m-%d_%H-%M-%S.wav", tm);
    else snprintf(rec_name, sizeof rec_name, "mix_%llu.wav", (unsigned long long)osGetTime());
    char path[160];
    snprintf(path, sizeof path, RECORD_DIR "/%s", rec_name);
    if (!engine_record_start(path)) app_status(app, "Can't write to the SD card");
}

void panel_record_toggle(App *app) { record_toggle(app); }

static void page_record(App *app, float W) {
    float secs;
    bool rec = engine_recording(&secs);
    float cx = W / 2, cy = Y0 + 66, r = 42;
    u32 red = RGB(255, 45, 55);
    if (rec) ui_circle(cx, cy, r + 6 + 3 * sinf(app->time * 6), ui_alpha(red, 60));
    ui_circle(cx, cy, r, ui_mix(red, COL_BG, rec ? 0 : 0.55f));
    ui_circle(cx, cy, r - 4, RGB(20, 8, 10));
    if (rec) ui_round_rect(cx - 14, cy - 14, 28, 28, 4, red);
    else ui_circle(cx, cy, 16, red);
    if (w_tap(app, ID(0), cx - r, cy - r, 2 * r, 2 * r)) record_toggle(app);
    char t[32];
    ui_fmt_time(rec ? secs : 0, t, sizeof t);
    ui_text(app, t, cx, cy + r + 6, 0.8f, rec ? red : COL_DIM, ALIGN_CENTER);
    ui_text_fit(app, rec ? rec_name : "Tap to record the master output (A button)", cx, cy + r + 32, 0.4f, rec ? COL_TEXT : COL_DIM, ALIGN_CENTER, W - 20);
    ui_text_fit(app, "Saved as WAV in sdmc:/3ds/nDJess/recordings", cx, cy + r + 48, 0.36f, COL_FAINT, ALIGN_CENTER, W - 20);
    float l, rr;
    engine_meters(&l, &rr);
    w_meter(16, Y0 + 10, 10, 120, l, deck_color(app, 0));
    w_meter(W - 26, Y0 + 10, 10, 120, rr, deck_color(app, 1));
}

// ---------------------------------------------------------------------------------------------

void panel_draw(App *app, float W) {
    ui_vgrad(0, 0, W, SCREEN_H, RGB(10, 11, 18), COL_BG);
    header_draw(app, W);
    tabs_draw(app, W);
    switch (app->cfg.page) {
    case PAGE_LIBRARY: browser_draw(app, W, Y0); break;
    case PAGE_MIXER: page_mixer(app, W); break;
    case PAGE_EQ: page_eq(app, W); break;
    case PAGE_FX: page_fx(app, W); break;
    case PAGE_SAMPLER: page_sampler(app, W); break;
    case PAGE_RECORD: page_record(app, W); break;
    case PAGE_SETTINGS: settings_draw(app, W, Y0); break;
    }
}
