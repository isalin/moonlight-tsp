/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MOONLIGHT_TSP_UI_DRAW_H
#define MOONLIGHT_TSP_UI_DRAW_H

#include <stdint.h>

/*
 * Drawing for the UI: the device's SDL2 renderer (Mali GLES on fbdev) and
 * SDL2_ttf. The UI draws a whole frame only when something changed, so
 * text textures are cached between frames.
 */
#define UI_W 1280
#define UI_H 720

enum ui_font { F_SMALL, F_BODY, F_TITLE, F_HUGE, F_COUNT };
enum ui_align { AL_LEFT, AL_CENTER, AL_RIGHT };

/* Colours are 0xRRGGBBAA. */
int draw_init(void);
void draw_quit(void);

void draw_clear(uint32_t rgba);
void draw_rect(int x, int y, int w, int h, uint32_t rgba);
void draw_round_rect(int x, int y, int w, int h, int r, uint32_t rgba);
void draw_disc(int cx, int cy, int r, uint32_t rgba);

/*
 * Draws S with its top-left (or centre/right edge) at X, Y; shortens it
 * with "..." to fit MAXW if MAXW > 0. Returns the drawn width.
 */
int draw_text(enum ui_font f, int x, int y, uint32_t rgba, const char *s,
              enum ui_align al, int maxw);
int draw_text_width(enum ui_font f, const char *s);
int draw_font_height(enum ui_font f);

/* Saves the frame being drawn as BMP (call before draw_present). */
int draw_screenshot(const char *path);
void draw_present(void);

const char *draw_info(void);

#endif
