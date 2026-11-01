#ifndef UI_FONT_H
#define UI_FONT_H

#include "ui_gl.h"

// The menus' text: Outfit (Google Fonts, OFL), baked once into a glyph atlas per style
// with stb_truetype. Latin-1 text (UTF-8 in).

typedef enum {
    UI_TEXT_SMALL,  // 22 px regular: hints, details
    UI_TEXT_LABEL,  // 26 px semibold: a game's name under its icon
    UI_TEXT_BODY,   // 30 px regular: settings rows
    UI_TEXT_CLOCK,  // 34 px semibold: the clock, the top bar
    UI_TEXT_TITLE,  // 48 px bold: panel titles
    UI_TEXT_HUGE,   // 72 px bold: the selected game's name
    UI_TEXT_STYLES
} ui_text_style;

#define UI_ALIGN_LEFT 0
#define UI_ALIGN_CENTER 1
#define UI_ALIGN_RIGHT 2

int ui_font_init(const char *font_dir); // 0 = ok
// every style a size up or down (the Text size: 1, or 1.15 for Large); bakes them again
void ui_font_set_scale(float scale);
float ui_text_width(ui_text_style style, const char *s);
float ui_text_height(ui_text_style style); // the line's ascent + descent
float ui_text_cap(ui_text_style style);     // the capitals' height
float ui_text_cap_mid(ui_text_style style); // from the line's top to the capitals' middle (an icon beside the text)
// y is the top of the line
void ui_text(ui_text_style style, float x, float y, const char *s, ui_color col, int align);
// clipped to max_w with "..." when longer
void ui_text_fit(ui_text_style style, float x, float y, float max_w, const char *s, ui_color col, int align);

#endif
