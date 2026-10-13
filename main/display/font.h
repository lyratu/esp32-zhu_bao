#pragma once
/*
 * font.h - tiny 8x8 bitmap font access (public-domain font8x8 by Daniel Hepper,
 * see font8x8_basic.h). Glyph bytes: 8 rows x 8 bits, bit0 = leftmost pixel.
 *
 * ASCII uses that 8x8 table. Everything else (Hanzi, full-width punctuation)
 * falls back to a generated 12x12 table, see font_cjk.h / font_cjk.c.
 */
#include <stdint.h>
#include <stdbool.h>

/* Cell geometry + the generated glyph table. Kept here (rather than only in
 * font_cjk.c) because the renderer in ui.c needs CJK_GLYPH_W/H to lay out
 * and wrap text. */
#include "font_cjk.h"

/* Returns pointer to the 8-row bitmap of printable ASCII char (0x20..0x7E).
 * Out-of-range characters map to '?'. */
const uint8_t *font_glyph(char ch);

/* 12x12 Hanzi cell. Returns false when the codepoint is not in the table. */
bool cjk_glyph(uint32_t cp, const uint16_t **rows);

/* Decode one UTF-8 sequence at `s`. Returns the codepoint and advances *s
 * past it. Malformed input yields '?' and advances one byte, so callers can
 * never loop forever. */
uint32_t utf8_next(const char **s);

/* Width in pixels of one character, at the given scale. Hanzi are 12*scale,
 * ASCII is 8*scale - the renderer uses this to lay text out. */
int char_adv(uint32_t cp, int scale);
