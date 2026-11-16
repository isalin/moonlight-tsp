/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ui_draw.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <SDL.h>
#include <SDL_ttf.h>

#define CACHE_N 160
#define CACHE_TEXT 96
#define DISC 128

static const int font_px[F_COUNT] = { 22, 28, 38, 150 };

static const char *const font_paths[] = {
    "/usr/trimui/res/regular.ttf",
    "/mnt/SDCARD/System/resources/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
};

struct tex_entry {
    SDL_Texture *t;
    int w, h;
    int font;
    unsigned long used;
    char text[CACHE_TEXT];
};

static struct {
    SDL_Window *win;
    SDL_Renderer *r;
    TTF_Font *font[F_COUNT];
    SDL_Texture *disc;
    SDL_Texture *icon[IC_COUNT];
    int icon_w[IC_COUNT], icon_h[IC_COUNT];
    struct tex_entry cache[CACHE_N];
    unsigned long frame;
    char info[256];
} D;

static void set_color(uint32_t c)
{
    SDL_SetRenderDrawColor(D.r, (Uint8)(c >> 24), (Uint8)(c >> 16),
                           (Uint8)(c >> 8), (Uint8)c);
}

static void tint(SDL_Texture *t, uint32_t c)
{
    SDL_SetTextureColorMod(t, (Uint8)(c >> 24), (Uint8)(c >> 16),
                           (Uint8)(c >> 8));
    SDL_SetTextureAlphaMod(t, (Uint8)c);
}

/*
 * Anti-aliased white masks, tinted when drawn. INSIDE says whether a point
 * (in pixels from the top-left corner) is in the shape; each pixel takes
 * 4x4 samples.
 */
static SDL_Texture *make_mask(int w, int h,
                              int (*inside)(float x, float y, int arg),
                              int arg)
{
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32,
                                                    SDL_PIXELFORMAT_ARGB8888);
    SDL_Texture *t;
    int x, y, sx, sy;

    if (s == NULL)
        return NULL;
    for (y = 0; y < h; y++) {
        Uint32 *row = (Uint32 *)((Uint8 *)s->pixels + y * s->pitch);

        for (x = 0; x < w; x++) {
            int in = 0;

            for (sy = 0; sy < 4; sy++)
                for (sx = 0; sx < 4; sx++)
                    in += inside((float)x + ((float)sx + 0.5f) / 4.0f,
                                 (float)y + ((float)sy + 0.5f) / 4.0f, arg);
            row[x] = ((Uint32)(in * 255 / 16) << 24) | 0xffffffu;
        }
    }
    t = SDL_CreateTextureFromSurface(D.r, s);
    SDL_FreeSurface(s);
    if (t != NULL)
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    return t;
}

/* The disc for circles and rounded corners. */
static int in_disc(float x, float y, int arg)
{
    float fx = x - DISC / 2.0f, fy = y - DISC / 2.0f;

    return fx * fx + fy * fy <= (DISC / 2.0f) * (DISC / 2.0f);
}

/*
 * Wi-Fi: a dot and three arcs around a point at the bottom centre, 45
 * degrees to each side of straight up. The arcs have round ends.
 */
#define WIFI_W 36
#define WIFI_H 26
#define WIFI_OX 18.0f
#define WIFI_OY 25.0f
static const float wifi_r[4][2] = {
    { 0.0f, 5.0f }, { 8.0f, 11.5f }, { 14.5f, 18.0f }, { 21.0f, 24.5f },
};

static int in_wifi(float x, float y, int layer)
{
    float dx = x - WIFI_OX, dy = WIFI_OY - y;
    float r0 = wifi_r[layer][0], r1 = wifi_r[layer][1];
    float d2 = dx * dx + dy * dy;

    if (dy >= 0.0f && fabsf(dx) <= dy && d2 >= r0 * r0 && d2 <= r1 * r1)
        return 1;
    if (layer > 0) {
        float m = (r0 + r1) / 2.0f * 0.70710678f, c = (r1 - r0) / 2.0f;
        float ex = fabsf(dx) - m, ey = dy - m;

        return ex * ex + ey * ey <= c * c;
    }
    return 0;
}

/* Lightning bolt (charging). */
#define BOLT_W 12
#define BOLT_H 18
static int in_bolt(float x, float y, int arg)
{
    static const float p[][2] = {
        { 8.0f, 0.0f }, { 1.0f, 10.5f }, { 5.5f, 10.5f }, { 4.0f, 18.0f },
        { 11.0f, 7.5f }, { 6.5f, 7.5f },
    };
    const int n = (int)(sizeof(p) / sizeof(p[0]));
    int i, j, in = 0;

    /* even-odd rule */
    for (i = 0, j = n - 1; i < n; j = i++)
        if ((p[i][1] > y) != (p[j][1] > y) &&
            x < (p[j][0] - p[i][0]) * (y - p[i][1]) / (p[j][1] - p[i][1]) +
                p[i][0])
            in = !in;
    return in;
}

static void make_icons(void)
{
    int i;

    for (i = 0; i < 4; i++) {
        D.icon[IC_WIFI_DOT + i] = make_mask(WIFI_W, WIFI_H, in_wifi, i);
        D.icon_w[IC_WIFI_DOT + i] = WIFI_W;
        D.icon_h[IC_WIFI_DOT + i] = WIFI_H;
    }
    D.icon[IC_BOLT] = make_mask(BOLT_W, BOLT_H, in_bolt, 0);
    D.icon_w[IC_BOLT] = BOLT_W;
    D.icon_h[IC_BOLT] = BOLT_H;
}

static const char *find_font(void)
{
    const char *env = getenv("MLTSP_FONT");
    unsigned i;

    if (env != NULL && access(env, R_OK) == 0)
        return env;
    for (i = 0; i < sizeof(font_paths) / sizeof(font_paths[0]); i++)
        if (access(font_paths[i], R_OK) == 0)
            return font_paths[i];
    return NULL;
}

int draw_init(void)
{
    SDL_DisplayMode dm;
    SDL_RendererInfo ri;
    const char *path;
    int i, w = UI_W, h = UI_H;

    /* Signals stay ours (signalfd); SDL must not swallow SIGTERM. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return -1;
    }
    /* The dummy driver (container tests) gets the native size. */
    if (strcmp(SDL_GetCurrentVideoDriver(), "dummy") != 0 &&
        SDL_GetCurrentDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0) {
        w = dm.w;
        h = dm.h;
    }
    D.win = SDL_CreateWindow("Moonlight HW", SDL_WINDOWPOS_UNDEFINED,
                             SDL_WINDOWPOS_UNDEFINED, w, h, SDL_WINDOW_SHOWN);
    if (D.win == NULL) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return -1;
    }
    D.r = SDL_CreateRenderer(D.win, -1, SDL_RENDERER_ACCELERATED |
                                        SDL_RENDERER_PRESENTVSYNC);
    if (D.r == NULL)
        D.r = SDL_CreateRenderer(D.win, -1, SDL_RENDERER_SOFTWARE);
    if (D.r == NULL) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        return -1;
    }
    SDL_RenderSetLogicalSize(D.r, UI_W, UI_H);
    SDL_SetRenderDrawBlendMode(D.r, SDL_BLENDMODE_BLEND);
    memset(&ri, 0, sizeof(ri));
    SDL_GetRendererInfo(D.r, &ri);

    if (TTF_Init() != 0) {
        fprintf(stderr, "TTF_Init: %s\n", TTF_GetError());
        return -1;
    }
    path = find_font();
    if (path == NULL) {
        fprintf(stderr, "no font found\n");
        return -1;
    }
    for (i = 0; i < F_COUNT; i++) {
        D.font[i] = TTF_OpenFont(path, font_px[i]);
        if (D.font[i] == NULL) {
            fprintf(stderr, "font %s: %s\n", path, TTF_GetError());
            return -1;
        }
    }
    D.disc = make_mask(DISC, DISC, in_disc, 0);
    make_icons();
    snprintf(D.info, sizeof(D.info), "video %s, display %dx%d, renderer %s, "
             "font %s", SDL_GetCurrentVideoDriver(), w, h,
             ri.name ? ri.name : "?", path);
    return 0;
}

void draw_quit(void)
{
    int i;

    for (i = 0; i < CACHE_N; i++)
        if (D.cache[i].t != NULL)
            SDL_DestroyTexture(D.cache[i].t);
    if (D.disc != NULL)
        SDL_DestroyTexture(D.disc);
    for (i = 0; i < IC_COUNT; i++)
        if (D.icon[i] != NULL)
            SDL_DestroyTexture(D.icon[i]);
    for (i = 0; i < F_COUNT; i++)
        if (D.font[i] != NULL)
            TTF_CloseFont(D.font[i]);
    TTF_Quit();
    if (D.r != NULL)
        SDL_DestroyRenderer(D.r);
    if (D.win != NULL)
        SDL_DestroyWindow(D.win);
    SDL_Quit();
    memset(&D, 0, sizeof(D));
}

const char *draw_info(void)
{
    return D.info;
}

void draw_clear(uint32_t rgba)
{
    D.frame++;
    set_color(rgba);
    SDL_RenderClear(D.r);
}

void draw_rect(int x, int y, int w, int h, uint32_t rgba)
{
    SDL_Rect rc = { x, y, w, h };

    if (w <= 0 || h <= 0)
        return;
    set_color(rgba);
    SDL_RenderFillRect(D.r, &rc);
}

void draw_disc(int cx, int cy, int r, uint32_t rgba)
{
    SDL_Rect dst = { cx - r, cy - r, 2 * r, 2 * r };

    if (D.disc == NULL)
        return;
    tint(D.disc, rgba);
    SDL_RenderCopy(D.r, D.disc, NULL, &dst);
}

void draw_round_rect(int x, int y, int w, int h, int r, uint32_t rgba)
{
    static const int qx[4] = { 0, 1, 0, 1 }, qy[4] = { 0, 0, 1, 1 };
    int i;

    if (r * 2 > h)
        r = h / 2;
    if (r * 2 > w)
        r = w / 2;
    if (D.disc == NULL || r <= 0) {
        draw_rect(x, y, w, h, rgba);
        return;
    }
    tint(D.disc, rgba);
    for (i = 0; i < 4; i++) {
        SDL_Rect src = { qx[i] * DISC / 2, qy[i] * DISC / 2, DISC / 2,
                         DISC / 2 };
        SDL_Rect dst = { qx[i] ? x + w - r : x, qy[i] ? y + h - r : y, r, r };

        SDL_RenderCopy(D.r, D.disc, &src, &dst);
    }
    draw_rect(x + r, y, w - 2 * r, h, rgba);
    draw_rect(x, y + r, r, h - 2 * r, rgba);
    draw_rect(x + w - r, y + r, r, h - 2 * r, rgba);
}

void draw_icon(enum ui_icon ic, int x, int y, uint32_t rgba)
{
    SDL_Rect dst = { x, y, D.icon_w[ic], D.icon_h[ic] };

    if (D.icon[ic] == NULL)
        return;
    tint(D.icon[ic], rgba);
    SDL_RenderCopy(D.r, D.icon[ic], NULL, &dst);
}

void draw_icon_size(enum ui_icon ic, int *w, int *h)
{
    *w = D.icon_w[ic];
    *h = D.icon_h[ic];
}

int draw_font_height(enum ui_font f)
{
    return D.font[f] != NULL ? TTF_FontHeight(D.font[f]) : 0;
}

int draw_text_width(enum ui_font f, const char *s)
{
    int w = 0, h = 0;

    if (D.font[f] == NULL || s[0] == '\0' ||
        TTF_SizeUTF8(D.font[f], s, &w, &h) != 0)
        return 0;
    return w;
}

/* Shortens S in BUF with "..." until it fits MAXW (UTF-8 aware). */
static const char *fit(enum ui_font f, const char *s, int maxw, char *buf,
                      size_t max)
{
    size_t n;

    if (maxw <= 0 || draw_text_width(f, s) <= maxw)
        return s;
    n = strlen(s);
    if (n + 4 > max)
        n = max - 4;
    while (n > 0) {
        /* step back one UTF-8 character */
        do
            n--;
        while (n > 0 && ((unsigned char)s[n] & 0xc0) == 0x80);
        memcpy(buf, s, n);
        memcpy(buf + n, "...", 4);
        if (draw_text_width(f, buf) <= maxw)
            return buf;
    }
    return "...";
}

static struct tex_entry *lookup(enum ui_font f, const char *s)
{
    struct tex_entry *e, *victim = &D.cache[0];
    SDL_Surface *surf;
    SDL_Color white = { 255, 255, 255, 255 };
    int i;

    for (i = 0; i < CACHE_N; i++) {
        e = &D.cache[i];
        if (e->t != NULL && e->font == (int)f && strcmp(e->text, s) == 0) {
            e->used = D.frame;
            return e;
        }
        if (e->t == NULL || (victim->t != NULL && e->used < victim->used))
            victim = e;
    }
    surf = TTF_RenderUTF8_Blended(D.font[f], s, white);
    if (surf == NULL)
        return NULL;
    if (victim->t != NULL)
        SDL_DestroyTexture(victim->t);
    victim->t = SDL_CreateTextureFromSurface(D.r, surf);
    victim->w = surf->w;
    victim->h = surf->h;
    SDL_FreeSurface(surf);
    if (victim->t == NULL)
        return NULL;
    SDL_SetTextureBlendMode(victim->t, SDL_BLENDMODE_BLEND);
    victim->font = (int)f;
    victim->used = D.frame;
    snprintf(victim->text, sizeof(victim->text), "%s", s);
    return victim;
}

int draw_text(enum ui_font f, int x, int y, uint32_t rgba, const char *s,
              enum ui_align al, int maxw)
{
    char buf[256];
    struct tex_entry *e;
    SDL_Rect dst;

    if (D.font[f] == NULL || s == NULL || s[0] == '\0')
        return 0;
    s = fit(f, s, maxw, buf, sizeof(buf));
    if (strlen(s) >= CACHE_TEXT) {
        /* Rare long text: render without caching. */
        SDL_Color white = { 255, 255, 255, 255 };
        SDL_Surface *surf = TTF_RenderUTF8_Blended(D.font[f], s, white);
        SDL_Texture *t;
        int w;

        if (surf == NULL)
            return 0;
        t = SDL_CreateTextureFromSurface(D.r, surf);
        dst.w = w = surf->w;
        dst.h = surf->h;
        SDL_FreeSurface(surf);
        if (t == NULL)
            return 0;
        dst.x = al == AL_CENTER ? x - w / 2 : al == AL_RIGHT ? x - w : x;
        dst.y = y;
        tint(t, rgba);
        SDL_RenderCopy(D.r, t, NULL, &dst);
        SDL_DestroyTexture(t);
        return w;
    }
    e = lookup(f, s);
    if (e == NULL)
        return 0;
    dst.w = e->w;
    dst.h = e->h;
    dst.x = al == AL_CENTER ? x - e->w / 2 : al == AL_RIGHT ? x - e->w : x;
    dst.y = y;
    tint(e->t, rgba);
    SDL_RenderCopy(D.r, e->t, NULL, &dst);
    return e->w;
}

int draw_screenshot(const char *path)
{
    int w = 0, h = 0, rc;
    SDL_Surface *s;

    if (SDL_GetRendererOutputSize(D.r, &w, &h) != 0)
        return -1;
    s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (s == NULL)
        return -1;
    rc = SDL_RenderReadPixels(D.r, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels,
                              s->pitch);
    if (rc == 0)
        rc = SDL_SaveBMP(s, path);
    SDL_FreeSurface(s);
    return rc;
}

void draw_present(void)
{
    SDL_RenderPresent(D.r);
}
