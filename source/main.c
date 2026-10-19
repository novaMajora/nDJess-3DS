#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "app.h"
#include "widgets.h"

static C3D_RenderTarget *top_target, *bottom_target;
static C3D_Tex logo_tex;
static Tex3DS_SubTexture logo_sub;

static const float PITCH_RANGE[PITCH_RANGES] = {0.08f, 0.16f, 0.5f};

// ---------------------------------------------------------------------------------------------
// App helpers

void app_status(App *app, const char *msg) {
    snprintf(app->status, sizeof app->status, "%s", msg);
    app->status_ttl = 3.0f;
}

void app_apply_config(App *app) {
    MixControls *m = engine_mix();
    m->master = app->cfg.master;
    m->curve = app->cfg.curve;
    m->sampler_vol = app->cfg.sampler_vol;
    m->split_cue = app->cfg.split_cue;
    m->auto_gain = app->cfg.auto_gain;
    m->pitch_range = PITCH_RANGE[app->cfg.pitch_range];
}

void app_config_changed(App *app) {
    app->cfg_dirty = true;
    app->cfg_save_timer = 1.0f;
    app_apply_config(app);
}

void app_set_page(App *app, int page) {
    app->cfg.page = (page + PAGE_COUNT) % PAGE_COUNT;
    app_config_changed(app);
}

static bool deck_has_track(App *app, int d) {
    int t = app->deck[d].tag;
    return app->deck[d].load != LOAD_EMPTY && t >= 0 && t < app->lib.count;
}

const char *app_track_title(App *app, int d) { return deck_has_track(app, d) ? app->lib.tracks[app->deck[d].tag].title : ""; }
const char *app_track_artist(App *app, int d) { return deck_has_track(app, d) ? app->lib.tracks[app->deck[d].tag].artist : ""; }
int app_track_album(App *app, int d) { return deck_has_track(app, d) ? app->lib.tracks[app->deck[d].tag].album_id : -1; }

void app_load(App *app, int deck, int track) {
    char msg[192];
    if (!app->audio_ok) {
        app_status(app, "Audio isn't running (see the message at startup)");
        return;
    }
    if (track < 0 || track >= app->lib.count) return;
    if (app->deck[deck].playing) {
        snprintf(msg, sizeof msg, "Deck %c is playing. Pause it before loading a new song.", 'A' + deck);
        app_status(app, msg);
        return;
    }
    engine_load(deck, app->lib.tracks[track].path, track);
    app->deck[deck].tag = track; // show it right away
    app->deck[deck].load = LOAD_LOADING;
    snprintf(msg, sizeof msg, "Deck %c: %s", 'A' + deck, app->lib.tracks[track].title);
    app_status(app, msg);
}

// ---------------------------------------------------------------------------------------------
// Startup screen

typedef struct { App *app; u64 last; } ScanUi;

static void draw_message(App *app, const char *title, const char *line) {
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    ui_begin_frame(app);
    C2D_TargetClear(top_target, COL_BG);
    C2D_SceneBegin(top_target);
    ui_vgrad(0, 0, TOP_W, SCREEN_H, RGB(14, 10, 22), COL_BG);
    if (app->has_logo) {
        float w = 360, h = w * app->logo_h / app->logo_w;
        C2D_DrawImageAt(app->logo, (TOP_W - w) / 2, 50, 0.5f, NULL, w / app->logo_w, h / app->logo_h);
    } else {
        ui_text(app, APP_NAME, TOP_W / 2, 80, 1.2f, DEFAULT_COLOR_A, ALIGN_CENTER);
    }
    ui_text(app, title, TOP_W / 2, 196, 0.5f, COL_TEXT, ALIGN_CENTER);
    C2D_TargetClear(bottom_target, COL_BG);
    C2D_SceneBegin(bottom_target);
    ui_text_fit(app, line, BOTTOM_W / 2, 110, 0.45f, COL_DIM, ALIGN_CENTER, BOTTOM_W - 30);
    C3D_FrameEnd(0);
}

static void scan_progress(int found, const char *current, void *user) {
    ScanUi *s = user;
    u64 now = osGetTime();
    if (now - s->last < 100) return;
    s->last = now;
    char title[64];
    snprintf(title, sizeof title, "Loading your music... %d songs", found);
    draw_message(s->app, title, current);
}

// ---------------------------------------------------------------------------------------------
// Input

static void update_touch(App *app, u32 down, u32 held, u32 up) {
    Touch *t = &app->touch;
    touchPosition tp;
    hidTouchRead(&tp);
    if (!t->held && !t->down && !t->up) t->owner = 0;
    if (t->up) t->owner = 0; // released last frame
    t->prev_x = t->x;
    t->prev_y = t->y;
    t->down = down & KEY_TOUCH;
    t->held = held & KEY_TOUCH;
    t->up = up & KEY_TOUCH;
    if (t->down) {
        t->start_x = t->prev_x = t->x = tp.px;
        t->start_y = t->prev_y = t->y = tp.py;
        t->owner = 0;
        t->moved = false;
        t->held_time = 0;
    } else if (t->held) {
        t->x = tp.px;
        t->y = tp.py;
        t->held_time += app->dt;
        int dx = t->x - t->start_x, dy = t->y - t->start_y;
        if (dx * dx + dy * dy > 8 * 8) t->moved = true;
    }
}

static void stick_scratch(App *app, bool deck_held[DECKS]) {
    circlePosition cp;
    hidCircleRead(&cp);
    float sens = app->cfg.stick_sens;
    if (app->cfg.scratch_mode == SCRATCH_VELOCITY) {
        // Stick left/right is the hand dragging the record back/forth; centered holds it still.
        float x = cp.dx;
        if (fabsf(x) < 14) x = 0;
        else x -= x > 0 ? 14 : -14;
        float speed = x / 140.0f * 2.5f * sens;
        for (int d = 0; d < DECKS; d++)
            if (deck_held[d]) engine_scratch_speed(d, speed);
    } else {
        // Spin: turning the stick round clockwise turns the record forward.
        float mag = sqrtf((float)(cp.dx * cp.dx + cp.dy * cp.dy));
        if (mag > 60) {
            float a = atan2f(cp.dy, cp.dx);
            if (app->stick_spin_valid) {
                float da = a - app->stick_angle;
                if (da > (float)M_PI) da -= 2 * (float)M_PI;
                if (da < -(float)M_PI) da += 2 * (float)M_PI;
                for (int d = 0; d < DECKS; d++)
                    if (deck_held[d]) engine_scratch_move(d, -da / (2 * (float)M_PI) * sens);
            }
            app->stick_angle = a;
            app->stick_spin_valid = true;
        } else {
            app->stick_spin_valid = false;
        }
    }
}

static void deck_combos(App *app, int d, u32 down, u32 held) {
    if (down & KEY_A) engine_toggle_play(d);
    if (down & KEY_B) engine_cue(d);
    if (down & KEY_X) engine_sync(d, !app->deck[d].synced);
    if (down & KEY_Y) engine_loop_toggle(d);
    DeckControls *ctl = engine_controls(d);
    float step = 0.005f / engine_mix()->pitch_range; // 0.5% per press
    if (down & KEY_DUP) engine_set_pitch(d, ctl->pitch + step);
    if (down & KEY_DDOWN) engine_set_pitch(d, ctl->pitch - step);
    if (held & KEY_DRIGHT) engine_bend(d, 0.6f);
    if (held & KEY_DLEFT) engine_bend(d, -0.6f);
}

static void handle_buttons(App *app, u32 down, u32 held) {
    static bool applied[DECKS];
    bool deck_held[DECKS] = {(held & KEY_L) != 0, (held & KEY_R) != 0};
    for (int d = 0; d < DECKS; d++) app->grab_stick[d] = deck_held[d] && app->deck[d].load == LOAD_READY;
    if (deck_held[0] || deck_held[1]) {
        stick_scratch(app, deck_held);
        for (int d = 0; d < DECKS; d++)
            if (deck_held[d]) deck_combos(app, d, down, held);
    } else {
        app->stick_spin_valid = false;
    }
    for (int d = 0; d < DECKS; d++) {
        bool want = app->grab_touch[d] || app->grab_stick[d];
        if (want != applied[d]) {
            engine_grab(d, want);
            applied[d] = want;
        }
    }

    // Crossfader on ZL/ZR and the C-Stick (New 3DS).
    MixControls *m = engine_mix();
    if (held & KEY_ZL) m->crossfader = fmaxf(0, m->crossfader - app->dt * 2);
    if (held & KEY_ZR) m->crossfader = fminf(1, m->crossfader + app->dt * 2);
    circlePosition cs;
    hidCstickRead(&cs);
    if (abs(cs.dx) > 20) m->crossfader = fminf(1, fmaxf(0, m->crossfader + cs.dx / 150.0f * app->dt * 3));

    if (down & KEY_SELECT) {
        app->cfg.swapped = !app->cfg.swapped;
        app->touch.owner = 0;
        for (int d = 0; d < DECKS; d++) app->grab_touch[d] = false;
        app_config_changed(app);
    }
    if (deck_held[0] || deck_held[1]) return; // the face buttons and D-pad went to the decks

    if (down & KEY_DLEFT) app_set_page(app, app->cfg.page - 1);
    if (down & KEY_DRIGHT) app_set_page(app, app->cfg.page + 1);
    switch (app->cfg.page) {
    case PAGE_LIBRARY: browser_buttons(app, down, held); break;
    case PAGE_SETTINGS: settings_buttons(app, down); break;
    case PAGE_SAMPLER:
        if (down & KEY_A) engine_pad(0);
        if (down & KEY_B) engine_pad(1);
        if (down & KEY_X) engine_pad(2);
        if (down & KEY_Y) engine_pad(3);
        break;
    case PAGE_RECORD:
        if (down & KEY_A) panel_record_toggle(app);
        break;
    default: break;
    }
    if ((down & KEY_START) && app->cfg.page != PAGE_LIBRARY) app_set_page(app, PAGE_LIBRARY);
}

// ---------------------------------------------------------------------------------------------
// Self-test build (make AUTOTEST=1): loads two songs, walks every page and saves screenshots to
// sdmc:/3ds/nDJess/shots/. Not part of the normal app.

#ifdef AUTOTEST
static void save_screenshot(const char *name) {
    mkdir(CONFIG_DIR "/shots", 0777);
    char path[160];
    snprintf(path, sizeof path, CONFIG_DIR "/shots/%s.bmp", name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    const int W = 400, H = 480, row = W * 3;
    u8 hdr[54] = {'B', 'M'};
    u32 size = 54 + row * H;
    memcpy(hdr + 2, &size, 4);
    hdr[10] = 54;
    hdr[14] = 40;
    memcpy(hdr + 18, &W, 4);
    memcpy(hdr + 22, &H, 4);
    hdr[26] = 1;
    hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    u16 tw, th, bw, bh;
    u8 *top = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &tw, &th);
    u8 *bot = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &bw, &bh);
    u8 *line = malloc(row);
    for (int y = H - 1; y >= 0; y--) { // BMP rows are bottom-up
        memset(line, 0, row);
        for (int x = 0; x < W; x++) {
            u8 *src = NULL;
            if (y < 240) src = top + (x * 240 + (239 - y)) * 3;
            else if (x >= 40 && x < 360) src = bot + ((x - 40) * 240 + (239 - (y - 240))) * 3;
            if (src) memcpy(line + x * 3, src, 3);
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
}

typedef enum { T_LOAD, T_PLAY, T_PAGE, T_SWAP, T_CAT, T_PAD, T_GRAB, T_FX, T_LOOP, T_HOT, T_SYNC, T_SHOT, T_QUIT } TestAct;
typedef struct { TestAct act; int a, b; int wait; const char *shot; } TestStep;

static const TestStep SCRIPT[] = {
    {T_PAGE, PAGE_LIBRARY, 0, 30, "01_start"},
    {T_LOAD, 0, 0, 10, NULL}, {T_LOAD, 1, 3, 90, NULL},
    {T_PLAY, 0, 0, 0, NULL}, {T_PLAY, 1, 0, 400, "02_playing_library"},
    {T_HOT, 0, 0, 30, NULL}, {T_PAGE, PAGE_MIXER, 0, 40, "03_mixer"},
    {T_PAGE, PAGE_EQ, 0, 40, "04_eq"},
    {T_LOOP, 0, 0, 10, NULL}, {T_FX, 0, 1, 10, NULL}, {T_PAGE, PAGE_FX, 0, 60, "05_fx"},
    {T_LOOP, 0, 0, 10, NULL}, {T_FX, 0, 0, 10, NULL},
    {T_PAD, 0, 0, 0, NULL}, {T_PAGE, PAGE_SAMPLER, 0, 6, "06_sampler"},
    {T_PAGE, PAGE_RECORD, 0, 40, "07_record"},
    {T_CAT, 0, 0, 0, NULL}, {T_PAGE, PAGE_SETTINGS, 0, 30, "08_settings_audio"},
    {T_CAT, 1, 0, 30, "09_settings_controller"},
    {T_CAT, 2, 0, 30, "10_settings_appearance"},
    {T_CAT, 5, 0, 30, "11_settings_about"},
    {T_SYNC, 1, 0, 10, NULL}, {T_GRAB, 0, 1, 40, "12_scratch"}, {T_GRAB, 0, 0, 30, NULL},
    {T_PAGE, PAGE_MIXER, 0, 0, NULL}, {T_SWAP, 0, 0, 40, "13_swapped"},
    {T_SWAP, 0, 0, 10, NULL}, {T_QUIT, 0, 0, 0, NULL},
};

static void autotest_step(App *app) {
    static int step = -1, wait = 60;
    if (wait > 0) { wait--; return; }
    if (step >= 0 && SCRIPT[step].shot) save_screenshot(SCRIPT[step].shot);
    step++;
    if (step >= (int)(sizeof SCRIPT / sizeof *SCRIPT)) { app->quit = true; return; }
    const TestStep *s = &SCRIPT[step];
    switch (s->act) {
    case T_LOAD: if (app->lib.count) app_load(app, s->a, app->lib.by_title[s->b % app->lib.count]); break;
    case T_PLAY: engine_play(s->a, true); break;
    case T_PAGE: app->cfg.page = s->a; break;
    case T_SWAP: app->cfg.swapped = !app->cfg.swapped; break;
    case T_CAT: app->set_cat = s->a; break;
    case T_PAD: engine_pad(s->a); break;
    case T_GRAB: engine_grab(s->a, s->b); if (s->b) engine_scratch_speed(s->a, -1.5f); break;
    case T_FX: engine_controls(s->a)->fx_on = s->b; break;
    case T_LOOP: engine_loop_toggle(s->a); break;
    case T_HOT: engine_hotcue(s->a, 0); break;
    case T_SYNC: engine_sync(s->a, true); break;
    default: app->quit = true; break;
    }
    wait = s->wait;
}
#endif

// ---------------------------------------------------------------------------------------------
// Main

static void draw_status(App *app, float W) {
    if (app->status_ttl <= 0 || !app->status[0]) return;
    float a = app->status_ttl < 0.4f ? app->status_ttl / 0.4f : 1;
    float w = fminf(W - 20, ui_text_width(app, app->status, 0.44f) + 24), x = (W - w) / 2, y = SCREEN_H - 30;
    ui_round_rect(x, y, w, 24, 8, RGBA(0, 0, 0, (int)(220 * a)));
    ui_round_frame(x, y, w, 24, 8, 1, ui_alpha(deck_color(app, 0), (int)(200 * a)));
    ui_text_fit(app, app->status, W / 2, y + 4, 0.44f, ui_alpha(COL_TEXT, (int)(255 * a)), ALIGN_CENTER, w - 16);
}

int main(void) {
    gfxInitDefault();
    romfsInit();
    mcuHwcInit();
    ptmuInit();
    osSetSpeedupEnable(true);
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS * 8);
    C2D_Prepare();
    top_target = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    bottom_target = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    aptSetSleepAllowed(false); // keep the music going with the lid closed
    srand((unsigned)time(NULL));

    static App app;
    memset(&app, 0, sizeof app);
    ui_init(&app);
    config_load(&app.cfg);
    app.wave_zoom = 6;
    app.has_logo = image_load("romfs:/logo.png", &app.logo, &logo_tex, &logo_sub, &app.logo_w, &app.logo_h);
    for (int d = 0; d < DECKS; d++) app.deck[d].tag = -1;

    char err[160] = "";
    draw_message(&app, "Starting the decks...", "");
    app.audio_ok = engine_init(err, sizeof err);
    app_apply_config(&app);

    ScanUi scan = {.app = &app};
    draw_message(&app, "Loading your music...", MUSIC_DIR);
    if (!library_scan(&app.lib, MUSIC_DIR, FAVORITES_PATH, scan_progress, &scan)) app_status(&app, "No MP3 folder: put your music in sdmc:/MP3");
    else if (!app.lib.count) app_status(&app, "No .mp3 files found in sdmc:/MP3");
    else if (app.lib.truncated) app_status(&app, "Large library: only the first 8000 songs were loaded");
    if (!app.audio_ok) app_status(&app, err);
    app.art = art_create(&app.lib);
    track_cache_load(&app);
    browser_init(&app);

    bool lid_closed = false;
    u64 last = osGetTime();
    float lid_check = 0;

    while (aptMainLoop() && !app.quit) {
        u64 now = osGetTime();
        app.dt = (now - last) / 1000.0f;
        if (app.dt > 0.1f) app.dt = 0.1f;
        last = now;
        app.time += app.dt;
        if (app.status_ttl > 0) app.status_ttl -= app.dt;

        hidScanInput();
        u32 down = hidKeysDown(), held = hidKeysHeld(), up = hidKeysUp();
        update_touch(&app, down, held, up);
#ifdef AUTOTEST
        autotest_step(&app);
#endif

        // Lid closed: backlights off, the music keeps playing.
        lid_check -= app.dt;
        if (lid_check <= 0) {
            lid_check = 0.25f;
            u8 shell = 1;
            PTMU_GetShellState(&shell);
            if (!shell != lid_closed) {
                lid_closed = !shell;
                gspLcdInit();
                if (lid_closed) GSPLCD_PowerOffAllBacklights();
                else GSPLCD_PowerOnAllBacklights();
                gspLcdExit();
            }
        }

        if (app.audio_ok) {
            for (int d = 0; d < DECKS; d++) engine_state(d, &app.deck[d]);
            handle_buttons(&app, down, held);
            engine_update(app.dt);
            int tag;
            float bpm, dur;
            for (int d = 0; d < DECKS; d++)
                if (engine_take_analysis(d, &tag, &bpm, &dur)) track_cache_add(&app, tag, bpm, dur);
        } else if (app.cfg.page == PAGE_LIBRARY || app.cfg.page == PAGE_SETTINGS) {
            handle_buttons(&app, down, held);
        }
        browser_update(&app);
        art_update(app.art);

        if (app.cfg_dirty) {
            app.cfg_save_timer -= app.dt;
            if (app.cfg_save_timer <= 0 && !app.touch.held) {
                config_save(&app.cfg);
                app.cfg_dirty = false;
            }
        }

        if (lid_closed) { gspWaitForVBlank(); continue; }

        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        ui_begin_frame(&app);
        C2D_TargetClear(top_target, COL_BG);
        C2D_SceneBegin(top_target);
        app.touch_on = false;
        if (app.cfg.swapped) panel_draw(&app, TOP_W);
        else decks_draw(&app, TOP_W);
        draw_status(&app, TOP_W);
        C2D_TargetClear(bottom_target, COL_BG);
        C2D_SceneBegin(bottom_target);
        app.touch_on = true;
        if (app.cfg.swapped) decks_draw(&app, BOTTOM_W);
        else panel_draw(&app, BOTTOM_W);
        C3D_FrameEnd(0);
    }

    if (app.cfg_dirty) config_save(&app.cfg);
    engine_exit();
    art_destroy(app.art);
    library_free(&app.lib);
    free(app.br.items);
    free(app.track_bpm);
    free(app.track_dur);
    if (app.has_logo) C3D_TexDelete(&logo_tex);
    C2D_TextBufDelete(app.textbuf);
    C2D_TextBufDelete(app.measurebuf);
    C2D_Fini();
    C3D_Fini();
    ptmuExit();
    mcuHwcExit();
    romfsExit();
    gfxExit();
    return 0;
}
