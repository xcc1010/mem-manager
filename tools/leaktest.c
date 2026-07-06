/* leaktest.c — deterministic anonymous-mmap leak to validate the mem_trace +
 * analyze_mem_trace.py --baseline pipeline before running it on real business.
 *
 * Reproduces the board symptom: RssAnon grows via large (>=128KB) allocations
 * that are never freed (glibc serves >=128KB from mmap -> anonymous VMAs).
 * It also does two things that MUST be correctly ignored by the growth diff:
 *   - churn(): alloc+free every iteration -> not live at snapshot -> absent.
 *   - a big stable baseline: allocated once, never freed -> constant -> delta 0.
 * So a correct run shows exactly one growing function: leak_here().
 *
 * Build:  gcc -O0 -g -o leaktest leaktest.c
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LEAK_CHUNK (256 * 1024)          /* >=128KB -> mmap (anonymous)      */

static void *keep[1 << 20];              /* hold leaked ptrs so they stay live */
static long  nkeep = 0;

static void leak_here(void)              /* THE leak: alloc, touch, never free */
{
    void *p = malloc(LEAK_CHUNK);
    if (!p || nkeep >= (1 << 20)) return;
    memset(p, 0xAB, LEAK_CHUNK);         /* fault in -> resident (mincore sees) */
    keep[nkeep++] = p;
}

static void churn(void)                  /* noise: net-zero, must NOT show up   */
{
    void *p = malloc(LEAK_CHUNK);
    if (p) { memset(p, 0x11, LEAK_CHUNK); free(p); }
}

int main(void)
{
    /* stable baseline: live the whole time, equal in both snapshots -> the
     * growth diff must cancel it out (appears only in absolute ranking). */
    void *base = malloc(64 * 1024 * 1024);
    if (base) memset(base, 1, 64 * 1024 * 1024);

    for (;;) {
        leak_here();
        churn();
        usleep(20 * 1000);               /* ~12.8 MiB/s leak (fast, for testing) */
    }
    (void)base;
    return 0;
}
