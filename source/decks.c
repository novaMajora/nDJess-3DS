// The decks view: two turntables with track info, scrolling waveforms, pitch faders and the
// crossfader. Drawn 400 px wide on the top screen, or 320 px on the touch screen after SELECT.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "widgets.h"

#define ID(i) WID(0x100000, i)
#define WHITE RGB(255, 255, 255)

u32 deck_color(App *app, int d) { return app->cfg.deck_color[d]; }

// Loudest block between t0 and t1 seconds (sampled), with its bass share. False if not decoded.
static bool wave_at(const u8 *w, int n, float bs, float t0, float t1, float *pk, float *bass) {
    int b0 = (int)floorf(t0 / bs), b1 = (int)floorf(t1 / bs);
    if (b1 < b0) { int t = b0; b0 = b1; b1 = t; }
    if (b1 == b0) b1 = b0 + 1;
    int step = (b1 - b0) / 6 + 1;
    float p = 0, ba = 0;
    bool any = false;
    for (int b = b0; b < b1; b += step) {
        if (b < 0 || b >= n) continue;
        u8 v = w[b * 2];
        if (!v) continue;
        any = true;
        float pv = (v - 1) / 254.0f;
        if (pv > p) {
            p = pv;
            ba = w[b * 2 + 1] / 255.0f;
        }
    }
    *pk = p;
    *bass = ba;
    return any;
}

static u32 wave_color(u32 c, float bass, bool dim) {
    u32 col = ui_mix(c, WHITE, 0.55f * (1 - bass));
    return dim ? ui_mix(col, COL_BG, 0.55f) : col;
}

void deck_overview(App *app, int d, float x, float y, float w, float h, bool seekable) {
    const DeckState *s = &app->deck[d];
    u32 c = deck_color(app, d);
    ui_rect(x, y, w, h, RGB(3, 4, 7));
    if (s->load != LOAD_READY || s->duration <= 0) return;
    int n;
    float bs;
    const u8 *wv = engine_wave(d, &n, &bs);
    float dur = s->duration, mid = y + h / 2;
    for (int i = 0; i < (int)w; i++) {
        float t0 = dur * i / w, t1 = dur * (i + 1) / w, pk, bass;
        if (!wave_at(wv, n, bs, t0, t1, &pk, &bass)) continue;
        float hh = pk * h / 2;
        if (hh < 0.5f) hh = 0.5f;
        ui_rect(x + i, mid - hh, 1, hh * 2, wave_color(c, bass, t1 < s->pos));
    }
    if (s->loop_on) ui_rect(x + w * s->loop_in / dur, y, w * (s->loop_out - s->loop_in) / dur + 1, h, RGBA(255, 255, 255, 50));
    for (int i = 0; i < HOTCUES; i++)
        if (s->hotcue_set[i]) {
            float hx = x + w * s->hotcue[i] / dur;
            ui_tri(hx - 3, y, hx + 3, y, hx, y + 4, RGB(255, 200, 40));
        }
    float cx = x + w * s->cue / dur;
    ui_tri(cx - 3, y + h, cx + 3, y + h, cx, y + h - 4, RGB(255, 140, 0));
    float px = x + w * fminf(s->pos / dur, 1);
    ui_rect(px - 0.5f, y - 1, 1.5f, h + 2, WHITE);

    if (!seekable) return;
    int id = ID(d);
    w_claim(app, id, x, y - 4, w, h + 8);
    if (w_owned(app, id) && app->touch.up) {
        float f = (app->touch.x - x) / w;
        engine_seek(d, (f < 0 ? 0 : f > 1 ? 1 : f) * dur);
    }
}

void deck_wave_h(App *app, int d, float x, float y, float w, float h, float seconds) {
    const DeckState *s = &app->deck[d];
    u32 c = deck_color(app, d);
    ui_rect(x, y, w, h, RGB(3, 4, 7));
    float mid = y + h / 2, spp = seconds / w, t_left = s->pos - seconds / 2;
    if (s->load == LOAD_READY) {
        int n;
        float bs;
        const u8 *wv = engine_wave(d, &n, &bs);
        if (s->loop_on) {
            float a = (s->loop_in - t_left) / spp, b = (s->loop_out - t_left) / spp;
            if (a < 0) a = 0;
            if (b > w) b = w;
            if (b > a) ui_rect(x + a, y, b - a, h, ui_alpha(c, 45));
        }
        if (s->bpm > 0) { // beat grid
            float beat = 60 / s->bpm;
            float first = s->beat_offset + ceilf((t_left - s->beat_offset) / beat) * beat;
            for (float t = first; t < t_left + seconds; t += beat) {
                int k = (int)roundf((t - s->beat_offset) / beat);
                ui_rect(x + (t - t_left) / spp, y, 1, h, k % 4 == 0 ? RGBA(255, 255, 255, 70) : RGBA(255, 255, 255, 25));
            }
        }
        for (int i = 0; i < (int)w; i++) {
            float t0 = t_left + i * spp, pk, bass;
            if (t0 < 0 || !wave_at(wv, n, bs, t0, t0 + spp, &pk, &bass)) continue;
            float hh = pk * (h / 2 - 1);
            if (hh < 0.5f) hh = 0.5f;
            ui_rect(x + i, mid - hh, 1, hh * 2, wave_color(c, bass, false));
        }
        for (int i = 0; i < HOTCUES; i++) {
            float hx = (s->hotcue[i] - t_left) / spp;
            if (s->hotcue_set[i] && hx >= 0 && hx < w) {
                ui_rect(x + hx, y, 1, h, RGB(255, 200, 40));
                char b[4] = {(char)('1' + i), 0};
                ui_rect(x + hx, y, 8, 9, RGB(255, 200, 40));
                ui_text(app, b, x + hx + 1, y - 1, 0.3f, RGB(0, 0, 0), ALIGN_LEFT);
            }
        }
    }
    ui_rect(x + w / 2 - 1, y, 2, h, WHITE);
}

// Vertical scrolling waveform: the future comes down from the top to the playhead line.
static void wave_v(App *app, int d, float x, float y, float w, float h, float playhead_y, float seconds) {
    const DeckState *s = &app->deck[d];
    u32 c = deck_color(app, d);
    if (s->load != LOAD_READY) return;
    int n;
    float bs;
    const u8 *wv = engine_wave(d, &n, &bs);
    float spp = seconds / h, cx = x + w / 2;
    if (s->bpm > 0) {
        float beat = 60 / s->bpm, t_top = s->pos + (playhead_y - y) * spp, t_bot = s->pos - (y + h - playhead_y) * spp;
        float first = s->beat_offset + ceilf((t_bot - s->beat_offset) / beat) * beat;
        for (float t = first; t < t_top; t += beat) {
            int k = (int)roundf((t - s->beat_offset) / beat);
            ui_rect(x, playhead_y - (t - s->pos) / spp, w, 1, k % 4 == 0 ? RGBA(255, 255, 255, 60) : RGBA(255, 255, 255, 22));
        }
    }
    for (int r = 0; r < (int)h; r += 1) {
        float ry = y + r, t0 = s->pos + (playhead_y - ry) * spp, pk, bass;
        if (t0 < 0 || !wave_at(wv, n, bs, t0, t0 + spp, &pk, &bass)) continue;
        float hw = pk * (w / 2 - 1);
        if (hw < 0.5f) hw = 0.5f;
        ui_rect(cx - hw, ry, hw * 2, 1, wave_color(c, bass, ry > playhead_y));
    }
}

static void platter(App *app, int d, float cx, float cy, float r) {
    const DeckState *s = &app->deck[d];
    u32 c = deck_color(app, d);
    bool held = app->grab_touch[d] || app->grab_stick[d] || s->grabbed;
    float angle = s->pos * (33.333f / 60) * 2 * (float)M_PI - (float)M_PI / 2;

    // Base with a dotted strobe ring and the track progress around it.
    ui_circle(cx, cy, r + 6, RGB(10, 11, 16));
    ui_ring(cx, cy, r + 6, r + 5, ui_mix(c, COL_BG, 0.5f), 48);
    for (int i = 0; i < 40; i++) {
        float a = i * 2 * (float)M_PI / 40 + (s->playing ? angle * 0.25f : 0);
        ui_circle(cx + cosf(a) * (r + 3), cy + sinf(a) * (r + 3), 0.9f, ui_mix(c, COL_BG, 0.45f));
    }
    if (s->duration > 0) {
        float f = s->pos / s->duration;
        if (f > 0.002f) ui_arc(cx, cy, r + 2, r, -(float)M_PI / 2, -(float)M_PI / 2 + 2 * (float)M_PI * fminf(f, 1), c);
    }
    // Vinyl with grooves.
    ui_circle(cx, cy, r, held ? RGB(26, 26, 32) : RGB(14, 14, 18));
    for (float gr = r * 0.42f; gr < r - 2; gr += r * 0.09f) ui_ring(cx, cy, gr + 0.7f, gr, RGB(36, 37, 46), 40);
    ui_ring(cx, cy, r, r - 1.5f, held ? WHITE : ui_mix(c, WHITE, 0.2f), 56);
    // Light streak that turns with the record.
    ui_arc(cx, cy, r - 3, r * 0.45f, angle + 0.5f, angle + 0.75f, RGBA(255, 255, 255, 22));
    ui_arc(cx, cy, r - 3, r * 0.45f, angle + 0.5f + (float)M_PI, angle + 0.75f + (float)M_PI, RGBA(255, 255, 255, 22));
    // Label with the cue marker line.
    float lr = r * 0.34f;
    ui_circle(cx, cy, lr, c);
    ui_circle(cx, cy, lr * 0.75f, ui_mix(c, RGB(0, 0, 0), 0.3f));
    ui_ring(cx, cy, lr * 0.5f, lr * 0.42f, ui_mix(c, WHITE, 0.4f), 20);
    ui_circle(cx, cy, 2.2f, RGB(220, 220, 228));
    ui_line(cx + cosf(angle) * lr * 0.55f, cy + sinf(angle) * lr * 0.55f, cx + cosf(angle) * (r - 3), cy + sinf(angle) * (r - 3), 2, WHITE);

    // Touch: the inside scratches; the rim nudges the tempo of a playing record.
    int id = ID(d);
    static float last_angle[DECKS];
    static bool rim[DECKS];
    Touch *t = &app->touch;
    if (w_claim(app, id, cx - r - 6, cy - r - 6, 2 * r + 12, 2 * r + 12)) {
        float dx = t->x - cx, dy = t->y - cy;
        rim[d] = s->playing && sqrtf(dx * dx + dy * dy) > r * 0.74f;
        last_angle[d] = atan2f(dy, dx);
        if (!rim[d]) app->grab_touch[d] = true;
    }
    if (w_owned(app, id)) {
        float dx = t->x - cx, dy = t->y - cy;
        if (dx * dx + dy * dy > 49) {
            float a = atan2f(dy, dx), da = a - last_angle[d];
            if (da > (float)M_PI) da -= 2 * (float)M_PI;
            if (da < -(float)M_PI) da += 2 * (float)M_PI;
            last_angle[d] = a;
            if (rim[d]) engine_bend(d, da * 14);
            else engine_scratch_move(d, da / (2 * (float)M_PI));
        }
        if (t->up) app->grab_touch[d] = false;
    } else {
        app->grab_touch[d] = false;
    }
}

static void info_panel(App *app, int d, float x, float y, float w, float h) {
    const DeckState *s = &app->deck[d];
    u32 c = deck_color(app, d);
    w_panel(x, y, w, h, ui_mix(c, COL_BG, 0.45f));
    float art = w < 150 ? 0 : h - 22, bw = w < 150 ? 38 : 42; // no art on the narrow touch-screen layout
    bool right = d == 1; // deck B is mirrored: BPM block on the left, art on the right
    float ax = right ? x + w - art - 4 : x + 4;
    float bx = right ? x + 4 : x + w - bw - 4;
    float tx = right ? bx + bw + 4 : (art > 0 ? ax + art + 5 : x + 5);
    float tw = w - art - bw - (art > 0 ? 17 : 12);
    int album = app_track_album(app, d);
    if (art <= 0) {
        // narrow layout: no cover
    } else if (s->load == LOAD_READY && album >= 0) {
        const C2D_Image *img = art_get(app->art, album);
        if (img) C2D_DrawImageAt(*img, ax, y + 4, 0.5f, NULL, art / img->subtex->width, art / img->subtex->height);
        else {
            ui_vgrad(ax, y + 4, art, art, ui_mix(c, COL_BG, 0.5f), COL_BG);
            ui_icon(ICON_NOTE, ax + art / 2, y + 4 + art / 2, art * 0.5f, c);
        }
    } else {
        ui_rect(ax, y + 4, art, art, RGB(10, 11, 16));
        ui_icon(ICON_ALBUM, ax + art / 2, y + 4 + art / 2, art * 0.6f, ui_mix(c, COL_BG, 0.5f));
    }
    char a[48], b[48];
    switch (s->load) {
    case LOAD_EMPTY:
        ui_text_fit(app, d == 0 ? "Deck A" : "Deck B", tx, y + 4, 0.48f, COL_TEXT, ALIGN_LEFT, tw);
        ui_text_fit(app, "Load a song", tx, y + 19, 0.38f, COL_DIM, ALIGN_LEFT, tw);
        break;
    case LOAD_LOADING:
        ui_text_fit(app, app_track_title(app, d), tx, y + 4, 0.46f, COL_TEXT, ALIGN_LEFT, tw);
        ui_text_fit(app, "Loading...", tx, y + 19, 0.38f, c, ALIGN_LEFT, tw);
        break;
    case LOAD_ERROR:
        ui_text_fit(app, app_track_title(app, d), tx, y + 4, 0.46f, COL_TEXT, ALIGN_LEFT, tw);
        ui_text_fit(app, s->error, tx, y + 19, 0.38f, RGB(255, 90, 80), ALIGN_LEFT, tw);
        break;
    case LOAD_READY:
        ui_text_fit(app, app_track_title(app, d), tx, y + 3, 0.46f, COL_TEXT, ALIGN_LEFT, tw);
        ui_text_fit(app, app_track_artist(app, d), tx, y + 16, 0.38f, ui_mix(c, WHITE, 0.3f), ALIGN_LEFT, tw);
        ui_fmt_time(s->pos, a, sizeof a);
        ui_fmt_time(s->duration, b, sizeof b);
        char line[100];
        snprintf(line, sizeof line, "%s / %s", a, b);
        ui_text_fit(app, line, tx, y + 28, 0.4f, c, ALIGN_LEFT, tw);
        break;
    }
    // BPM and SYNC.
    ui_text(app, "BPM", right ? bx + 1 : bx + bw - 1, y + 2, 0.3f, COL_DIM, right ? ALIGN_LEFT : ALIGN_RIGHT);
    if (s->load == LOAD_READY && s->bpm > 0) snprintf(a, sizeof a, "%.1f", s->bpm * s->tempo);
    else snprintf(a, sizeof a, s->load == LOAD_READY ? "..." : "---");
    ui_text(app, a, right ? bx + 1 : bx + bw - 1, y + 9, 0.5f, COL_TEXT, right ? ALIGN_LEFT : ALIGN_RIGHT);
    if (w_button(app, ID(d), bx + 2, y + h - 34, bw - 4, 14, "SYNC", ICON_NONE, c, s->synced)) {
        if (s->synced) engine_sync(d, false);
        else if (s->bpm <= 0 || app->deck[1 - d].bpm <= 0) app_status(app, "SYNC needs the BPM of both decks (still analysing?)");
        else engine_sync(d, true);
    }
    deck_overview(app, d, x + 4, y + h - 16, w - 8, 12, true);
}

static void deck_side(App *app, float W, int d) {
    const DeckState *s = &app->deck[d];
    u32 c = deck_color(app, d);
    float sc = W / 400.0f;
    float info_w = W / 2 - 3 - 24 * sc;
    info_panel(app, d, d == 0 ? 3 : W - 3 - info_w, 22, info_w, 62);

    // Mirrored layout: x for deck A, flipped for deck B.
#define MX(xa, w) (d == 0 ? (xa) : W - (xa) - (w))
    float bw = 34 * sc, bh = 26;
    float bx = MX(3, bw);
    if (w_pad(app, ID(d), bx, 92, bw, bh, "CUE", ICON_NONE, c, false)) engine_cue(d);
    if (w_pad(app, ID(d), bx, 124, bw, bh, NULL, ICON_NONE, c, s->playing)) engine_toggle_play(d);
    ui_sym_playpause(bx + bw / 2 - 2, 124 + bh / 2, 11, s->playing ? WHITE : ui_mix(c, WHITE, 0.25f));
    if (w_pad(app, ID(d), bx, 156, bw, bh, "SLIP", ICON_NONE, c, s->slip)) engine_slip_toggle(d);
    if (w_pad(app, ID(d), bx, 188, bw, bh, "LOOP", ICON_NONE, c, s->loop_on)) engine_loop_toggle(d);

    // Pitch fader with headphone cue above it.
    float pw = 18 * sc + 2, px = MX(W / 2 - 24 * sc - pw, pw);
    DeckControls *ctl = engine_controls(d);
    if (w_pad(app, ID(d), px - 1, 88, pw + 2, 16, NULL, ICON_HEADPHONES, c, ctl->pfl)) ctl->pfl = !ctl->pfl;
    float v = (ctl->pitch + 1) / 2;
    if (w_vfader(app, ID(d), px, 108, pw, 84, &v, 0.5f, c, true)) engine_set_pitch(d, v * 2 - 1);
    char t[16];
    snprintf(t, sizeof t, "%+.1f%%", (s->tempo - 1) * 100);
    ui_text(app, t, px + pw / 2, 194, 0.32f, s->synced ? c : COL_DIM, ALIGN_CENTER);

    // Platter between the buttons and the pitch fader.
    float left = d == 0 ? bx + bw + 4 : px + pw + 4, right = d == 0 ? px - 4 : bx - 4;
    float r = (right - left) / 2 - 5;
    if (r > 52) r = 52;
    platter(app, d, (left + right) / 2, 150, r);
#undef MX
}

void decks_draw(App *app, float W) {
    ui_vgrad(0, 0, W, SCREEN_H, RGB(10, 11, 18), COL_BG);
    header_draw(app, W);
    for (int d = 0; d < DECKS; d++) deck_side(app, W, d);

    // Center: the two scrolling waveforms meeting at the playhead line.
    float sc = W / 400.0f, cw = 36 * sc, cx0 = W / 2 - cw / 2, top = 22, bottom = 206, ph = 150;
    ui_round_rect(cx0 - 2, top, cw + 4, bottom - top, 4, RGB(3, 4, 7));
    for (int d = 0; d < DECKS; d++) wave_v(app, d, cx0 + d * cw / 2, top + 2, cw / 2, bottom - top - 4, ph, 5.0f);
    ui_rect(cx0 - 2, ph - 1, cw + 4, 2, WHITE);

    // Crossfader.
    MixControls *m = engine_mix();
    float fw = 112 * sc, fx = W / 2 - fw / 2;
    ui_text(app, "A", fx - 8, 213, 0.4f, deck_color(app, 0), ALIGN_CENTER);
    ui_text(app, "B", fx + fw + 8, 213, 0.4f, deck_color(app, 1), ALIGN_CENTER);
    w_hfader(app, ID(0), fx, 212, fw, 22, &m->crossfader, 0.5f, WHITE, true);
}
