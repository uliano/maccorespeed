/* platform.c - system information, per-process CPU counters and thermal sensors (macOS)
 *
 * Everything here works without root:
 *  - clock, cycles, instructions and CPU energy come from proc_pid_rusage(RUSAGE_INFO_V6),
 *    which on Apple Silicon also splits them between the fastest core type and the others;
 *  - temperatures come from the SMC (core sensors) or, as a fallback, from the private
 *    IOHIDEventSystem API (die sensors), the same sources used by tools like Stats/macmon;
 *  - thermal pressure comes from the public notify(3) key used by the OS.
 */
#include "platform.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/sysctl.h>
#include <sys/resource.h>
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <notify.h>
#include <libkern/OSThermalNotification.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/ps/IOPSKeys.h>

#define FOURCC(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

/* ------------------------------------------------------------------------------------------ */
/* time                                                                                        */

uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

void sleep_until_ns(uint64_t t)
{
    for (;;) {
        uint64_t now = now_ns();
        if (now >= t) return;
        uint64_t d = t - now;
        struct timespec ts = { (time_t)(d / 1000000000u), (long)(d % 1000000000u) };
        nanosleep(&ts, NULL);
    }
}

void sleep_s(double s) { sleep_until_ns(now_ns() + (uint64_t)(s * 1e9)); }

/* ------------------------------------------------------------------------------------------ */
/* system information                                                                          */

static void sysctl_str(const char *name, char *buf, size_t len)
{
    size_t l = len;
    if (sysctlbyname(name, buf, &l, NULL, 0) != 0) buf[0] = 0;
    buf[len - 1] = 0;
}

static long long sysctl_int(const char *name, long long def)
{
    int64_t v = 0;
    size_t l = sizeof v;
    if (sysctlbyname(name, &v, &l, NULL, 0) != 0) return def;
    return l == 4 ? (long long)(int32_t)v : (long long)v;
}

static void product_name(char *buf, size_t len)
{
    buf[0] = 0;
    io_registry_entry_t e = IORegistryEntryFromPath(MACH_PORT_NULL, "IODeviceTree:/product");
    if (!e) return;
    CFTypeRef v = IORegistryEntryCreateCFProperty(e, CFSTR("product-name"), kCFAllocatorDefault, 0);
    if (v) {
        if (CFGetTypeID(v) == CFDataGetTypeID()) {
            CFIndex n = CFDataGetLength((CFDataRef)v);
            if (n > (CFIndex)len - 1) n = (CFIndex)len - 1;
            memcpy(buf, CFDataGetBytePtr((CFDataRef)v), (size_t)n);
            buf[n] = 0;
        } else if (CFGetTypeID(v) == CFStringGetTypeID()) {
            CFStringGetCString((CFStringRef)v, buf, (CFIndex)len, kCFStringEncodingUTF8);
        }
        CFRelease(v);
    }
    IOObjectRelease(e);
}

static void power_info(sysinfo_t *si)
{
    strcpy(si->power_source, "unknown");
    si->battery_pct = -1;
    CFTypeRef info = IOPSCopyPowerSourcesInfo();
    if (info) {
        CFStringRef src = IOPSGetProvidingPowerSourceType(info);
        if (src) CFStringGetCString(src, si->power_source, sizeof si->power_source, kCFStringEncodingUTF8);
        CFArrayRef list = IOPSCopyPowerSourcesList(info);
        if (list) {
            for (CFIndex i = 0; i < CFArrayGetCount(list); i++) {
                CFDictionaryRef d = IOPSGetPowerSourceDescription(info, CFArrayGetValueAtIndex(list, i));
                if (!d) continue;
                CFNumberRef cur = CFDictionaryGetValue(d, CFSTR(kIOPSCurrentCapacityKey));
                CFNumberRef max = CFDictionaryGetValue(d, CFSTR(kIOPSMaxCapacityKey));
                int c = 0, m = 0;
                if (cur && max && CFNumberGetValue(cur, kCFNumberIntType, &c) &&
                    CFNumberGetValue(max, kCFNumberIntType, &m) && m > 0)
                    si->battery_pct = (int)lround(100.0 * c / m);
            }
            CFRelease(list);
        }
        CFRelease(info);
    }

    /* Low Power Mode / High Power Mode are only exposed through pmset. */
    si->low_power_mode = -1;
    si->power_mode = -1;
    FILE *p = popen("/usr/bin/pmset -g 2>/dev/null", "r");
    if (p) {
        char line[256], key[64];
        int val;
        while (fgets(line, sizeof line, p)) {
            if (sscanf(line, " %63s %d", key, &val) != 2) continue;
            if (!strcmp(key, "lowpowermode")) si->low_power_mode = val;
            else if (!strcmp(key, "powermode")) si->power_mode = val;
        }
        pclose(p);
    }
}

void sysinfo_get(sysinfo_t *si)
{
    memset(si, 0, sizeof *si);
    product_name(si->model_name, sizeof si->model_name);
    sysctl_str("hw.model", si->hw_model, sizeof si->hw_model);
    sysctl_str("machdep.cpu.brand_string", si->cpu_brand, sizeof si->cpu_brand);
    if (!si->model_name[0]) strcpy(si->model_name, si->hw_model);

    char ver[32] = "", build[32] = "";
    sysctl_str("kern.osproductversion", ver, sizeof ver);
    sysctl_str("kern.osversion", build, sizeof build);
    snprintf(si->os_version, sizeof si->os_version, "%s (%s)", ver, build);

    si->ncpu = (int)sysctl_int("hw.logicalcpu", 1);
    si->mem_gb = (double)sysctl_int("hw.memsize", 0) / (1024.0 * 1024 * 1024);
    si->nlevels = (int)sysctl_int("hw.nperflevels", 1);
    if (si->nlevels < 1) si->nlevels = 1;
    if (si->nlevels > MAX_LEVELS) si->nlevels = MAX_LEVELS;
    for (int i = 0; i < si->nlevels; i++) {
        char name[64];
        snprintf(name, sizeof name, "hw.perflevel%d.logicalcpu", i);
        si->level_cores[i] = (int)sysctl_int(name, 0);
        snprintf(name, sizeof name, "hw.perflevel%d.name", i);
        sysctl_str(name, si->level_name[i], sizeof si->level_name[i]);
        if (!si->level_name[i][0]) snprintf(si->level_name[i], sizeof si->level_name[i], "Level%d", i);
    }
    if (si->nlevels == 1) si->level_cores[0] = si->ncpu;
    power_info(si);
}

/* ------------------------------------------------------------------------------------------ */
/* counters                                                                                    */

static double g_tick_ns = 1.0;              /* mach absolute time unit in ns (125/3 on Apple Silicon) */
static host_t g_host;
static uint32_t g_prev_ticks[512][CPU_STATE_MAX];
static double g_host_busy, g_host_total;
static int g_host_ready;

void counters_init(void)
{
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    g_tick_ns = (double)tb.numer / tb.denom;
    g_host = mach_host_self();
}

/* Accumulate system-wide busy/total ticks (per-CPU 32-bit counters, wrap-safe). */
static void host_update(void)
{
    natural_t ncpu = 0;
    processor_info_array_t info;
    mach_msg_type_number_t cnt;
    if (host_processor_info(g_host, PROCESSOR_CPU_LOAD_INFO, &ncpu, &info, &cnt) != KERN_SUCCESS) return;
    processor_cpu_load_info_t load = (processor_cpu_load_info_t)info;
    if (ncpu > 512) ncpu = 512;
    for (natural_t i = 0; i < ncpu; i++) {
        for (int s = 0; s < CPU_STATE_MAX; s++) {
            uint32_t cur = load[i].cpu_ticks[s];
            uint32_t d = cur - g_prev_ticks[i][s];
            g_prev_ticks[i][s] = cur;
            if (!g_host_ready) continue;
            g_host_total += d;
            if (s != CPU_STATE_IDLE) g_host_busy += d;
        }
    }
    g_host_ready = 1;
    vm_deallocate(mach_task_self(), (vm_address_t)info, cnt * sizeof(integer_t));
}

void counters_sample(counters_t *c)
{
    memset(c, 0, sizeof *c);
    host_update();
    c->host_busy = g_host_busy;
    c->host_total = g_host_total;

    struct rusage_info_v6 ru;
    if (proc_pid_rusage(getpid(), RUSAGE_INFO_V6, (rusage_info_t *)&ru) == 0) {
        c->ok = c->split = 1;
        c->p_s = (double)(ru.ri_user_ptime + ru.ri_system_ptime) * g_tick_ns * 1e-9;
        c->pcycles = (double)ru.ri_pcycles;
        c->energy_j = (double)ru.ri_energy_nj * 1e-9;
        c->penergy_j = (double)ru.ri_penergy_nj * 1e-9;
    } else if (proc_pid_rusage(getpid(), RUSAGE_INFO_V4, (rusage_info_t *)&ru) == 0) {
        c->ok = 1;
    }
    if (c->ok) {
        c->cpu_s = (double)(ru.ri_user_time + ru.ri_system_time) * g_tick_ns * 1e-9;
        c->cycles = (double)ru.ri_cycles;
        c->instr = (double)ru.ri_instructions;
    }
    c->t_ns = now_ns();
}

void counters_diff(const counters_t *a, const counters_t *b, metrics_t *m)
{
    m->wall_s = (double)(b->t_ns - a->t_ns) * 1e-9;
    m->p_ghz = m->e_ghz = m->p_busy = m->e_busy = m->ipc = NAN;
    m->watts = m->p_watts = m->e_watts = m->bg_cores = NAN;
    if (m->wall_s <= 0) return;

    double ncpu_ticks = b->host_total - a->host_total;
    double cpu = 0;
    if (a->ok && b->ok) {
        cpu = b->cpu_s - a->cpu_s;
        double cyc = b->cycles - a->cycles;
        if (cyc > 0) m->ipc = (b->instr - a->instr) / cyc;
        if (a->split && b->split) {
            double pt = b->p_s - a->p_s, et = cpu - pt;
            double pc = b->pcycles - a->pcycles, ec = cyc - pc;
            m->p_busy = pt / m->wall_s;
            m->e_busy = et / m->wall_s;
            if (pt > 1e-3 && pc > 0) m->p_ghz = pc / pt * 1e-9;
            if (et > 1e-3 && ec > 0) m->e_ghz = ec / et * 1e-9;
            m->watts = (b->energy_j - a->energy_j) / m->wall_s;
            m->p_watts = (b->penergy_j - a->penergy_j) / m->wall_s;
            m->e_watts = m->watts - m->p_watts;
        } else {
            m->p_busy = cpu / m->wall_s;
            if (cpu > 1e-3 && cyc > 0) m->p_ghz = cyc / cpu * 1e-9;
        }
    }
    if (ncpu_ticks > 0) {
        /* busy fraction of all CPUs, converted to cores, minus what we used ourselves */
        int n = (int)sysconf(_SC_NPROCESSORS_ONLN);
        double busy = (b->host_busy - a->host_busy) / ncpu_ticks * n;
        double bg = busy - cpu / m->wall_s;
        m->bg_cores = bg > 0 ? bg : 0;
    }
}

/* ------------------------------------------------------------------------------------------ */
/* SMC                                                                                         */

typedef struct { uint8_t major, minor, build, reserved; uint16_t release; } smc_vers_t;
typedef struct { uint16_t version, length; uint32_t cpu_plimit, gpu_plimit, mem_plimit; } smc_plimit_t;
typedef struct { uint32_t data_size, data_type; uint8_t data_attributes; } smc_keyinfo_t;
typedef struct {
    uint32_t key;
    smc_vers_t vers;
    smc_plimit_t plimit;
    smc_keyinfo_t keyinfo;
    uint8_t result, status, data8;
    uint32_t data32;
    uint8_t bytes[32];
} smc_param_t;
_Static_assert(sizeof(smc_param_t) == 80, "SMC parameter block must be 80 bytes");

enum { SMC_KERNEL_INDEX = 2, SMC_CMD_READ_BYTES = 5, SMC_CMD_READ_INDEX = 8, SMC_CMD_READ_KEYINFO = 9 };

typedef struct { uint32_t key, size, type; } smc_key_t;

static io_connect_t g_smc;
#define MAX_TKEYS 96
static smc_key_t g_tkeys[MAX_TKEYS];
static int g_ntkeys;
static smc_key_t g_fkeys[8];
static int g_nfkeys;

static int smc_call(smc_param_t *in, smc_param_t *out)
{
    size_t osz = sizeof *out;
    kern_return_t kr = IOConnectCallStructMethod(g_smc, SMC_KERNEL_INDEX, in, sizeof *in, out, &osz);
    return (kr == KERN_SUCCESS && out->result == 0) ? 0 : -1;
}

static int smc_key_info(uint32_t key, smc_key_t *k)
{
    smc_param_t in, out;
    memset(&in, 0, sizeof in);
    memset(&out, 0, sizeof out);
    in.key = key;
    in.data8 = SMC_CMD_READ_KEYINFO;
    if (smc_call(&in, &out)) return -1;
    k->key = key;
    k->size = out.keyinfo.data_size;
    k->type = out.keyinfo.data_type;
    return 0;
}

static double smc_value(const smc_key_t *k)
{
    smc_param_t in, out;
    memset(&in, 0, sizeof in);
    memset(&out, 0, sizeof out);
    in.key = k->key;
    in.keyinfo.data_size = k->size;
    in.data8 = SMC_CMD_READ_BYTES;
    if (smc_call(&in, &out)) return NAN;
    const uint8_t *b = out.bytes;
    switch (k->type) {
    case FOURCC('f', 'l', 't', ' '): { float f; memcpy(&f, b, 4); return f; }
    case FOURCC('f', 'p', 'e', '2'): return ((b[0] << 8) | b[1]) / 4.0;
    case FOURCC('s', 'p', '7', '8'): return (int16_t)((b[0] << 8) | b[1]) / 256.0;
    case FOURCC('u', 'i', '8', ' '): return b[0];
    case FOURCC('u', 'i', '1', '6'): return (b[0] << 8) | b[1];
    case FOURCC('u', 'i', '3', '2'): return (double)(((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3]);
    }
    return NAN;
}

static void smc_scan(void)
{
    io_service_t svc = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("AppleSMC"));
    if (!svc) return;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &g_smc);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS) { g_smc = 0; return; }

    smc_key_t k;
    if (smc_key_info(FOURCC('#', 'K', 'E', 'Y'), &k)) return;
    double count = smc_value(&k);
    for (uint32_t i = 0; i < (uint32_t)(isnan(count) ? 0 : count) && g_ntkeys < MAX_TKEYS; i++) {
        smc_param_t in, out;
        memset(&in, 0, sizeof in);
        memset(&out, 0, sizeof out);
        in.data8 = SMC_CMD_READ_INDEX;
        in.data32 = i;
        if (smc_call(&in, &out)) continue;
        /* Apple Silicon CPU core sensors: Tp.. (performance), Te.. (efficiency), Tf.. (M3 P-cores) */
        char c0 = (char)(out.key >> 24), c1 = (char)(out.key >> 16);
        if (c0 != 'T' || (c1 != 'p' && c1 != 'e' && c1 != 'f')) continue;
        if (smc_key_info(out.key, &k) || k.type != FOURCC('f', 'l', 't', ' ') || k.size != 4) continue;
        double v = smc_value(&k);
        if (v > 5 && v < 130) g_tkeys[g_ntkeys++] = k;
    }
    if (smc_key_info(FOURCC('F', 'N', 'u', 'm'), &k) == 0) {
        double nf = smc_value(&k);
        for (int f = 0; f < (int)(isnan(nf) ? 0 : nf) && f < 8; f++)
            if (smc_key_info(FOURCC('F', '0' + f, 'A', 'c'), &g_fkeys[g_nfkeys]) == 0) g_nfkeys++;
    }
}

/* ------------------------------------------------------------------------------------------ */
/* IOHID temperature sensors (private API, fallback when SMC core sensors are missing)          */

typedef struct __IOHIDEvent *IOHIDEventRef;
typedef struct __IOHIDServiceClient *IOHIDServiceClientRef;
typedef struct __IOHIDEventSystemClient *IOHIDEventSystemClientRef;
extern IOHIDEventSystemClientRef IOHIDEventSystemClientCreate(CFAllocatorRef allocator);
extern int IOHIDEventSystemClientSetMatching(IOHIDEventSystemClientRef client, CFDictionaryRef match);
extern CFArrayRef IOHIDEventSystemClientCopyServices(IOHIDEventSystemClientRef client);
extern IOHIDEventRef IOHIDServiceClientCopyEvent(IOHIDServiceClientRef, int64_t type, int32_t options, int64_t timestamp);
extern CFTypeRef IOHIDServiceClientCopyProperty(IOHIDServiceClientRef service, CFStringRef property);
extern double IOHIDEventGetFloatValue(IOHIDEventRef event, int32_t field);

#define HID_EVENT_TEMPERATURE 15
#define HID_FIELD_TEMPERATURE (HID_EVENT_TEMPERATURE << 16)

static IOHIDEventSystemClientRef g_hid;
static CFArrayRef g_hid_all;
#define MAX_HID 64
static IOHIDServiceClientRef g_hid_sel[MAX_HID];
static int g_nhid;

static void hid_name(IOHIDServiceClientRef s, char *buf, size_t len)
{
    buf[0] = 0;
    CFTypeRef name = IOHIDServiceClientCopyProperty(s, CFSTR("Product"));
    if (!name) return;
    if (CFGetTypeID(name) == CFStringGetTypeID())
        CFStringGetCString((CFStringRef)name, buf, (CFIndex)len, kCFStringEncodingUTF8);
    CFRelease(name);
}

static double hid_temp(IOHIDServiceClientRef s)
{
    IOHIDEventRef ev = IOHIDServiceClientCopyEvent(s, HID_EVENT_TEMPERATURE, 0, 0);
    if (!ev) return NAN;
    double t = IOHIDEventGetFloatValue(ev, HID_FIELD_TEMPERATURE);
    CFRelease(ev);
    return t;
}

static void hid_scan(void)
{
    int page = 0xff00, usage = 5; /* Apple vendor page, temperature sensor */
    CFNumberRef p = CFNumberCreate(NULL, kCFNumberIntType, &page);
    CFNumberRef u = CFNumberCreate(NULL, kCFNumberIntType, &usage);
    const void *keys[] = { CFSTR("PrimaryUsagePage"), CFSTR("PrimaryUsage") };
    const void *vals[] = { p, u };
    CFDictionaryRef match = CFDictionaryCreate(NULL, keys, vals, 2, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    g_hid = IOHIDEventSystemClientCreate(kCFAllocatorDefault);
    if (g_hid) {
        IOHIDEventSystemClientSetMatching(g_hid, match);
        g_hid_all = IOHIDEventSystemClientCopyServices(g_hid);
    }
    CFRelease(match);
    CFRelease(p);
    CFRelease(u);
    if (!g_hid_all) return;

    /* Prefer CPU cluster sensors (M1/M2: "pACC/eACC MTR Temp Sensor"), else PMU die sensors. */
    const char *patterns[] = { "ACC MTR", "tdie" };
    for (int pass = 0; pass < 2 && g_nhid == 0; pass++) {
        for (CFIndex i = 0; i < CFArrayGetCount(g_hid_all) && g_nhid < MAX_HID; i++) {
            IOHIDServiceClientRef s = (IOHIDServiceClientRef)CFArrayGetValueAtIndex(g_hid_all, i);
            char name[128];
            hid_name(s, name, sizeof name);
            double t = hid_temp(s);
            if (strstr(name, patterns[pass]) && t > 5 && t < 130) g_hid_sel[g_nhid++] = s;
        }
    }
}

/* ------------------------------------------------------------------------------------------ */
/* public sensor API                                                                           */

static char g_temp_desc[96] = "not available";

void sensors_init(void)
{
    smc_scan();
    if (g_ntkeys >= 2) {
        snprintf(g_temp_desc, sizeof g_temp_desc, "hottest of %d SMC CPU core sensors", g_ntkeys);
        return;
    }
    g_ntkeys = 0;
    hid_scan();
    if (g_nhid > 0) snprintf(g_temp_desc, sizeof g_temp_desc, "hottest of %d IOHID die sensors", g_nhid);
}

int sensors_have_temp(void) { return g_ntkeys > 0 || g_nhid > 0; }
const char *sensors_temp_desc(void) { return g_temp_desc; }
int sensors_fan_count(void) { return g_nfkeys; }

double sensors_cpu_temp(void)
{
    double best = NAN;
    for (int i = 0; i < g_ntkeys; i++) {
        double v = smc_value(&g_tkeys[i]);
        if (v > 5 && v < 130 && !(v <= best)) best = v;
    }
    for (int i = 0; i < g_nhid && g_ntkeys == 0; i++) {
        double v = hid_temp(g_hid_sel[i]);
        if (v > 5 && v < 130 && !(v <= best)) best = v;
    }
    return best;
}

double sensors_fan_rpm(void)
{
    double sum = 0;
    int n = 0;
    for (int i = 0; i < g_nfkeys; i++) {
        double v = smc_value(&g_fkeys[i]);
        if (v >= 0 && v < 20000) { sum += v; n++; }
    }
    return n ? sum / n : NAN;
}

int thermal_pressure(void)
{
    static int token = -1;
    if (token == -1 && notify_register_check(kOSThermalNotificationPressureLevelName, &token) != NOTIFY_STATUS_OK)
        token = -2;
    if (token < 0) return -1;
    uint64_t state = 0;
    if (notify_get_state(token, &state) != NOTIFY_STATUS_OK) return -1;
    return (int)state;
}

const char *thermal_pressure_name(int level)
{
    static const char *names[] = { "nominal", "moderate", "heavy", "trapping", "sleeping" };
    return (level >= 0 && level <= 4) ? names[level] : "?";
}

void sensors_dump(FILE *f)
{
    fprintf(f, "CPU temperature source: %s\n", g_temp_desc);
    if (g_ntkeys) {
        fprintf(f, "SMC CPU sensors:");
        for (int i = 0; i < g_ntkeys; i++) {
            uint32_t k = g_tkeys[i].key;
            fprintf(f, "%s %c%c%c%c=%.1f", i % 8 ? "" : "\n ", (char)(k >> 24), (char)(k >> 16), (char)(k >> 8),
                    (char)k, smc_value(&g_tkeys[i]));
        }
        fprintf(f, "\n");
    }
    if (!g_hid_all) hid_scan();
    if (g_hid_all) {
        fprintf(f, "IOHID temperature sensors:");
        for (CFIndex i = 0; i < CFArrayGetCount(g_hid_all); i++) {
            IOHIDServiceClientRef s = (IOHIDServiceClientRef)CFArrayGetValueAtIndex(g_hid_all, i);
            char name[128];
            hid_name(s, name, sizeof name);
            fprintf(f, "\n  %-32s %6.1f", name, hid_temp(s));
        }
        fprintf(f, "\n");
    }
    fprintf(f, "Fans: %d", g_nfkeys);
    if (g_nfkeys) fprintf(f, " (mean %.0f rpm)", sensors_fan_rpm());
    fprintf(f, "\nThermal pressure: %s\n", thermal_pressure_name(thermal_pressure()));
}
