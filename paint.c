/* ============================================================================
 *  paint.c -- Minimal fixed-size drawing pad
 * ==========================================================================*/
#include "paint.h"

static paint_u32 canvas[PAINT_W * PAINT_CANVAS_H];
static paint_u32 color;
static paint_bool active;
static int last_x, last_y;
static paint_bool has_last;

static const paint_u32 palette[8] = {
    0x000000, 0xFFFFFF, 0xE04040, 0x40C040,
    0x4080E0, 0xE0C040, 0xC040C0, 0x40C0C0
};

void paint_init(void)
{
    paint_clear();
    color = 0x000000;
    active = PAINT_FALSE;
    has_last = PAINT_FALSE;
}

void paint_open(void) { active = PAINT_TRUE; }
paint_bool paint_is_active(void) { return active; }

void paint_clear(void)
{
    for (int i = 0; i < PAINT_W * PAINT_CANVAS_H; i++)
        canvas[i] = 0xFFFFFF;
    has_last = PAINT_FALSE;
}

static void plot(int x, int y)
{
    if (x < 0 || y < 0 || x >= PAINT_W || y >= PAINT_CANVAS_H) return;
    canvas[y * PAINT_W + x] = color;
    /* tiny 2x2 for better visibility */
    if (x + 1 < PAINT_W) canvas[y * PAINT_W + x + 1] = color;
    if (y + 1 < PAINT_CANVAS_H) canvas[(y + 1) * PAINT_W + x] = color;
    if (x + 1 < PAINT_W && y + 1 < PAINT_CANVAS_H)
        canvas[(y + 1) * PAINT_W + x + 1] = color;
}

static void line(int x0, int y0, int x1, int y1)
{
    int dx = x1 - x0; if (dx < 0) dx = -dx;
    int dy = y1 - y0; if (dy < 0) dy = -dy;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        plot(x0, y0);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

void paint_draw(int x, int y, int w, int h,
                int mouse_x, int mouse_y, paint_bool mouse_down,
                void (*fill_rect)(int,int,int,int,paint_u32),
                void (*draw_str)(int,int,const char*,paint_u32),
                paint_u32 col_bg, paint_u32 col_btn, paint_u32 col_text,
                paint_u32 col_accent)
{
    (void)mouse_x; (void)mouse_y; (void)mouse_down; (void)col_bg;

    int pad = 6;
    int tool_h = 28;
    int cv_x = x + pad;
    int cv_y = y + pad + tool_h + 4;
    int cv_w = PAINT_W;
    int cv_h = PAINT_CANVAS_H;
    if (cv_w > w - 2 * pad) cv_w = w - 2 * pad;
    if (cv_h > h - tool_h - pad * 2 - 4) cv_h = h - tool_h - pad * 2 - 4;

    /* toolbar */
    fill_rect(x + pad, y + pad, w - 2 * pad, tool_h, col_btn);
    draw_str(x + pad + 6, y + pad + 8, "Clear", col_text);

    /* color swatches */
    for (int i = 0; i < 8; i++) {
        int sx = x + pad + 60 + i * 18;
        int sy = y + pad + 6;
        fill_rect(sx, sy, 14, 16, palette[i]);
        if (color == palette[i])
            fill_rect(sx - 1, sy - 1, 16, 18, col_accent);
    }

    /* canvas background */
    fill_rect(cv_x, cv_y, cv_w, cv_h, 0xFFFFFF);

    /* blit canvas (nearest, clipped) */
    for (int py = 0; py < cv_h && py < PAINT_CANVAS_H; py++) {
        for (int px = 0; px < cv_w && px < PAINT_W; px++) {
            paint_u32 c = canvas[py * PAINT_W + px];
            if (c != 0xFFFFFF)
                fill_rect(cv_x + px, cv_y + py, 1, 1, c);
        }
    }
}

paint_bool paint_input(int cx, int cy, int w, int h,
                       int mx, int my, paint_bool down, paint_bool prev_down)
{
    (void)w; (void)h; /* reserved for future canvas scaling */
    if (!active) return PAINT_FALSE;

    int pad = 6;
    int tool_h = 28;
    int cv_x = cx + pad;
    int cv_y = cy + pad + tool_h + 4;

    /* Clear button */
    if (down && !prev_down) {
        if (mx >= cx + pad && mx < cx + pad + 50 &&
            my >= cy + pad && my < cy + pad + tool_h) {
            paint_clear();
            return PAINT_TRUE;
        }
        /* color pick */
        for (int i = 0; i < 8; i++) {
            int sx = cx + pad + 60 + i * 18;
            int sy = cy + pad + 6;
            if (mx >= sx && mx < sx + 14 && my >= sy && my < sy + 16) {
                color = palette[i];
                return PAINT_TRUE;
            }
        }
    }

    /* drawing */
    if (mx >= cv_x && mx < cv_x + PAINT_W && my >= cv_y && my < cv_y + PAINT_CANVAS_H) {
        int px = mx - cv_x;
        int py = my - cv_y;
        if (down) {
            if (has_last)
                line(last_x, last_y, px, py);
            else
                plot(px, py);
            last_x = px; last_y = py; has_last = PAINT_TRUE;
            return PAINT_TRUE;
        }
    }
    if (!down) has_last = PAINT_FALSE;
    return PAINT_FALSE;
}
