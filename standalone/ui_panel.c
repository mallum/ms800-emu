/*
 * MS800 front-panel UI, drawn from artwork traced off a real-unit photo
 * (charcoal steel fascia, Eurostile-Extended-style white lettering, rubber
 * keys in square recesses, slanted red 7-segment display).
 *
 * The art is pre-rendered by tools/gen_panel_art.py at ART_SCALE x for HiDPI
 * and compiled into the binary:
 *   assets/panel.bmp    static fascia with labels, wordmark and keys up
 *   assets/sprites.bmp  key-down, rocker off/on, lit digits 0-F
 * and positions come from the generated panel_layout.h. If the BMPs cannot
 * be decoded the panel falls back to plain rectangles so it stays usable.
 */
#include "ui_panel.h"
#include <stdio.h>

/* Visual grid column -> (top button, bottom button) logical index.
   The on-screen layout order does not match the scan-matrix index order
   (see the enum in ui_panel.h), so this mapping is explicit rather than
   computed by offset. */
static const struct { int top_idx, bot_idx; } grid[BTN_COLS] = {
    { MS800_BTN_CHANNEL,    MS800_BTN_FUNCTION  },
    { MS800_BTN_PATCH,      MS800_BTN_PARAM     },
    { MS800_BTN_UP,         MS800_BTN_DOWN      },
    { MS800_BTN_UP_UP,      MS800_BTN_DOWN_DOWN },
    { MS800_BTN_NEXT_PARAM, MS800_BTN_ENTER     },
};

static SDL_Texture *tex_panel, *tex_sprites;

static int col_x(int col) { return BTN_X0 + col * BTN_PITCH; }

/* Panel art compiled into the binary (see CMakeLists.txt / cmake/embed.cmake). */
extern const unsigned char panel_bmp[], sprites_bmp[];
extern const unsigned long panel_bmp_size, sprites_bmp_size;

static SDL_Texture *load_bmp(SDL_Renderer *ren, const unsigned char *data, unsigned long size, const char *name) {
    SDL_Surface *s = SDL_LoadBMP_RW(SDL_RWFromConstMem(data, (int)size), 1);
    if (!s) {
        fprintf(stderr, "ui_panel: cannot decode embedded %s (%s); using plain panel\n", name, SDL_GetError());
        return NULL;
    }
    SDL_Texture *t = SDL_CreateTextureFromSurface(ren, s);
    SDL_FreeSurface(s);
    return t;
}

void ui_panel_init(SDL_Renderer *ren) {
    /* Art is ART_SCALE x; let SDL map logical px onto the HiDPI output. */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    SDL_RenderSetLogicalSize(ren, UI_WIN_W, UI_WIN_H);
    tex_panel = load_bmp(ren, panel_bmp, panel_bmp_size, "panel.bmp");
    tex_sprites = load_bmp(ren, sprites_bmp, sprites_bmp_size, "sprites.bmp");
    if (!tex_sprites && tex_panel) { SDL_DestroyTexture(tex_panel); tex_panel = NULL; }
}

/* Blit a sprite-atlas cell (sx in atlas px, size in logical px) to (x,y). */
static void sprite(SDL_Renderer *ren, int sx, int w, int h, int x, int y) {
    SDL_Rect src = { sx, 0, w * ART_SCALE, h * ART_SCALE };
    SDL_Rect dst = { x, y, w, h };
    SDL_RenderCopy(ren, tex_sprites, &src, &dst);
}

static void draw_plain(SDL_Renderer *ren, const ms800_t *m, int powered_on) {
    SDL_SetRenderDrawColor(ren, 52, 55, 60, 255);
    SDL_RenderClear(ren);
    SDL_Rect sw = { SWITCH_X, SWITCH_Y, SWITCH_W, SWITCH_H };
    SDL_SetRenderDrawColor(ren, powered_on ? 70 : 20, powered_on ? 200 : 20, powered_on ? 110 : 22, 255);
    SDL_RenderFillRect(ren, &sw);
    for (int i = 0; i < BTN_COLS; i++) {
        int ids[2] = { grid[i].top_idx, grid[i].bot_idx }, ys[2] = { ROW1_Y, ROW2_Y };
        for (int r = 0; r < 2; r++) {
            SDL_Rect b = { col_x(i), ys[r], BTN_W, BTN_H };
            int hot = ms800_button(m, ids[r]);
            SDL_SetRenderDrawColor(ren, hot ? 30 : 60, hot ? 31 : 62, hot ? 34 : 68, 255);
            SDL_RenderFillRect(ren, &b);
        }
    }
}

void ui_panel_draw(SDL_Renderer *ren, const ms800_t *m, uint8_t display_byte, int powered_on) {
    if (!tex_panel) { draw_plain(ren, m, powered_on); return; }

    SDL_RenderCopy(ren, tex_panel, NULL, NULL);

    sprite(ren, powered_on ? SPR_ROCKER_ON_X : SPR_ROCKER_OFF_X, SWITCH_W, SWITCH_H, SWITCH_X, SWITCH_Y);

    for (int i = 0; i < BTN_COLS; i++) {
        if (ms800_button(m, grid[i].top_idx)) sprite(ren, SPR_KEYDOWN_X, BTN_W, BTN_H, col_x(i), ROW1_Y);
        if (ms800_button(m, grid[i].bot_idx)) sprite(ren, SPR_KEYDOWN_X, BTN_W, BTN_H, col_x(i), ROW2_Y);
    }

    /* Lit digits are black-backed with glow, so add them over the red filter. */
    if (powered_on) {
        int step = DIG_SPR_W * ART_SCALE;
        SDL_SetTextureBlendMode(tex_sprites, SDL_BLENDMODE_ADD);
        sprite(ren, SPR_DIGIT0_X + ((display_byte >> 4) & 0xF) * step, DIG_SPR_W, DIG_SPR_H, DIG1_X, DIG_Y);
        sprite(ren, SPR_DIGIT0_X + ( display_byte       & 0xF) * step, DIG_SPR_W, DIG_SPR_H, DIG2_X, DIG_Y);
        SDL_SetTextureBlendMode(tex_sprites, SDL_BLENDMODE_NONE);
    }
}

int ui_panel_hit_test_button(int x, int y) {
    for (int i = 0; i < BTN_COLS; i++) {
        int bx = col_x(i);
        SDL_Rect top = { bx, ROW1_Y, BTN_W, BTN_H };
        SDL_Rect bot = { bx, ROW2_Y, BTN_W, BTN_H };
        SDL_Point p = { x, y };
        if (SDL_PointInRect(&p, &top)) return grid[i].top_idx;
        if (SDL_PointInRect(&p, &bot)) return grid[i].bot_idx;
    }
    return 0;
}

int ui_panel_hit_test_power(int x, int y) {
    SDL_Rect sw = { SWITCH_X, SWITCH_Y, SWITCH_W, SWITCH_H };
    SDL_Point p = { x, y };
    return SDL_PointInRect(&p, &sw);
}
