// Settings page (Audio, Controller, Appearance, Browser, Recording, About) and the config file.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "app.h"
#include "widgets.h"

#define ID(i) WID(0x400000, i)
#define WHITE RGB(255, 255, 255)
#define ROW 30

enum { CAT_AUDIO, CAT_CONTROLLER, CAT_APPEARANCE, CAT_BROWSER, CAT_RECORDING, CAT_ABOUT, SET_CATS };
static const struct { const char *name; Icon icon; int items; } SET[SET_CATS] = {
    {"Audio", ICON_SPEAKER, 6}, {"Controller", ICON_GAMEPAD, 2}, {"Appearance", ICON_PALETTE, 6},
    {"Browser", ICON_FOLDER, 0}, {"Recording", ICON_RECORD, 0}, {"About", ICON_INFO, 0},
};

static const u32 PRESETS[12] = {
    RGB(240, 40, 72), RGB(30, 150, 255), RGB(255, 40, 200), RGB(150, 70, 255), RGB(0, 220, 240), RGB(0, 210, 150),
    RGB(60, 230, 90), RGB(180, 240, 40), RGB(255, 215, 0), RGB(255, 130, 20), RGB(235, 235, 245), RGB(255, 120, 170),
};
static const char *PITCH_NAMES[PITCH_RANGES] = {"±8%", "±16%", "±50%"};
static const char *SCRATCH_NAMES[2] = {"Velocity", "Spin"};

// ---------------------------------------------------------------------------------------------
// Config file

void config_defaults(Config *c) {
    *c = (Config){
        .deck_color = {DEFAULT_COLOR_A, DEFAULT_COLOR_B},
        .master = 0.85f, .curve = 0.3f, .sampler_vol = 0.8f, .stick_sens = 1.0f,
        .split_cue = false, .auto_gain = true, .pitch_range = PITCH_8, .scratch_mode = SCRATCH_VELOCITY,
        .swapped = false, .page = PAGE_LIBRARY,
    };
}

static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

static u32 parse_color(const char *hex, u32 fallback) {
    unsigned v;
    if (sscanf(hex, "%6x", &v) != 1) return fallback;
    return RGB(v >> 16, v >> 8, v);
}

void config_load(Config *c) {
    config_defaults(c);
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        char key[32], val[64];
        if (sscanf(line, "%31[^=]=%63s", key, val) != 2) continue;
        float v = (float)atof(val);
        if (!strcmp(key, "colorA")) c->deck_color[0] = parse_color(val, DEFAULT_COLOR_A);
        else if (!strcmp(key, "colorB")) c->deck_color[1] = parse_color(val, DEFAULT_COLOR_B);
        else if (!strcmp(key, "master")) c->master = clamp01(v);
        else if (!strcmp(key, "curve")) c->curve = clamp01(v);
        else if (!strcmp(key, "sampler")) c->sampler_vol = clamp01(v);
        else if (!strcmp(key, "stick")) c->stick_sens = v < 0.25f ? 0.25f : v > 2.5f ? 2.5f : v;
        else if (!strcmp(key, "headphone_preview")) c->split_cue = v != 0;
        else if (!strcmp(key, "auto_gain")) c->auto_gain = v != 0;
        else if (!strcmp(key, "pitch_range")) c->pitch_range = (int)v >= 0 && (int)v < PITCH_RANGES ? (int)v : PITCH_8;
        else if (!strcmp(key, "scratch")) c->scratch_mode = (int)v == SCRATCH_SPIN ? SCRATCH_SPIN : SCRATCH_VELOCITY;
        else if (!strcmp(key, "swapped")) c->swapped = v != 0;
        else if (!strcmp(key, "page")) c->page = (int)v >= 0 && (int)v < PAGE_COUNT ? (int)v : PAGE_LIBRARY;
    }
    fclose(f);
}

void config_save(const Config *c) {
    mkdir("sdmc:/3ds", 0777);
    mkdir(CONFIG_DIR, 0777);
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) return;
    for (int d = 0; d < DECKS; d++) {
        u32 col = c->deck_color[d];
        fprintf(f, "color%c=%02X%02X%02X\n", 'A' + d, (unsigned)(col & 0xFF), (unsigned)(col >> 8 & 0xFF), (unsigned)(col >> 16 & 0xFF));
    }
    fprintf(f, "master=%.3f\ncurve=%.3f\nsampler=%.3f\nstick=%.2f\nheadphone_preview=%d\nauto_gain=%d\npitch_range=%d\nscratch=%d\nswapped=%d\npage=%d\n",
            c->master, c->curve, c->sampler_vol, c->stick_sens, c->split_cue, c->auto_gain, c->pitch_range, c->scratch_mode, c->swapped, c->page);
    fclose(f);
}

// ---------------------------------------------------------------------------------------------
// Values shared by touch and buttons

static int channel(u32 c, int ch) { return (int)(c >> (8 * ch) & 0xFF); }

static u32 with_channel(u32 c, int ch, int v) {
    v = v < 0 ? 0 : v > 255 ? 255 : v;
    return (c & ~(0xFFu << (8 * ch))) | (u32)v << (8 * ch) | 0xFF000000u;
}

static void reset_colors(App *app) {
    app->cfg.deck_color[0] = DEFAULT_COLOR_A;
    app->cfg.deck_color[1] = DEFAULT_COLOR_B;
    app_config_changed(app);
    app_status(app, "Deck colors reset to red and blue");
}

static int preset_index(u32 c) {
    for (int i = 0; i < 12; i++)
        if ((PRESETS[i] & 0xFFFFFF) == (c & 0xFFFFFF)) return i;
    return -1;
}

// A/B buttons on the focused item: dir +1 / -1.
static void adjust(App *app, int dir) {
    Config *c = &app->cfg;
    float step = 0.05f * dir;
    switch (app->set_cat) {
    case CAT_AUDIO:
        switch (app->set_item) {
        case 0: c->master = clamp01(c->master + step); break;
        case 1: c->curve = clamp01(c->curve + step); break;
        case 2: c->split_cue = !c->split_cue; break;
        case 3: c->auto_gain = !c->auto_gain; break;
        case 4: c->pitch_range = (c->pitch_range + dir + PITCH_RANGES) % PITCH_RANGES; break;
        case 5: c->sampler_vol = clamp01(c->sampler_vol + step); break;
        }
        break;
    case CAT_CONTROLLER:
        if (app->set_item == 0) c->scratch_mode = !c->scratch_mode;
        else c->stick_sens = fminf(2.5f, fmaxf(0.25f, c->stick_sens + 0.125f * dir));
        break;
    case CAT_APPEARANCE: {
        u32 *col = &c->deck_color[app->color_deck];
        switch (app->set_item) {
        case 0: app->color_deck = !app->color_deck; return;
        case 1: *col = PRESETS[((preset_index(*col) + dir) % 12 + 12) % 12]; break;
        case 2: case 3: case 4: *col = with_channel(*col, app->set_item - 2, channel(*col, app->set_item - 2) + 15 * dir); break;
        case 5: reset_colors(app); return;
        }
        break;
    }
    default: return;
    }
    app_config_changed(app);
}

void settings_buttons(App *app, u32 down) {
    int n = SET[app->set_cat].items;
    if (down & KEY_DUP && app->set_item > 0) app->set_item--;
    if (down & KEY_DDOWN && app->set_item < n - 1) app->set_item++;
    if (down & KEY_A) adjust(app, 1);
    if (down & KEY_B) adjust(app, -1);
    if (down & (KEY_X | KEY_Y)) {
        app->set_cat = (app->set_cat + ((down & KEY_X) ? 1 : SET_CATS - 1)) % SET_CATS;
        app->set_item = 0;
    }
}

// ---------------------------------------------------------------------------------------------
// Drawing

static void focus(App *app, int item, float x, float y, float w) {
    if (app->set_item == item && !app->touch_on) ui_round_frame(x - 3, y - 2, w + 6, ROW - 2, 4, 1, ui_alpha(deck_color(app, 0), 160));
    else if (app->set_item == item) ui_rect(x - 3, y + 4, 2, ROW - 14, deck_color(app, 0));
}

static void slider_row(App *app, int item, float x, float y, float w, const char *label, float *v, float def, char *valtxt) {
    focus(app, item, x, y, w);
    ui_text(app, label, x, y - 1, 0.4f, COL_TEXT, ALIGN_LEFT);
    if (valtxt) ui_text(app, valtxt, x + w, y - 1, 0.36f, COL_DIM, ALIGN_RIGHT);
    if (w_hfader(app, ID(item), x, y + 11, w, 16, v, def, deck_color(app, 0), false)) {
        app->set_item = item;
        app_config_changed(app);
    }
}

static void toggle_row(App *app, int item, float x, float y, float w, const char *label, const char *sub, bool *on) {
    focus(app, item, x, y, w);
    ui_text(app, label, x, y + (sub ? -1 : 3), 0.42f, COL_TEXT, ALIGN_LEFT);
    if (sub) ui_text_fit(app, sub, x, y + 13, 0.32f, COL_DIM, ALIGN_LEFT, w - 44);
    if (w_toggle(app, ID(item), x + w - 34, y + 2, on, deck_color(app, 0))) {
        app->set_item = item;
        app_config_changed(app);
    }
}

static void choice_row(App *app, int item, float x, float y, float w, const char *label, const char *value) {
    focus(app, item, x, y, w);
    ui_text(app, label, x, y + 3, 0.42f, COL_TEXT, ALIGN_LEFT);
    float bw = w * 0.42f;
    if (w_button(app, ID(item), x + w - bw, y, bw, 22, value, ICON_NONE, deck_color(app, 0), false)) {
        app->set_item = item;
        adjust(app, 1);
    }
}

static void lines(App *app, float x, float y, float w, const char *const *text, int n) {
    for (int i = 0; i < n; i++) ui_text_fit(app, text[i], x, y + i * 15, 0.36f, i % 2 ? COL_DIM : COL_TEXT, ALIGN_LEFT, w);
}

static void curve_icon(float x, float y, float w, float h, float curve, u32 c) {
    // Deck A gain across the fader for this curve (like the mixer's).
    float px = x, py = y;
    for (int i = 1; i <= 16; i++) {
        float f = i / 16.0f, g = (1 - curve) * cosf(f * (float)M_PI / 2) + curve * fminf(1, (1 - f) * 12);
        float nx = x + w * f, ny = y + h * (1 - g);
        if (i > 1) ui_line(px, py, nx, ny, 1.5f, c);
        px = nx;
        py = ny;
    }
}

static void page_audio(App *app, float x, float y, float w) {
    Config *c = &app->cfg;
    char buf[16];
    snprintf(buf, sizeof buf, "%d%%", (int)roundf(c->master * 100));
    slider_row(app, 0, x, y, w, "Master Volume", &c->master, 0.85f, buf);
    y += ROW;
    focus(app, 1, x, y, w);
    ui_text(app, "Crossfader Curve", x, y - 1, 0.4f, COL_TEXT, ALIGN_LEFT);
    if (w_hfader(app, ID(0), x, y + 11, w - 80, 16, &c->curve, 0.3f, deck_color(app, 0), false)) { app->set_item = 1; app_config_changed(app); }
    if (w_button(app, ID(0), x + w - 74, y, 34, 24, NULL, ICON_NONE, COL_DIM, c->curve < 0.05f)) { c->curve = 0; app_config_changed(app); }
    curve_icon(x + w - 70, y + 5, 26, 14, 0, WHITE);
    if (w_button(app, ID(0), x + w - 36, y, 34, 24, NULL, ICON_NONE, COL_DIM, c->curve > 0.95f)) { c->curve = 1; app_config_changed(app); }
    curve_icon(x + w - 32, y + 5, 26, 14, 1, WHITE);
    y += ROW;
    toggle_row(app, 2, x, y, w, "Headphone Preview", "Cued decks in the left ear, master in the right", &c->split_cue);
    y += ROW;
    toggle_row(app, 3, x, y, w, "Auto Gain", "Level every track to the same loudness", &c->auto_gain);
    y += ROW;
    choice_row(app, 4, x, y, w, "Pitch Range", PITCH_NAMES[c->pitch_range]);
    y += ROW;
    snprintf(buf, sizeof buf, "%d%%", (int)roundf(c->sampler_vol * 100));
    slider_row(app, 5, x, y, w, "Sampler Volume", &c->sampler_vol, 0.8f, buf);
}

static void page_controller(App *app, float x, float y, float w) {
    Config *c = &app->cfg;
    choice_row(app, 0, x, y, w, "Scratch Mode", SCRATCH_NAMES[c->scratch_mode]);
    y += ROW;
    float v = (c->stick_sens - 0.25f) / 2.25f;
    char buf[16];
    snprintf(buf, sizeof buf, "%.1fx", c->stick_sens);
    slider_row(app, 1, x, y, w, "Stick Speed", &v, (1 - 0.25f) / 2.25f, buf);
    c->stick_sens = 0.25f + v * 2.25f;
    y += ROW + 2;
    const char *mode = c->scratch_mode == SCRATCH_SPIN ? "Circle Pad: spin it round" : "Circle Pad: drag back / forth";
    const char *const text[] = {
        "Hold L / R / both: grab deck A / B", mode,
        "L/R + A play, B cue, X sync, Y loop", "L/R + Up/Down pitch, Left/Right nudge",
        "SELECT: swap screens", "D-Pad Left/Right: change page",
        "ZL / ZR, C-Stick: crossfader", "(New 3DS)",
    };
    lines(app, x, y, w, text, 8);
}

static void page_appearance(App *app, float x, float y, float w) {
    Config *c = &app->cfg;
    int d = app->color_deck;
    // Deck selector.
    focus(app, 0, x, y, w);
    for (int i = 0; i < DECKS; i++)
        if (w_button(app, ID(i), x + i * (w / 2 + 2), y, w / 2 - 2, 22, i ? "Deck B" : "Deck A", ICON_NONE, c->deck_color[i], d == i)) {
            app->color_deck = i;
            app->set_item = 0;
        }
    y += ROW;
    // Preset swatches.
    focus(app, 1, x, y, w);
    float sw = (w - 11 * 3) / 12;
    for (int i = 0; i < 12; i++) {
        float sx = x + i * (sw + 3);
        bool cur = (PRESETS[i] & 0xFFFFFF) == (c->deck_color[d] & 0xFFFFFF);
        if (cur) ui_round_rect(sx - 2, y - 2, sw + 4, 26, 4, WHITE);
        ui_round_rect(sx, y, sw, 22, 3, PRESETS[i]);
        if (w_tap(app, ID(i), sx, y, sw, 22)) {
            c->deck_color[d] = PRESETS[i];
            app->set_item = 1;
            app_config_changed(app);
        }
    }
    y += ROW;
    // Red, green, blue.
    static const char *names[3] = {"Red", "Green", "Blue"};
    static const u32 tint[3] = {RGB(255, 60, 60), RGB(60, 230, 90), RGB(60, 130, 255)};
    for (int ch = 0; ch < 3; ch++) {
        focus(app, 2 + ch, x, y, w);
        ui_text(app, names[ch], x, y + 3, 0.4f, COL_TEXT, ALIGN_LEFT);
        float v = channel(c->deck_color[d], ch) / 255.0f;
        if (w_hfader(app, ID(ch), x + 44, y, w - 80, 22, &v, 0.5f, tint[ch], false)) {
            c->deck_color[d] = with_channel(c->deck_color[d], ch, (int)roundf(v * 255));
            app->set_item = 2 + ch;
            app_config_changed(app);
        }
        char buf[8];
        snprintf(buf, sizeof buf, "%d", channel(c->deck_color[d], ch));
        ui_text(app, buf, x + w, y + 4, 0.38f, COL_DIM, ALIGN_RIGHT);
        y += 26;
    }
    y += 4;
    focus(app, 5, x, y, w);
    if (w_button(app, ID(0), x, y, w - 60, 24, "Reset to default colors", ICON_NONE, COL_DIM, false)) {
        app->set_item = 5;
        reset_colors(app);
    }
    // Preview of both colors.
    ui_round_rect(x + w - 54, y, 26, 24, 4, c->deck_color[0]);
    ui_round_rect(x + w - 26, y, 26, 24, 4, c->deck_color[1]);
    ui_text(app, "A", x + w - 41, y + 4, 0.44f, WHITE, ALIGN_CENTER);
    ui_text(app, "B", x + w - 13, y + 4, 0.44f, WHITE, ALIGN_CENTER);
}

static void page_browser(App *app, float x, float y, float w) {
    Library *L = &app->lib;
    char a[64], b[64], c[64], d[64];
    snprintf(a, sizeof a, "%d songs, %d artists, %d albums", L->count, L->artist_count, L->album_count);
    snprintf(b, sizeof b, "%d genres, %d playlists", L->genre_count, L->playlist_count);
    snprintf(c, sizeof c, "BPM known for %d of %d songs", app->analysed_count, L->count);
    snprintf(d, sizeof d, "Songs: %s (subfolders too)", MUSIC_DIR);
    const char *const text[] = {
        "Music Folder", d, "Library", a, " ", b, "Analysis", c,
        "Playlists", ".m3u files in the music folder", "Tip", "A song's BPM appears once it was loaded",
    };
    lines(app, x, y, w, text, 12);
}

static void page_recording(App *app, float x, float y, float w) {
    float secs;
    bool rec = engine_recording(&secs);
    char t[32], s[48];
    ui_fmt_time(secs, t, sizeof t);
    snprintf(s, sizeof s, rec ? "Recording now (%s)" : "Not recording", t);
    const char *const text[] = {
        "Folder", RECORD_DIR, "Format", "WAV, 44.1 kHz, 16-bit stereo", "What's recorded",
        "The master output, with FX and sampler", "Status", s, "Start / stop", "Rec page: the big button or A",
    };
    lines(app, x, y, w, text, 10);
}

static void page_about(App *app, float x, float y, float w) {
    if (app->has_logo) {
        float lw = w - 10, lh = lw * app->logo_h / app->logo_w;
        C2D_DrawImageAt(app->logo, x + 5, y, 0.5f, NULL, lw / app->logo_w, lh / app->logo_h);
        y += lh + 4;
    } else {
        ui_text(app, APP_NAME, x + w / 2, y, 0.9f, deck_color(app, 0), ALIGN_CENTER);
        y += 30;
    }
    char v[48];
    snprintf(v, sizeof v, "Version %s  -  3DS DJ Mixer", APP_VERSION);
    ui_text(app, v, x + w / 2, y, 0.4f, COL_DIM, ALIGN_CENTER);
    y += 22;
    // The dedication.
    float pulse = 1 + 0.08f * sinf(app->time * 3);
    ui_icon(ICON_HEART, x + w / 2, y + 8, 16 * pulse, deck_color(app, 0));
    y += 22;
    ui_text_fit(app, "made in honor of my wife.", x + w / 2, y, 0.48f, COL_TEXT, ALIGN_CENTER, w);
    ui_text_fit(app, "i love you always", x + w / 2, y + 16, 0.48f, ui_mix(deck_color(app, 0), WHITE, 0.35f), ALIGN_CENTER, w);
    y += 40;
    ui_text_fit(app, "by Nova White", x + w / 2, y, 0.34f, COL_DIM, ALIGN_CENTER, w);
    ui_text_fit(app, "minimp3, stb_image, libctru, citro2d", x + w / 2, y + 13, 0.3f, COL_FAINT, ALIGN_CENTER, w);
}

void settings_draw(App *app, float W, float y0) {
    // Category list.
    float lw = W * 0.34f, rh = 26;
    w_panel(2, y0, lw, 6 * rh + 8, COL_EDGE);
    for (int i = 0; i < SET_CATS; i++) {
        float y = y0 + 4 + i * rh;
        bool sel = app->set_cat == i;
        if (sel) {
            ui_round_rect(5, y, lw - 6, rh - 2, 4, deck_color(app, 0));
            ui_round_vgrad(6, y + 1, lw - 8, rh - 4, 3, ui_mix(deck_color(app, 0), RGB(0, 0, 0), 0.3f), ui_mix(deck_color(app, 0), RGB(0, 0, 0), 0.6f));
        }
        ui_icon(SET[i].icon, 17, y + rh / 2 - 1, 13, sel ? WHITE : COL_DIM);
        ui_text_fit(app, SET[i].name, 30, y + 4, 0.42f, sel ? WHITE : COL_TEXT, ALIGN_LEFT, lw - 32);
        if (w_tap(app, ID(i), 4, y, lw - 4, rh - 1)) {
            app->set_cat = i;
            app->set_item = 0;
        }
    }
    if (!app->touch_on) ui_text_fit(app, "X/Y: section  A/B: change", 4, y0 + 6 * rh + 14, 0.32f, COL_FAINT, ALIGN_LEFT, lw);

    // The section.
    float x = lw + 10, w = W - x - 6, y = y0 + 2;
    switch (app->set_cat) {
    case CAT_AUDIO: page_audio(app, x, y, w); break;
    case CAT_CONTROLLER: page_controller(app, x, y, w); break;
    case CAT_APPEARANCE: page_appearance(app, x, y, w); break;
    case CAT_BROWSER: page_browser(app, x, y, w); break;
    case CAT_RECORDING: page_recording(app, x, y, w); break;
    case CAT_ABOUT: page_about(app, x, y, w); break;
    }
}
