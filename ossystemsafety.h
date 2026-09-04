/* ============================================================================
 *  ossystemsafety.h -- Minimal high-impact safety layer for Titan
 *
 *  Design: small code, hard checks, fail-closed (panic / reject).
 *  Not a full MMU security model -- that needs paging. This stops the
 *  common C memory mistakes that actually crash hobby kernels.
 * ==========================================================================*/
#ifndef OS_SYSTEM_SAFETY_H
#define OS_SYSTEM_SAFETY_H

typedef unsigned char  s_u8;
typedef unsigned int   s_u32;
typedef int            s_bool;
#define S_TRUE  1
#define S_FALSE 0

typedef void (*safety_panic_fn)(const char *msg);

/* Call once at boot after heap is known. */
void safety_init(void *heap_base, s_u32 heap_cap, safety_panic_fn panic_cb);

/* ---- integer overflow guards (size math) ---- */
s_bool safety_add_ok(s_u32 a, s_u32 b, s_u32 *out);
s_bool safety_mul_ok(s_u32 a, s_u32 b, s_u32 *out);

/* ---- bounded memory ops (never write past dest_cap) ---- */
void   safety_memset(void *dst, s_u8 val, s_u32 n, s_u32 dst_cap);
s_bool safety_memcpy(void *dst, s_u32 dst_cap, const void *src, s_u32 n);
s_u32  safety_strlen(const char *s, s_u32 max);
s_bool safety_strncpy(char *dst, s_u32 dst_cap, const char *src);

/* ---- pointer / region checks ---- */
s_bool safety_heap_owns(const void *p, s_u32 len);
void   safety_require(s_bool cond, const char *msg); /* panic if false */

/* ---- secrets ---- */
void   safety_wipe(void *p, s_u32 n); /* scrub password buffers etc. */

/* ---- hardened bump allocator helper ---- */
/* Align size up to 16; returns 0 on overflow. */
s_u32  safety_align16(s_u32 size);
/* Check heap_used + need fits in cap. */
s_bool safety_heap_fit(s_u32 used, s_u32 need, s_u32 cap);

#endif
