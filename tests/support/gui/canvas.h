// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
/* Host stand-in for the Flipper's <gui/canvas.h>: the part of the Canvas API
 * that flipper/tpmsflipper/wt_ui.h draws with, on a 128 x 64 one-bit
 * bitmap. Text is drawn with the device's own fonts (extract_fonts.py reads
 * them from the SDK firmware image) through a decoder for the u8g2 glyph
 * format, so string widths and glyph shapes are the device's. Circles and
 * rounded corners follow u8g2's midpoint algorithm.
 *
 * Two things the device does not have, for the tests:
 *   - canvas->clipped counts pixels a draw call tried to set outside the
 *     screen, and canvas->text_clipped counts strings that did not fit;
 *   - canvas_host_write_pbm() saves the bitmap.
 *
 * Define WT_CANVAS_IMPL in exactly one .c file before including this header.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define WT_SCREEN_W 128
#define WT_SCREEN_H 64

typedef enum {
    ColorWhite = 0x00,
    ColorBlack = 0x01,
    ColorXOR = 0x02,
} Color;

typedef enum {
    FontPrimary,
    FontSecondary,
    FontKeyboard,
    FontBigNumbers,
    FontTotalNumber,
} Font;

typedef enum {
    AlignLeft,
    AlignRight,
    AlignTop,
    AlignBottom,
    AlignCenter,
} Align;

typedef struct Canvas {
    uint8_t px[WT_SCREEN_H][WT_SCREEN_W];
    Color color;
    const unsigned char* font;
    unsigned clipped; /* pixels outside the screen */
    unsigned text_clipped; /* strings that ran off the screen */
    char last_clipped[48];
} Canvas;

size_t canvas_width(const Canvas* canvas);
size_t canvas_height(const Canvas* canvas);
void canvas_clear(Canvas* canvas);
void canvas_set_color(Canvas* canvas, Color color);
void canvas_set_font(Canvas* canvas, Font font);
void canvas_draw_str(Canvas* canvas, int32_t x, int32_t y, const char* str);
void canvas_draw_str_aligned(Canvas* canvas, int32_t x, int32_t y, Align horizontal, Align vertical, const char* str);
uint16_t canvas_string_width(Canvas* canvas, const char* str);
void canvas_draw_xbm(Canvas* canvas, int32_t x, int32_t y, size_t width, size_t height, const uint8_t* bitmap);
void canvas_draw_dot(Canvas* canvas, int32_t x, int32_t y);
void canvas_draw_box(Canvas* canvas, int32_t x, int32_t y, size_t width, size_t height);
void canvas_draw_frame(Canvas* canvas, int32_t x, int32_t y, size_t width, size_t height);
void canvas_draw_line(Canvas* canvas, int32_t x1, int32_t y1, int32_t x2, int32_t y2);
void canvas_draw_circle(Canvas* canvas, int32_t x, int32_t y, size_t radius);
void canvas_draw_disc(Canvas* canvas, int32_t x, int32_t y, size_t radius);
void canvas_draw_rframe(Canvas* canvas, int32_t x, int32_t y, size_t width, size_t height, size_t radius);
void canvas_draw_rbox(Canvas* canvas, int32_t x, int32_t y, size_t width, size_t height, size_t radius);

void canvas_host_init(Canvas* canvas);
bool canvas_host_write_pbm(const Canvas* canvas, const char* path);

#ifdef WT_CANVAS_IMPL
#include <string.h>
#include "wt_fonts_gen.h"

void canvas_host_init(Canvas* c) {
    memset(c, 0, sizeof(*c));
    c->color = ColorBlack;
    c->font = wt_font_secondary;
}
size_t canvas_width(const Canvas* c) {
    (void)c;
    return WT_SCREEN_W;
}
size_t canvas_height(const Canvas* c) {
    (void)c;
    return WT_SCREEN_H;
}
void canvas_clear(Canvas* c) {
    memset(c->px, 0, sizeof(c->px));
}
void canvas_set_color(Canvas* c, Color color) {
    c->color = color;
}
void canvas_set_font(Canvas* c, Font font) {
    c->font = font == FontPrimary   ? wt_font_primary :
              font == FontKeyboard  ? wt_font_keyboard :
              font == FontBigNumbers ? wt_font_bignum :
                                      wt_font_secondary;
}
static void wt_px(Canvas* c, int32_t x, int32_t y) {
    if(x < 0 || y < 0 || x >= WT_SCREEN_W || y >= WT_SCREEN_H) {
        c->clipped++;
        return;
    }
    if(c->color == ColorXOR)
        c->px[y][x] ^= 1;
    else
        c->px[y][x] = c->color == ColorBlack;
}
void canvas_draw_dot(Canvas* c, int32_t x, int32_t y) {
    wt_px(c, x, y);
}
static void wt_hline(Canvas* c, int32_t x, int32_t y, int32_t w) {
    for(int32_t i = 0; i < w; i++) wt_px(c, x + i, y);
}
static void wt_vline(Canvas* c, int32_t x, int32_t y, int32_t h) {
    for(int32_t i = 0; i < h; i++) wt_px(c, x, y + i);
}
void canvas_draw_box(Canvas* c, int32_t x, int32_t y, size_t w, size_t h) {
    for(size_t j = 0; j < h; j++) wt_hline(c, x, y + (int32_t)j, (int32_t)w);
}
void canvas_draw_frame(Canvas* c, int32_t x, int32_t y, size_t w, size_t h) {
    if(!w || !h) return;
    wt_hline(c, x, y, (int32_t)w);
    if(h > 1) wt_hline(c, x, y + (int32_t)h - 1, (int32_t)w);
    if(h > 2) {
        wt_vline(c, x, y + 1, (int32_t)h - 2);
        if(w > 1) wt_vline(c, x + (int32_t)w - 1, y + 1, (int32_t)h - 2);
    }
}
void canvas_draw_line(Canvas* c, int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    /* u8g2_DrawLine */
    int32_t dx = x1 > x2 ? x1 - x2 : x2 - x1, dy = y1 > y2 ? y1 - y2 : y2 - y1;
    bool swapxy = false;
    int32_t t;
    if(dy > dx) {
        swapxy = true;
        t = dx, dx = dy, dy = t;
        t = x1, x1 = y1, y1 = t;
        t = x2, x2 = y2, y2 = t;
    }
    if(x1 > x2) {
        t = x1, x1 = x2, x2 = t;
        t = y1, y1 = y2, y2 = t;
    }
    int32_t err = dx >> 1, ystep = y2 > y1 ? 1 : -1, y = y1;
    for(int32_t x = x1; x <= x2; x++) {
        if(swapxy)
            wt_px(c, y, x);
        else
            wt_px(c, x, y);
        err -= dy;
        if(err < 0) {
            y += ystep;
            err += dx;
        }
    }
}
enum { WT_UR = 1, WT_UL = 2, WT_LL = 4, WT_LR = 8, WT_ALL = 15 };
static void wt_circle_section(Canvas* c, int32_t x, int32_t y, int32_t x0, int32_t y0, int opt) {
    if(opt & WT_UR) wt_px(c, x0 + x, y0 - y), wt_px(c, x0 + y, y0 - x);
    if(opt & WT_UL) wt_px(c, x0 - x, y0 - y), wt_px(c, x0 - y, y0 - x);
    if(opt & WT_LR) wt_px(c, x0 + x, y0 + y), wt_px(c, x0 + y, y0 + x);
    if(opt & WT_LL) wt_px(c, x0 - x, y0 + y), wt_px(c, x0 - y, y0 + x);
}
static void wt_disc_section(Canvas* c, int32_t x, int32_t y, int32_t x0, int32_t y0, int opt) {
    if(opt & WT_UR) wt_vline(c, x0 + x, y0 - y, y + 1), wt_vline(c, x0 + y, y0 - x, x + 1);
    if(opt & WT_UL) wt_vline(c, x0 - x, y0 - y, y + 1), wt_vline(c, x0 - y, y0 - x, x + 1);
    if(opt & WT_LR) wt_vline(c, x0 + x, y0, y + 1), wt_vline(c, x0 + y, y0, x + 1);
    if(opt & WT_LL) wt_vline(c, x0 - x, y0, y + 1), wt_vline(c, x0 - y, y0, x + 1);
}
static void wt_arc(Canvas* c, int32_t x0, int32_t y0, int32_t rad, int opt, bool fill) {
    int32_t f = 1 - rad, ddx = 1, ddy = -2 * rad, x = 0, y = rad;
    (fill ? wt_disc_section : wt_circle_section)(c, x, y, x0, y0, opt);
    while(x < y) {
        if(f >= 0) {
            y--;
            ddy += 2;
            f += ddy;
        }
        x++;
        ddx += 2;
        f += ddx;
        (fill ? wt_disc_section : wt_circle_section)(c, x, y, x0, y0, opt);
    }
}
void canvas_draw_circle(Canvas* c, int32_t x, int32_t y, size_t r) {
    wt_arc(c, x, y, (int32_t)r, WT_ALL, false);
}
void canvas_draw_disc(Canvas* c, int32_t x, int32_t y, size_t r) {
    wt_arc(c, x, y, (int32_t)r, WT_ALL, true);
}
void canvas_draw_rframe(Canvas* c, int32_t x, int32_t y, size_t w, size_t h, size_t r) {
    /* u8g2_DrawRFrame */
    int32_t xl = x + (int32_t)r, yu = y + (int32_t)r;
    int32_t xr = x + (int32_t)w - (int32_t)r - 1, yl = y + (int32_t)h - (int32_t)r - 1;
    wt_arc(c, xl, yu, (int32_t)r, WT_UL, false);
    wt_arc(c, xr, yu, (int32_t)r, WT_UR, false);
    wt_arc(c, xl, yl, (int32_t)r, WT_LL, false);
    wt_arc(c, xr, yl, (int32_t)r, WT_LR, false);
    int32_t ww = (int32_t)w - 2 * (int32_t)r - 2, hh = (int32_t)h - 2 * (int32_t)r - 2;
    xl++, yu++;
    if(ww >= 3) {
        wt_hline(c, xl, y, ww);
        wt_hline(c, xl, y + (int32_t)h - 1, ww);
    }
    if(hh >= 3) {
        wt_vline(c, x, yu, hh);
        wt_vline(c, x + (int32_t)w - 1, yu, hh);
    }
}
void canvas_draw_rbox(Canvas* c, int32_t x, int32_t y, size_t w, size_t h, size_t r) {
    /* u8g2_DrawRBox */
    int32_t xl = x + (int32_t)r, yu = y + (int32_t)r;
    int32_t xr = x + (int32_t)w - (int32_t)r - 1, yl = y + (int32_t)h - (int32_t)r - 1;
    wt_arc(c, xl, yu, (int32_t)r, WT_UL, true);
    wt_arc(c, xr, yu, (int32_t)r, WT_UR, true);
    wt_arc(c, xl, yl, (int32_t)r, WT_LL, true);
    wt_arc(c, xr, yl, (int32_t)r, WT_LR, true);
    int32_t ww = (int32_t)w - 2 * (int32_t)r - 2, hh = (int32_t)h - 2 * (int32_t)r - 2;
    xl++, yu++;
    if(ww >= 3) {
        canvas_draw_box(c, xl, y, (size_t)ww, r + 1);
        canvas_draw_box(c, xl, yl, (size_t)ww, r + 1);
    }
    if(hh >= 3) canvas_draw_box(c, x, yu, w, (size_t)hh);
}
void canvas_draw_xbm(Canvas* c, int32_t x, int32_t y, size_t w, size_t h, const uint8_t* bitmap) {
    size_t stride = (w + 7) / 8;
    for(size_t j = 0; j < h; j++)
        for(size_t i = 0; i < w; i++)
            if(bitmap[j * stride + i / 8] & (1u << (i % 8))) wt_px(c, x + (int32_t)i, y + (int32_t)j);
}

/* ----- u8g2 font decoding ---------------------------------------------------- */
typedef struct {
    const unsigned char* ptr;
    unsigned bit;
} WtBits;
static unsigned wt_bits(WtBits* b, unsigned cnt) {
    unsigned val = *b->ptr >> b->bit, end = b->bit + cnt;
    if(end >= 8) {
        b->ptr++;
        val |= (unsigned)*b->ptr << (8 - b->bit);
        end -= 8;
    }
    b->bit = end;
    return val & ((1u << cnt) - 1u);
}
static int wt_sbits(WtBits* b, unsigned cnt) {
    return (int)wt_bits(b, cnt) - (1 << (cnt - 1));
}
static const unsigned char* wt_glyph(const unsigned char* font, unsigned ch) {
    const unsigned char* p = font + 23;
    if(ch >= 'a')
        p += (font[19] << 8) | font[20];
    else if(ch >= 'A')
        p += (font[17] << 8) | font[18];
    for(; p[1]; p += p[1])
        if(p[0] == ch) return p + 2;
    return NULL;
}
/* Draws one glyph with its origin at (x, baseline y) when c is not NULL.
 * Returns the advance; *right gets the x just past its last column. */
static int wt_draw_glyph(Canvas* c, const unsigned char* font, int32_t x, int32_t y, unsigned ch, int32_t* right) {
    const unsigned char* g = wt_glyph(font, ch);
    if(right) *right = x;
    if(!g) return 0;
    WtBits b = {g, 0};
    int gw = (int)wt_bits(&b, font[4]), gh = (int)wt_bits(&b, font[5]);
    int ox = wt_sbits(&b, font[6]), oy = wt_sbits(&b, font[7]), dx = wt_sbits(&b, font[8]);
    if(right) *right = x + ox + gw;
    if(gw > 0 && c) {
        int lx = 0, ly = 0;
        int32_t tx = x + ox, ty = y - (gh + oy);
        while(ly < gh) {
            unsigned zeros = wt_bits(&b, font[2]), ones = wt_bits(&b, font[3]);
            do {
                for(int pass = 0; pass < 2; pass++) {
                    unsigned len = pass ? ones : zeros;
                    while(len) {
                        unsigned run = (unsigned)(gw - lx);
                        if(run > len) run = len;
                        if(pass)
                            for(unsigned i = 0; i < run; i++) wt_px(c, tx + lx + (int)i, ty + ly);
                        lx += (int)run;
                        len -= run;
                        if(lx >= gw) lx = 0, ly++;
                    }
                }
            } while(wt_bits(&b, 1));
        }
    }
    return dx;
}
uint16_t canvas_string_width(Canvas* c, const char* str) {
    /* u8g2_GetStrWidth: the advances, the last glyph counted by its ink. */
    int32_t x = 0, right = 0;
    int dx = 0;
    for(const char* s = str; *s; s++) {
        dx = wt_draw_glyph(NULL, c->font, x, 0, (unsigned char)*s, &right);
        x += dx;
    }
    if(*str && right > x - dx) return (uint16_t)right;
    return (uint16_t)x;
}
void canvas_draw_str(Canvas* c, int32_t x, int32_t y, const char* str) {
    unsigned before = c->clipped;
    for(const char* s = str; *s; s++) x += wt_draw_glyph(c, c->font, x, y, (unsigned char)*s, NULL);
    if(c->clipped != before) {
        c->text_clipped++;
        snprintf(c->last_clipped, sizeof(c->last_clipped), "%s", str);
    }
}
void canvas_draw_str_aligned(Canvas* c, int32_t x, int32_t y, Align horizontal, Align vertical, const char* str) {
    int ascent = (signed char)c->font[13];
    if(horizontal == AlignRight)
        x -= canvas_string_width(c, str);
    else if(horizontal == AlignCenter)
        x -= canvas_string_width(c, str) / 2;
    if(vertical == AlignTop)
        y += ascent;
    else if(vertical == AlignCenter)
        y += ascent / 2;
    canvas_draw_str(c, x, y, str);
}
bool canvas_host_write_pbm(const Canvas* c, const char* path) {
    FILE* f = fopen(path, "w");
    if(!f) return false;
    fprintf(f, "P1\n%d %d\n", WT_SCREEN_W, WT_SCREEN_H);
    for(int y = 0; y < WT_SCREEN_H; y++) {
        for(int x = 0; x < WT_SCREEN_W; x++) fputc(c->px[y][x] ? '1' : '0', f);
        fputc('\n', f);
    }
    return fclose(f) == 0;
}
#endif
