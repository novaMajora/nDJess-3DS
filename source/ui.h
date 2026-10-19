// Drawing helpers on top of citro2d (from nTapeDeck), plus nDJess's icons.
#pragma once

#include <citro2d.h>
#include <stdbool.h>

struct App;

// Same layout as C2D_Color32, but usable in constant initializers.
#define RGBA(r, g, b, a) ((u32)((r) & 0xFF) | ((u32)((g) & 0xFF) << 8) | ((u32)((b) & 0xFF) << 16) | ((u32)((a) & 0xFF) << 24))
#define RGB(r, g, b) RGBA((r), (g), (b), 0xFF)

// The nDJess palette (deck colors come from the settings).
#define COL_BG RGB(6, 7, 11)
#define COL_PANEL RGB(15, 17, 24)
#define COL_PANEL2 RGB(22, 25, 34)
#define COL_EDGE RGB(40, 45, 58)
#define COL_TEXT RGB(232, 236, 244)
#define COL_DIM RGB(130, 138, 156)
#define COL_FAINT RGB(70, 76, 92)

enum { ALIGN_LEFT = 0, ALIGN_CENTER = 1, ALIGN_RIGHT = 2 };

typedef enum {
    ICON_NONE, ICON_NOTE, ICON_FOLDER, ICON_ARTIST, ICON_ALBUM, ICON_GENRE, ICON_PLAYLIST, ICON_SETTINGS,
    ICON_HEADPHONES, ICON_MIXER, ICON_EQ, ICON_FX, ICON_PADS, ICON_RECORD, ICON_SEARCH, ICON_INFO,
    ICON_PALETTE, ICON_GAMEPAD, ICON_HEART, ICON_SPEAKER,
} Icon;

void ui_init(struct App *app);
void ui_begin_frame(struct App *app);

// Text with the system font. `size` is a scale (0.5 ≈ 15 px tall). Returns the drawn width.
float ui_text(struct App *app, const char *s, float x, float y, float size, u32 color, int align);
// Like ui_text, but shortens with "..." to fit `maxw`.
float ui_text_fit(struct App *app, const char *s, float x, float y, float size, u32 color, int align, float maxw);
float ui_text_width(struct App *app, const char *s, float size);
float ui_line_height(float size);

void ui_rect(float x, float y, float w, float h, u32 c);
void ui_vgrad(float x, float y, float w, float h, u32 top, u32 bottom);
void ui_hgrad(float x, float y, float w, float h, u32 left, u32 right);
void ui_round_rect(float x, float y, float w, float h, float r, u32 c);
void ui_round_vgrad(float x, float y, float w, float h, float r, u32 top, u32 bottom);
void ui_round_frame(float x, float y, float w, float h, float r, float t, u32 c); // rounded outline
void ui_circle(float cx, float cy, float r, u32 c);
void ui_ring(float cx, float cy, float r_outer, float r_inner, u32 c, int segments);
void ui_arc(float cx, float cy, float r_outer, float r_inner, float a0, float a1, u32 c); // radians, clockwise
void ui_line(float x0, float y0, float x1, float y1, float thickness, u32 c);
void ui_frame(float x, float y, float w, float h, float t, u32 c); // rectangle outline
void ui_tri(float x0, float y0, float x1, float y1, float x2, float y2, u32 c);

void ui_sym_play(float cx, float cy, float s, u32 c);
void ui_sym_pause(float cx, float cy, float s, u32 c);
void ui_sym_playpause(float cx, float cy, float s, u32 c);
void ui_icon(Icon icon, float cx, float cy, float s, u32 c);

void ui_fmt_time(float seconds, char *buf, int len); // 02:16
bool ui_hit(int px, int py, float x, float y, float w, float h);
u32 ui_mix(u32 a, u32 b, float t); // blend two colors
u32 ui_alpha(u32 c, int a);        // same color, new alpha
