/* main.c - maccorespeed: single-core, multi-core burst and multi-core sustained (thermal steady
 * state) CPU benchmark for Apple Silicon Macs.
 *
 * Phases:
 *   1. single-core   one thread (user-interactive QoS -> fastest core), best of N runs per kernel
 *   2. burst         all cores, a few seconds in total, started cool: performance before throttling
 *   3. sustained     all cores, continuous rounds of the same kernels until score and temperature
 *                    stop drifting, then measured for a fixed time: performance after throttling
 */
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/qos.h>
#include <IOKit/pwr_mgt/IOPMLib.h>

#include "kernels.h"
#include "platform.h"

#define VERSION "1.0"
#ifndef BUILD_INFO
#define BUILD_INFO "unknown build flags"
#endif

/* ------------------------------------------------------------------------------------------ */
/* options                                                                                     */

typedef struct {
    int single, burst, sustained;
    int threads;
    double st_warm, st_dur;
    int st_reps;
    double burst_warm, burst_dur;
    double round_warm, round_dur;
    double window, measure, max_time, tol_pct, tol_temp, print_every;
    int cooldown;
    double cool_min, cool_max;
    const char *csv;
    int list_sensors, selftest_only;
} opts_t;

static opts_t O = {
    .single = 1, .burst = 1, .sustained = 1,
    .st_warm = 0.3, .st_dur = 1.0, .st_reps = 3,
    .burst_warm = 0.2, .burst_dur = 1.0,
    .round_warm = 0.05, .round_dur = 1.0,
    .window = 120, .measure = 60, .max_time = 1200, .tol_pct = 1.0, .tol_temp = 1.0, .print_every = 10,
    .cooldown = 1, .cool_min = 20, .cool_max = 180,
};

static void usage(const char *argv0)
{
    printf("usage: %s [options]\n"
           "\n"
           "Runs a single-core test, a short multi-core burst (before thermal throttling) and a\n"
           "sustained multi-core test measured once performance and temperature are at steady state.\n"
           "\n"
           "  --single, --burst, --sustained   run only the selected phase(s) (default: all)\n"
           "  -t, --threads N     threads for the multi-core phases (default: all cores)\n"
           "  --max-time MIN      sustained phase time limit in minutes (default 20)\n"
           "  --window S          steady-state detection window in seconds (default 120)\n"
           "  --measure S         measurement time once steady, in seconds (default 60)\n"
           "  --tol PCT           max score drift at steady state, %%/min (default 1.0)\n"
           "  --tol-temp C        max temperature drift at steady state, C/min (default 1.0)\n"
           "  --round S           seconds per kernel in a sustained round (default 1.0)\n"
           "  --no-cooldown       do not wait for the CPU to cool down between phases\n"
           "  --csv FILE          write the sustained time series to FILE\n"
           "  --quick             short run, only to check that everything works\n"
           "  --list-sensors      print the thermal sensors found and exit\n"
           "  --selftest          check that the kernels compute correct results and exit\n"
           "  -h, --help          this help\n",
           argv0);
}

static double arg_num(int argc, char **argv, int *i)
{
    if (*i + 1 >= argc) {
        fprintf(stderr, "missing value for %s\n", argv[*i]);
        exit(2);
    }
    char *end;
    double v = strtod(argv[++*i], &end);
    if (*end) {
        fprintf(stderr, "invalid number for %s: %s\n", argv[*i - 1], argv[*i]);
        exit(2);
    }
    return v;
}

static void parse_args(int argc, char **argv)
{
    int sel = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--single")) sel |= 1;
        else if (!strcmp(a, "--burst")) sel |= 2;
        else if (!strcmp(a, "--sustained")) sel |= 4;
        else if (!strcmp(a, "-t") || !strcmp(a, "--threads")) O.threads = (int)arg_num(argc, argv, &i);
        else if (!strcmp(a, "--max-time")) O.max_time = 60 * arg_num(argc, argv, &i);
        else if (!strcmp(a, "--window")) O.window = arg_num(argc, argv, &i);
        else if (!strcmp(a, "--measure")) O.measure = arg_num(argc, argv, &i);
        else if (!strcmp(a, "--tol")) O.tol_pct = arg_num(argc, argv, &i);
        else if (!strcmp(a, "--tol-temp")) O.tol_temp = arg_num(argc, argv, &i);
        else if (!strcmp(a, "--round")) O.round_dur = arg_num(argc, argv, &i);
        else if (!strcmp(a, "--no-cooldown")) O.cooldown = 0;
        else if (!strcmp(a, "--csv")) {
            if (i + 1 >= argc) { fprintf(stderr, "missing value for --csv\n"); exit(2); }
            O.csv = argv[++i];
        } else if (!strcmp(a, "--quick")) {
            O.st_warm = 0.1; O.st_dur = 0.3; O.st_reps = 1;
            O.burst_warm = 0.1; O.burst_dur = 0.3;
            O.round_warm = 0.02; O.round_dur = 0.3;
            O.window = 12; O.measure = 6; O.max_time = 40; O.print_every = 3;
            O.cool_min = 2; O.cool_max = 5;
        } else if (!strcmp(a, "--list-sensors")) O.list_sensors = 1;
        else if (!strcmp(a, "--selftest")) O.selftest_only = 1;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(argv[0]); exit(0); }
        else {
            fprintf(stderr, "unknown option: %s (see --help)\n", a);
            exit(2);
        }
    }
    if (sel) {
        O.single = !!(sel & 1);
        O.burst = !!(sel & 2);
        O.sustained = !!(sel & 4);
    }
    if (O.window < 4 * (O.round_dur + O.round_warm) * K_COUNT) O.window = 4 * (O.round_dur + O.round_warm) * K_COUNT;
}

/* ------------------------------------------------------------------------------------------ */
/* worker pool                                                                                 */

static volatile sig_atomic_t g_interrupted;

static void on_sigint(int sig)
{
    (void)sig;
    g_interrupted = 1;
    signal(SIGINT, SIG_DFL); /* a second Ctrl-C quits immediately */
}

typedef struct pool pool_t;

typedef struct {
    pool_t *pool;
    int id;
    pthread_t tid;
    kctx_t *ctx;
    uint64_t units, t0, t1, sink;
} worker_t;

struct pool {
    int n;
    worker_t *w;
    pthread_mutex_t mu;
    pthread_cond_t go, done;
    unsigned gen;
    int ndone, nready, quit;
    int kernel;
    uint64_t t_start, t_stop;
};

/* Run units of kernel k: unmeasured until t_start (warm-up), then count units until t_stop. */
static void run_job(worker_t *w, int k, uint64_t t_start, uint64_t t_stop)
{
    uint64_t chk = 0, n = 0, t;
    while ((t = now_ns()) < t_start) chk += kernel_run(w->ctx, k);
    uint64_t t0 = t;
    do {
        chk += kernel_run(w->ctx, k);
        n++;
        t = now_ns();
    } while (t < t_stop);
    w->units = n;
    w->t0 = t0;
    w->t1 = t;
    w->sink += chk;
}

static void *worker_main(void *arg)
{
    worker_t *w = arg;
    pool_t *p = w->pool;
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    w->ctx = kctx_create(w->id);
    pthread_mutex_lock(&p->mu);
    p->nready++;
    pthread_cond_broadcast(&p->done);
    unsigned seen = p->gen;
    for (;;) {
        while (p->gen == seen && !p->quit) pthread_cond_wait(&p->go, &p->mu);
        if (p->quit) break;
        seen = p->gen;
        int k = p->kernel;
        uint64_t ts = p->t_start, te = p->t_stop;
        pthread_mutex_unlock(&p->mu);
        run_job(w, k, ts, te);
        pthread_mutex_lock(&p->mu);
        if (++p->ndone == p->n) pthread_cond_broadcast(&p->done);
    }
    pthread_mutex_unlock(&p->mu);
    kctx_destroy(w->ctx);
    return NULL;
}

static pool_t *pool_create(int n)
{
    pool_t *p = calloc(1, sizeof *p);
    p->n = n;
    p->w = calloc((size_t)n, sizeof *p->w);
    pthread_mutex_init(&p->mu, NULL);
    pthread_cond_init(&p->go, NULL);
    pthread_cond_init(&p->done, NULL);
    for (int i = 0; i < n; i++) {
        p->w[i].pool = p;
        p->w[i].id = i;
        pthread_create(&p->w[i].tid, NULL, worker_main, &p->w[i]);
    }
    pthread_mutex_lock(&p->mu);
    while (p->nready < n) pthread_cond_wait(&p->done, &p->mu);
    pthread_mutex_unlock(&p->mu);
    return p;
}

static void pool_destroy(pool_t *p)
{
    pthread_mutex_lock(&p->mu);
    p->quit = 1;
    pthread_cond_broadcast(&p->go);
    pthread_mutex_unlock(&p->mu);
    for (int i = 0; i < p->n; i++) pthread_join(p->w[i].tid, NULL);
    free(p->w);
    free(p);
}

typedef struct {
    double rate;   /* kernel_info[k].unit per second, all threads together */
    metrics_t m;
    double temp;
} meas_t;

/* All threads run kernel k: warm_s seconds of warm-up, then dur_s seconds measured. */
static void measure(pool_t *p, int k, double warm_s, double dur_s, meas_t *r)
{
    uint64_t t_start = now_ns() + (uint64_t)(warm_s * 1e9);
    uint64_t t_stop = t_start + (uint64_t)(dur_s * 1e9);
    counters_t c0, c1;

    pthread_mutex_lock(&p->mu);
    p->kernel = k;
    p->t_start = t_start;
    p->t_stop = t_stop;
    p->ndone = 0;
    p->gen++;
    pthread_cond_broadcast(&p->go);
    pthread_mutex_unlock(&p->mu);

    sleep_until_ns(t_start);
    counters_sample(&c0);
    /* read the (slow, ~3 ms) SMC sensors while the kernel runs, so the cores never sit idle */
    sleep_until_ns(t_start + (uint64_t)(dur_s * 0.9e9));
    r->temp = sensors_cpu_temp();
    pthread_mutex_lock(&p->mu);
    while (p->ndone < p->n) pthread_cond_wait(&p->done, &p->mu);
    pthread_mutex_unlock(&p->mu);
    counters_sample(&c1);

    double units_per_s = 0;
    for (int i = 0; i < p->n; i++) {
        worker_t *w = &p->w[i];
        if (w->t1 > w->t0) units_per_s += (double)w->units / ((double)(w->t1 - w->t0) * 1e-9);
    }
    r->rate = units_per_s * kernel_info[k].work;
    counters_diff(&c0, &c1, &r->m);
}

/* ------------------------------------------------------------------------------------------ */
/* results and formatting                                                                      */

static sysinfo_t SI;
static char L0[16], L1[16]; /* short names of the core types: "S", "E" */
static double g_idle_temp = NAN;

typedef struct {
    int valid;
    double rate[K_COUNT];
    metrics_t m[K_COUNT];
    double score;
    double bg_max;
} phase_t;

static phase_t R_st, R_burst, R_sust;

typedef struct {
    double t; /* end of round, seconds since the start of the phase */
    double score;
    double rate[K_COUNT];
    metrics_t m;
    double temp, fan;
    int pressure;
    int measuring;
} round_t;

static round_t *g_rounds;
static int g_nrounds, g_cap_rounds;
static double g_steady_at = NAN, g_sust_drift = NAN;
static int g_not_steady;

static double score_of(const double *rate)
{
    double s = 0;
    for (int k = 0; k < K_COUNT; k++) {
        if (!(rate[k] > 0)) return NAN;
        s += log(rate[k] / kernel_info[k].ref);
    }
    return 1000.0 * exp(s / K_COUNT);
}

static const char *fmt_time(double s, char *buf)
{
    int t = (int)lround(s);
    snprintf(buf, 16, "%d:%02d", t / 60, t % 60);
    return buf;
}

static const char *fmt_num(double v, char *buf, int width)
{
    if (isnan(v)) snprintf(buf, 32, "%*s", width, "-");
    else if (v >= 1000) snprintf(buf, 32, "%*.0f", width, v);
    else if (v >= 100) snprintf(buf, 32, "%*.1f", width, v);
    else if (v >= 10) snprintf(buf, 32, "%*.2f", width, v);
    else snprintf(buf, 32, "%*.3f", width, v);
    return buf;
}

static const char *fmt_f(double v, const char *fmt, char *buf, int width)
{
    if (isnan(v)) snprintf(buf, 32, "%*s", width, "-");
    else snprintf(buf, 32, fmt, width, v);
    return buf;
}

static void rule(void)
{
    printf("────────────────────────────────────────────────────────────────────────────────\n");
}

/* ------------------------------------------------------------------------------------------ */
/* phases                                                                                      */

static void cooldown(const char *before)
{
    if (!O.cooldown || g_interrupted) return;
    uint64_t t0 = now_ns();
    if (!sensors_have_temp()) {
        printf("\n  cool-down before the %s test: %.0f s (no temperature sensor)\n", before, O.cool_min);
        sleep_s(O.cool_min);
        return;
    }
    /* Wait until the CPU is back at the idle temperature measured at start, or until it stops
     * cooling (5 s average dropped by less than 0.5 C in 15 s), within [cool_min, cool_max]. */
    double target = isnan(g_idle_temp) ? 50 : g_idle_temp + 3;
    double hist[1024], avg = sensors_cpu_temp(), el = 0;
    int n = 0;
    printf("\n  cool-down before the %s test: %.1f C", before, avg);
    fflush(stdout);
    for (;;) {
        sleep_s(1.0);
        if (n < 1024) hist[n++] = sensors_cpu_temp();
        int m = n < 5 ? n : 5;
        avg = 0;
        for (int i = n - m; i < n; i++) avg += hist[i] / m;
        el = (double)(now_ns() - t0) * 1e-9;
        if (g_interrupted || el >= O.cool_max) break;
        if (el < O.cool_min) continue;
        if (avg <= target) break;
        if (n >= 20) {
            double old = 0;
            for (int i = n - 20; i < n - 15; i++) old += hist[i] / 5;
            if (avg > old - 0.5) break;
        }
    }
    printf(" -> %.1f C in %.0f s\n", avg, el);
}

static int g_phase, g_nphases;

static void phase_title(const char *name, const char *fmt, ...)
{
    va_list ap;
    printf("\n[%d/%d] %s  ", ++g_phase, g_nphases, name);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

static void phase_single(void)
{
    phase_title("Single-core", "1 thread, best of %d x %.1f s per kernel", O.st_reps, O.st_dur);
    printf("  %-6s %-22s %16s  %5s  %5s  %5s\n", "kernel", "workload", "rate", "GHz", "IPC", "CPU W");
    pool_t *p = pool_create(1);
    for (int k = 0; k < K_COUNT && !g_interrupted; k++) {
        meas_t best = { 0 }, r;
        for (int rep = 0; rep < O.st_reps && !g_interrupted; rep++) {
            measure(p, k, rep == 0 ? O.st_warm : 0.02, O.st_dur, &r);
            if (r.rate > best.rate) best = r;
        }
        R_st.rate[k] = best.rate;
        R_st.m[k] = best.m;
        if (best.m.bg_cores > R_st.bg_max) R_st.bg_max = best.m.bg_cores;
        char a[32], b[32], c[32], d[32];
        printf("  %-6s %-22s %8s %-8s %5s  %5s  %5s", kernel_info[k].name, kernel_info[k].desc,
               fmt_num(best.rate, a, 8), kernel_info[k].unit, fmt_f(best.m.p_ghz, "%*.2f", b, 5),
               fmt_f(best.m.ipc, "%*.2f", c, 5), fmt_f(best.m.watts, "%*.1f", d, 5));
        double on_fast = best.m.p_busy / (best.m.p_busy + best.m.e_busy);
        if (on_fast < 0.9) printf("  (only %.0f%% on %s-cores)", 100 * on_fast, L0);
        printf("\n");
    }
    pool_destroy(p);
    if (g_interrupted) return;
    R_st.score = score_of(R_st.rate);
    R_st.valid = 1;
    printf("  => single-core score: %.0f\n", R_st.score);
}

/* column headers for the two core groups, e.g. "S GHz" / "E GHz" */
static char H0[24], H1[24];
static int W0, W1;

static void phase_burst(pool_t *p)
{
    phase_title("Multi-core burst", "%d threads, %.1f s warm-up + %.1f s per kernel (%.0f s in total)", p->n,
                O.burst_warm, O.burst_dur, K_COUNT * (O.burst_warm + O.burst_dur));
    printf("  %-6s %16s  %5s  %*s  %*s  %9s  %5s  %6s\n", "kernel", "rate", "x1T", W0, H0, W1, H1,
           "busy S/E", "CPU W", "temp C");
    for (int k = 0; k < K_COUNT && !g_interrupted; k++) {
        meas_t r;
        measure(p, k, O.burst_warm, O.burst_dur, &r);
        R_burst.rate[k] = r.rate;
        R_burst.m[k] = r.m;
        if (r.m.bg_cores > R_burst.bg_max) R_burst.bg_max = r.m.bg_cores;
        char a[32], b[32], c[32], d[32], e[32], f[32];
        printf("  %-6s %8s %-8s %5s  %*s  %*s  %4.1f/%-4.1f  %5s  %6s", kernel_info[k].name,
               fmt_num(r.rate, a, 8), kernel_info[k].unit,
               R_st.valid ? fmt_f(r.rate / R_st.rate[k], "%*.2f", b, 5) : "    -",
               W0, fmt_f(r.m.p_ghz, "%*.2f", c, W0), W1, fmt_f(r.m.e_ghz, "%*.2f", d, W1), r.m.p_busy,
               r.m.e_busy, fmt_f(r.m.watts, "%*.1f", e, 5), fmt_f(r.temp, "%*.0f", f, 6));
        if (r.m.bg_cores > 1.0) printf("  (other processes: %.1f cores)", r.m.bg_cores);
        printf("\n");
    }
    if (g_interrupted) return;
    R_burst.score = score_of(R_burst.rate);
    R_burst.valid = 1;
    printf("  => multi-core burst score: %.0f", R_burst.score);
    if (R_st.valid) printf("  (%.2f x single-core)", R_burst.score / R_st.score);
    printf("\n");
}

/* Weighted average of the round metrics in [from, to). */
static void rounds_avg(int from, int to, round_t *out)
{
    memset(out, 0, sizeof *out);
    double w = 0, tw = 0, fw = 0, gp = 0, ge = 0;
    out->pressure = -1;
    for (int i = from; i < to; i++) {
        const round_t *r = &g_rounds[i];
        double dt = r->m.wall_s;
        w += dt;
        out->score += r->score * dt;
        for (int k = 0; k < K_COUNT; k++) out->rate[k] += r->rate[k] * dt;
        out->m.p_busy += r->m.p_busy * dt;
        out->m.e_busy += r->m.e_busy * dt;
        out->m.watts += r->m.watts * dt;
        out->m.bg_cores += r->m.bg_cores * dt;
        if (!isnan(r->m.p_ghz)) { out->m.p_ghz += r->m.p_ghz * dt; gp += dt; }
        if (!isnan(r->m.e_ghz)) { out->m.e_ghz += r->m.e_ghz * dt; ge += dt; }
        if (!isnan(r->temp)) { out->temp += r->temp * dt; tw += dt; }
        if (!isnan(r->fan)) { out->fan += r->fan * dt; fw += dt; }
        if (r->pressure > out->pressure) out->pressure = r->pressure;
    }
    if (w <= 0) return;
    out->score /= w;
    for (int k = 0; k < K_COUNT; k++) out->rate[k] /= w;
    out->m.p_busy /= w;
    out->m.e_busy /= w;
    out->m.watts /= w;
    out->m.bg_cores /= w;
    out->m.p_ghz = gp > 0 ? out->m.p_ghz / gp : NAN;
    out->m.e_ghz = ge > 0 ? out->m.e_ghz / ge : NAN;
    out->temp = tw > 0 ? out->temp / tw : NAN;
    out->fan = fw > 0 ? out->fan / fw : NAN;
    out->m.wall_s = w;
    out->t = g_rounds[to - 1].t;
}

static void print_round_header(double ref)
{
    printf("  %5s  %6s  %6s  %*s  %*s  %9s  %5s  %6s", "time", "score", ref > 0 ? "%burst" : "", W0, H0, W1,
           H1, "busy S/E", "CPU W", "temp C");
    if (sensors_fan_count()) printf("  %7s", "fan rpm");
    printf("  %s\n", "pressure");
}

static void print_rounds(int from, int to, double ref)
{
    round_t a;
    rounds_avg(from, to, &a);
    char t[16], s0[32], s1[32], s2[32], s3[32], s4[32], s5[32];
    printf("  %5s  %6.0f  %6s  %*s  %*s  %4.1f/%-4.1f  %5s  %6s", fmt_time(a.t, t), a.score,
           ref > 0 ? fmt_f(100 * a.score / ref, "%*.1f", s0, 6) : "", W0, fmt_f(a.m.p_ghz, "%*.2f", s1, W0), W1,
           fmt_f(a.m.e_ghz, "%*.2f", s2, W1), a.m.p_busy, a.m.e_busy, fmt_f(a.m.watts, "%*.1f", s3, 5),
           fmt_f(a.temp, "%*.0f", s4, 6));
    if (sensors_fan_count()) printf("  %7s", fmt_f(a.fan, "%*.0f", s5, 7));
    printf("  %s", a.pressure >= 0 ? thermal_pressure_name(a.pressure) : "-");
    if (a.m.bg_cores > 1.0) printf("  (other processes: %.1f cores)", a.m.bg_cores);
    printf("\n");
}

/* Compare the two halves of the last `window` seconds: score drift in %/min, temperature in C/min. */
static int steady_check(double *drift, double *tdrift)
{
    double T = g_rounds[g_nrounds - 1].t, mid = T - O.window / 2, lo = T - O.window;
    double sa = 0, sb = 0, ta = 0, tb = 0;
    int na = 0, nb = 0, nta = 0, ntb = 0;
    for (int i = g_nrounds - 1; i >= 0 && g_rounds[i].t > lo; i--) {
        const round_t *r = &g_rounds[i];
        if (r->t <= mid) {
            sa += r->score; na++;
            if (!isnan(r->temp)) { ta += r->temp; nta++; }
        } else {
            sb += r->score; nb++;
            if (!isnan(r->temp)) { tb += r->temp; ntb++; }
        }
    }
    *drift = *tdrift = NAN;
    if (na < 2 || nb < 2) return 0;
    double per_min = 60.0 / (O.window / 2);
    *drift = (sb / nb - sa / na) / (sa / na) * 100 * per_min;
    int ok = fabs(*drift) <= O.tol_pct;
    if (nta >= 2 && ntb >= 2) {
        *tdrift = (tb / ntb - ta / nta) * per_min;
        ok = ok && fabs(*tdrift) <= O.tol_temp;
    }
    return ok;
}

static void phase_sustained(pool_t *p, FILE *csv)
{
    double ref = R_burst.valid ? R_burst.score : 0;
    phase_title("Multi-core sustained", "%d threads, rounds of %d x %.1f s, until steady state (max %.0f min)",
                p->n, K_COUNT, O.round_dur, O.max_time / 60);
    printf("  steady state = score drift < %.1f %%/min and temperature drift < %.1f C/min over %.0f s,\n"
           "  then measured for %.0f s. Ctrl-C stops early and prints what has been measured.\n",
           O.tol_pct, O.tol_temp, O.window, O.measure);
    print_round_header(ref);
    if (csv) {
        fprintf(csv, "t_s,score,pct_burst");
        for (int k = 0; k < K_COUNT; k++) fprintf(csv, ",%s_%s", kernel_info[k].name, kernel_info[k].unit);
        fprintf(csv, ",p_ghz,e_ghz,p_busy,e_busy,cpu_w,p_w,e_w,temp_c,fan_rpm,pressure,bg_cores,measuring\n");
    }

    enum { WARMING, MEASURING } state = WARMING;
    double t_meas = 0, next_print = O.print_every;
    int printed = 0;
    uint64_t t0 = now_ns();
    for (;;) {
        round_t R;
        memset(&R, 0, sizeof R);
        counters_t ca, cb;
        counters_sample(&ca);
        double tsum = 0;
        int tn = 0, k;
        R.pressure = -1;
        for (k = 0; k < K_COUNT && !g_interrupted; k++) {
            meas_t r;
            measure(p, k, O.round_warm, O.round_dur, &r);
            R.rate[k] = r.rate;
            if (!isnan(r.temp)) { tsum += r.temp; tn++; }
            int pr = thermal_pressure();
            if (pr > R.pressure) R.pressure = pr;
        }
        if (k < K_COUNT) break; /* interrupted mid-round */
        counters_sample(&cb);
        counters_diff(&ca, &cb, &R.m);
        R.t = (double)(cb.t_ns - t0) * 1e-9;
        R.score = score_of(R.rate);
        R.temp = tn ? tsum / tn : NAN;
        R.fan = sensors_fan_rpm();
        R.measuring = state == MEASURING;
        if (g_nrounds == g_cap_rounds) {
            g_cap_rounds = g_cap_rounds ? 2 * g_cap_rounds : 256;
            g_rounds = realloc(g_rounds, (size_t)g_cap_rounds * sizeof *g_rounds);
        }
        g_rounds[g_nrounds++] = R;
        if (ref <= 0) ref = R.score; /* no burst phase: first round is the reference */

        if (csv) {
            fprintf(csv, "%.2f,%.1f,%.2f", R.t, R.score, 100 * R.score / ref);
            for (int j = 0; j < K_COUNT; j++) fprintf(csv, ",%.4g", R.rate[j]);
            fprintf(csv, ",%.3f,%.3f,%.2f,%.2f,%.2f,%.2f,%.2f,%.1f,%.0f,%d,%.2f,%d\n", R.m.p_ghz, R.m.e_ghz,
                    R.m.p_busy, R.m.e_busy, R.m.watts, R.m.p_watts, R.m.e_watts, R.temp, R.fan, R.pressure,
                    R.m.bg_cores, R.measuring);
            fflush(csv);
        }

        int state_changed = 0;
        if (state == WARMING) {
            double drift = NAN, tdrift = NAN;
            int steady = R.t >= O.window && steady_check(&drift, &tdrift);
            if (steady || R.t >= O.max_time - O.measure) {
                state = MEASURING;
                t_meas = R.t;
                state_changed = 1;
                print_rounds(printed, g_nrounds, ref);
                printed = g_nrounds;
                char t[16], tdr[48] = "temperature n/a";
                if (!isnan(tdrift)) snprintf(tdr, sizeof tdr, "temperature %+.2f C/min", tdrift);
                if (steady) {
                    g_steady_at = R.t;
                    printf("  -- steady state at %s (score %+.2f %%/min, %s): measuring for %.0f s\n",
                           fmt_time(R.t, t), drift, tdr, O.measure);
                } else {
                    g_not_steady = 1;
                    printf("  -- time limit: still drifting (score %+.2f %%/min, %s), measuring the last %.0f s\n",
                           drift, tdr, O.measure);
                }
            }
        }
        if (!state_changed && (R.t >= next_print || (state == MEASURING && R.t - t_meas >= O.measure))) {
            print_rounds(printed, g_nrounds, ref);
            printed = g_nrounds;
            next_print = R.t + O.print_every;
        } else if (state_changed) {
            next_print = R.t + O.print_every;
        }
        if (state == MEASURING && R.t - t_meas >= O.measure) break;
        if (g_interrupted) break;
    }
    if (printed < g_nrounds) print_rounds(printed, g_nrounds, ref);

    /* The sustained result is the mean of the rounds measured after steady state; if interrupted
     * before that, the last `measure` seconds are used and the result is flagged. */
    int from = g_nrounds;
    while (from > 0 && g_rounds[from - 1].measuring) from--;
    if (from == g_nrounds) {
        if (g_nrounds == 0) return;
        double tend = g_rounds[g_nrounds - 1].t;
        from = g_nrounds - 1;
        while (from > 0 && g_rounds[from - 1].t > tend - O.measure) from--;
        g_not_steady = 1;
    }
    round_t a;
    rounds_avg(from, g_nrounds, &a);
    memcpy(R_sust.rate, a.rate, sizeof a.rate);
    R_sust.m[0] = a.m;
    R_sust.score = a.score;
    R_sust.bg_max = a.m.bg_cores;
    R_sust.valid = 1;
    {
        /* drift during the measurement itself, as a sanity check */
        int h = from + (g_nrounds - from) / 2;
        if (h > from && h < g_nrounds) {
            round_t x, y;
            rounds_avg(from, h, &x);
            rounds_avg(h, g_nrounds, &y);
            double dt = (y.t - x.t) / 60.0;
            if (dt > 0) g_sust_drift = (y.score - x.score) / x.score * 100 / dt;
        }
    }
    printf("  => multi-core sustained score: %.0f", R_sust.score);
    if (R_burst.valid) printf("  (%.1f%% of burst)", 100 * R_sust.score / R_burst.score);
    if (g_not_steady) printf("  [steady state not reached]");
    printf("\n");
    if (R_burst.valid) {
        printf("  per kernel, sustained vs burst:");
        for (int k = 0; k < K_COUNT; k++)
            printf("  %s %.0f%%", kernel_info[k].name, 100 * R_sust.rate[k] / R_burst.rate[k]);
        printf("\n");
    }
}

/* ------------------------------------------------------------------------------------------ */
/* summary                                                                                     */

static void print_chart(double ref)
{
    enum { W = 64, H = 8 };
    if (g_nrounds < 2) return;
    double col[W];
    int cnt[W];
    memset(col, 0, sizeof col);
    memset(cnt, 0, sizeof cnt);
    double tmax = g_rounds[g_nrounds - 1].t, tmin = g_rounds[0].t;
    int width = g_nrounds < W ? g_nrounds : W;
    for (int i = 0; i < g_nrounds; i++) {
        int x = (int)((g_rounds[i].t - tmin) / (tmax - tmin) * (width - 1) + 0.5);
        col[x] += 100 * g_rounds[i].score / ref;
        cnt[x]++;
    }
    double lo = 1e9, hi = 0;
    for (int x = 0; x < width; x++) {
        col[x] = cnt[x] ? col[x] / cnt[x] : col[x > 0 ? x - 1 : 0];
        if (col[x] < lo) lo = col[x];
        if (col[x] > hi) hi = col[x];
    }
    if (hi < 100) hi = 100;
    lo = floor((lo - 2) / 5) * 5;
    hi = ceil(hi / 5) * 5;
    static const char *blocks[] = { " ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█" };
    printf("\n  sustained score over time, %% of burst:\n");
    for (int row = H - 1; row >= 0; row--) {
        double rlo = lo + (hi - lo) * row / H, rhi = lo + (hi - lo) * (row + 1) / H;
        if (row % 2 == 1) printf("  %4.0f%% ┤", rhi);
        else printf("        │");
        for (int x = 0; x < width; x++) {
            double v = col[x];
            int e = v >= rhi ? 8 : v <= rlo ? 0 : (int)((v - rlo) / (rhi - rlo) * 8);
            printf("%s", blocks[e]);
        }
        printf("\n");
    }
    char a[16], b[16];
    printf("  %4.0f%% └", lo);
    for (int x = 0; x < width; x++) printf("─");
    printf("\n         %-*s%s\n", width - 5, fmt_time(tmin, a), fmt_time(tmax, b));
}

static void summary(void)
{
    char t[16];
    printf("\n");
    rule();
    printf("SUMMARY  %s, %s, %d cores", SI.model_name, SI.cpu_brand, SI.ncpu);
    if (SI.nlevels >= 2) printf(" (%d %s + %d %s)", SI.level_cores[0], SI.level_name[0], SI.level_cores[1],
                                SI.level_name[1]);
    printf("\n");
    rule();
    if (R_st.valid) printf("  Single-core score             %6.0f\n", R_st.score);
    if (R_burst.valid) {
        printf("  Multi-core burst score        %6.0f", R_burst.score);
        if (R_st.valid) printf("   %.2f x single-core", R_burst.score / R_st.score);
        printf("\n");
    }
    if (R_sust.valid) {
        printf("  Multi-core sustained score    %6.0f", R_sust.score);
        if (R_burst.valid) printf("   %.1f%% of burst (%+.1f%%)", 100 * R_sust.score / R_burst.score,
                                  100 * (R_sust.score / R_burst.score - 1));
        printf("\n");
        if (!isnan(g_steady_at)) printf("  Steady state reached after    %6s", fmt_time(g_steady_at, t));
        else printf("  Steady state                  %6s", "no");
        if (!isnan(g_sust_drift)) printf("   (drift while measuring %+.2f %%/min)", g_sust_drift);
        printf("\n");

        /* first time the score went below 97% of the reference and stayed there for two rounds */
        double ref = R_burst.valid ? R_burst.score : g_rounds[0].score;
        for (int i = 0; i + 1 < g_nrounds; i++)
            if (g_rounds[i].score < 0.97 * ref && g_rounds[i + 1].score < 0.97 * ref) {
                printf("  Throttling onset (< 97%%)      %6s   (rounds of %.0f s)\n", fmt_time(g_rounds[i].t, t),
                       K_COUNT * (O.round_dur + O.round_warm));
                break;
            }
        char a[32], b[32], c[32];
        if (R_burst.valid) {
            /* time-weighted average over the burst kernels */
            double bp = 0, be = 0, bw = 0, wp = 0, we = 0, ww = 0;
            for (int k = 0; k < K_COUNT; k++) {
                const metrics_t *m = &R_burst.m[k];
                if (!isnan(m->p_ghz)) { bp += m->p_ghz * m->wall_s; wp += m->wall_s; }
                if (!isnan(m->e_ghz)) { be += m->e_ghz * m->wall_s; we += m->wall_s; }
                if (!isnan(m->watts)) { bw += m->watts * m->wall_s; ww += m->wall_s; }
            }
            printf("  Burst clock and power         %s-cores %s GHz, %s-cores %s GHz, CPU %s W\n", L0,
                   fmt_f(wp > 0 ? bp / wp : NAN, "%*.2f", a, 1), L1, fmt_f(we > 0 ? be / we : NAN, "%*.2f", b, 1),
                   fmt_f(ww > 0 ? bw / ww : NAN, "%*.1f", c, 1));
        }
        const metrics_t *s = &R_sust.m[0];
        printf("  Sustained clock and power     %s-cores %s GHz, %s-cores %s GHz, CPU %s W\n", L0,
               fmt_f(s->p_ghz, "%*.2f", a, 1), L1, fmt_f(s->e_ghz, "%*.2f", b, 1), fmt_f(s->watts, "%*.1f", c, 1));
        print_chart(R_burst.valid ? R_burst.score : g_rounds[0].score);
    }
    double bg = fmax(R_st.bg_max, fmax(R_burst.bg_max, R_sust.bg_max));
    if (bg > 1.0)
        printf("\n  WARNING: other processes used up to %.1f cores during the test; the scores are lower\n"
               "  than they should be. Close other apps / wait for background jobs and run again.\n", bg);
    rule();
}

/* ------------------------------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    parse_args(argc, argv);
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    counters_init();
    sensors_init();
    if (O.list_sensors) {
        sensors_dump(stdout);
        return 0;
    }
    char msg[256];
    kernels_global_init();
    int bad = kernels_selftest(msg, sizeof msg);
    if (O.selftest_only) {
        printf("%s\n", msg);
        return bad;
    }

    sysinfo_get(&SI);
    /* core type labels from the OS names: "S"(uper)/"E"(fficiency), "P"/"E" on M1-M4 */
    snprintf(L0, sizeof L0, "%c", SI.level_name[0][0]);
    L1[0] = 0;
    for (int i = 1; i < SI.nlevels; i++)
        snprintf(L1 + strlen(L1), sizeof L1 - strlen(L1), "%s%c", i > 1 ? "+" : "", SI.level_name[i][0]);
    if (!L1[0]) strcpy(L1, "-");
    snprintf(H0, sizeof H0, "%s GHz", L0);
    snprintf(H1, sizeof H1, "%s GHz", L1);
    W0 = (int)strlen(H0);
    W1 = (int)strlen(H1);
    if (O.threads <= 0) O.threads = SI.ncpu;
    g_nphases = O.single + O.burst + O.sustained;

    rule();
    printf("maccorespeed %s - CPU single-core, multi-core burst and sustained benchmark\n", VERSION);
    rule();
    printf("  Machine   %s [%s]\n", SI.model_name, SI.hw_model);
    printf("  CPU       %s, %d cores", SI.cpu_brand, SI.ncpu);
    for (int i = 0; i < SI.nlevels && SI.nlevels > 1; i++)
        printf("%s%d %s", i ? " + " : " = ", SI.level_cores[i], SI.level_name[i]);
    printf(", %.0f GB RAM\n", SI.mem_gb);
    printf("  macOS     %s\n", SI.os_version);
    printf("  Build     %s\n", BUILD_INFO);
    printf("  Power     %s", SI.power_source);
    if (SI.battery_pct >= 0) printf(", battery %d%%", SI.battery_pct);
    if (SI.low_power_mode >= 0) printf(", Low Power Mode %s", SI.low_power_mode ? "ON" : "off");
    if (SI.power_mode >= 0)
        printf(", power mode %s", SI.power_mode == 1 ? "LOW" : SI.power_mode == 2 ? "HIGH" : "automatic");
    printf("\n");
    printf("  Sensors   CPU temperature = %s; %d fan%s\n", sensors_temp_desc(), sensors_fan_count(),
           sensors_fan_count() == 1 ? "" : "s");
    printf("  Self-test %s\n", msg);
    if (bad) {
        printf("  self-test failed: the compiled kernels produce wrong results, aborting\n");
        return 1;
    }

    counters_t c0, c1;
    metrics_t idle;
    counters_sample(&c0);
    sleep_s(1.0);
    counters_sample(&c1);
    counters_diff(&c0, &c1, &idle);
    g_idle_temp = sensors_cpu_temp();
    int pr = thermal_pressure();
    char tb[32];
    printf("  Idle      CPU %s C, other processes using %.2f cores, thermal pressure %s\n",
           fmt_f(g_idle_temp, "%*.1f", tb, 1), idle.bg_cores, thermal_pressure_name(pr));
    if (idle.bg_cores > 1.0)
        printf("  WARNING   the machine is not idle: close other apps / wait for background jobs.\n");
    if (pr > 0)
        printf("  WARNING   thermal pressure is already %s: the Mac is hot, results will be lower.\n",
               thermal_pressure_name(pr));
    if (SI.low_power_mode == 1 || SI.power_mode == 1)
        printf("  WARNING   Low Power Mode is on: results will be much lower.\n");

    /* keep the Mac awake for the whole run (released automatically at exit) */
    IOPMAssertionID awake;
    IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleSystemSleep, kIOPMAssertionLevelOn,
                                CFSTR("maccorespeed benchmark"), &awake);

    signal(SIGINT, on_sigint);
    FILE *csv = NULL;
    if (O.csv && O.sustained) {
        csv = fopen(O.csv, "w");
        if (!csv) perror(O.csv);
    }

    if (O.single) phase_single();
    if ((O.burst || O.sustained) && !g_interrupted) {
        pool_t *p = pool_create(O.threads);
        if (O.burst) {
            if (O.single) cooldown("burst");
            if (!g_interrupted) phase_burst(p);
        }
        if (O.sustained && !g_interrupted) {
            if (O.single || O.burst) cooldown("sustained");
            if (!g_interrupted) phase_sustained(p, csv);
        }
        pool_destroy(p);
    }
    if (csv) {
        fclose(csv);
        printf("  time series: %s\n", O.csv);
    }
    if (g_interrupted) printf("\n  (interrupted)\n");
    summary();
    return 0;
}
