/* ============================================================================
 *  paint.h -- Minimal drawing pad for Titan Kernel
 *  Fixed canvas, no heap growth, bounds-checked.
 * ==========================================================================*/
#ifndef PAINT_H
#define PAINT_H

typedef unsigned int  paint_u32;
typedef int           paint_bool;
#define PAINT_TRUE  1
#define PAINT_FALSE 0

#define PAINT_W  240
#define PAINT_CANVAS_H  160

void paint_init(void);
void paint_open(void);
paint_bool paint_is_active(void);

/* Draw the paint UI into client area */
void paint_draw(int x, int y, int w, int h,
                int mouse_x, int mouse_y, paint_bool mouse_down,
                void (*fill_rect)(int,int,int,int,paint_u32),
                void (*draw_str)(int,int,const char*,paint_u32),
                paint_u32 col_bg, paint_u32 col_btn, paint_u32 col_text,
                paint_u32 col_accent);

/* Click / drag handling. Returns 1 if consumed */
paint_bool paint_input(int cx, int cy, int w, int h,
                       int mx, int my, paint_bool down, paint_bool prev_down);

void paint_clear(void);

#endif
