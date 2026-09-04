/* ============================================================================
 *  applauncher.c -- Fixed app list with simple scroll
 * ==========================================================================*/
#include "applauncher.h"

typedef struct {
    char name[AL_NAME_LEN + 1];
    int  id;
    al_bool used;
} al_app_t;

static al_app_t apps[AL_MAX_APPS];
static int n_apps;
static int scroll;      /* first visible index */
static al_bool open_flag;
static int visible_rows; /* set during draw from height */

void al_init(void)
{
    n_apps = 0;
    scroll = 0;
    open_flag = AL_FALSE;
    for (int i = 0; i < AL_MAX_APPS; i++) apps[i].used = AL_FALSE;
}

void al_open(void)  { open_flag = AL_TRUE; scroll = 0; }
void al_close(void) { open_flag = AL_FALSE; }
al_bool al_is_open(void) { return open_flag; }

void al_register(const char *name, int id)
{
    if (n_apps >= AL_MAX_APPS || !name) return;
    int i = n_apps++;
    apps[i].used = AL_TRUE;
    apps[i].id = id;
    int n = 0;
    while (name[n] && n < AL_NAME_LEN) { apps[i].name[n] = name[n]; n++; }
    apps[i].name[n] = 0;
}

void al_scroll(int delta)
{
    if (n_apps <= 0) return;
    scroll += delta;
    if (scroll < 0) scroll = 0;
    int max_s = n_apps - 1;
    if (visible_rows > 0 && n_apps > visible_rows)
        max_s = n_apps - visible_rows;
    if (max_s < 0) max_s = 0;
    if (scroll > max_s) scroll = max_s;
}

void al_draw(int x, int y, int w, int h,
             int mouse_x, int mouse_y,
             void (*fill_rect)(int,int,int,int,al_u32),
             void (*draw_str)(int,int,const char*,al_u32),
             al_u32 col_bg, al_u32 col_item, al_u32 col_hov,
             al_u32 col_text, al_u32 col_accent)
{
    if (!open_flag) return;

    int pad = 10;
    int title_h = 28;
    int row_h = 32;
    int list_y = y + title_h + 4;
    int list_h = h - title_h - pad - 4;
    if (list_h < row_h) list_h = row_h;
    visible_rows = list_h / row_h;
    if (visible_rows < 1) visible_rows = 1;

    fill_rect(x, y, w, h, col_bg);
    draw_str(x + pad, y + 8, "Applications  (Ctrl+Alt+A)", col_text);
    /* close hint */
    draw_str(x + w - 70, y + 8, "[Esc]", col_text);

    int max_show = visible_rows;
    if (scroll + max_show > n_apps) max_show = n_apps - scroll;
    if (max_show < 0) max_show = 0;

    for (int i = 0; i < max_show; i++) {
        int idx = scroll + i;
        if (idx < 0 || idx >= n_apps) continue;
        int ry = list_y + i * row_h;
        int rx = x + pad;
        int rw = w - 2 * pad - 12; /* leave space for scrollbar */
        al_bool hov = (mouse_x >= rx && mouse_x < rx + rw &&
                       mouse_y >= ry && mouse_y < ry + row_h - 2);
        fill_rect(rx, ry, rw, row_h - 4, hov ? col_hov : col_item);
        if (hov) {
            /* accent left bar */
            fill_rect(rx, ry, 3, row_h - 4, col_accent);
        }
        draw_str(rx + 12, ry + 8, apps[idx].name, col_text);
    }

    /* simple scrollbar if needed */
    if (n_apps > visible_rows && visible_rows > 0) {
        int sb_x = x + w - 10;
        int sb_h = list_h;
        fill_rect(sb_x, list_y, 4, sb_h, col_item);
        int thumb_h = sb_h * visible_rows / n_apps;
        if (thumb_h < 12) thumb_h = 12;
        int max_scroll = n_apps - visible_rows;
        int thumb_y = list_y;
        if (max_scroll > 0)
            thumb_y = list_y + (scroll * (sb_h - thumb_h)) / max_scroll;
        fill_rect(sb_x, thumb_y, 4, thumb_h, col_accent);
    }
}

int al_click(int cx, int cy, int w, int h, int mx, int my)
{
    if (!open_flag) return -1;

    int pad = 10;
    int title_h = 28;
    int row_h = 32;
    int list_y = cy + title_h + 4;
    int list_h = h - title_h - pad - 4;
    if (list_h < row_h) list_h = row_h;
    int rows = list_h / row_h;
    if (rows < 1) rows = 1;

    /* Esc area / title click does nothing special here */

    int max_show = rows;
    if (scroll + max_show > n_apps) max_show = n_apps - scroll;

    for (int i = 0; i < max_show; i++) {
        int idx = scroll + i;
        if (idx < 0 || idx >= n_apps) continue;
        int ry = list_y + i * row_h;
        int rx = cx + pad;
        int rw = w - 2 * pad - 12;
        if (mx >= rx && mx < rx + rw && my >= ry && my < ry + row_h - 2)
            return apps[idx].id;
    }
    return -1;
}
