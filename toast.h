/* ============================================================================
 *  toast.h -- Minimal transient notifications
 * ==========================================================================*/
#ifndef TOAST_H
#define TOAST_H

typedef unsigned int toast_u32;
typedef int          toast_bool;
#define TOAST_TRUE  1
#define TOAST_FALSE 0

#define TOAST_MAX_LEN 48

void toast_init(void);
void toast_show(const char *msg, toast_u32 duration_ticks);
toast_bool toast_active(void);

void toast_draw(int screen_w, int screen_h, toast_u32 now_ticks,
                void (*fill_rect)(int,int,int,int,toast_u32),
                void (*draw_str)(int,int,const char*,toast_u32),
                toast_u32 col_bg, toast_u32 col_text, toast_u32 col_accent);

#endif
