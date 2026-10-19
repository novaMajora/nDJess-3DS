// Immediate-mode touch controls: each call draws the control and, when the view is on the touch
// screen, handles the touch that started on it.
#pragma once

#include "app.h"

// A control id unique to its call site; `base` separates source files, `i` repeated controls.
#define WID(base, i) ((base) + __LINE__ * 16 + (i))

bool w_claim(App *app, int id, float x, float y, float w, float h); // true on the frame a touch starts inside
bool w_owned(App *app, int id);                                      // this control holds the touch
bool w_tap(App *app, int id, float x, float y, float w, float h);   // released inside, no drag
bool w_double_tap(App *app, int id, float x, float y, float w, float h);

void w_button_draw(App *app, float x, float y, float w, float h, const char *label, Icon icon, u32 c, bool lit);
bool w_button(App *app, int id, float x, float y, float w, float h, const char *label, Icon icon, u32 c, bool lit); // fires on tap
bool w_pad(App *app, int id, float x, float y, float w, float h, const char *label, Icon icon, u32 c, bool lit);    // fires on press

// Values are 0..1. Knobs and faders move relative to the drag; a double tap resets to `def`.
bool w_knob(App *app, int id, float cx, float cy, float r, float *v, float def, u32 c, const char *label);
bool w_vfader(App *app, int id, float x, float y, float w, float h, float *v, float def, u32 c, bool center);
bool w_hfader(App *app, int id, float x, float y, float w, float h, float *v, float def, u32 c, bool center);
bool w_toggle(App *app, int id, float x, float y, bool *on, u32 c); // a 34x18 switch

void w_meter(float x, float y, float w, float h, float level, u32 c); // vertical LED meter
void w_panel(float x, float y, float w, float h, u32 edge);          // dark rounded panel
void w_label(App *app, const char *s, float x, float y, u32 c);       // small caps-style section label
