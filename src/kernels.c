/* kernels.c - benchmark workloads
 *
 * Four small, cache-resident kernels that stress different parts of a core:
 *   sort   quicksort of random 32-bit keys           branchy integer code, branch mispredictions
 *   lz     LZ77 compression of synthetic text        integer + byte loads, hash table lookups
 *   nbody  Benchmarks Game 5-body simulation         scalar double precision, sqrt/div latency
 *   sgemm  single precision matrix multiply          SIMD FMA throughput (NEON 8x8 micro-kernel)
 * Every call of kernel_run() does a fixed amount of work (one "unit"), so throughput can be
 * measured by counting units in a time window. Working sets fit in L1/L2: the benchmark
 * measures the cores, not DRAM.
 */
#include "kernels.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#define SORT_N      32768
#define LZ_BLOCK    (64 * 1024)
#define LZ_NBLOCKS  16
#define LZ_HBITS    14
#define LZ_OUT_CAP  (LZ_BLOCK + LZ_BLOCK / 8 + 64)
#define NBODY_STEPS 5000
#define GEMM_N      192

/* Reference rates: single-core results of a MacBook Air M5 (Apple clang 21, -mcpu=apple-m5),
 * so that it scores ~1000. */
const kernel_info_t kernel_info[K_COUNT] = {
    { "sort",  "quicksort 32K x u32",   "Melem/s",  SORT_N / 1e6,                              30.5 },
    { "lz",    "LZ77 compress 64 KB",   "MB/s",     LZ_BLOCK / 1e6,                            1110 },
    { "nbody", "5-body sim, double",    "Msteps/s", NBODY_STEPS / 1e6,                         28.1 },
    { "sgemm", "SGEMM 192^3, fp32 FMA", "GFLOPS",   2.0 * GEMM_N * GEMM_N * GEMM_N / 1e9,      116 },
};

struct kctx {
    int id;
    uint64_t seed;
    uint64_t iter[K_COUNT];
    uint32_t *sort_buf;
    uint8_t *lz_out;
    uint32_t *lz_ht;
    float *A, *B, *C, *Ap, *Bp;
};

static void *xalloc(size_t n)
{
    void *p = NULL;
    if (posix_memalign(&p, 128, n) != 0) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    memset(p, 0, n); /* first touch from the calling thread */
    return p;
}

static inline uint64_t splitmix64(uint64_t *s)
{
    uint64_t z = (*s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* ------------------------------------------------------------------------------------------ */
/* sort                                                                                        */

static inline void swap_u32(uint32_t *a, uint32_t *b)
{
    uint32_t t = *a;
    *a = *b;
    *b = t;
}

static void insertion_sort(uint32_t *a, ptrdiff_t n)
{
    for (ptrdiff_t i = 1; i < n; i++) {
        uint32_t v = a[i];
        ptrdiff_t j = i - 1;
        while (j >= 0 && a[j] > v) {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = v;
    }
}

/* Median-of-3 Hoare quicksort, recursing into the smaller side. */
static void quicksort_u32(uint32_t *a, ptrdiff_t n)
{
    while (n > 16) {
        ptrdiff_t m = n >> 1;
        if (a[m] < a[0]) swap_u32(&a[m], &a[0]);
        if (a[n - 1] < a[m]) {
            swap_u32(&a[n - 1], &a[m]);
            if (a[m] < a[0]) swap_u32(&a[m], &a[0]);
        }
        uint32_t p = a[m];
        ptrdiff_t i = -1, j = n;
        for (;;) {
            do i++; while (a[i] < p);
            do j--; while (a[j] > p);
            if (i >= j) break;
            swap_u32(&a[i], &a[j]);
        }
        ptrdiff_t left = j + 1; /* a[0..left) <= p <= a[left..n) */
        if (left < n - left) {
            quicksort_u32(a, left);
            a += left;
            n -= left;
        } else {
            quicksort_u32(a + left, n - left);
            n = left;
        }
    }
    insertion_sort(a, n);
}

static void sort_fill(uint32_t *a, uint64_t seed)
{
    for (int i = 0; i < SORT_N; i += 2) {
        uint64_t r = splitmix64(&seed);
        a[i] = (uint32_t)r;
        a[i + 1] = (uint32_t)(r >> 32);
    }
}

/* New random data every call: a repeated input would let the branch predictor learn it. */
static uint64_t run_sort(kctx_t *c)
{
    uint32_t *a = c->sort_buf;
    sort_fill(a, c->seed ^ (c->iter[K_SORT]++ * 0xD1B54A32D192ED03ull));
    quicksort_u32(a, SORT_N);
    return (uint64_t)a[0] + a[SORT_N / 2] + a[SORT_N - 1];
}

/* ------------------------------------------------------------------------------------------ */
/* lz                                                                                          */

static uint8_t *g_corpus; /* LZ_NBLOCKS * LZ_BLOCK bytes of text-like data, shared read-only */

/* Text-like data: words from a random vocabulary with a Zipf-like frequency, some recurring
 * multi-word phrases, and punctuation. */
static void corpus_init(void)
{
    enum { NWORDS = 2048, NPHRASES = 256, PHRASE_PCT = 25 };
    static char words[NWORDS][12];
    static int wlen[NWORDS];
    static int phrase[NPHRASES][6], plen[NPHRASES];
    uint64_t s = 42;
#define ZIPF(n) ((int)(pow((double)(splitmix64(&s) >> 11) * 0x1.0p-53, 3) * (n)))
    for (int i = 0; i < NWORDS; i++) {
        wlen[i] = 2 + (int)(splitmix64(&s) % 9);
        for (int j = 0; j < wlen[i]; j++) words[i][j] = (char)('a' + splitmix64(&s) % 26);
    }
    for (int i = 0; i < NPHRASES; i++) {
        plen[i] = 2 + (int)(splitmix64(&s) % 5);
        for (int j = 0; j < plen[i]; j++) phrase[i][j] = ZIPF(NWORDS);
    }
    size_t n = (size_t)LZ_NBLOCKS * LZ_BLOCK, pos = 0;
    g_corpus = xalloc(n);
    int nw = 0;
    while (pos < n) {
        int seq[6], len = 1;
        if ((int)(splitmix64(&s) % 100) < PHRASE_PCT) {
            int ph = ZIPF(NPHRASES);
            len = plen[ph];
            memcpy(seq, phrase[ph], sizeof(int) * (size_t)len);
        } else {
            seq[0] = ZIPF(NWORDS);
        }
        for (int i = 0; i < len && pos < n; i++, nw++) {
            int w = seq[i];
            for (int j = 0; j < wlen[w] && pos < n; j++)
                g_corpus[pos++] = (uint8_t)(nw == 0 && j == 0 ? words[w][j] - 32 : words[w][j]);
            if (pos < n && i + 1 < len) g_corpus[pos++] = ' ';
        }
        unsigned r = (unsigned)(splitmix64(&s) % 100);
        if (pos < n && nw > 5 && r < 12) {
            g_corpus[pos++] = '.';
            nw = 0;
            if (pos < n) g_corpus[pos++] = r < 3 ? '\n' : ' ';
        } else if (pos < n && r < 18) {
            g_corpus[pos++] = ',';
            if (pos < n) g_corpus[pos++] = ' ';
        } else if (pos < n) {
            g_corpus[pos++] = ' ';
        }
    }
#undef ZIPF
}

static inline uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

static inline size_t put_varint(uint8_t *out, size_t op, size_t v)
{
    while (v >= 128) {
        out[op++] = (uint8_t)(v | 128);
        v >>= 7;
    }
    out[op++] = (uint8_t)v;
    return op;
}

static size_t get_varint(const uint8_t *in, size_t *ip)
{
    size_t v = 0;
    int shift = 0;
    uint8_t b;
    do {
        b = in[(*ip)++];
        v |= (size_t)(b & 127) << shift;
        shift += 7;
    } while (b & 128);
    return v;
}

/* Greedy LZ77 (LZ4-like): 4-byte hash -> last position, 64 KB window.
 * Format: varint literal count, literals, 16-bit offset, varint (match length - 4). */
static size_t lz_compress(const uint8_t *in, size_t n, uint8_t *out, uint32_t *ht)
{
    memset(ht, 0, sizeof(uint32_t) << LZ_HBITS);
    size_t ip = 0, anchor = 0, op = 0;
    const size_t limit = n > 16 ? n - 16 : 0;
    while (ip < limit) {
        uint32_t seq = rd32(in + ip);
        uint32_t h = (seq * 2654435761u) >> (32 - LZ_HBITS);
        size_t ref = ht[h];
        ht[h] = (uint32_t)ip;
        if (ref < ip && ip - ref < 65536 && rd32(in + ref) == seq) {
            size_t len = 4;
            for (;;) {
                if (ip + len + 8 > n) {
                    while (ip + len < n && in[ip + len] == in[ref + len]) len++;
                    break;
                }
                uint64_t x = rd64(in + ip + len) ^ rd64(in + ref + len);
                if (x) {
                    len += (size_t)__builtin_ctzll(x) >> 3;
                    break;
                }
                len += 8;
            }
            op = put_varint(out, op, ip - anchor);
            memcpy(out + op, in + anchor, ip - anchor);
            op += ip - anchor;
            out[op++] = (uint8_t)(ip - ref);
            out[op++] = (uint8_t)((ip - ref) >> 8);
            op = put_varint(out, op, len - 4);
            ip += len;
            anchor = ip;
        } else {
            ip++;
        }
    }
    op = put_varint(out, op, n - anchor);
    memcpy(out + op, in + anchor, n - anchor);
    return op + (n - anchor);
}

static size_t lz_decompress(const uint8_t *in, size_t insz, uint8_t *out, size_t cap)
{
    size_t ip = 0, op = 0;
    while (ip < insz) {
        size_t lit = get_varint(in, &ip);
        if (op + lit > cap || ip + lit > insz) return (size_t)-1;
        memcpy(out + op, in + ip, lit);
        ip += lit;
        op += lit;
        if (ip >= insz) break;
        size_t off = in[ip] | (size_t)in[ip + 1] << 8;
        ip += 2;
        size_t len = get_varint(in, &ip) + 4;
        if (off == 0 || off > op || op + len > cap) return (size_t)-1;
        for (size_t i = 0; i < len; i++) out[op + i] = out[op - off + i];
        op += len;
    }
    return op;
}

static uint64_t run_lz(kctx_t *c)
{
    size_t blk = (size_t)((c->iter[K_LZ]++ + (uint64_t)c->id) % LZ_NBLOCKS);
    size_t n = lz_compress(g_corpus + blk * LZ_BLOCK, LZ_BLOCK, c->lz_out, c->lz_ht);
    return n + c->lz_out[n >> 1];
}

/* ------------------------------------------------------------------------------------------ */
/* nbody (The Computer Language Benchmarks Game)                                               */

typedef struct { double x, y, z, vx, vy, vz, mass; } body_t;

#define NB 5
#define PI 3.141592653589793
#define SOLAR_MASS (4 * PI * PI)
#define DAYS_PER_YEAR 365.24

static const body_t nbody_init[NB] = {
    { 0, 0, 0, 0, 0, 0, SOLAR_MASS },
    { 4.84143144246472090e+00, -1.16032004402742839e+00, -1.03622044471123109e-01,
      1.66007664274403694e-03 * DAYS_PER_YEAR, 7.69901118419740425e-03 * DAYS_PER_YEAR,
      -6.90460016972063023e-05 * DAYS_PER_YEAR, 9.54791938424326609e-04 * SOLAR_MASS },
    { 8.34336671824457987e+00, 4.12479856412430479e+00, -4.03523417114321381e-01,
      -2.76742510726862411e-03 * DAYS_PER_YEAR, 4.99852801234917238e-03 * DAYS_PER_YEAR,
      2.30417297573763929e-05 * DAYS_PER_YEAR, 2.85885980666130812e-04 * SOLAR_MASS },
    { 1.28943695621391310e+01, -1.51111514016986312e+01, -2.23307578892655734e-01,
      2.96460137564761618e-03 * DAYS_PER_YEAR, 2.37847173959480950e-03 * DAYS_PER_YEAR,
      -2.96589568540237556e-05 * DAYS_PER_YEAR, 4.36624404335156298e-05 * SOLAR_MASS },
    { 1.53796971148509165e+01, -2.59193146099879641e+01, 1.79258772950371181e-01,
      2.68067772490389322e-03 * DAYS_PER_YEAR, 1.62824170038242295e-03 * DAYS_PER_YEAR,
      -9.51592254519715870e-05 * DAYS_PER_YEAR, 5.15138902046611451e-05 * SOLAR_MASS },
};

static void nbody_offset_momentum(body_t *b)
{
    double px = 0, py = 0, pz = 0;
    for (int i = 0; i < NB; i++) {
        px += b[i].vx * b[i].mass;
        py += b[i].vy * b[i].mass;
        pz += b[i].vz * b[i].mass;
    }
    b[0].vx = -px / SOLAR_MASS;
    b[0].vy = -py / SOLAR_MASS;
    b[0].vz = -pz / SOLAR_MASS;
}

static double nbody_energy(const body_t *b)
{
    double e = 0;
    for (int i = 0; i < NB; i++) {
        e += 0.5 * b[i].mass * (b[i].vx * b[i].vx + b[i].vy * b[i].vy + b[i].vz * b[i].vz);
        for (int j = i + 1; j < NB; j++) {
            double dx = b[i].x - b[j].x, dy = b[i].y - b[j].y, dz = b[i].z - b[j].z;
            e -= b[i].mass * b[j].mass / sqrt(dx * dx + dy * dy + dz * dz);
        }
    }
    return e;
}

static void nbody_advance(body_t *b, double dt)
{
    for (int i = 0; i < NB; i++) {
        for (int j = i + 1; j < NB; j++) {
            double dx = b[i].x - b[j].x, dy = b[i].y - b[j].y, dz = b[i].z - b[j].z;
            double dist = sqrt(dx * dx + dy * dy + dz * dz);
            double mag = dt / (dist * dist * dist);
            double mi = b[i].mass * mag, mj = b[j].mass * mag;
            b[i].vx -= dx * mj;
            b[i].vy -= dy * mj;
            b[i].vz -= dz * mj;
            b[j].vx += dx * mi;
            b[j].vy += dy * mi;
            b[j].vz += dz * mi;
        }
    }
    for (int i = 0; i < NB; i++) {
        b[i].x += dt * b[i].vx;
        b[i].y += dt * b[i].vy;
        b[i].z += dt * b[i].vz;
    }
}

static uint64_t run_nbody(kctx_t *c)
{
    (void)c;
    body_t b[NB];
    memcpy(b, nbody_init, sizeof b);
    nbody_offset_momentum(b);
    for (int s = 0; s < NBODY_STEPS; s++) nbody_advance(b, 0.01);
    double r = b[1].x + b[4].vz;
    uint64_t bits;
    memcpy(&bits, &r, sizeof bits);
    return bits;
}

/* ------------------------------------------------------------------------------------------ */
/* sgemm: C = A * B, n x n row-major, A and B packed into 8-wide panels, 8x8 register tiles     */

static void gemm_pack_a(const float *A, float *Ap, int n)
{
    for (int p = 0; p < n; p += 8)
        for (int k = 0; k < n; k++)
            for (int r = 0; r < 8; r++) *Ap++ = A[(size_t)(p + r) * n + k];
}

static void gemm_pack_b(const float *B, float *Bp, int n)
{
    for (int q = 0; q < n; q += 8)
        for (int k = 0; k < n; k++, Bp += 8) memcpy(Bp, B + (size_t)k * n + q, 8 * sizeof(float));
}

#if defined(__aarch64__)
static void gemm_kernel_8x8(int K, const float *restrict a, const float *restrict b, float *restrict c, int ldc)
{
    float32x4_t c00 = vdupq_n_f32(0), c01 = c00, c10 = c00, c11 = c00, c20 = c00, c21 = c00, c30 = c00, c31 = c00;
    float32x4_t c40 = c00, c41 = c00, c50 = c00, c51 = c00, c60 = c00, c61 = c00, c70 = c00, c71 = c00;
    for (int k = 0; k < K; k++, a += 8, b += 8) {
        float32x4_t b0 = vld1q_f32(b), b1 = vld1q_f32(b + 4);
        float32x4_t a0 = vld1q_f32(a), a1 = vld1q_f32(a + 4);
        c00 = vfmaq_laneq_f32(c00, b0, a0, 0); c01 = vfmaq_laneq_f32(c01, b1, a0, 0);
        c10 = vfmaq_laneq_f32(c10, b0, a0, 1); c11 = vfmaq_laneq_f32(c11, b1, a0, 1);
        c20 = vfmaq_laneq_f32(c20, b0, a0, 2); c21 = vfmaq_laneq_f32(c21, b1, a0, 2);
        c30 = vfmaq_laneq_f32(c30, b0, a0, 3); c31 = vfmaq_laneq_f32(c31, b1, a0, 3);
        c40 = vfmaq_laneq_f32(c40, b0, a1, 0); c41 = vfmaq_laneq_f32(c41, b1, a1, 0);
        c50 = vfmaq_laneq_f32(c50, b0, a1, 1); c51 = vfmaq_laneq_f32(c51, b1, a1, 1);
        c60 = vfmaq_laneq_f32(c60, b0, a1, 2); c61 = vfmaq_laneq_f32(c61, b1, a1, 2);
        c70 = vfmaq_laneq_f32(c70, b0, a1, 3); c71 = vfmaq_laneq_f32(c71, b1, a1, 3);
    }
    vst1q_f32(c + 0 * ldc, c00); vst1q_f32(c + 0 * ldc + 4, c01);
    vst1q_f32(c + 1 * ldc, c10); vst1q_f32(c + 1 * ldc + 4, c11);
    vst1q_f32(c + 2 * ldc, c20); vst1q_f32(c + 2 * ldc + 4, c21);
    vst1q_f32(c + 3 * ldc, c30); vst1q_f32(c + 3 * ldc + 4, c31);
    vst1q_f32(c + 4 * ldc, c40); vst1q_f32(c + 4 * ldc + 4, c41);
    vst1q_f32(c + 5 * ldc, c50); vst1q_f32(c + 5 * ldc + 4, c51);
    vst1q_f32(c + 6 * ldc, c60); vst1q_f32(c + 6 * ldc + 4, c61);
    vst1q_f32(c + 7 * ldc, c70); vst1q_f32(c + 7 * ldc + 4, c71);
}
#else
static void gemm_kernel_8x8(int K, const float *restrict a, const float *restrict b, float *restrict c, int ldc)
{
    float acc[8][8] = { { 0 } };
    for (int k = 0; k < K; k++, a += 8, b += 8)
        for (int r = 0; r < 8; r++)
            for (int j = 0; j < 8; j++) acc[r][j] += a[r] * b[j];
    for (int r = 0; r < 8; r++)
        for (int j = 0; j < 8; j++) c[r * ldc + j] = acc[r][j];
}
#endif

static void gemm(kctx_t *c, int n)
{
    gemm_pack_a(c->A, c->Ap, n);
    gemm_pack_b(c->B, c->Bp, n);
    for (int q = 0; q < n; q += 8)
        for (int p = 0; p < n; p += 8)
            gemm_kernel_8x8(n, c->Ap + (size_t)p * n, c->Bp + (size_t)q * n, c->C + (size_t)p * n + q, n);
}

static uint64_t run_sgemm(kctx_t *c)
{
    gemm(c, GEMM_N);
    float v = c->C[(c->iter[K_SGEMM]++ * 7919) % ((uint64_t)GEMM_N * GEMM_N)];
    uint32_t bits;
    memcpy(&bits, &v, sizeof bits);
    return bits;
}

/* ------------------------------------------------------------------------------------------ */

int kernels_global_init(void)
{
    corpus_init();
    return 0;
}

kctx_t *kctx_create(int id)
{
    kctx_t *c = xalloc(sizeof *c);
    c->id = id;
    c->seed = 0x5EED0000ull + (uint64_t)id * 0x9E3779B97F4A7C15ull;
    c->sort_buf = xalloc(SORT_N * sizeof(uint32_t));
    c->lz_out = xalloc(LZ_OUT_CAP);
    c->lz_ht = xalloc(sizeof(uint32_t) << LZ_HBITS);
    size_t nn = (size_t)GEMM_N * GEMM_N;
    c->A = xalloc(nn * sizeof(float));
    c->B = xalloc(nn * sizeof(float));
    c->C = xalloc(nn * sizeof(float));
    c->Ap = xalloc(nn * sizeof(float));
    c->Bp = xalloc(nn * sizeof(float));
    uint64_t s = c->seed;
    for (size_t i = 0; i < nn; i++) {
        c->A[i] = (float)((double)(splitmix64(&s) >> 11) * 0x1.0p-52 - 1.0);
        c->B[i] = (float)((double)(splitmix64(&s) >> 11) * 0x1.0p-52 - 1.0);
    }
    return c;
}

void kctx_destroy(kctx_t *c)
{
    if (!c) return;
    free(c->sort_buf);
    free(c->lz_out);
    free(c->lz_ht);
    free(c->A);
    free(c->B);
    free(c->C);
    free(c->Ap);
    free(c->Bp);
    free(c);
}

uint64_t kernel_run(kctx_t *c, int k)
{
    switch (k) {
    case K_SORT: return run_sort(c);
    case K_LZ: return run_lz(c);
    case K_NBODY: return run_nbody(c);
    default: return run_sgemm(c);
    }
}

int kernels_selftest(char *msg, size_t len)
{
    kctx_t *c = kctx_create(0);
    int bad = 0;
    char *p = msg;
    size_t left = len;
#define REPORT(...)                                        \
    do {                                                   \
        int w_ = snprintf(p, left, __VA_ARGS__);           \
        if (w_ > 0 && (size_t)w_ < left) { p += w_; left -= (size_t)w_; } \
    } while (0)

    /* sort: output ordered and a permutation of the input (sum and xor preserved) */
    uint64_t sum0 = 0, x0 = 0, sum1 = 0, x1 = 0;
    sort_fill(c->sort_buf, 12345);
    for (int i = 0; i < SORT_N; i++) { sum0 += c->sort_buf[i]; x0 ^= c->sort_buf[i]; }
    quicksort_u32(c->sort_buf, SORT_N);
    int ordered = 1;
    for (int i = 0; i < SORT_N; i++) {
        sum1 += c->sort_buf[i];
        x1 ^= c->sort_buf[i];
        if (i && c->sort_buf[i - 1] > c->sort_buf[i]) ordered = 0;
    }
    int ok = ordered && sum0 == sum1 && x0 == x1;
    bad |= !ok;
    REPORT("sort %s", ok ? "ok" : "FAILED");

    /* lz: round trip on every block */
    uint8_t *tmp = malloc(LZ_BLOCK);
    size_t total = 0;
    ok = 1;
    for (int b = 0; b < LZ_NBLOCKS; b++) {
        size_t n = lz_compress(g_corpus + (size_t)b * LZ_BLOCK, LZ_BLOCK, c->lz_out, c->lz_ht);
        total += n;
        if (lz_decompress(c->lz_out, n, tmp, LZ_BLOCK) != LZ_BLOCK ||
            memcmp(tmp, g_corpus + (size_t)b * LZ_BLOCK, LZ_BLOCK) != 0) ok = 0;
    }
    free(tmp);
    bad |= !ok;
    REPORT(", lz %s (ratio %.2f)", ok ? "ok" : "FAILED", (double)LZ_NBLOCKS * LZ_BLOCK / (double)total);

    /* nbody: published output for n = 1000 is -0.169075164 / -0.169087605 */
    body_t b[NB];
    memcpy(b, nbody_init, sizeof b);
    nbody_offset_momentum(b);
    double e0 = nbody_energy(b);
    for (int s = 0; s < 1000; s++) nbody_advance(b, 0.01);
    double e1 = nbody_energy(b);
    ok = fabs(e0 - -0.169075164) < 1e-9 && fabs(e1 - -0.169087605) < 1e-9;
    bad |= !ok;
    REPORT(", nbody %s (%.9f)", ok ? "ok" : "FAILED", e1);

    /* sgemm: compare with a double precision reference */
    const int n = GEMM_N;
    gemm(c, n);
    double maxerr = 0;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) {
            double r = 0;
            for (int k = 0; k < n; k++) r += (double)c->A[i * n + k] * c->B[k * n + j];
            double e = fabs(r - c->C[i * n + j]);
            if (e > maxerr) maxerr = e;
        }
    ok = maxerr < 1e-3;
    bad |= !ok;
    REPORT(", sgemm %s (max err %.1e)", ok ? "ok" : "FAILED", maxerr);
#undef REPORT
    kctx_destroy(c);
    return bad;
}
