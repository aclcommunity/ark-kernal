/* ============================================================================
 *  cliphist.h -- Minimal clipboard history (last 5 entries)
 * ==========================================================================*/
#ifndef CLIPHIST_H
#define CLIPHIST_H

typedef unsigned int clip_u32;
typedef int          clip_bool;
#define CLIP_TRUE  1
#define CLIP_FALSE 0

#define CLIP_HIST_N    5
#define CLIP_HIST_LEN  96

void cliphist_init(void);
void cliphist_push(const char *s, clip_u32 len);
const char *cliphist_get(int idx); /* 0 = newest, NULL if empty */
clip_u32 cliphist_count(void);
void cliphist_clear(void);

#endif
