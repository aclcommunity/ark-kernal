/* ============================================================================
 *  cliphist.c -- Fixed-size clipboard history
 * ==========================================================================*/
#include "cliphist.h"

static char entries[CLIP_HIST_N][CLIP_HIST_LEN + 1];
static clip_u32 count;
static int head; /* next write slot */

void cliphist_init(void)
{
    count = 0;
    head = 0;
    for (int i = 0; i < CLIP_HIST_N; i++)
        entries[i][0] = 0;
}

void cliphist_push(const char *s, clip_u32 len)
{
    if (!s || len == 0) return;
    if (len > CLIP_HIST_LEN) len = CLIP_HIST_LEN;
    for (clip_u32 i = 0; i < len; i++)
        entries[head][i] = s[i];
    entries[head][len] = 0;
    head = (head + 1) % CLIP_HIST_N;
    if (count < CLIP_HIST_N) count++;
}

const char *cliphist_get(int idx)
{
    if (idx < 0 || (clip_u32)idx >= count) return 0;
    /* newest is at (head-1), then older */
    int pos = head - 1 - idx;
    while (pos < 0) pos += CLIP_HIST_N;
    return entries[pos];
}

clip_u32 cliphist_count(void) { return count; }

void cliphist_clear(void)
{
    count = 0;
    head = 0;
    for (int i = 0; i < CLIP_HIST_N; i++)
        entries[i][0] = 0;
}
