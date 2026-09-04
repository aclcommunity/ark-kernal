/* ============================================================================
 *  toast.c -- Single-slot toast, fixed buffer
 * ==========================================================================*/
#include "toast.h"

static char msg[TOAST_MAX_LEN + 1];
static toast_u32 until;
static toast_bool on;

void toast_init(void)
{
    msg[0] = 0;
    until = 0;
    on = TOAST_FALSE;
}

void toast_show(const char *s, toast_u32 duration_ticks)
{
    if (!s) return;
    int i = 0;
    while (s[i] && i < TOAST_MAX_LEN) { msg[i] = s[i]; i++; }
    msg[i] = 0;
    until = duration_ticks;
    on = TOAST_TRUE;
}

toast_bool toast_active(void) { return on; }

void toast_draw(int screen_w, int screen_h, toast_u32 now_ticks,
                void (*fill_rect)(int,int,int,int,toast_u32),
                void (*draw_str)(int,int,const char*,toast_u32),
                toast_u32 col_bg, toast_u32 col_text, toast_u32 col_accent)
{
    if (!on) return;
    if (now_ticks >= until) { on = TOAST_FALSE; return; }

    int n = 0;
    while (msg[n]) n++;
    int tw = n * 8 + 24;
    if (tw < 80) tw = 80;
    int th = 36;
    int x = (screen_w - tw) / 2;
    int y = screen_h - 52 - th - 12;
    if (x < 8) x = 8;

    fill_rect(x, y, tw, th, col_bg);
    fill_rect(x, y, 3, th, col_accent);
    draw_str(x + 12, y + 10, msg, col_text);
}
