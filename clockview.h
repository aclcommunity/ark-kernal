/* ============================================================================
 *  clockview.h -- Full-screen modern clock / date view
 * ==========================================================================*/
#ifndef CLOCKVIEW_H
#define CLOCKVIEW_H

typedef unsigned int cv_u32;
typedef int          cv_bool;
#define CV_TRUE  1
#define CV_FALSE 0

void cv_init(void);
void cv_open(void);
void cv_close(void);
cv_bool cv_is_open(void);

/* anim_t: 0..255 open progress (caller advances with ticks) */
void cv_tick(cv_u32 now_ticks);

void cv_draw(int screen_w, int screen_h,
             const char *time_str, const char *date_str,
             int mouse_x, int mouse_y,
             void (*fill_rect)(int,int,int,int,cv_u32),
             void (*fill_rounded)(int,int,int,int,int,cv_u32,cv_u32),
             void (*draw_str)(int,int,const char*,cv_u32),
             void (*draw_str_big)(int,int,const char*,cv_u32,int),
             cv_u32 accent);

/* Click anywhere closes. Returns 1 if closed. */
cv_bool cv_click(void);

#endif
