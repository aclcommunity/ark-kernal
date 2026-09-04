/* ============================================================================
 *  applauncher.h -- Minimal app list launcher (Ctrl+Alt+A)
 *  Fixed list, scroll, no heap.
 * ==========================================================================*/
#ifndef APPLAUNCHER_H
#define APPLAUNCHER_H

typedef unsigned int  al_u32;
typedef int           al_bool;
#define AL_TRUE  1
#define AL_FALSE 0

#define AL_MAX_APPS 12
#define AL_NAME_LEN 24

typedef void (*al_open_fn)(int app_id);

void al_init(void);
void al_open(void);
void al_close(void);
al_bool al_is_open(void);

/* Register apps once at boot. id is passed back to open_fn */
void al_register(const char *name, int id);

void al_draw(int x, int y, int w, int h,
             int mouse_x, int mouse_y,
             void (*fill_rect)(int,int,int,int,al_u32),
             void (*draw_str)(int,int,const char*,al_u32),
             al_u32 col_bg, al_u32 col_item, al_u32 col_hov,
             al_u32 col_text, al_u32 col_accent);

/* Returns app id (>=0) if an item was clicked, or -1 */
int al_click(int cx, int cy, int w, int h, int mx, int my);

/* Scroll: delta >0 down, <0 up */
void al_scroll(int delta);

#endif
