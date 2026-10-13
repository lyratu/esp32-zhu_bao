#pragma once
/*
 * ssd1306.h - minimal framebuffer driver for 128x64 SSD1306 / SH1106 OLED
 * over the new (v6.x) i2c_master API.
 */
#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>

#define SSD1306_W 128
#define SSD1306_H 64   /* 0.96" panel: 8 pages (display area 21.744x10.864mm) */

typedef struct {
    int     port;       /* I2C port number, e.g. 0            */
    int     sda;        /* GPIO, e.g. 3                       */
    int     scl;        /* GPIO, e.g. 4                       */
    int     freq_hz;    /* e.g. 400000                        */
    uint8_t addr;       /* 0x3C or 0x3D                       */
    bool    sh1106;     /* true = SH1106 controller           */
} ssd1306_cfg_t;

esp_err_t ssd1306_init(const ssd1306_cfg_t *cfg);

void ssd1306_clear(void);
void ssd1306_pixel(int x, int y, int on);
void ssd1306_hline(int x0, int x1, int y, int on);
void ssd1306_vline(int x, int y0, int y1, int on);
void ssd1306_rect(int x, int y, int w, int h, int on);
void ssd1306_fill_rect(int x, int y, int w, int h, int on);
void ssd1306_flush(void);
