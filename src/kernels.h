/* kernels.h - benchmark workloads */
#ifndef KERNELS_H
#define KERNELS_H

#include <stddef.h>
#include <stdint.h>

enum { K_SORT, K_LZ, K_NBODY, K_SGEMM, K_COUNT };

typedef struct {
    const char *name;
    const char *desc;
    const char *unit;     /* unit of the reported rate */
    double work;          /* work done by one call of kernel_run, in `unit` x seconds */
    double ref;           /* reference rate, scores are 1000 x geomean(rate / ref) */
} kernel_info_t;

extern const kernel_info_t kernel_info[K_COUNT];

typedef struct kctx kctx_t;

int kernels_global_init(void);           /* shared read-only data; call once from main */
kctx_t *kctx_create(int id);             /* per-thread buffers; call from the worker (first touch) */
void kctx_destroy(kctx_t *c);
uint64_t kernel_run(kctx_t *c, int k);   /* one unit of work; returns a value to keep the result alive */
int kernels_selftest(char *msg, size_t len);  /* 0 = all kernels produce correct results */

#endif
