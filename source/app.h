// nDJess: a two-deck DJ mixer for the Nintendo 3DS.
#pragma once

#include <3ds.h>
#include <citro2d.h>
#include <stdbool.h>

#include "art.h"
#include "engine.h"
#include "library.h"
#include "ui.h"

#define APP_NAME "nDJess"
#define APP_VERSION "0.1.0"
#define MUSIC_DIR "sdmc:/MP3"
#define CONFIG_DIR "sdmc:/3ds/nDJess"
#define CONFIG_PATH CONFIG_DIR "/config.ini"
#define FAVORITES_PATH CONFIG_DIR "/favorites.m3u"
#define TRACK_CACHE_PATH CONFIG_DIR "/tracks.txt"
#define RECORD_DIR CONFIG_DIR "/recordings"
#define SAMPLES_DIR CONFIG_DIR "/samples"

#define TOP_W 400
#define BOTTOM_W 320
#define SCREEN_H 240

#define DEFAULT_COLOR_A RGB(240, 40, 72)
#define DEFAULT_COLOR_B RGB(30, 150, 255)

// Pages of the panel view (the screen without the decks).
typedef enum { PAGE_LIBRARY, PAGE_MIXER, PAGE_EQ, PAGE_FX, PAGE_SAMPLER, PAGE_RECORD, PAGE_SETTINGS, PAGE_COUNT } Page;

enum { SCRATCH_VELOCITY, SCRATCH_SPIN };
enum { PITCH_8, PITCH_16, PITCH_50, PITCH_RANGES };

typedef struct {
    u32 deck_color[DECKS];
    float master, curve, sampler_vol, stick_sens;
    bool split_cue, auto_gain;
    int pitch_range;
    int scratch_mode;
    bool swapped; // SELECT: decks on the touch screen, panel on top
    int page;
} Config;

// One touch on the bottom screen, owned by the control it started on.
typedef struct {
    bool down, held, up;  // began / is held / ended this frame
    int x, y, start_x, start_y, prev_x, prev_y;
    int owner;            // id of the control that claimed this touch, 0 = none
    float held_time;
    bool moved;           // travelled more than a few pixels
} Touch;

typedef enum { CAT_SONGS, CAT_ARTISTS, CAT_ALBUMS, CAT_GENRES, CAT_PLAYLISTS, CAT_COUNT } BrowseCat;

typedef struct {
    BrowseCat cat;
    int group;        // -1: the list of artists/albums/... ; otherwise the open one
    int group_cursor; // cursor to restore when going back to the group list
    int *items;       // track indices, or group indices when group == -1 (and cat != CAT_SONGS)
    int count;
    int cursor;
    float scroll;
    char filter[64];  // search text for All Songs
    float drag_scroll;
} Browser;

typedef struct App {
    Library lib;
    ArtCache *art;
    Config cfg;
    bool cfg_dirty;
    float cfg_save_timer;
    float time, dt;
    char status[192];
    float status_ttl;
    C2D_TextBuf textbuf, measurebuf;
    C2D_Font font;
    bool quit, audio_ok;
    DeckState deck[DECKS]; // engine snapshot, refreshed every frame
    float *track_bpm;      // per library track, 0 = unknown (from the analysis cache)
    float *track_dur;
    int analysed_count;
    Browser br;
    int set_cat, set_item;  // settings: category, item cursor
    int color_deck;         // settings > appearance: which deck's color is edited
    int fx_deck;            // effects page: deck shown
    float wave_zoom;        // seconds visible in the zoomed waveforms
    Touch touch;
    bool touch_on;          // the view being drawn is on the touch screen
    bool grab_touch[DECKS], grab_stick[DECKS];
    float stick_angle;
    bool stick_spin_valid;
    C2D_Image logo;
    int logo_w, logo_h;
    bool has_logo;
} App;

// main.c
void app_status(App *app, const char *msg);
void app_load(App *app, int deck, int track);
void app_set_page(App *app, int page);
void app_config_changed(App *app); // saved shortly
void app_apply_config(App *app);   // push settings into the engine
void config_defaults(Config *cfg);
const char *app_track_title(App *app, int deck);
const char *app_track_artist(App *app, int deck);
int app_track_album(App *app, int deck);

// decks.c: the two turntables (top screen, or bottom after SELECT)
void decks_draw(App *app, float W);
// Whole-track waveform with markers; tapping it seeks when `seekable`.
void deck_overview(App *app, int d, float x, float y, float w, float h, bool seekable);
// Waveform around the playhead (centered), `seconds` wide.
void deck_wave_h(App *app, int d, float x, float y, float w, float h, float seconds);
u32 deck_color(App *app, int d);
// panel.c: header, page tabs and the mixer / EQ / FX / sampler / record pages
void panel_draw(App *app, float W);
void panel_record_toggle(App *app);
void header_draw(App *app, float W);
// browser.c
void browser_init(App *app);
void browser_draw(App *app, float W, float y);
void browser_buttons(App *app, u32 down, u32 held);
void browser_update(App *app); // outside the frame: the search keyboard
void track_cache_load(App *app);
void track_cache_add(App *app, int track, float bpm, float duration);
// settings.c
void settings_draw(App *app, float W, float y);
void settings_buttons(App *app, u32 down);
void config_load(Config *cfg);
void config_save(const Config *cfg);
