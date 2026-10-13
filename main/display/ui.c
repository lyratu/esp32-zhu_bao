#include "ui.h"
#include "ssd1306.h"
#include "font.h"
#include "app_state.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

/* ---------- text primitives ---------- */

/* Plain 8x8 ASCII glyph (bit0 = leftmost pixel in font8x8_basic.h). */
static void draw_char(int x, int y, char ch, int scale, bool invert)
{
    const uint8_t *g = font_glyph(ch);
    for (int r = 0; r < 8; r++) {
        uint8_t row = g[r];
        for (int c = 0; c < 8; c++) {
            int on = (row >> c) & 1;
            if (invert) {
                on = !on;
            }
            if (on) {
                ssd1306_fill_rect(x + c * scale, y + r * scale, scale, scale, 1);
            }
        }
    }
}

/* Draw one 12x12 Hanzi cell. `rows` is 12 uint16, bit x = column x. */
static void draw_cjk(int x, int y, const uint16_t *rows, int scale, bool invert)
{
    for (int r = 0; r < CJK_GLYPH_H; r++) {
        uint16_t row = rows[r];
        for (int c = 0; c < CJK_GLYPH_W; c++) {
            int on = (row >> c) & 1;
            if (invert) {
                on = !on;
            }
            if (on) {
                ssd1306_fill_rect(x + c * scale, y + r * scale, scale, scale, 1);
            }
        }
    }
}

/* A character is either the 8x8 ASCII glyph or a 12x12 Hanzi; this dispatches
 * to the right one and reports the drawn width in pixels. */
static int draw_glyph(int x, int y, uint32_t cp, int scale, bool invert)
{
    const uint16_t *rows;
    if (cp < 0x80) {
        /* keep ASCII on the original crisp 8x8 path */
        draw_char(x, y, (char)cp, scale, invert);
        return 8 * scale;
    }
    if (cjk_glyph(cp, &rows)) {
        draw_cjk(x, y, rows, scale, invert);
        return CJK_GLYPH_W * scale;
    }
    /* Unknown Hanzi: draw a hollow box so a missing glyph is obvious instead
     * of silently rendering as nothing. */
    int w = CJK_GLYPH_W * scale;
    int h = CJK_GLYPH_H * scale;
    ssd1306_rect(x, y, w, h, invert ? 0 : 1);
    return w;
}

int ui_text_w(const char *s, int scale)
{
    int w = 0;
    const char *p = s;
    while (*p) {
        uint32_t cp = utf8_next(&p);
        if (!cp) break;
        w += char_adv(cp, scale);
    }
    return w;
}

void ui_text(int x, int y, const char *s, int scale, bool invert)
{
    const char *p = s;
    while (*p) {
        uint32_t cp = utf8_next(&p);
        if (!cp) break;
        x += draw_glyph(x, y, cp, scale, invert);
    }
}

void ui_text_right(int xr, int y, const char *s, int scale, bool invert)
{
    ui_text(xr - ui_text_w(s, scale), y, s, scale, invert);
}

void ui_text_center(int y, const char *s, int scale, bool invert)
{
    int x = (SSD1306_W - ui_text_w(s, scale)) / 2;
    if (x < 0) x = 0;
    ui_text(x, y, s, scale, invert);
}

/* Largest scale (1..max) at which `s` still fits in `maxw` pixels. Used so a
 * long value like an IP address shrinks instead of getting cut off. */
int ui_fit_scale(const char *s, int maxw, int max_scale)
{
    for (int sc = max_scale; sc > 1; sc--) {
        if (ui_text_w(s, sc) <= maxw) {
            return sc;
        }
    }
    return 1;
}

/* Draw `s` centred horizontally, auto-shrinking to fit `maxw`. */
void ui_text_center_fit(int y, const char *s, int maxw, int max_scale,
                        bool invert)
{
    int sc = ui_fit_scale(s, maxw, max_scale);
    int x = (SSD1306_W - ui_text_w(s, sc)) / 2;
    if (x < 0) x = 0;
    ui_text(x, y, s, sc, invert);
}

/* Word/character-aware wrapping.
 *
 * Breaks on spaces when possible so Latin words stay whole, but falls back to
 * a hard break at the character boundary - which is what CJK wants, since
 * Hanzi have no spaces. Returns the number of lines drawn. Each line is
 * clipped to `maxw` pixels.
 *
 * `lh` is the line height in pixels; pass the scaled glyph height so lines do
 * not overlap at scale > 1. */
int ui_text_wrap(int x, int y, int maxw, const char *s, int scale, int lh,
                 bool invert)
{
    if (!s || !*s || lh <= 0) {
        return 0;
    }

    int line = 0;
    const char *p = s;

    while (*p) {
        /* Build the longest prefix that fits maxw. Track the last space so a
         * Latin word can be moved whole to the next line. */
        const char *line_start = p;
        const char *last_space = NULL;
        int w = 0;
        const char *q = p;

        while (*q) {
            const char *before = q;
            uint32_t cp = utf8_next(&q);
            if (!cp) break;
            int adv = char_adv(cp, scale);
            if (w + adv > maxw) {
                q = before;   /* do not consume the overflowing char */
                break;
            }
            if (cp == ' ') {
                last_space = before;
            }
            w += adv;
        }

        if (q == line_start) {
            /* Not even one character fits (maxw too small): take one char so
             * the loop always advances. */
            uint32_t cp = utf8_next(&q);
            if (!cp) break;
            w = char_adv(cp, scale);
        } else if (last_space && *q) {
            /* More input remains and we have a space: break after the space
             * and skip it on the next line. */
            q = last_space + 1;
            w = ui_text_w_range(line_start, last_space, scale);
        }

        /* Draw this line without copying it into a buffer. */
        int cx = x;
        const char *r = line_start;
        while (r < q) {
            uint32_t cp = utf8_next(&r);
            if (!cp) break;
            cx += draw_glyph(cx, y + line * lh, cp, scale, invert);
        }
        (void)w;

        line++;
        p = q;
        /* skip a single leading space on the continuation line */
        if (*p == ' ') {
            p++;
        }
    }
    return line;
}

/* Width of the half-open range [a,b) in pixels, without needing NUL. */
int ui_text_w_range(const char *a, const char *b, int scale)
{
    int w = 0;
    const char *p = a;
    while (p < b) {
        uint32_t cp = utf8_next(&p);
        if (!cp) break;
        w += char_adv(cp, scale);
    }
    return w;
}


/* ---------- layout grid for the 128x64 panel ----------
 *
 * The monitor screen uses five horizontal bands:
 *   y=0..11   header: device name + link dot | state
 *   y=12      separator rule
 *   y=13..24  temps (nozzle / bed)
 *   y=26..41  big percent (scale 2)       | layer counter
 *   y=43..50  full-width progress bar
 *   y=52..63  bottom: remaining time | speed, or an inverted error code
 * ASCII glyphs inside a CJK row get a +2px vertical offset so both share an
 * optical centre line. The title strip used by portal screens doubles as its
 * own separator line. */

/* Mixed Latin/CJK line at scale 1. */
static void mixed_text(int x, int y, const char *s, bool invert)
{
    const char *p = s;
    while (*p) {
        uint32_t cp = utf8_next(&p);
        if (!cp) break;
        x += draw_glyph(x, y + (cp < 0x80 ? 2 : 0), cp, 1, invert);
    }
}

static void mixed_right(int xr, int y, const char *s, bool invert)
{
    mixed_text(xr - ui_text_w(s, 1), y, s, invert);
}

static void mixed_center(int xc, int y, const char *s, bool invert)
{
    mixed_text(xc - ui_text_w(s, 1) / 2, y, s, invert);
}

/* Inverted full-width title bar (rows 0..11). */
static void title_strip(const char *s)
{
    ssd1306_fill_rect(0, 0, SSD1306_W, 12, 1);
    mixed_center(SSD1306_W / 2, 0, s, true);
}

/* Copy `s` into `out`, trimming whole UTF-8 characters from the end until the
 * line fits `maxw` pixels. Device names have no length contract (they come
 * from the cloud) and would otherwise collide with the state label. */
static void fit_copy(char *out, size_t outlen, const char *s, int maxw)
{
    strlcpy(out, s, outlen);
    while (out[0] && ui_text_w(out, 1) > maxw) {
        size_t n = strlen(out) - 1;
        while (n > 0 && ((unsigned char)out[n] & 0xC0) == 0x80) n--;
        out[n] = 0;
    }
}

/* small degree ring (4x4) drawn after a temperature value */
static void draw_degree(int x, int y)
{
    ssd1306_pixel(x + 1, y + 0, 1);
    ssd1306_pixel(x + 2, y + 0, 1);
    ssd1306_pixel(x + 0, y + 1, 1);
    ssd1306_pixel(x + 3, y + 1, 1);
    ssd1306_pixel(x + 0, y + 2, 1);
    ssd1306_pixel(x + 3, y + 2, 1);
    ssd1306_pixel(x + 1, y + 3, 1);
    ssd1306_pixel(x + 2, y + 3, 1);
}

/* "喷:220°" group for one temperature, drawn inside a 12px CJK row. */
static void draw_temp(int x, int y, const char *label, float cur)
{
    char num[12];
    snprintf(num, sizeof(num), "%s:%d", label, (int)(cur + 0.5f));
    mixed_text(x, y, num, false);
    draw_degree(x + ui_text_w(num, 1) + 1, y + 3);
}

static void progress_bar(int x, int y, int w, int h, int pct, bool printing, uint32_t tnow)
{
    ssd1306_rect(x, y, w, h, 1);
    int iw = w - 2;
    int fill = (iw * pct) / 100;
    if (printing && fill < 5) {
        /* marching dashes while there is nothing to fill yet */
        int off = (int)((tnow / 250) % 8);
        for (int i = -8; i < iw; i += 8) {
            int a = i + off;
            int b = a + 3;
            if (a < 0) a = 0;
            if (b > iw) b = iw;
            if (b > a) {
                ssd1306_fill_rect(x + 1 + a, y + 2, b - a, h - 4, 1);
            }
        }
    } else if (fill > 0) {
        ssd1306_fill_rect(x + 1, y + 2, fill, h - 4, 1);
    }
}

/* ---------- status helpers ---------- */

static const char *state_display(const char *gcode, bool mqtt_up, bool got_report)
{
    if (!mqtt_up) {
        return got_report ? "重连中" : "连接中";
    }
    if (strcmp(gcode, "...") == 0) {
        return "等待数据";
    }
    const char *s = gcode;
    if (!strcmp(s, "IDLE"))        return "空闲";
    if (!strcmp(s, "PRINTING") || !strcmp(s, "RUNNING"))  return "打印中";
    if (!strcmp(s, "PAUSE"))       return "已暂停";
    if (!strcmp(s, "PAUSED"))      return "已暂停";
    if (!strcmp(s, "FINISH"))      return "已完成";
    if (!strcmp(s, "FAILED"))      return "失败";
    if (!strcmp(s, "SLICING"))     return "切片中";
    if (!strcmp(s, "PREPARE"))     return "准备中";
    /* Unknown state: show the raw gcode rather than inventing a label. */
    return s;
}

/* Bambu report "spd_level": 1=silent 2=standard 3=sport 4=ludicrous. */
static const char *spd_name(int lvl)
{
    switch (lvl) {
    case 1:  return "静音";
    case 2:  return "标准";
    case 3:  return "运动";
    case 4:  return "狂暴";
    default: return "--";   /* no report yet */
    }
}

/* ---------- screens ---------- */

static void screen_splash(uint32_t tnow)
{
    mixed_center(SSD1306_W / 2, 4, "拓竹监视器", false);
    ui_text_center(20, "BAMBU MONITOR", 1, false);
    mixed_center(SSD1306_W / 2, 36, "正在启动", false);
    /* animated dots trailing the label */
    int dots = 1 + (int)((tnow / 400) % 3);
    int dx = SSD1306_W / 2 + ui_text_w("正在启动", 1) / 2 + 6;
    for (int i = 0; i < dots; i++) {
        ssd1306_fill_rect(dx + i * 6, 41, 3, 3, 1);
    }
}

static void screen_portal(uint32_t tnow)
{
    (void)tnow;
    /* No inverted title strip on this screen: at 12px it costs a fifth of
     * the panel and pushed the how-to lines past the bottom edge. A plain
     * centered title does the same job for free. */
    mixed_center(SSD1306_W / 2, 0, "配网设置", false);
    char l[48];

    /* why we are here: g_state.err now only carries genuine failures left
     * over from the previous boot ("WIFI/LOGIN/NO DEVICE FAIL->CONFIG").
     * First boot and the BOOT long-press pass no reason at all. */
    int y = 13;
    if (g_state.err[0]) {
        int lines = ui_text_wrap(0, y, SSD1306_W, g_state.err, 1, 11, false);
        y += lines * 11 + 2;
    }

    /* the two facts the user needs, on every portal screen: which AP to
     * join and which address to open once connected */
    char ap[24];
    fit_copy(ap, sizeof(ap), g_state.ap_ssid, SSD1306_W - 40);
    snprintf(l, sizeof(l), "热点:%s", ap);
    mixed_center(SSD1306_W / 2, y, l, false);
    ui_text_center(y + 13, "192.168.4.1", 1, false);

    /* with no error line there is room for the how-to as well */
    if (y == 13) {
        mixed_center(SSD1306_W / 2, 36, "手机连接上述热点", false);
        mixed_center(SSD1306_W / 2, 49, "浏览器打开地址", false);
    }
}

/* shown for a few seconds right before rebooting into the portal */
static void screen_portal_wait(uint32_t tnow)
{
    title_strip("! 登录失败 !");
    if (g_state.err[0]) {
        ui_text_wrap(0, 14, SSD1306_W, g_state.err, 1, 12, false);
    } else {
        mixed_center(SSD1306_W / 2, 14, "请打开配网页面", false);
    }
    if ((tnow / 400) % 2) {
        mixed_center(SSD1306_W / 2, 30, "正在重启", false);
    }
}

/* Draw the device IP in the same size as the rest of the text.
 *
 * A 13-char address ("192.168.5.246") is 104px at scale 1 and fits on one
 * line, which reads calmly; a pathological long one is shrunk to fit rather
 * than clipped. */
static void draw_ip(int y, const char *ip, bool invert)
{
    if (!ip || !ip[0]) {
        ui_text_center(y, "无地址", 1, invert);
        return;
    }
    ui_text_center_fit(y, ip, SSD1306_W - 2, 1, invert);
}

/* STA online but the Bambu account is not configured yet: point the phone at
 * the device IP on the same WiFi so the login page can be opened. */
static void screen_login_portal(uint32_t tnow)
{
    if (g_state.err[0]) {
        title_strip("! 出错了 !");
        ui_text_wrap(0, 14, SSD1306_W, g_state.err, 1, 12, false);
        return;
    }

    title_strip("拓竹账号登录");
    mixed_center(SSD1306_W / 2, 14, "手机连同一WiFi", false);

    /* the IP blinks so it is easy to spot between the static lines */
    bool show = (tnow / 600) % 2 || g_state.ip[0] == 0;
    draw_ip(28, g_state.ip, !show);

    mixed_center(SSD1306_W / 2, 42, "浏览器打开上面地址", false);
    mixed_center(SSD1306_W / 2, 54, "登录后即可监控", false);
}

static void screen_status(const char *title, const char *sub)
{
    title_strip(title);
    mixed_center(SSD1306_W / 2, 20, sub, false);
}

static void screen_monitor(uint32_t tnow)
{
    monitor_state_t v;
    xSemaphoreTake(g_state_mux, portMAX_DELAY);
    v = g_state;
    xSemaphoreGive(g_state_mux);

    ssd1306_clear();
    char line[32];
    /* A1 series reports "RUNNING" while printing; X1/P1 use "PRINTING". */
    bool printing = !strcmp(v.gcode_state, "PRINTING") ||
                    !strcmp(v.gcode_state, "RUNNING");

    /* header (y=0..11): device name + link dot | state */
    const char *state = state_display(v.gcode_state, v.mqtt_up, v.got_report);
    int state_w = ui_text_w(state, 1);
    fit_copy(line, sizeof(line), v.dev_name[0] ? v.dev_name : "A1 mini",
             SSD1306_W - state_w - 12);
    mixed_text(0, 0, line, false);
    int nx = ui_text_w(line, 1) + 3;
    if (v.dev_online && v.mqtt_up) {
        bool on = ((tnow / 700) % 2) == 0;
        ssd1306_fill_rect(nx, 4, 5, 5, on);   /* filled = link alive */
    }
    mixed_right(SSD1306_W, 0, state, false);
    ssd1306_hline(0, SSD1306_W - 1, 12, 1);

    /* temps row (y=13..24) */
    draw_temp(0, 13, "喷", v.nozzle_temp);
    draw_temp(62, 13, "床", v.bed_temp);

    /* big percent (y=26..41) | layer counter */
    char pct[8] = "--";
    if (v.percent > 0 || printing) {
        snprintf(pct, sizeof(pct), "%d%%", v.percent);
    }
    ui_text(0, 26, pct, 2, false);
    if (v.layer > 0 || v.total_layer > 0) {
        snprintf(line, sizeof(line), "层:%d/%d", v.layer, v.total_layer);
        mixed_right(SSD1306_W, 28, line, false);
    }

    /* full-width progress bar (y=43..50) */
    progress_bar(0, 43, SSD1306_W, 8, v.percent, printing, tnow);

    /* bottom row (y=52..63): remaining time | speed, or an inverted error code */
    if (v.print_error) {
        snprintf(line, sizeof(line), "错误 %08lX", (unsigned long)v.print_error);
        int w = ui_text_w(line, 1);
        ssd1306_fill_rect(0, 52, w + 6, 12, 1);
        mixed_text(3, 52, line, true);
    } else {
        if (printing && v.remain_min > 0) {
            /* "3h 12min" while over an hour, "45min" below that */
            char eta[16];
            int h = v.remain_min / 60;
            if (h > 99) h = 99;
            if (h > 0) {
                snprintf(eta, sizeof(eta), "%dh %dmin", h, v.remain_min % 60);
            } else {
                snprintf(eta, sizeof(eta), "%dmin", v.remain_min);
            }
            ui_text(0, 54, eta, 1, false);
        }
        mixed_right(SSD1306_W, 52, spd_name(v.spd_lvl), false);
    }
}

static void screen_error(void)
{
    title_strip("! 出错了 !");
    ui_text_wrap(0, 14, SSD1306_W, g_state.err, 1, 12, false);
    if ((now_ms() / 400) % 2) {
        mixed_center(SSD1306_W / 2, 50, "正在重试", false);
    }
}

/* ---------- public ---------- */

void ui_init(void)
{
}

void ui_render(void)
{
    uint32_t tnow = now_ms();
    monitor_state_t v;
    xSemaphoreTake(g_state_mux, portMAX_DELAY);
    v = g_state;
    xSemaphoreGive(g_state_mux);

    switch (v.phase) {
    case APP_PHASE_BOOT:
        ssd1306_clear();
        screen_splash(tnow);
        break;
    case APP_PHASE_PORTAL:
        ssd1306_clear();
        screen_portal(tnow);
        break;
    case APP_PHASE_LOGIN_PORTAL:
        ssd1306_clear();
        screen_login_portal(tnow);
        break;
    case APP_PHASE_WIFI_CONN:
        ssd1306_clear();
        screen_status("WiFi", "正在连接");
        break;
    case APP_PHASE_WIFI_FAIL:
        ssd1306_clear();
        screen_status("WiFi", "连接失败 进入配网");
        break;
    case APP_PHASE_LOGIN:
        ssd1306_clear();
        screen_status("云端", "正在登录");
        break;
    case APP_PHASE_DEVICE:
        ssd1306_clear();
        screen_status("云端", "获取设备列表");
        break;
    case APP_PHASE_MQTT_CONN:
        ssd1306_clear();
        screen_status("MQTT", "正在连接");
        break;
    case APP_PHASE_RUNNING:
        screen_monitor(tnow);
        break;
    case APP_PHASE_PORTAL_WAIT:
        ssd1306_clear();
        screen_portal_wait(tnow);
        break;
    case APP_PHASE_ERROR:
    default:
        screen_error();
        break;
    }
    ssd1306_flush();
}
