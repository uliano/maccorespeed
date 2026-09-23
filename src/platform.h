/* platform.h - system information, per-process CPU counters and thermal sensors (macOS) */
#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdint.h>
#include <stdio.h>

#define MAX_LEVELS 4

typedef struct {
    char model_name[128];        /* "MacBook Air (15-inch, M5)" */
    char hw_model[64];           /* "Mac17,4" */
    char cpu_brand[128];         /* "Apple M5" */
    char os_version[64];         /* "27.0 (26A428)" */
    int ncpu;
    int nlevels;                 /* core types: 0 = fastest */
    int level_cores[MAX_LEVELS];
    char level_name[MAX_LEVELS][32];
    double mem_gb;
    char power_source[32];       /* "AC Power" / "Battery Power" */
    int battery_pct;             /* -1 if no battery */
    int low_power_mode;          /* -1 unknown, 0 off, 1 on */
    int power_mode;              /* -1 n/a; 0 automatic, 1 low power, 2 high power */
} sysinfo_t;

void sysinfo_get(sysinfo_t *si);

uint64_t now_ns(void);
void sleep_until_ns(uint64_t t);
void sleep_s(double s);

/* Snapshot of this process' CPU counters (proc_pid_rusage) plus system-wide load. */
typedef struct {
    uint64_t t_ns;
    int ok;                      /* rusage sample valid */
    int split;                   /* per core type split + energy available (RUSAGE_INFO_V6) */
    double cpu_s, p_s;           /* CPU time: total and on level-0 cores */
    double cycles, pcycles, instr;
    double energy_j, penergy_j;
    double host_busy, host_total;
} counters_t;

/* Derived metrics over an interval between two snapshots. NAN when unavailable. */
typedef struct {
    double wall_s;
    double p_ghz, e_ghz;         /* effective clock on level-0 cores / on the other cores */
    double p_busy, e_busy;       /* average number of cores of each kind kept busy by us */
    double ipc;
    double watts, p_watts, e_watts; /* CPU energy billed to this process / wall time */
    double bg_cores;             /* CPU used by other processes, in cores */
} metrics_t;

void counters_init(void);
void counters_sample(counters_t *c);
void counters_diff(const counters_t *a, const counters_t *b, metrics_t *m);

/* Thermal sensors: SMC core sensors if present, IOHID die sensors otherwise. */
void sensors_init(void);
int sensors_have_temp(void);
double sensors_cpu_temp(void);   /* hottest CPU sensor in deg C, NAN if unavailable */
const char *sensors_temp_desc(void);
int sensors_fan_count(void);
double sensors_fan_rpm(void);    /* mean actual fan speed, NAN if no fans */
int thermal_pressure(void);      /* 0 nominal .. 4 sleeping, -1 unknown */
const char *thermal_pressure_name(int level);
void sensors_dump(FILE *f);

#endif
