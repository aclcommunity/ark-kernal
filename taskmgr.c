/* ============================================================================
 *  taskmgr.c -- Fixed window list + focus / close buttons
 * ==========================================================================*/
#include "taskmgr.h"

static tm_entry_t entries[TM_MAX];
static int n;
static tm_bool open_flag;
static int last_close;

void tm_init(void)
{
    n = 0;
    open_flag = TM_FALSE;
    last_close = -1;
    for (int i = 0; i < TM_MAX; i++) entries[i].used = TM_FALSE;
}

void tm_open(void)  { open_flag = TM_TRUE; }
void tm_close(void) { open_flag = TM_FALSE; }
tm_bool tm_is_open(void) { return open_flag; }

void tm_clear(void)
{
    n = 0;
    for (int i = 0; i < TM_MAX; i++) entries[i].used = TM_FALSE;
}

void tm_add(const char *name, int win_id)
{
    if (n >= TM_MAX || !name) return;
    int i = n++;
    entries[i].used = TM_TRUE;
    entries[i].win_id = win_id;
    int k = 0;
    while (name[k] && k < TM_NAME) { entries[i].name[k] = name[k]; k++; }
    entries[i].name[k] = 0;
}

int tm_last_close_id(void) { return last_close; }

void tm_draw(int x, int y, int w, int h,
             int mouse_x, int mouse_y,
             void (*fill_rect)(int,int,int,int,tm_u32),
             void (*draw_str)(int,int,const char*,tm_u32),
             tm_u32 col_bg, tm_u32 col_item, tm_u32 col_hov,
             tm_u32 col_text, tm_u32 col_accent, tm_u32 col_danger)
{
    (void)col_accent; /* reserved for future selection highlight */
    if (!open_flag) return;
    int pad = 10, title_h = 28, row_h = 34;
    fill_rect(x, y, w, h, col_bg);
    draw_str(x + pad, y + 8, "Task Manager", col_text);

    int list_y = y + title_h + 4;
    for (int i = 0; i < n; i++) {
        int ry = list_y + i * row_h;
        if (ry + row_h > y + h - 4) break;
        int rx = x + pad;
        int rw = w - 2 * pad;
        tm_bool hov = (mouse_x >= rx && mouse_x < rx + rw &&
                       mouse_y >= ry && mouse_y < ry + row_h - 2);
        fill_rect(rx, ry, rw, row_h - 4, hov ? col_hov : col_item);
        draw_str(rx + 10, ry + 8, entries[i].name, col_text);
        /* close button */
        int bx = rx + rw - 28, by = ry + 4;
        tm_bool chov = (mouse_x >= bx && mouse_x < bx + 22 &&
                        mouse_y >= by && mouse_y < by + 22);
        fill_rect(bx, by, 22, 22, chov ? col_danger : col_item);
        draw_str(bx + 7, by + 4, "X", col_text);
    }
    if (n == 0)
        draw_str(x + pad, list_y + 8, "(no open windows)", col_text);
}

int tm_click(int cx, int cy, int w, int h, int mx, int my)
{
    last_close = -1;
    if (!open_flag) return -1;
    int pad = 10, title_h = 28, row_h = 34;
    int list_y = cy + title_h + 4;
    for (int i = 0; i < n; i++) {
        int ry = list_y + i * row_h;
        if (ry + row_h > cy + h - 4) break;
        int rx = cx + pad;
        int rw = w - 2 * pad;
        int bx = rx + rw - 28, by = ry + 4;
        if (mx >= bx && mx < bx + 22 && my >= by && my < by + 22) {
            last_close = entries[i].win_id;
            return -2;
        }
        if (mx >= rx && mx < rx + rw && my >= ry && my < ry + row_h - 2)
            return entries[i].win_id;
    }
    return -1;
}
