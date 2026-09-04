/* ============================================================================
 *  calculator.c -- Minimal integer calculator (Titan)
 *  No heap, no floats, fixed display, all bounds checked.
 * ==========================================================================*/
#include "calculator.h"

#define DISP_MAX 16
/* 32-bit long limits (freestanding — no limits.h) */
#define CALC_LONG_MAX  2147483647L
#define CALC_LONG_MIN  (-2147483647L - 1L)

static char  disp[DISP_MAX + 1];
static calc_u32 disp_len;
static long  acc;          /* accumulator */
static long  cur;          /* current entry */
static char  op;           /* pending operator: + - * / or 0 */
static calc_bool fresh;    /* next digit starts new number */
static calc_bool active;
static calc_bool overflowed; /* sticky until Clear */

static void disp_set(long v)
{
    /* very small itoa, handles negative and zero; avoids UB on LONG_MIN */
    char tmp[DISP_MAX + 1];
    calc_u32 i = 0;
    calc_bool neg = CALC_FALSE;
    unsigned long uv;
    if (v < 0) {
        neg = CALC_TRUE;
        /* careful: -LONG_MIN is undefined; cast via unsigned */
        uv = (unsigned long)(-(v + 1)) + 1UL;
    } else {
        uv = (unsigned long)v;
    }
    if (uv == 0) tmp[i++] = '0';
    else {
        while (uv > 0 && i < DISP_MAX - 1) {
            tmp[i++] = (char)('0' + (uv % 10));
            uv /= 10;
        }
    }
    if (neg && i < DISP_MAX) tmp[i++] = '-';
    /* reverse into disp */
    disp_len = 0;
    while (i > 0 && disp_len < DISP_MAX) {
        disp[disp_len++] = tmp[--i];
    }
    disp[disp_len] = 0;
}

static void disp_error(void)
{
    disp[0] = 'E'; disp[1] = 'r'; disp[2] = 'r'; disp[3] = 0;
    disp_len = 3;
    overflowed = CALC_TRUE;
    cur = 0;
    acc = 0;
    op = 0;
    fresh = CALC_TRUE;
}

static void disp_clear(void)
{
    disp[0] = '0';
    disp[1] = 0;
    disp_len = 1;
    cur = 0;
    fresh = CALC_TRUE;
    overflowed = CALC_FALSE;
}

/* Safe arithmetic — returns 0 on overflow */
static calc_bool safe_add(long a, long b, long *out)
{
    if (b > 0 && a > CALC_LONG_MAX - b) return CALC_FALSE;
    if (b < 0 && a < CALC_LONG_MIN - b) return CALC_FALSE;
    *out = a + b;
    return CALC_TRUE;
}

static calc_bool safe_sub(long a, long b, long *out)
{
    if (b > 0 && a < CALC_LONG_MIN + b) return CALC_FALSE;
    if (b < 0 && a > CALC_LONG_MAX + b) return CALC_FALSE;
    *out = a - b;
    return CALC_TRUE;
}

static calc_bool safe_mul(long a, long b, long *out)
{
    if (a == 0 || b == 0) { *out = 0; return CALC_TRUE; }
    /* LONG_MIN * -1 overflows */
    if ((a == CALC_LONG_MIN && b == -1) || (b == CALC_LONG_MIN && a == -1))
        return CALC_FALSE;
    if (a > 0 && b > 0 && a > CALC_LONG_MAX / b) return CALC_FALSE;
    if (a > 0 && b < 0 && b < CALC_LONG_MIN / a) return CALC_FALSE;
    if (a < 0 && b > 0 && a < CALC_LONG_MIN / b) return CALC_FALSE;
    if (a < 0 && b < 0 && a < CALC_LONG_MAX / b) return CALC_FALSE;
    *out = a * b;
    return CALC_TRUE;
}

static calc_bool safe_digit_append(long cur_val, int digit, long *out)
{
    /* cur_val * 10 + digit without overflow */
    long t;
    if (!safe_mul(cur_val, 10L, &t)) return CALC_FALSE;
    if (!safe_add(t, (long)digit, out)) return CALC_FALSE;
    return CALC_TRUE;
}

static void apply(void)
{
    long result = 0;
    calc_bool ok = CALC_TRUE;
    if (op == '+') ok = safe_add(acc, cur, &result);
    else if (op == '-') ok = safe_sub(acc, cur, &result);
    else if (op == '*') ok = safe_mul(acc, cur, &result);
    else if (op == '/') {
        if (cur == 0) { disp_error(); return; }
        /* LONG_MIN / -1 overflows on 2's complement */
        if (acc == CALC_LONG_MIN && cur == -1) { disp_error(); return; }
        result = acc / cur;
        ok = CALC_TRUE;
    } else {
        result = cur;
        ok = CALC_TRUE;
    }
    if (!ok) { disp_error(); return; }
    acc = result;
    disp_set(acc);
    cur = acc;
    op = 0;
    fresh = CALC_TRUE;
}

void calc_init(void)
{
    disp_clear();
    acc = 0;
    op = 0;
    active = CALC_FALSE;
    overflowed = CALC_FALSE;
}

void calc_open(void)
{
    active = CALC_TRUE;
}

calc_bool calc_is_active(void)
{
    return active;
}

/* Button layout: 4 columns x 5 rows (integer-only; no decimal)
 *  C  /  *  -
 *  7  8  9  +
 *  4  5  6
 *  1  2  3  =
 *  0        =
 */
static const char *btns[5][4] = {
    { "C", "/", "*", "-" },
    { "7", "8", "9", "+" },
    { "4", "5", "6", " " },
    { "1", "2", "3", "=" },
    { "0", " ", " ", "=" }
};

void calc_draw(int x, int y, int w, int h,
               int mouse_x, int mouse_y,
               void (*fill_rect)(int,int,int,int,calc_u32),
               void (*draw_str)(int,int,const char*,calc_u32),
               calc_u32 col_bg, calc_u32 col_btn, calc_u32 col_text,
               calc_u32 col_accent, calc_u32 col_display)
{
    if (!active) return;
    (void)col_bg;

    int pad = 8;
    int disp_h = 36;
    int grid_y = y + pad + disp_h + 8;
    int grid_h = h - (pad + disp_h + 8 + pad);
    int grid_w = w - 2 * pad;
    int bw = grid_w / 4;
    int bh = grid_h / 5;

    /* display */
    fill_rect(x + pad, y + pad, w - 2 * pad, disp_h, col_display);
    draw_str(x + pad + 10, y + pad + 10, disp, col_text);

    /* buttons */
    for (int r = 0; r < 5; r++) {
        for (int c = 0; c < 4; c++) {
            int bx = x + pad + c * bw;
            int by = grid_y + r * bh;
            calc_bool hov = (mouse_x >= bx && mouse_x < bx + bw - 2 &&
                             mouse_y >= by && mouse_y < by + bh - 2);
            calc_u32 bg = hov ? col_accent : col_btn;
            fill_rect(bx, by, bw - 3, bh - 3, bg);
            const char *lab = btns[r][c];
            if (lab[0] != ' ')
                draw_str(bx + (bw / 2) - 4, by + (bh / 2) - 6, lab, col_text);
        }
    }
}

calc_bool calc_click(int cx, int cy, int w, int h, int mx, int my)
{
    if (!active) return CALC_FALSE;

    int pad = 8;
    int disp_h = 36;
    int grid_y = cy + pad + disp_h + 8;
    int grid_h = h - (pad + disp_h + 8 + pad);
    int grid_w = w - 2 * pad;
    int bw = grid_w / 4;
    int bh = grid_h / 5;

    if (my < grid_y) return CALC_FALSE;

    int r = (my - grid_y) / bh;
    int c = (mx - (cx + pad)) / bw;
    if (r < 0 || r > 4 || c < 0 || c > 3) return CALC_FALSE;

    const char *lab = btns[r][c];
    char ch = lab[0];
    if (ch == ' ') return CALC_TRUE;

    if (ch == 'C') {
        disp_clear();
        acc = 0;
        op = 0;
        return CALC_TRUE;
    }
    if (overflowed) return CALC_TRUE; /* only Clear recovers from Err */
    if (ch == '=' ) {
        apply();
        return CALC_TRUE;
    }
    if (ch == '+' || ch == '-' || ch == '*' || ch == '/') {
        if (op) apply();
        else { acc = cur; }
        if (overflowed) return CALC_TRUE;
        op = ch;
        fresh = CALC_TRUE;
        return CALC_TRUE;
    }
    /* digit */
    if (ch >= '0' && ch <= '9') {
        if (fresh) {
            cur = 0;
            disp_len = 0;
            fresh = CALC_FALSE;
        }
        if (disp_len >= DISP_MAX - 1) return CALC_TRUE;
        {
            long next;
            if (!safe_digit_append(cur, ch - '0', &next)) {
                disp_error();
                return CALC_TRUE;
            }
            cur = next;
        }
        disp_set(cur);
        return CALC_TRUE;
    }
    return CALC_TRUE;
}

calc_bool calc_key(char c)
{
    if (!active) return CALC_FALSE;
    if (c == 'c' || c == 'C' || c == 27) { /* Esc also clears */
        disp_clear();
        acc = 0;
        op = 0;
        return CALC_TRUE;
    }
    if (overflowed) return CALC_TRUE; /* only Clear recovers from Err */
    if (c >= '0' && c <= '9') {
        if (fresh) { cur = 0; disp_len = 0; fresh = CALC_FALSE; }
        if (disp_len >= DISP_MAX - 1) return CALC_TRUE;
        {
            long next;
            if (!safe_digit_append(cur, c - '0', &next)) {
                disp_error();
                return CALC_TRUE;
            }
            cur = next;
        }
        disp_set(cur);
        return CALC_TRUE;
    }
    if (c == '+' || c == '-' || c == '*' || c == '/') {
        if (op) apply();
        else acc = cur;
        if (overflowed) return CALC_TRUE;
        op = c;
        fresh = CALC_TRUE;
        return CALC_TRUE;
    }
    if (c == '=' || c == '\n') {
        apply();
        return CALC_TRUE;
    }
    return CALC_FALSE;
}
