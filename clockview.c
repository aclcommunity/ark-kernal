/* ============================================================================
 *  clockview.c -- Full-screen clock with simple open animation
 * ==========================================================================*/
#include "clockview.h"

static cv_bool open_flag;
static cv_u32  anim;       /* 0..256 */
static cv_u32  anim_start;
static cv_bool animating;
static cv_u32  pulse;

void cv_init(void)
{
    open_flag = CV_FALSE;
    anim = 0;
    animating = CV_FALSE;
    pulse = 0;
}

void cv_open(void)
{
    open_flag = CV_TRUE;
    anim = 0;
    animating = CV_TRUE;
    anim_start = 0; /* set on first tick */
}

void cv_close(void)
{
    open_flag = CV_FALSE;
    anim = 0;
    animating = CV_FALSE;
}

cv_bool cv_is_open(void) { return open_flag; }

void cv_tick(cv_u32 now_ticks)
{
    if (!open_flag) return;
    if (animating) {
        if (anim_start == 0) anim_start = now_ticks ? now_ticks : 1;
        cv_u32 dt = now_ticks - anim_start;
        /* ~0.4s open at 100Hz */
        anim = dt * 6;
        if (anim >= 256) { anim = 256; animating = CV_FALSE; }
    }
    pulse = now_ticks;
}

cv_bool cv_click(void)
{
    if (!open_flag) return CV_FALSE;
    cv_close();
    return CV_TRUE;
}

#if 0 /* reserved for future soft gradient clock face */
static cv_u32 blend_u8(cv_u32 a, cv_u32 b, cv_u32 t /*0..256*/)
{
    return a + ((b - a) * t) / 256;
}
#endif

void cv_draw(int screen_w, int screen_h,
             const char *time_str, const char *date_str,
             int mouse_x, int mouse_y,
             void (*fill_rect)(int,int,int,int,cv_u32),
             void (*fill_rounded)(int,int,int,int,int,cv_u32,cv_u32),
             void (*draw_str)(int,int,const char*,cv_u32),
             void (*draw_str_big)(int,int,const char*,cv_u32,int),
             cv_u32 accent)
{
    (void)mouse_x; (void)mouse_y; (void)fill_rounded;
    if (!open_flag || !time_str || !date_str) return;

    cv_u32 t = anim;
    if (t > 256) t = 256;

    /* dim backdrop */
    cv_u32 dim = (t * 180) / 256;
    /* approximate full-screen dim with a few large rects of dark color;
       true per-pixel alpha would be too heavy — solid overlay with intensity */
    cv_u32 overlay = 0x000000;
    (void)dim;
    fill_rect(0, 0, screen_w, screen_h, overlay);
    /* secondary soft layers for depth */
    int margin = (int)((256 - t) * 40 / 256);
    int card_w = screen_w - 120 - margin * 2;
    int card_h = screen_h - 140 - margin * 2;
    if (card_w < 320) card_w = 320;
    if (card_h < 220) card_h = 220;
    int card_x = (screen_w - card_w) / 2;
    int card_y = (screen_h - card_h) / 2;

    /* outer glow rings (accent) */
    for (int i = 3; i >= 0; i--) {
        int grow = i * 10 + (int)((256 - t) * 8 / 256);
        fill_rect(card_x - grow, card_y - grow, card_w + grow * 2, card_h + grow * 2,
                  accent);
    }

    /* main card */
    fill_rect(card_x, card_y, card_w, card_h, 0x141822);
    /* top accent line */
    fill_rect(card_x, card_y, card_w, 3, accent);
    /* inner panel */
    fill_rect(card_x + 16, card_y + 16, card_w - 32, card_h - 32, 0x1a2030);

    /* large time — scale based on width */
    int scale = 10;
    if (card_w < 500) scale = 7;
    if (card_w < 400) scale = 5;

    int tlen = 0;
    while (time_str[tlen]) tlen++;
    int tw = tlen * 6 * scale;
    int th = 7 * scale;
    int tx = card_x + (card_w - tw) / 2;
    int ty = card_y + card_h / 2 - th - 20;

    /* subtle pulse on seconds via accent tint every other second-ish */
    cv_u32 time_col = 0xFFFFFF;
    if (((pulse / 50) & 1) == 0)
        time_col = accent;

    draw_str_big(tx, ty, time_str, time_col, scale);

    /* date */
    int dlen = 0;
    while (date_str[dlen]) dlen++;
    int dw = dlen * 8;
    int dx = card_x + (card_w - dw) / 2;
    int dy = ty + th + 28;
    draw_str(dx, dy, date_str, 0xB0B8C8);

    /* hint */
    const char *hint = "Click anywhere to close";
    int hlen = 0;
    while (hint[hlen]) hlen++;
    int hx = card_x + (card_w - hlen * 8) / 2;
    draw_str(hx, card_y + card_h - 36, hint, 0x6A7388);
}
