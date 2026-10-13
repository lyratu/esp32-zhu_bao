/* Public-domain 8x8 font (Daniel Hepper / Marcel Sondaar). See header comment
 * in font8x8_basic.h. Included once here; access via font.h/font_glyph().
 *
 * Hanzi come from font_cjk.h, a generated 12x12 table containing exactly the
 * characters this UI uses (see scripts/gen_cjk_font.py). A full CJK font is
 * megabytes; this is under 3 KB. */
#include "font8x8_basic.h"
#include "font_cjk.h"
#include "font.h"

const uint8_t *font_glyph(char ch)
{
    unsigned char c = (unsigned char)ch;
    if (c < 0x20 || c > 0x7E) {
        c = '?';
    }
    return (const uint8_t *)&font8x8_basic[c][0];
}

/* Binary search the sorted codepoint table. Sorted order is guaranteed by the
 * generator (it emits sorted(set(...))), which also keeps the file diffable. */
bool cjk_glyph(uint32_t cp, const uint16_t **rows)
{
    int lo = 0, hi = (int)CJK_GLYPH_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint32_t c = cjk_glyphs[mid].cp;
        if (cp == c) {
            if (rows) *rows = cjk_glyphs[mid].rows;
            return true;
        }
        if (cp < c) hi = mid - 1;
        else        lo = mid + 1;
    }
    return false;
}

uint32_t utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    if (!p || !*p) {
        return 0;
    }
    uint32_t cp;
    int extra;

    if (p[0] < 0x80) {                 /* ASCII */
        cp = p[0];
        extra = 0;
    } else if ((p[0] & 0xE0) == 0xC0) { /* 2-byte */
        cp = p[0] & 0x1F;
        extra = 1;
    } else if ((p[0] & 0xF0) == 0xE0) { /* 3-byte (covers BMP CJK) */
        cp = p[0] & 0x0F;
        extra = 2;
    } else if ((p[0] & 0xF8) == 0xF0) { /* 4-byte */
        cp = p[0] & 0x07;
        extra = 3;
    } else {                           /* malformed lead byte */
        *s = (const char *)p + 1;
        return '?';
    }

    for (int i = 1; i <= extra; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            /* Truncated sequence: consume only the lead byte so the caller
             * makes progress and we never spin. */
            *s = (const char *)p + 1;
            return '?';
        }
        cp = (cp << 6) | (p[i] & 0x3F);
    }

    *s = (const char *)(p + 1 + extra);
    return cp;
}

int char_adv(uint32_t cp, int scale)
{
    if (cp < 0x80) {
        return 8 * scale;              /* ASCII: 8x8 cell */
    }
    return CJK_GLYPH_W * scale;        /* Hanzi: 12x12 cell */
}
