/* ============================================================================
 *  calculator.h -- Minimal 4-function calculator for Titan Kernel
 *  Design: fixed buffers, no heap, no floats, integer only, bounds-checked.
 * ==========================================================================*/
#ifndef CALCULATOR_H
#define CALCULATOR_H

typedef unsigned int  calc_u32;
typedef int           calc_bool;
#define CALC_TRUE  1
#define CALC_FALSE 0

/* Call once at boot */
void calc_init(void);

/* Open the calculator window (sets external visibility) */
void calc_open(void);

/* Returns 1 if calculator is the focused content that needs drawing */
calc_bool calc_is_active(void);

/* Draw into the client area of a window.
 * x,y = top-left of client area, w,h = size of client area */
void calc_draw(int x, int y, int w, int h,
               int mouse_x, int mouse_y,
               void (*fill_rect)(int,int,int,int,calc_u32),
               void (*draw_str)(int,int,const char*,calc_u32),
               calc_u32 col_bg, calc_u32 col_btn, calc_u32 col_text,
               calc_u32 col_accent, calc_u32 col_display);

/* Handle left-click inside the client area. Returns 1 if handled. */
calc_bool calc_click(int cx, int cy, int w, int h, int mx, int my);

/* Handle a key press while calculator is focused. Returns 1 if handled. */
calc_bool calc_key(char c);

#endif
