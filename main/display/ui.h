#pragma once
/*
 * ui.h - screen renderer for the 128x64 OLED (0.96" panel).
 * Reads a snapshot of g_state and paints one of the screens:
 *   splash / portal setup / wifi / cloud progress / monitor / error.
 *
 * Monitor screen bands: header | temps | big percent | progress bar | bottom.
 *
 * Text helpers are UTF-8 aware: ASCII uses an 8x8 font, Hanzi a generated
 * 12x12 one (font_cjk.h). Long strings can be shrunk to fit or auto-wrapped,
 * which matters on a 128px-wide panel.
 */
#include <stdbool.h>

void ui_init(void);
void ui_render(void);   /* call periodically (e.g. 4 Hz) from ui task */

/* --- text helpers (also used by the screens in ui.c) --- */

/* Pixel width of a UTF-8 string at `scale`. */
int ui_text_w(const char *s, int scale);
int ui_text_w_range(const char *a, const char *b, int scale);

/* Basic drawing. */
void ui_text(int x, int y, const char *s, int scale, bool invert);
void ui_text_right(int xr, int y, const char *s, int scale, bool invert);
void ui_text_center(int y, const char *s, int scale, bool invert);

/* Largest scale in 1..max_scale that keeps `s` within `maxw` pixels. */
int ui_fit_scale(const char *s, int maxw, int max_scale);

/* Centred, auto-shrinking to fit `maxw`. Prevents truncation of long values
 * such as the device IP. */
void ui_text_center_fit(int y, const char *s, int maxw, int max_scale,
                        bool invert);

/* Draw `s` starting at (x,y) wrapped into `maxw` pixels, one line every `lh`
 * pixels. Breaks on spaces for Latin, at character boundaries for CJK.
 * Returns the number of lines drawn. */
int ui_text_wrap(int x, int y, int maxw, const char *s, int scale, int lh,
                 bool invert);
