// The music library page: All Songs (with search), Artists, Albums, Genres and Playlists, and
// loading the selected song onto deck A or B. Also the BPM/duration cache from the analysis.
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "app.h"
#include "widgets.h"

#define ID(i) WID(0x300000, i)
#define ROW_H 30
#define WHITE RGB(255, 255, 255)

static const struct { const char *name; Icon icon; } CATS[CAT_COUNT] = {
    {"Songs", ICON_NOTE}, {"Artists", ICON_ARTIST}, {"Albums", ICON_ALBUM}, {"Genres", ICON_GENRE}, {"Lists", ICON_PLAYLIST},
};

static bool want_search;

// ---------------------------------------------------------------------------------------------
// BPM / duration cache: "bpm<TAB>seconds<TAB>path" per line

void track_cache_load(App *app) {
    int n = app->lib.count ? app->lib.count : 1;
    app->track_bpm = calloc(n, sizeof(float));
    app->track_dur = calloc(n, sizeof(float));
    FILE *f = fopen(TRACK_CACHE_PATH, "r");
    if (!f || !app->track_bpm || !app->track_dur) { if (f) fclose(f); return; }
    char line[1100];
    while (fgets(line, sizeof line, f)) {
        float bpm, dur;
        char path[1024];
        if (sscanf(line, "%f\t%f\t%1023[^\r\n]", &bpm, &dur, path) != 3) continue;
        int t = library_find_path(&app->lib, path);
        if (t < 0) continue;
        if (app->track_bpm[t] <= 0 && bpm > 0) app->analysed_count++;
        app->track_bpm[t] = bpm;
        app->track_dur[t] = dur;
    }
    fclose(f);
}

void track_cache_add(App *app, int track, float bpm, float duration) {
    if (track < 0 || track >= app->lib.count || !app->track_bpm) return;
    if (app->track_bpm[track] == bpm && app->track_dur[track] == duration) return;
    if (app->track_bpm[track] <= 0 && bpm > 0) app->analysed_count++;
    app->track_bpm[track] = bpm;
    app->track_dur[track] = duration;
    mkdir("sdmc:/3ds", 0777);
    mkdir(CONFIG_DIR, 0777);
    FILE *f = fopen(TRACK_CACHE_PATH, "a"); // later lines win when loading
    if (!f) return;
    fprintf(f, "%.2f\t%.2f\t%s\n", bpm, duration, app->lib.tracks[track].path);
    fclose(f);
}

// ---------------------------------------------------------------------------------------------
// Lists

static bool contains_ci(const char *hay, const char *needle) {
    if (!*needle) return true;
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) { h++; n++; }
        if (!*n) return true;
    }
    return false;
}

static int group_count(App *app, BrowseCat cat) {
    switch (cat) {
    case CAT_ARTISTS: return app->lib.artist_count;
    case CAT_ALBUMS: return app->lib.album_count;
    case CAT_GENRES: return app->lib.genre_count;
    case CAT_PLAYLISTS: return app->lib.playlist_count;
    default: return 0;
    }
}

static const char *group_name(App *app, BrowseCat cat, int g) {
    switch (cat) {
    case CAT_ARTISTS: return app->lib.artists[g].name;
    case CAT_ALBUMS: return app->lib.albums[g].name;
    case CAT_GENRES: return app->lib.genres[g].name;
    case CAT_PLAYLISTS: return app->lib.playlists[g].name;
    default: return "";
    }
}

static int group_tracks(App *app, BrowseCat cat, int g, const int **out) {
    switch (cat) {
    case CAT_ARTISTS: *out = app->lib.artists[g].tracks; return app->lib.artists[g].count;
    case CAT_ALBUMS: *out = app->lib.albums[g].tracks; return app->lib.albums[g].count;
    case CAT_GENRES: *out = app->lib.genres[g].tracks; return app->lib.genres[g].count;
    case CAT_PLAYLISTS: *out = app->lib.playlists[g].tracks; return app->lib.playlists[g].count;
    default: *out = NULL; return 0;
    }
}

static bool showing_tracks(const Browser *b) { return b->cat == CAT_SONGS || b->group >= 0; }

static void rebuild(App *app) {
    Browser *b = &app->br;
    Library *L = &app->lib;
    free(b->items);
    b->items = NULL;
    b->count = 0;
    int cap = L->count > 0 ? L->count : 1;
    if (!showing_tracks(b)) cap = group_count(app, b->cat) + 1;
    b->items = malloc(cap * sizeof(int));
    if (!b->items) return;
    if (b->cat == CAT_SONGS) {
        for (int i = 0; i < L->count; i++) {
            int t = L->by_title[i];
            if (contains_ci(L->tracks[t].title, b->filter) || contains_ci(L->tracks[t].artist, b->filter)) b->items[b->count++] = t;
        }
    } else if (b->group < 0) {
        int n = group_count(app, b->cat);
        for (int i = 0; i < n; i++) b->items[b->count++] = i;
    } else {
        const int *tr;
        int n = group_tracks(app, b->cat, b->group, &tr);
        free(b->items);
        b->items = malloc((n ? n : 1) * sizeof(int));
        if (!b->items) return;
        memcpy(b->items, tr, n * sizeof(int));
        b->count = n;
    }
    if (b->cursor >= b->count) b->cursor = b->count - 1;
    if (b->cursor < 0) b->cursor = 0;
}

void browser_init(App *app) {
    Browser *b = &app->br;
    memset(b, 0, sizeof *b);
    b->group = -1;
    rebuild(app);
}

static void set_cat(App *app, int cat) {
    Browser *b = &app->br;
    b->cat = (BrowseCat)((cat + CAT_COUNT) % CAT_COUNT);
    b->group = -1;
    b->cursor = 0;
    b->scroll = 0;
    rebuild(app);
}

static void open_group(App *app) {
    Browser *b = &app->br;
    if (showing_tracks(b) || b->cursor >= b->count) return;
    b->group_cursor = b->cursor;
    b->group = b->items[b->cursor];
    b->cursor = 0;
    b->scroll = 0;
    rebuild(app);
}

static bool go_back(App *app) {
    Browser *b = &app->br;
    if (b->cat == CAT_SONGS && b->filter[0]) {
        b->filter[0] = 0;
        rebuild(app);
        return true;
    }
    if (b->group < 0) return false;
    b->group = -1;
    rebuild(app);
    b->cursor = b->group_cursor;
    return true;
}

static void load_selected(App *app, int deck) {
    Browser *b = &app->br;
    if (!showing_tracks(b)) { open_group(app); return; }
    if (b->cursor < 0 || b->cursor >= b->count) return;
    app_load(app, deck, b->items[b->cursor]);
}

static void keep_visible(Browser *b, int visible) {
    if (b->cursor < b->scroll) b->scroll = b->cursor;
    if (b->cursor > b->scroll + visible - 1) b->scroll = b->cursor - visible + 1;
}

void browser_buttons(App *app, u32 down, u32 held) {
    Browser *b = &app->br;
    static float rep;
    if (held & (KEY_DUP | KEY_DDOWN)) rep += app->dt;
    else rep = 0;
    bool again = rep > 0.35f;
    if (again) rep = 0.29f;
    u32 press = down | (again ? held : 0);
    int step = 1;
    if (press & KEY_DUP) b->cursor -= step;
    if (press & KEY_DDOWN) b->cursor += step;
    if (b->cursor >= b->count) b->cursor = b->count - 1;
    if (b->cursor < 0) b->cursor = 0;
    if (press & (KEY_DUP | KEY_DDOWN)) keep_visible(b, 5);
    if (down & KEY_A) load_selected(app, 0);
    if (down & KEY_X) {
        if (showing_tracks(b)) load_selected(app, 1);
        else open_group(app);
    }
    if (down & KEY_B) go_back(app);
    if (down & KEY_Y) set_cat(app, b->cat + 1);
    if (down & KEY_START) want_search = true;
}

void browser_update(App *app) {
    if (!want_search) return;
    want_search = false;
    Browser *b = &app->br;
    SwkbdState kb;
    swkbdInit(&kb, SWKBD_TYPE_NORMAL, 2, sizeof b->filter - 1);
    swkbdSetHintText(&kb, "Search songs and artists");
    swkbdSetInitialText(&kb, b->filter);
    char out[sizeof b->filter];
    if (swkbdInputText(&kb, out, sizeof out) != SWKBD_BUTTON_RIGHT) return;
    if (b->cat != CAT_SONGS) set_cat(app, CAT_SONGS);
    snprintf(b->filter, sizeof b->filter, "%s", out);
    b->cursor = 0;
    b->scroll = 0;
    rebuild(app);
    if (!b->count) app_status(app, "No songs match your search");
}

// ---------------------------------------------------------------------------------------------
// Drawing

static void draw_row(App *app, int i, float x, float y, float w, bool sel) {
    Browser *b = &app->br;
    Library *L = &app->lib;
    u32 a = deck_color(app, 0);
    if (sel) {
        ui_round_rect(x, y + 1, w, ROW_H - 2, 4, a);
        ui_round_vgrad(x + 1, y + 2, w - 2, ROW_H - 4, 3, ui_mix(a, RGB(0, 0, 0), 0.35f), ui_mix(a, RGB(0, 0, 0), 0.65f));
    } else if (i % 2) {
        ui_rect(x, y, w, ROW_H, RGBA(255, 255, 255, 8));
    }
    float as = ROW_H - 6, tx = x + as + 9;
    u32 sub = sel ? RGB(255, 220, 225) : COL_DIM;
    if (showing_tracks(b)) {
        int t = b->items[i];
        const Track *tr = &L->tracks[t];
        const C2D_Image *img = art_get(app->art, tr->album_id);
        if (img) C2D_DrawImageAt(*img, x + 4, y + 3, 0.5f, NULL, as / img->subtex->width, as / img->subtex->height);
        else {
            ui_rect(x + 4, y + 3, as, as, RGB(24, 26, 36));
            ui_icon(ICON_NOTE, x + 4 + as / 2, y + ROW_H / 2, as * 0.5f, sel ? WHITE : COL_DIM);
        }
        char bpm[16], dur[16];
        float bv = app->track_bpm ? app->track_bpm[t] : 0, dv = app->track_dur ? app->track_dur[t] : 0;
        if (bv > 0) snprintf(bpm, sizeof bpm, "%.0f BPM", bv);
        else snprintf(bpm, sizeof bpm, "--- BPM");
        if (dv > 0) ui_fmt_time(dv, dur, sizeof dur);
        else snprintf(dur, sizeof dur, "--:--");
        float rx = x + w - 6;
        ui_text(app, bpm, rx, y + 2, 0.38f, sel ? WHITE : COL_TEXT, ALIGN_RIGHT);
        ui_text(app, dur, rx, y + 15, 0.36f, sub, ALIGN_RIGHT);
        // Badges for songs on a deck.
        float bx = rx - 58;
        for (int d = DECKS - 1; d >= 0; d--)
            if (app->deck[d].load != LOAD_EMPTY && app->deck[d].tag == t) {
                ui_round_rect(bx - 12, y + 8, 13, 13, 3, deck_color(app, d));
                ui_text(app, d ? "B" : "A", bx - 5.5f, y + 8, 0.36f, WHITE, ALIGN_CENTER);
                bx -= 16;
            }
        float tw = bx - 14 - tx;
        ui_text_fit(app, tr->title, tx, y + 1, 0.46f, sel ? WHITE : COL_TEXT, ALIGN_LEFT, tw);
        ui_text_fit(app, tr->artist, tx, y + 15, 0.38f, sub, ALIGN_LEFT, tw);
    } else {
        int g = b->items[i];
        if (b->cat == CAT_ALBUMS) {
            const C2D_Image *img = art_get(app->art, g);
            if (img) C2D_DrawImageAt(*img, x + 4, y + 3, 0.5f, NULL, as / img->subtex->width, as / img->subtex->height);
            else ui_icon(ICON_ALBUM, x + 4 + as / 2, y + ROW_H / 2, as * 0.6f, sel ? WHITE : COL_DIM);
        } else {
            ui_icon(b->cat == CAT_PLAYLISTS && g == 0 ? ICON_HEART : CATS[b->cat].icon, x + 4 + as / 2, y + ROW_H / 2, as * 0.55f, sel ? WHITE : a);
        }
        const int *tr;
        int n = group_tracks(app, b->cat, g, &tr);
        char cnt[24];
        snprintf(cnt, sizeof cnt, n == 1 ? "1 song" : "%d songs", n);
        float cw = ui_text(app, cnt, x + w - 18, y + 8, 0.38f, sub, ALIGN_RIGHT);
        ui_line(x + w - 12, y + ROW_H / 2 - 4, x + w - 8, y + ROW_H / 2, 1.5f, sub);
        ui_line(x + w - 12, y + ROW_H / 2 + 4, x + w - 8, y + ROW_H / 2, 1.5f, sub);
        ui_text_fit(app, group_name(app, b->cat, g), tx, y + 7, 0.48f, sel ? WHITE : COL_TEXT, ALIGN_LEFT, w - (tx - x) - cw - 26);
    }
}

void browser_draw(App *app, float W, float y0) {
    Browser *b = &app->br;
    u32 a = deck_color(app, 0);
    // Category tabs and search.
    float sw = 26, tw = (W - 6 - sw - 2 * CAT_COUNT) / CAT_COUNT;
    for (int c = 0; c < CAT_COUNT; c++)
        if (w_button(app, ID(c), 3 + c * (tw + 2), y0, tw, 20, CATS[c].name, ICON_NONE, a, b->cat == c && b->group < 0)) {
            if (b->cat == c) { b->group = -1; rebuild(app); }
            else set_cat(app, c);
        }
    if (w_button(app, ID(0), W - 3 - sw, y0, sw, 20, NULL, ICON_SEARCH, a, b->filter[0] != 0)) want_search = true;

    // Title line: where we are.
    float ly = y0 + 23;
    char title[96];
    if (b->cat == CAT_SONGS && b->filter[0]) snprintf(title, sizeof title, "Search: \"%s\" (%d)", b->filter, b->count);
    else if (b->group >= 0) snprintf(title, sizeof title, "%s", group_name(app, b->cat, b->group));
    else if (b->cat == CAT_SONGS) snprintf(title, sizeof title, "All Songs (%d)", b->count);
    else snprintf(title, sizeof title, "%s", CATS[b->cat].name);
    if (b->group >= 0 || (b->cat == CAT_SONGS && b->filter[0])) {
        if (w_button(app, ID(0), 3, ly, 40, 16, "Back", ICON_NONE, COL_DIM, false)) go_back(app);
        ui_text_fit(app, title, 48, ly, 0.42f, COL_TEXT, ALIGN_LEFT, W - 56);
    } else {
        ui_text_fit(app, title, 6, ly, 0.42f, COL_DIM, ALIGN_LEFT, W - 12);
    }

    // The list, scrolled by dragging.
    float lx = 3, lw = W - 6, top = ly + 18, h = 212 - top;
    int visible = (int)(h / ROW_H);
    float max = b->count - visible;
    if (max < 0) max = 0;
    int id = ID(0);
    Touch *t = &app->touch;
    if (w_claim(app, id, lx, top, lw, h)) b->drag_scroll = b->scroll;
    if (w_owned(app, id)) {
        if (t->moved) b->scroll = b->drag_scroll - (t->y - t->start_y) / (float)ROW_H;
        else if (t->up) {
            int row = (int)(b->scroll + (t->y - top) / ROW_H);
            if (row >= 0 && row < b->count) {
                if (row == b->cursor && !showing_tracks(b)) open_group(app);
                b->cursor = row;
            }
        }
    }
    if (b->scroll > max) b->scroll = max;
    if (b->scroll < 0) b->scroll = 0;
    C2D_Flush();
    C3D_SetScissor(GPU_SCISSOR_NORMAL, 240 - (int)(top + h), (int)(app->touch_on ? BOTTOM_W - (lx + lw) : TOP_W - (lx + lw)), 240 - (int)top, (int)((app->touch_on ? BOTTOM_W : TOP_W) - lx));
    int first = (int)b->scroll;
    for (int r = 0; r <= visible; r++) {
        int i = first + r;
        if (i >= b->count) break;
        draw_row(app, i, lx, top + (i - b->scroll) * ROW_H, lw - 6, i == b->cursor);
    }
    C2D_Flush();
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    if (!b->count) {
        const char *msg = app->lib.count ? "Nothing here" : "No songs: put MP3s in sdmc:/MP3";
        ui_text(app, msg, W / 2, top + 40, 0.46f, COL_DIM, ALIGN_CENTER);
    }
    // Scrollbar.
    if (b->count > visible) {
        float bh = h * visible / b->count, by = top + (h - bh) * (b->scroll / (max > 0 ? max : 1));
        ui_round_rect(W - 6, by, 3, bh, 1.5f, COL_FAINT);
    }

    // Load buttons.
    float by = 215, bw = (W - 12) / 2;
    bool tracks = showing_tracks(b);
    if (w_button(app, ID(0), 4, by, bw, 22, tracks ? "Load Deck A  (A)" : "Open  (A)", ICON_NONE, deck_color(app, 0), false)) load_selected(app, 0);
    if (w_button(app, ID(0), 8 + bw, by, bw, 22, tracks ? "Load Deck B  (X)" : "Open  (X)", ICON_NONE, deck_color(app, 1), false)) load_selected(app, tracks ? 1 : 0);
}
