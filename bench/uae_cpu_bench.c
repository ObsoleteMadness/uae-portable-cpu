/*
 * uae_cpu_bench - CPU core throughput benchmark for regression tracking
 *
 * Runs small 68020 programs in each execution mode and reports the best
 * wall time of several runs:
 *
 *   interpreter  jit_enabled = false
 *   jit          jit_enabled; translated code calls the region handlers
 *   jit-direct   jit_enabled + jit_direct_memory; RAM is accessed inline
 *
 * Every run is checked against a reference computed in C, so the benchmark
 * is also a correctness test: it exits with status 1 when any result is
 * wrong (the bench_smoke CTest runs it with --quick). Timings are only
 * comparable between builds on the same machine; record them with --csv.
 *
 * Usage: uae_cpu_bench [--quick] [--repeat N] [--csv FILE] [workload ...]
 */

#ifndef _WIN32
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <time.h>
#endif

#include "uae_cpu.h"

#define RAM_SIZE 0x100000
#define CODE_ADDR 0x1000
#define STOP_ADDR 0x2000      /* every exception vector: exec return */
#define DATA_ADDR 0x80000     /* 64 KB byte pattern */
#define COPY_ADDR 0xA0000
#define DEVICE_ADDR 0x400000
#define OP_EXEC_RETURN 0x7100 /* MOVEQ with bit 8 set: not a valid encoding */
#define RUN_TIME_LIMIT 120.0  /* seconds; a longer run counts as a failure */

enum { MODE_INTERP, MODE_JIT, MODE_JIT_DIRECT, MODE_COUNT };
static const char *const mode_names[MODE_COUNT] = { "interpreter", "jit", "jit-direct" };

static uae_cpu_t *s_cpu;
static uint8_t *s_ram;
static bool s_done;
static uint32_t s_stop_pc;
static uint32_t s_device_reads;

typedef struct {
    const char *name;
    const char *what;
    const uint16_t *code;
    size_t words;
    uint32_t end_pc;  /* address of the exec-return opcode */
    uint32_t outer;   /* outer passes at full scale (D7 + 1) */
    bool device;      /* maps the counting device at DEVICE_ADDR */
    bool (*check)(uint32_t outer);
} workload_t;

static double now_seconds(void)
{
#ifdef _WIN32
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (double)count.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

static uint32_t reg(uae_reg_t r) { return uae_cpu_get_reg(s_cpu, r); }
static uint8_t pattern(uint32_t i) { return (uint8_t)(i * 131 + 7); }

static void w16(uint32_t addr, uint16_t v)
{
    s_ram[addr] = (uint8_t)(v >> 8);
    s_ram[addr + 1] = (uint8_t)v;
}

static void w32(uint32_t addr, uint32_t v)
{
    w16(addr, (uint16_t)(v >> 16));
    w16(addr + 2, (uint16_t)v);
}

/* ------------------------------------------------------------------------
 * Workloads. Each program counts D7 down with an outer DBF, so the host sets
 * D7 = passes - 1 before running.
 * ---------------------------------------------------------------------- */

/* Register-only arithmetic: core dispatch and flag handling. */
static const uint16_t arith_code[] = {
    0x7000,         /* 1000 MOVEQ #0,D0          */
    0x7200,         /* 1002 MOVEQ #0,D1          */
    0x3C3C, 0xFFFF, /* 1004 outer: MOVE.W #$FFFF,D6 */
    0x5281,         /* 1008 inner: ADDQ.L #1,D1  */
    0xD081,         /* 100A ADD.L D1,D0          */
    0xE398,         /* 100C ROL.L #1,D0          */
    0xB380,         /* 100E EOR.L D1,D0          */
    0x51CE, 0xFFF6, /* 1010 DBF D6,inner         */
    0x51CF, 0xFFEE, /* 1014 DBF D7,outer         */
    OP_EXEC_RETURN  /* 1018                      */
};

static bool check_arith(uint32_t outer)
{
    uint32_t d0 = 0, d1 = 0;
    for (uint32_t n = 0; n < outer; n++) {
        for (uint32_t i = 0; i < 0x10000; i++) {
            d1++;
            d0 += d1;
            d0 = (d0 << 1) | (d0 >> 31);
            d0 ^= d1;
        }
    }
    return reg(UAE_REG_D0) == d0 && reg(UAE_REG_D1) == d1;
}

/* One byte read per five instructions of mixing. */
static const uint16_t bytemix_code[] = {
    0x7000,                 /* 1000 MOVEQ #0,D0            */
    0x7400,                 /* 1002 MOVEQ #0,D2            */
    0x7200,                 /* 1004 MOVEQ #0,D1            */
    0x41F9, 0x0008, 0x0000, /* 1006 outer: LEA $80000,A0   */
    0x263C, 0x0001, 0x0000, /* 100C MOVE.L #$10000,D3      */
    0x1218,                 /* 1012 inner: MOVE.B (A0)+,D1 */
    0xB300,                 /* 1014 EOR.B D1,D0            */
    0xE798,                 /* 1016 ROL.L #3,D0            */
    0xD481,                 /* 1018 ADD.L D1,D2            */
    0x5383,                 /* 101A SUBQ.L #1,D3           */
    0x66F4,                 /* 101C BNE inner              */
    0x51CF, 0xFFE6,         /* 101E DBF D7,outer           */
    OP_EXEC_RETURN          /* 1022                        */
};

static bool check_bytemix(uint32_t outer)
{
    uint32_t d0 = 0, d1 = 0, d2 = 0;
    for (uint32_t n = 0; n < outer; n++) {
        for (uint32_t i = 0; i < 0x10000; i++) {
            d1 = (d1 & ~0xFFu) | pattern(i);
            d0 = (d0 & ~0xFFu) | ((d0 ^ d1) & 0xFF);
            d0 = (d0 << 3) | (d0 >> 29);
            d2 += d1;
        }
    }
    return reg(UAE_REG_D0) == d0 && reg(UAE_REG_D2) == d2;
}

/* Memory-heavy: copy 64 KB as longs, then sum it back as bytes. */
static const uint16_t memcopy_code[] = {
    0x7400,                 /* 1000 MOVEQ #0,D2              */
    0x7200,                 /* 1002 MOVEQ #0,D1              */
    0x41F9, 0x0008, 0x0000, /* 1004 outer: LEA $80000,A0     */
    0x43F9, 0x000A, 0x0000, /* 100A LEA $A0000,A1            */
    0x3C3C, 0x3FFF,         /* 1010 MOVE.W #$3FFF,D6         */
    0x22D8,                 /* 1014 copy: MOVE.L (A0)+,(A1)+ */
    0x51CE, 0xFFFC,         /* 1016 DBF D6,copy              */
    0x43F9, 0x000A, 0x0000, /* 101A LEA $A0000,A1            */
    0x3C3C, 0xFFFF,         /* 1020 MOVE.W #$FFFF,D6         */
    0x1219,                 /* 1024 sum: MOVE.B (A1)+,D1     */
    0xD481,                 /* 1026 ADD.L D1,D2              */
    0x51CE, 0xFFFA,         /* 1028 DBF D6,sum               */
    0x51CF, 0xFFD6,         /* 102C DBF D7,outer             */
    OP_EXEC_RETURN          /* 1030                          */
};

static bool check_memcopy(uint32_t outer)
{
    uint32_t sum = 0;
    for (uint32_t i = 0; i < 0x10000; i++)
        sum += pattern(i);
    return reg(UAE_REG_D2) == sum * outer &&
           memcmp(s_ram + COPY_ADDR, s_ram + DATA_ADDR, 0x10000) == 0;
}

/* Device reads: every access must reach the region handler in every mode. */
static const uint16_t device_code[] = {
    0x7400,                 /* 1000 MOVEQ #0,D2            */
    0x41F9, 0x0040, 0x0000, /* 1002 LEA $400000,A0         */
    0x3C3C, 0xFFFF,         /* 1008 outer: MOVE.W #$FFFF,D6 */
    0x3210,                 /* 100C inner: MOVE.W (A0),D1  */
    0xD441,                 /* 100E ADD.W D1,D2            */
    0x51CE, 0xFFFA,         /* 1010 DBF D6,inner           */
    0x51CF, 0xFFF2,         /* 1014 DBF D7,outer           */
    OP_EXEC_RETURN          /* 1018                        */
};

static bool check_device(uint32_t outer)
{
    uint32_t reads = outer * 0x10000u;
    uint16_t d2 = 0;
    for (uint32_t k = 0; k < reads; k++)
        d2 = (uint16_t)(d2 + (uint16_t)(k * 3 + 1));
    return s_device_reads == reads && (reg(UAE_REG_D2) & 0xFFFF) == d2;
}

#define WORDS(a) (sizeof(a) / sizeof((a)[0]))

static const workload_t workloads[] = {
    { "arith",   "register arithmetic, no memory access",  arith_code,   WORDS(arith_code),   0x1018, 250, false, check_arith },
    { "bytemix", "byte reads mixed into a checksum",       bytemix_code, WORDS(bytemix_code), 0x1022, 200, false, check_bytemix },
    { "memcopy", "64 KB long copy plus byte sum",          memcopy_code, WORDS(memcopy_code), 0x1030, 350, false, check_memcopy },
    { "device",  "word reads from a custom-mapped device", device_code,  WORDS(device_code),  0x1018, 100, true,  check_device },
};
#define WORKLOAD_COUNT (sizeof(workloads) / sizeof(workloads[0]))

/* ------------------------------------------------------------------------
 * Host side
 * ---------------------------------------------------------------------- */

static int on_illegal(void *ud, uint16_t opcode, uint32_t pc)
{
    (void)ud;
    if (opcode != OP_EXEC_RETURN)
        return 0;
    s_done = true;
    s_stop_pc = pc;
    uae_cpu_end_timeslice(s_cpu);
    return 1;
}

static uint8_t dev_r8(void *ud, uint32_t a) { (void)ud; (void)a; return 0; }
static uint16_t dev_r16(void *ud, uint32_t a)
{
    (void)ud;
    (void)a;
    return (uint16_t)(s_device_reads++ * 3 + 1);
}
static uint32_t dev_r32(void *ud, uint32_t a) { (void)ud; (void)a; return 0; }
static void dev_w8(void *ud, uint32_t a, uint8_t v) { (void)ud; (void)a; (void)v; }
static void dev_w16(void *ud, uint32_t a, uint16_t v) { (void)ud; (void)a; (void)v; }
static void dev_w32(void *ud, uint32_t a, uint32_t v) { (void)ud; (void)a; (void)v; }

/*
 * Guest RAM at address 0, which is also the JIT memory base. Direct JIT
 * access may reach base + any 32-bit address, so where possible the whole
 * guest space is reserved without access and only the RAM is committed.
 */
static uint8_t *alloc_guest_ram(void)
{
#if !defined(_WIN32) && UINTPTR_MAX > 0xFFFFFFFFu
    size_t span = (size_t)1 << 32;
    int flags = MAP_PRIVATE | MAP_ANON;
#ifdef MAP_NORESERVE
    flags |= MAP_NORESERVE;
#endif
    void *p = mmap(NULL, span, PROT_NONE, flags, -1, 0);
    if (p != MAP_FAILED) {
        if (mprotect(p, RAM_SIZE, PROT_READ | PROT_WRITE) == 0)
            return (uint8_t *)p;
        munmap(p, span);
    }
#endif
    return (uint8_t *)calloc(1, RAM_SIZE);
}

/*
 * Runs one workload once.
 *
 * Arguments:
 *   w: Workload.
 *   mode: MODE_*.
 *   outer: Outer passes.
 *   ok: Receives whether the run finished at the expected PC with the
 *       reference result.
 *   code_size: Receives the bytes of translated code afterwards.
 *
 * Returns:
 *   Wall time of the execute loop in seconds.
 */
static double run_once(const workload_t *w, int mode, uint32_t outer, bool *ok, uint32_t *code_size)
{
    uae_cpu_config_t cfg;
    uae_cpu_host_hooks_t hooks;
    double t0, elapsed;

    memset(&cfg, 0, sizeof(cfg));
    cfg.cpu_type = UAE_CPU_TYPE_68020;
    cfg.fpu_type = UAE_FPU_NONE;
    cfg.fpu_softfloat = true;
    cfg.jit_enabled = mode != MODE_INTERP;
    cfg.jit_direct_memory = mode == MODE_JIT_DIRECT;
    uae_cpu_set_config(s_cpu, &cfg);

    memset(s_ram, 0, RAM_SIZE);
    uae_cpu_map_memory(s_cpu, 0, RAM_SIZE, s_ram, UAE_MEM_RAM | UAE_MEM_CACHEABLE | UAE_MEM_JIT_DIRECT);
    uae_cpu_set_jit_memory_base(s_cpu, s_ram);
    if (w->device)
        uae_cpu_map_custom(s_cpu, DEVICE_ADDR, 0x10000, dev_r8, dev_r16, dev_r32,
                           dev_w8, dev_w16, dev_w32, NULL);

    w32(0x0, 0x70000);
    w32(0x4, CODE_ADDR);
    for (uint32_t v = 2; v < 256; v++)
        w32(v * 4, STOP_ADDR);
    w16(STOP_ADDR, OP_EXEC_RETURN);
    for (size_t i = 0; i < w->words; i++)
        w16(CODE_ADDR + (uint32_t)(i * 2), w->code[i]);
    for (uint32_t i = 0; i < 0x10000; i++)
        s_ram[DATA_ADDR + i] = pattern(i);

    memset(&hooks, 0, sizeof(hooks));
    hooks.illegal = on_illegal;
    uae_cpu_set_host_hooks(s_cpu, &hooks);
    uae_cpu_reset(s_cpu);
    uae_cpu_set_reg(s_cpu, UAE_REG_D7, outer - 1);

    s_done = false;
    s_stop_pc = 0;
    s_device_reads = 0;
    t0 = now_seconds();
    while (!s_done && now_seconds() - t0 < RUN_TIME_LIMIT)
        uae_cpu_execute(s_cpu, 10000000);
    elapsed = now_seconds() - t0;

    *ok = s_done && s_stop_pc == w->end_pc && w->check(outer);
    *code_size = uae_cpu_get_jit_code_size(s_cpu);

    if (w->device)
        uae_cpu_unmap_memory(s_cpu, DEVICE_ADDR, 0x10000);
    uae_cpu_unmap_memory(s_cpu, 0, RAM_SIZE);
    return elapsed;
}

static void usage(const char *argv0)
{
    printf("Usage: %s [--quick] [--repeat N] [--csv FILE] [workload ...]\n\n", argv0);
    printf("  --quick     1/25 of the full workload size (correctness smoke test)\n");
    printf("  --repeat N  runs per workload and mode; the best time is reported (default 3)\n");
    printf("  --csv FILE  also write workload,mode,passes,seconds,speedup,check rows to FILE\n\n");
    printf("Workloads:\n");
    for (size_t i = 0; i < WORKLOAD_COUNT; i++)
        printf("  %-8s %s\n", workloads[i].name, workloads[i].what);
}

int main(int argc, char **argv)
{
    bool quick = false;
    int repeat = 3;
    const char *csv_path = NULL;
    bool selected[WORKLOAD_COUNT];
    bool any_selected = false;
    bool have_jit;
    int failures = 0;
    FILE *csv = NULL;

    memset(selected, 0, sizeof(selected));
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--quick") == 0) {
            quick = true;
        } else if (strcmp(argv[i], "--repeat") == 0 && i + 1 < argc) {
            repeat = atoi(argv[++i]);
            if (repeat < 1)
                repeat = 1;
        } else if (strcmp(argv[i], "--csv") == 0 && i + 1 < argc) {
            csv_path = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            size_t w;
            for (w = 0; w < WORKLOAD_COUNT; w++) {
                if (strcmp(argv[i], workloads[w].name) == 0)
                    break;
            }
            if (w == WORKLOAD_COUNT) {
                fprintf(stderr, "unknown argument or workload: %s\n\n", argv[i]);
                usage(argv[0]);
                return 2;
            }
            selected[w] = true;
            any_selected = true;
        }
    }

    s_ram = alloc_guest_ram();
    if (!s_ram) {
        fprintf(stderr, "cannot allocate guest RAM\n");
        return 2;
    }
    if (csv_path) {
        csv = fopen(csv_path, "w");
        if (!csv) {
            fprintf(stderr, "cannot write %s\n", csv_path);
            return 2;
        }
        fprintf(csv, "workload,mode,passes,seconds,speedup,check\n");
    }

    uae_cpu_global_init();
    s_cpu = uae_cpu_create(NULL);

    /* Probe: a JIT build translates the arithmetic loop. */
    {
        bool ok;
        uint32_t code_size;
        run_once(&workloads[0], MODE_JIT, 1, &ok, &code_size);
        have_jit = code_size > 0;
    }

    printf("uae_cpu_bench: 68020, best of %d run%s, %s size%s\n", repeat, repeat == 1 ? "" : "s",
           quick ? "quick" : "full", have_jit ? "" : ", no JIT in this build");
    printf("%-8s %-12s %7s %10s %10s  %s\n", "workload", "mode", "passes", "seconds", "vs interp", "check");

    for (size_t w = 0; w < WORKLOAD_COUNT; w++) {
        const workload_t *wl = &workloads[w];
        uint32_t outer = quick ? (wl->outer / 25 ? wl->outer / 25 : 1) : wl->outer;
        double interp_time = 0.0;

        if (any_selected && !selected[w])
            continue;
        for (int mode = 0; mode < MODE_COUNT; mode++) {
            double best = 0.0;
            bool all_ok = true;

            if (mode != MODE_INTERP && !have_jit)
                continue;
            for (int r = 0; r < repeat; r++) {
                bool ok;
                uint32_t code_size;
                double t = run_once(wl, mode, outer, &ok, &code_size);
                if (!ok)
                    all_ok = false;
                if (r == 0 || t < best)
                    best = t;
            }
            if (mode == MODE_INTERP)
                interp_time = best;
            if (!all_ok)
                failures++;

            printf("%-8s %-12s %7u %8.3f s %9.2fx  %s\n", wl->name, mode_names[mode], outer, best,
                   best > 0.0 ? interp_time / best : 0.0, all_ok ? "ok" : "WRONG RESULT");
            fflush(stdout);
            if (csv)
                fprintf(csv, "%s,%s,%u,%.6f,%.3f,%s\n", wl->name, mode_names[mode], outer, best,
                        best > 0.0 ? interp_time / best : 0.0, all_ok ? "ok" : "wrong");
        }
    }

    uae_cpu_destroy(s_cpu);
    if (csv)
        fclose(csv);
    if (failures) {
        printf("uae_cpu_bench: %d result(s) did not match the reference\n", failures);
        return 1;
    }
    return 0;
}
