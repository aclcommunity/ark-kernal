/* ============================================================================
 *  taskmgr.h -- Minimal task/window list
 * ==========================================================================*/
#ifndef TASKMGR_H
#define TASKMGR_H

typedef unsigned int tm_u32;
typedef int          tm_bool;
#define TM_TRUE  1
#define TM_FALSE 0

#define TM_MAX  12
#define TM_NAME 28

typedef struct {
    char name[TM_NAME + 1];
    int  win_id;
    tm_bool used;
} tm_entry_t;

void tm_init(void);
void tm_open(void);
void tm_close(void);
tm_bool tm_is_open(void);

/* Rebuild list from current visible windows — caller fills via tm_add */
void tm_clear(void);
void tm_add(const char *name, int win_id);

void tm_draw(int x, int y, int w, int h,
             int mouse_x, int mouse_y,
             void (*fill_rect)(int,int,int,int,tm_u32),
             void (*draw_str)(int,int,const char*,tm_u32),
             tm_u32 col_bg, tm_u32 col_item, tm_u32 col_hov,
             tm_u32 col_text, tm_u32 col_accent, tm_u32 col_danger);

/* Returns: -1 none, >=0 focus win_id, -2 close that entry (use tm_last_close_id) */
int tm_click(int cx, int cy, int w, int h, int mx, int my);
int tm_last_close_id(void);

#endif
