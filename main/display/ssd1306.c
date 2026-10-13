#include "ssd1306.h"
#include <string.h>
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ssd1306";

static uint8_t s_fb[SSD1306_W * SSD1306_H / 8];
static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;
static bool s_sh1106 = false;

static esp_err_t cmd1(uint8_t c)
{
    uint8_t b[2] = { 0x00, c };
    return i2c_master_transmit(s_dev, b, 2, -1);
}

/* Send commands ONE BY ONE with a small settle delay. Some 0.98" panels
 * (notably SH1106 clones) return ESP_ERR_INVALID_RESPONSE if a long
 * command burst arrives back-to-back, because they need time to process. */
static esp_err_t cmd_seq_slow(const uint8_t *c, size_t n, int settle_us)
{
    for (size_t i = 0; i < n; i++) {
        esp_err_t e = cmd1(c[i]);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "cmd 0x%02X failed: %s (idx %u/%u)",
                     c[i], esp_err_to_name(e), (unsigned)i, (unsigned)n);
            return e;
        }
        esp_rom_delay_us(settle_us);
    }
    return ESP_OK;
}

static esp_err_t cmd_list(const uint8_t *c, size_t n)
{
    uint8_t b[80];
    if (n > 40) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (size_t i = 0; i < n; i++) {
        b[2 * i] = 0x00;
        b[2 * i + 1] = c[i];
    }
    return i2c_master_transmit(s_dev, b, 2 * n, -1);
}

static esp_err_t data_bytes(const uint8_t *d, size_t n)
{
    static uint8_t b[SSD1306_W + 1];   /* control byte + one page/frame chunk */
    if (n > SSD1306_W) {
        return ESP_ERR_INVALID_SIZE;
    }
    b[0] = 0x40;
    memcpy(&b[1], d, n);
    return i2c_master_transmit(s_dev, b, n + 1, -1);
}

/* Send up to 8 zero data bytes - used to clear the SH1106's hidden columns. */
static esp_err_t data_zeros(int n)
{
    uint8_t b[9];
    b[0] = 0x40;
    memset(&b[1], 0, n);
    return i2c_master_transmit(s_dev, b, n + 1, -1);
}

/* One-time clear of the WHOLE controller RAM, including the columns outside
 * the visible window. The SH1106 has 132 columns of RAM but this module only
 * wires 128 to the glass; whatever sits in the hidden columns at power-on can
 * ghost through at the panel edge (seen as a stray vertical line). Zero all
 * of it once here - flushes afterwards only write the visible 128 columns,
 * which is enough because nothing ever writes the hidden ones again. */
static esp_err_t ram_clear_all(void)
{
    static uint8_t zeros[SSD1306_W];   /* .bss, already zero */
    esp_err_t err = ESP_OK;
    for (int p = 0; p < SSD1306_H / 8 && err == ESP_OK; p++) {
        err = cmd1(0xB0 | p);          /* page            */
        if (err == ESP_OK) err = cmd1(0x00);  /* col = 0  */
        if (err == ESP_OK) err = cmd1(0x10);
        if (err == ESP_OK) err = data_bytes(zeros, sizeof(zeros)); /* cols 0..127 */
        if (err == ESP_OK) err = data_zeros(4);   /* hidden cols 128..131 */
    }
    return err;
}

esp_err_t ssd1306_init(const ssd1306_cfg_t *cfg)
{
    if (s_dev) {
        return ESP_OK; /* already up */
    }
    i2c_master_bus_config_t bc = {
        .i2c_port = cfg->port,
        .sda_io_num = cfg->sda,
        .scl_io_num = cfg->scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bc, &s_bus), TAG, "i2c bus init");

    /* Many low-cost "SSD1315" / SH1106-clone panels are only reliable at a
     * low I2C clock. Try progressively slower speeds until the controller
     * answers to a plain 0xAE (display off) command. */
    static const int speeds[] = { 100000, 50000, 10000 };
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = cfg->addr,
        .scl_speed_hz = speeds[0],
    };
    esp_err_t err = i2c_master_bus_add_device(s_bus, &dc, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add i2c device: %s", esp_err_to_name(err));
        return err;
    }
    s_sh1106 = cfg->sh1106;

    bool link_ok = false;
    for (size_t k = 0; k < sizeof(speeds) / sizeof(speeds[0]); k++) {
        i2c_device_config_t nd = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = cfg->addr,
            .scl_speed_hz = speeds[k],
        };
        err = i2c_master_bus_rm_device(s_dev);
        if (err != ESP_OK) { ESP_LOGW(TAG, "rm device: %s", esp_err_to_name(err)); }
        err = i2c_master_bus_add_device(s_bus, &nd, &s_dev);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "re-add i2c device: %s", esp_err_to_name(err));
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
        err = cmd1(0xAE);                 /* display off: universal command */
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "I2C link OK at %d Hz", speeds[k]);
            link_ok = true;
            break;
        }
        ESP_LOGW(TAG, "no answer at %d Hz: %s", speeds[k], esp_err_to_name(err));
    }
    if (!link_ok) {
        ESP_LOGE(TAG, "OLED did not answer at any I2C speed - check wiring/pull-ups");
        /* keep going with the slowest speed so the UI still tries */
    }

    /* Common init sequence. We enable BOTH charge-pump opcodes: each
     * controller ignores the other's command, so unknown 0.98" panels
     * (SSD1306/SSD1315-compatible or SH1106-compatible) both light up.
     * NOTE: 0x20 (memory addressing mode) is SSD1306-only and unknown to
     * SH1106, so it is deliberately omitted - flushing always uses page
     * addressing which both controllers understand. */
    uint8_t seq[24];
    size_t i = 0;
    seq[i++] = 0xAE;                 /* display off               */
    seq[i++] = 0xD5; seq[i++] = 0x80;/* clock div                 */
    seq[i++] = 0xA8; seq[i++] = 0x3F;/* multiplex 1/64 (128x64 panel) */
    seq[i++] = 0xD3; seq[i++] = 0x00;/* display offset            */
    seq[i++] = 0x40;                 /* start line 0              */
    seq[i++] = 0x8D; seq[i++] = 0x14; /* charge pump (SSD1306) */
    seq[i++] = 0xAD; seq[i++] = 0x8B; /* DC-DC on    (SH1106)  */
    seq[i++] = 0xA1;                 /* segment remap             */
    seq[i++] = 0xC8;                 /* COM scan direction        */
    seq[i++] = 0xDA; seq[i++] = 0x12;/* COM pins (128x64: alternative) */
    seq[i++] = 0x81; seq[i++] = 0xCF;/* contrast                  */
    seq[i++] = 0xD9; seq[i++] = s_sh1106 ? 0x2F : 0xF1; /* precharge */
    seq[i++] = 0xDB; seq[i++] = 0x40;/* VCOM detect               */
    seq[i++] = 0xA4;                 /* display from RAM          */
    seq[i++] = 0xA6;                 /* normal (non-inverted)     */
    seq[i++] = 0xAF;                 /* display on                */
    err = cmd_seq_slow(seq, i, 3000);/* 3 ms settle between commands */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init cmd failed: %s", esp_err_to_name(err));
        return err;
    }
    memset(s_fb, 0, sizeof(s_fb));
    err = ram_clear_all();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ram clear failed: %s", esp_err_to_name(err));
    }
    ssd1306_flush();
    ESP_LOGI(TAG, "%s OLED ready at 0x%02X (sda=%d scl=%d)",
             s_sh1106 ? "SH1106" : "SSD1306/SSD1315", cfg->addr, cfg->sda, cfg->scl);
    return ESP_OK;
}

void ssd1306_clear(void)
{
    memset(s_fb, 0, sizeof(s_fb));
}

void ssd1306_pixel(int x, int y, int on)
{
    if (x < 0 || x >= SSD1306_W || y < 0 || y >= SSD1306_H) {
        return;
    }
    int page = y >> 3;
    uint8_t bit = 1u << (y & 7);
    if (on) {
        s_fb[page * SSD1306_W + x] |= bit;
    } else {
        s_fb[page * SSD1306_W + x] &= ~bit;
    }
}

void ssd1306_hline(int x0, int x1, int y, int on)
{
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    for (int x = x0; x <= x1; x++) {
        ssd1306_pixel(x, y, on);
    }
}

void ssd1306_vline(int x, int y0, int y1, int on)
{
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    for (int y = y0; y <= y1; y++) {
        ssd1306_pixel(x, y, on);
    }
}

void ssd1306_rect(int x, int y, int w, int h, int on)
{
    ssd1306_hline(x, x + w - 1, y, on);
    ssd1306_hline(x, x + w - 1, y + h - 1, on);
    ssd1306_vline(x, y, y + h - 1, on);
    ssd1306_vline(x + w - 1, y, y + h - 1, on);
}

void ssd1306_fill_rect(int x, int y, int w, int h, int on)
{
    for (int j = 0; j < h; j++) {
        ssd1306_hline(x, x + w - 1, y + j, on);
    }
}

void ssd1306_flush(void)
{
    if (!s_dev) {
        return;
    }
    /* Page-addressing flush: the same protocol works on SSD1306 AND SH1106
     * (SH1106 lacks the 0x21/0x22 column/page-range commands, so the
     * per-page method is used for both controllers).
     *
     * The framebuffer is written starting at column 0: on this module the
     * visible window IS RAM columns 0..127 (proven by the stray boot-garbage
     * line showing up at the physical left edge). Starting at column 2 like
     * generic SH1106 code would shift the whole picture 2px right and clip
     * the last two columns into hidden RAM. The hidden columns 128..131 are
     * zeroed once at init (ram_clear_all) and never touched again. */
    esp_err_t err = ESP_OK;
    for (int p = 0; p < SSD1306_H / 8 && err == ESP_OK; p++) {
        err = cmd1(0xB0 | p);
        if (err == ESP_OK) err = cmd1(0x00);   /* column = 0 */
        if (err == ESP_OK) err = cmd1(0x10);
        if (err == ESP_OK) {
            err = data_bytes(&s_fb[p * SSD1306_W], SSD1306_W);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "flush page %d failed: %s", p, esp_err_to_name(err));
        }
    }
    if (err != ESP_OK) {
        static int64_t last_log_us = -1;
        int64_t now = esp_timer_get_time();
        if (last_log_us < 0 || now - last_log_us > 5000000) {
            last_log_us = now;
            ESP_LOGE(TAG, "flush i2c error: %s", esp_err_to_name(err));
        }
    }
}
