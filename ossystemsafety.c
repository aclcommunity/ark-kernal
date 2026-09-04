/* ============================================================================
 *  ossystemsafety.c -- Fail-closed memory / size safety for Titan Kernel
 * ==========================================================================*/
#include "ossystemsafety.h"

static s_u8 *g_heap = 0;
static s_u32 g_heap_cap = 0;
static safety_panic_fn g_panic = 0;

void safety_init(void *heap_base, s_u32 heap_cap, safety_panic_fn panic_cb) {
    g_heap = (s_u8 *)heap_base;
    g_heap_cap = heap_cap;
    g_panic = panic_cb;
}

void safety_require(s_bool cond, const char *msg) {
    if (cond) return;
    if (g_panic) g_panic(msg ? msg : "safety_require failed");
    for (;;) { __asm__ volatile ("cli; hlt"); }
}

s_bool safety_add_ok(s_u32 a, s_u32 b, s_u32 *out) {
    s_u32 s = a + b;
    if (s < a) return S_FALSE; /* overflow */
    if (out) *out = s;
    return S_TRUE;
}

s_bool safety_mul_ok(s_u32 a, s_u32 b, s_u32 *out) {
    if (a != 0 && b > 0xFFFFFFFFu / a) return S_FALSE;
    if (out) *out = a * b;
    return S_TRUE;
}

s_u32 safety_align16(s_u32 size) {
    s_u32 aligned;
    if (!safety_add_ok(size, 15u, &aligned)) return 0;
    return aligned & ~15u;
}

s_bool safety_heap_fit(s_u32 used, s_u32 need, s_u32 cap) {
    s_u32 sum;
    if (!safety_add_ok(used, need, &sum)) return S_FALSE;
    return sum <= cap ? S_TRUE : S_FALSE;
}

s_bool safety_heap_owns(const void *p, s_u32 len) {
    if (!g_heap || !p) return S_FALSE;
    const s_u8 *pb = (const s_u8 *)p;
    if (pb < g_heap) return S_FALSE;
    s_u32 off = (s_u32)(pb - g_heap);
    s_u32 end;
    if (!safety_add_ok(off, len, &end)) return S_FALSE;
    return end <= g_heap_cap ? S_TRUE : S_FALSE;
}

void safety_memset(void *dst, s_u8 val, s_u32 n, s_u32 dst_cap) {
    if (!dst) return;
    if (n > dst_cap) n = dst_cap;
    s_u8 *d = (s_u8 *)dst;
    for (s_u32 i = 0; i < n; i++) d[i] = val;
}

s_bool safety_memcpy(void *dst, s_u32 dst_cap, const void *src, s_u32 n) {
    if (!dst || !src) return S_FALSE;
    if (n > dst_cap) return S_FALSE;
    s_u8 *d = (s_u8 *)dst;
    const s_u8 *s = (const s_u8 *)src;
    for (s_u32 i = 0; i < n; i++) d[i] = s[i];
    return S_TRUE;
}

s_u32 safety_strlen(const char *s, s_u32 max) {
    if (!s) return 0;
    s_u32 n = 0;
    while (n < max && s[n]) n++;
    return n;
}

s_bool safety_strncpy(char *dst, s_u32 dst_cap, const char *src) {
    if (!dst || dst_cap == 0) return S_FALSE;
    if (!src) { dst[0] = 0; return S_TRUE; }
    s_u32 i = 0;
    while (i + 1 < dst_cap && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
    return S_TRUE;
}

void safety_wipe(void *p, s_u32 n) {
    if (!p || n == 0) return;
    volatile s_u8 *d = (volatile s_u8 *)p;
    for (s_u32 i = 0; i < n; i++) d[i] = 0;
}
