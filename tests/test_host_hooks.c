/*
 * test_host_hooks.c - Host hook contract tests for UAE Portable CPU Core
 *
 * One scenario per hook in HOST_HOOKS.md. Uses only the public context API
 * (uae_cpu.h), so the same file also links against the symbol-isolated
 * library (built with UAE_TEST_ISOLATED alongside isolation_collisions.c).
 *
 * Every exception vector points at 0x2000, which holds the host exec-return
 * trap (0x7100): taking any exception ends the run, and the exception hook
 * records which vector fired.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include "uae_cpu.h"

#define RAM_SIZE 0x100000
#define HANDLER_ADDR 0x2000
#define NESTED_ADDR 0x3000
#define DEVICE_ADDR 0x400000

enum {
    OP_EXEC_RETURN = 0x7100, /* MOVEQ with bit 8 set: not a valid 680x0 encoding */
    OP_SET_D0 = 0x7101,
    OP_NESTED = 0x7102,
    OP_NOP = 0x4E71
};

static uint8_t s_ram_buf[RAM_SIZE];
static uint8_t *s_ram = s_ram_buf;  /* guest RAM at 0, also the JIT memory base */
static bool s_jit_follow_cacr;  /* boot() option for the CACR-gated JIT scenario */
static bool s_jit_direct;       /* UAE_TEST_JIT_DIRECT: jit_direct_memory in a 4 GB window */
static uae_fpu_type_t s_fpu_type = UAE_FPU_NONE;  /* boot() options for the FPU scenarios */
static bool s_fpu_native;       /* fpu_softfloat = false */
static bool s_jit_fpu;          /* jit_fpu (with UAE_TEST_JIT) */
static uae_cpu_t *s_cpu;
static int s_failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        s_failures++; \
    } \
} while (0)

/* Everything the hooks observed during one scenario. */
typedef struct {
    int illegal_calls;
    uint32_t last_illegal_pc;
    int max_depth;
    int aline_calls;
    int fline_calls;
    uae_fline_reason_t last_reason;
    int exceptions[256];
    uint32_t fault_pc[256];
    bool saw_interrupt;
    int dbf_calls;
    int dbf_dreg;
    uint16_t dbf_count;
    uint32_t dbf_credit;
    int irq_level;
    int trap_calls;
    int last_trap;
} record_t;

static record_t R;

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

/* Handles the three host traps and, in the reserved-opcode scenario, NOP. */
static int on_illegal(void *ud, uint16_t opcode, uint32_t pc)
{
    int depth = uae_cpu_execute_depth(s_cpu);
    (void)ud;

    R.illegal_calls++;
    R.last_illegal_pc = pc;
    if (depth > R.max_depth)
        R.max_depth = depth;

    switch (opcode) {
    case OP_EXEC_RETURN:
        uae_cpu_end_timeslice(s_cpu);
        return 1;
    case OP_SET_D0:
        uae_cpu_set_reg(s_cpu, UAE_REG_D0, 0xCAFE);
        return 1;
    case OP_NESTED: {
        /* Run a guest subroutine from inside the hook, then resume the caller. */
        uint32_t saved = uae_cpu_get_pc(s_cpu);
        uae_cpu_set_pc(s_cpu, NESTED_ADDR);
        uae_cpu_execute(s_cpu, 10000);
        uae_cpu_set_pc(s_cpu, saved);
        return 1;
    }
    case OP_NOP:
        return 1;
    default:
        return 0;
    }
}

static int on_aline(void *ud, uint16_t opcode, uint32_t pc)
{
    (void)ud;
    (void)pc;
    R.aline_calls++;
    return opcode == 0xA9F4;
}

static int on_fline(void *ud, uint16_t opcode, uint32_t pc, uae_fline_reason_t reason)
{
    (void)ud;
    (void)opcode;
    R.fline_calls++;
    R.last_reason = reason;
    if (reason == UAE_FLINE_FPU_ABSENT) {
        /* Two-word FPU instruction: the hook must skip the extension word itself. */
        uae_cpu_set_pc(s_cpu, pc + 4);
        return 1;
    }
    /* Single-word MMU instruction: default resume at pc + 2. */
    return reason == UAE_FLINE_MMU_ABSENT;
}

static void on_exception(void *ud, const uae_cpu_exception_info_t *info)
{
    (void)ud;
    if (info->vector >= 0 && info->vector < 256) {
        R.exceptions[info->vector]++;
        R.fault_pc[info->vector] = info->fault_pc;
    }
    if (info->interrupt)
        R.saw_interrupt = true;
}

static int on_get_irq(void *ud)
{
    (void)ud;
    return R.irq_level;
}

static uint32_t on_dbf_spin(void *ud, int dreg, uint16_t count)
{
    (void)ud;
    R.dbf_calls++;
    R.dbf_dreg = dreg;
    R.dbf_count = count;
    return R.dbf_credit;
}

static int on_trap(void *ud, int trap_nr)
{
    (void)ud;
    R.trap_calls++;
    R.last_trap = trap_nr;
    return trap_nr == 3;
}

/* Custom device whose 16-bit reads fault. */
static uint8_t dev_r8(void *ud, uint32_t a) { (void)ud; (void)a; return 0; }
static uint16_t dev_r16(void *ud, uint32_t a)
{
    (void)ud;
    uae_cpu_raise_bus_error(s_cpu, a, false, 2);
    return 0x1234;
}
static uint32_t dev_r32(void *ud, uint32_t a) { (void)ud; (void)a; return 0; }
static void dev_w8(void *ud, uint32_t a, uint8_t v) { (void)ud; (void)a; (void)v; }
static void dev_w16(void *ud, uint32_t a, uint16_t v) { (void)ud; (void)a; (void)v; }
static void dev_w32(void *ud, uint32_t a, uint32_t v) { (void)ud; (void)a; (void)v; }

/*
 * Configures the CPU, maps RAM at 0, loads code at 0x1000, points every
 * vector at the exec-return handler, installs every hook and resets.
 */
static void boot(uae_cpu_type_t cpu_type, const uint16_t *code, size_t words)
{
    uae_cpu_config_t cfg;
    uae_cpu_host_hooks_t hooks;

    memset(&cfg, 0, sizeof(cfg));
    cfg.cpu_type = cpu_type;
    cfg.fpu_type = s_fpu_type;
    cfg.fpu_softfloat = !s_fpu_native;
    cfg.unmapped_bus_error = true;
    /* UAE_TEST_JIT=1 runs every 68020+ scenario through the JIT. */
    if (getenv("UAE_TEST_JIT")) {
        cfg.jit_enabled = true;
        cfg.jit_cache_size = 8192;
        cfg.jit_follow_cacr = s_jit_follow_cacr;
        cfg.jit_direct_memory = s_jit_direct;
        cfg.jit_fpu = s_jit_fpu;
    }
    uae_cpu_set_config(s_cpu, &cfg);

    memset(s_ram, 0, RAM_SIZE);
    memset(&R, 0, sizeof(R));
    if (s_jit_direct)
        uae_cpu_map_memory(s_cpu, 0, RAM_SIZE, s_ram, UAE_MEM_RAM | UAE_MEM_CACHEABLE | UAE_MEM_JIT_DIRECT);
    else
        uae_cpu_map_ram(s_cpu, 0, RAM_SIZE, s_ram);
    /* RAM starts at guest 0, so the buffer itself is the flat JIT window. */
    uae_cpu_set_jit_memory_base(s_cpu, s_ram);

    w32(0x0, 0x80000);
    w32(0x4, 0x1000);
    for (uint32_t v = 2; v < 256; v++)
        w32(v * 4, HANDLER_ADDR);
    w16(HANDLER_ADDR, OP_EXEC_RETURN);
    for (size_t i = 0; i < words; i++)
        w16(0x1000 + (uint32_t)(i * 2), code[i]);

    memset(&hooks, 0, sizeof(hooks));
    hooks.illegal = on_illegal;
    hooks.aline = on_aline;
    hooks.fline = on_fline;
    hooks.exception = on_exception;
    hooks.get_irq = on_get_irq;
    hooks.dbf_spin = on_dbf_spin;
    uae_cpu_set_host_hooks(s_cpu, &hooks);
    uae_cpu_set_trap_hook(s_cpu, on_trap, NULL);

    uae_cpu_reset(s_cpu);
}

static uint32_t reg(uae_reg_t r) { return uae_cpu_get_reg(s_cpu, r); }

static void test_host_trap_opcodes(void)
{
    static const uint16_t code[] = { OP_SET_D0, OP_EXEC_RETURN, OP_NOP };
    printf("[*] illegal hook: host-trap opcodes\n");
    boot(UAE_CPU_TYPE_68020, code, 3);
    uae_cpu_execute(s_cpu, 10000);
    CHECK(R.illegal_calls == 2);
    CHECK(reg(UAE_REG_D0) == 0xCAFE);
    CHECK(R.last_illegal_pc == 0x1002);
    CHECK(reg(UAE_REG_PC) == 0x1004);
    CHECK(R.exceptions[4] == 0);
    CHECK(uae_cpu_execute_depth(s_cpu) == 0);
}

static void test_unhandled_illegal(void)
{
    static const uint16_t code[] = { 0x4AFC };
    printf("[*] illegal hook declines: vector 4 + exception observer\n");
    boot(UAE_CPU_TYPE_68020, code, 1);
    uae_cpu_execute(s_cpu, 10000);
    CHECK(R.exceptions[4] == 1);
    CHECK(R.fault_pc[4] == 0x1000);
    CHECK(reg(UAE_REG_PC) == HANDLER_ADDR + 2);
}

static void test_nested_execute(void)
{
    static const uint16_t code[] = { OP_NESTED, OP_EXEC_RETURN };
    printf("[*] nested uae_cpu_execute from a hook\n");
    boot(UAE_CPU_TYPE_68020, code, 2);
    w16(NESTED_ADDR, 0x7205);          /* MOVEQ #5,D1 */
    w16(NESTED_ADDR + 2, OP_EXEC_RETURN);
    uae_cpu_execute(s_cpu, 10000);
    CHECK(reg(UAE_REG_D1) == 5);
    CHECK(R.max_depth == 2);
    CHECK(reg(UAE_REG_PC) == 0x1004);
    CHECK(uae_cpu_execute_depth(s_cpu) == 0);
}

static void test_aline(void)
{
    static const uint16_t code[] = { 0xA9F4, 0xA000 };
    printf("[*] aline hook\n");
    boot(UAE_CPU_TYPE_68020, code, 2);
    uae_cpu_execute(s_cpu, 10000);
    CHECK(R.aline_calls == 2);
    CHECK(R.exceptions[10] == 1);
    CHECK(R.fault_pc[10] == 0x1002);
}

static void test_fline_fpu_absent(void)
{
    static const uint16_t code[] = { 0xF200, 0x0000, OP_EXEC_RETURN };  /* FMOVE FP0,FP0 */
    printf("[*] fline hook: FPU absent\n");
    boot(UAE_CPU_TYPE_68020, code, 3);
    uae_cpu_execute(s_cpu, 10000);
    CHECK(R.fline_calls == 1);
    CHECK(R.last_reason == UAE_FLINE_FPU_ABSENT);
    CHECK(R.exceptions[11] == 0);
    CHECK(reg(UAE_REG_PC) == 0x1006);
}

static void test_fline_mmu_absent(void)
{
    static const uint16_t code[] = { 0xF588, OP_EXEC_RETURN };  /* PLPAW (A0) */
    printf("[*] fline hook: MMU absent\n");
    boot(UAE_CPU_TYPE_68040, code, 2);
    uae_cpu_execute(s_cpu, 10000);
    CHECK(R.fline_calls == 1);
    CHECK(R.last_reason == UAE_FLINE_MMU_ABSENT);
    CHECK(R.exceptions[11] == 0);
    CHECK(reg(UAE_REG_PC) == 0x1004);
}

static void test_trap_hook(void)
{
    static const uint16_t code[] = { 0x4E43, 0x4E44 };  /* TRAP #3; TRAP #4 */
    printf("[*] trap hook: consume TRAP #3, decline TRAP #4\n");
    boot(UAE_CPU_TYPE_68020, code, 2);
    uae_cpu_execute(s_cpu, 10000);
    CHECK(R.trap_calls == 2);
    CHECK(R.last_trap == 4);
    CHECK(R.exceptions[32 + 3] == 0);
    CHECK(R.exceptions[32 + 4] == 1);
}

static void test_dbf_spin(void)
{
    /* MOVE.W #100,D0; DBF D0,*-2; exec return */
    static const uint16_t code[] = { 0x303C, 0x0064, 0x51C8, 0xFFFE, OP_EXEC_RETURN };
    uint64_t before;

    printf("[*] dbf_spin hook\n");
    boot(UAE_CPU_TYPE_68020, code, 5);
    R.dbf_credit = 100000;
    uae_cpu_set_reg(s_cpu, UAE_REG_D0, 0xABCD0000);
    before = uae_cpu_get_cycles(s_cpu);
    uae_cpu_execute(s_cpu, 1000000);
    CHECK(R.dbf_calls == 1);
    CHECK(R.dbf_dreg == 0);
    CHECK(R.dbf_count == 100);
    CHECK(reg(UAE_REG_D0) == 0xABCDFFFF);
    CHECK(uae_cpu_get_cycles(s_cpu) - before >= 100000);
    CHECK(reg(UAE_REG_PC) == 0x100A);

    printf("[*] dbf_spin hook declines: loop runs normally\n");
    boot(UAE_CPU_TYPE_68020, code, 5);
    R.dbf_credit = 0;
    uae_cpu_set_reg(s_cpu, UAE_REG_D0, 0xABCD0000);
    uae_cpu_execute(s_cpu, 1000000);
    CHECK(R.dbf_calls == 101); /* offered once per iteration while declining */
    CHECK(reg(UAE_REG_D0) == 0xABCDFFFF);
    CHECK(reg(UAE_REG_PC) == 0x100A);
}

static void test_bus_errors(void)
{
    static const uint16_t device[] = { 0x3439, 0x0040, 0x0000, OP_EXEC_RETURN };    /* MOVE.W $400000,D2 */
    static const uint16_t unmapped[] = { 0x3439, 0x0080, 0x0000, OP_EXEC_RETURN };  /* MOVE.W $800000,D2 */

    printf("[*] uae_cpu_raise_bus_error from a memory callback\n");
    boot(UAE_CPU_TYPE_68020, device, 4);
    uae_cpu_map_custom(s_cpu, DEVICE_ADDR, 0x10000, dev_r8, dev_r16, dev_r32,
                       dev_w8, dev_w16, dev_w32, NULL);
    uae_cpu_set_reg(s_cpu, UAE_REG_D2, 0x5555);
    uae_cpu_execute(s_cpu, 10000);
    CHECK(R.exceptions[2] == 1);
    CHECK(R.fault_pc[2] == 0x1000);
    CHECK(reg(UAE_REG_D2) == 0x5555);
    CHECK(reg(UAE_REG_PC) == HANDLER_ADDR + 2);
    uae_cpu_unmap_memory(s_cpu, DEVICE_ADDR, 0x10000);

    printf("[*] unmapped_bus_error config\n");
    boot(UAE_CPU_TYPE_68020, unmapped, 4);
    uae_cpu_set_reg(s_cpu, UAE_REG_D2, 0x5555);
    uae_cpu_execute(s_cpu, 10000);
    CHECK(R.exceptions[2] == 1);
    CHECK(reg(UAE_REG_D2) == 0x5555);
}

static void test_get_irq(void)
{
    static const uint16_t code[] = { 0x60FE };  /* BRA.S * */
    printf("[*] get_irq hook\n");
    boot(UAE_CPU_TYPE_68020, code, 1);
    uae_cpu_set_reg(s_cpu, UAE_REG_SR, 0x2000);
    R.irq_level = 3;
    uae_cpu_signal_irq(s_cpu);
    uae_cpu_execute(s_cpu, 10000);
    CHECK(R.exceptions[24 + 3] == 1);
    CHECK(R.saw_interrupt);
    CHECK(reg(UAE_REG_PC) == HANDLER_ADDR + 2);
}

static void test_reserved_opcodes(void)
{
    static const uint16_t code[] = { OP_NOP, OP_EXEC_RETURN };
    printf("[*] reserved opcode ranges\n");
    boot(UAE_CPU_TYPE_68020, code, 2);
    CHECK(uae_cpu_reserve_opcodes(s_cpu, OP_NOP, OP_NOP) == 0);
    CHECK(uae_cpu_reserve_opcodes(s_cpu, 2, 1) == -1);
    uae_cpu_execute(s_cpu, 10000);
    CHECK(R.illegal_calls == 2);

    uae_cpu_clear_reserved_opcodes(s_cpu);
    boot(UAE_CPU_TYPE_68020, code, 2);
    uae_cpu_execute(s_cpu, 10000);
    CHECK(R.illegal_calls == 1);
}

static void test_mem_flags(void)
{
    static uint8_t fast[0x10000];
    static uint8_t rom[0x10000];
    uint32_t f;

    printf("[*] memory region flags\n");
    boot(UAE_CPU_TYPE_68020, NULL, 0);
    uae_cpu_map_memory(s_cpu, 0x200000, sizeof(fast), fast, UAE_MEM_RAM | UAE_MEM_JIT_DIRECT);
    uae_cpu_map_memory(s_cpu, 0x300000, sizeof(rom), rom,
                       UAE_MEM_ROM | UAE_MEM_JIT_DIRECT | UAE_MEM_JIT_UNSAFE_BURST);
    uae_cpu_map_custom(s_cpu, DEVICE_ADDR, 0x10000, dev_r8, dev_r16, dev_r32,
                       dev_w8, dev_w16, dev_w32, NULL);

    f = uae_cpu_get_mem_flags(s_cpu, 0x200000);
    CHECK((f & UAE_MEM_RAM) && (f & UAE_MEM_JIT_DIRECT));
    f = uae_cpu_get_mem_flags(s_cpu, 0x300000);
    CHECK((f & UAE_MEM_ROM) && (f & UAE_MEM_JIT_UNSAFE_BURST));
    CHECK(uae_cpu_get_mem_flags(s_cpu, DEVICE_ADDR) & UAE_MEM_IO);
    CHECK(uae_cpu_get_mem_flags(s_cpu, 0x900000) == 0);

    uae_cpu_unmap_memory(s_cpu, 0x200000, sizeof(fast));
    uae_cpu_unmap_memory(s_cpu, 0x300000, sizeof(rom));
    uae_cpu_unmap_memory(s_cpu, DEVICE_ADDR, 0x10000);
}

/*
 * Translated memory access and flag-heavy arithmetic, checked against a
 * reference computed in C. Four passes over 16 KB: byte reads, byte / word /
 * long writes, EOR / ROL / ADD / SUBQ / BNE / DBF, then a long read. This
 * would have caught the JIT calling the wrong addrbank accessor.
 */
static void test_translated_memory_loops(void)
{
    static const uint16_t code[] = {
        0x7000, 0x7400,                 /* MOVEQ #0,D0; MOVEQ #0,D2           */
        0x3E3C, 0x0003,                 /* MOVE.W #3,D7          (4 passes)   */
        0x43F9, 0x000A, 0x0000,         /* LEA $A0000,A1                      */
        0x41F9, 0x0008, 0x0000,         /* outer: LEA $80000,A0               */
        0x263C, 0x0000, 0x4000,         /* MOVE.L #$4000,D3                   */
        0x1218,                         /* inner: MOVE.B (A0)+,D1             */
        0xB300,                         /* EOR.B D1,D0                        */
        0xE798,                         /* ROL.L #3,D0                        */
        0xD481,                         /* ADD.L D1,D2                        */
        0x12C1,                         /* MOVE.B D1,(A1)+                    */
        0x5383,                         /* SUBQ.L #1,D3                       */
        0x66F2,                         /* BNE inner                          */
        0x22C0,                         /* MOVE.L D0,(A1)+                    */
        0x32C2,                         /* MOVE.W D2,(A1)+                    */
        0x51CF, 0xFFE0,                 /* DBF D7,outer                       */
        0x2839, 0x000A, 0x0000,         /* MOVE.L $A0000,D4                   */
        OP_EXEC_RETURN
    };
    static uint8_t ref[4 * (0x4000 + 6)];
    uint32_t d0 = 0, d1 = 0, d2 = 0, d4;
    size_t o = 0;

    printf("[*] translated memory loops match the C reference\n");
    for (int pass = 0; pass < 4; pass++) {
        for (uint32_t i = 0; i < 0x4000; i++) {
            uint8_t b = (uint8_t)(i * 131 + 7);
            d1 = (d1 & ~0xFFu) | b;
            d0 = (d0 & ~0xFFu) | ((d0 ^ d1) & 0xFF);
            d0 = (d0 << 3) | (d0 >> 29);
            d2 += d1;
            ref[o++] = b;
        }
        ref[o++] = (uint8_t)(d0 >> 24); ref[o++] = (uint8_t)(d0 >> 16);
        ref[o++] = (uint8_t)(d0 >> 8);  ref[o++] = (uint8_t)d0;
        ref[o++] = (uint8_t)(d2 >> 8);  ref[o++] = (uint8_t)d2;
    }
    d4 = ((uint32_t)ref[0] << 24) | ((uint32_t)ref[1] << 16) | ((uint32_t)ref[2] << 8) | ref[3];

    boot(UAE_CPU_TYPE_68020, code, sizeof(code) / sizeof(code[0]));
    for (uint32_t i = 0; i < 0x4000; i++)
        s_ram[0x80000 + i] = (uint8_t)(i * 131 + 7);
    for (int pass = 0; pass < 50 && reg(UAE_REG_PC) != 0x1038; pass++)
        uae_cpu_execute(s_cpu, 1000000);

    CHECK(reg(UAE_REG_PC) == 0x1038);
    CHECK(reg(UAE_REG_D0) == d0);
    CHECK(reg(UAE_REG_D1) == d1);
    CHECK(reg(UAE_REG_D2) == d2);
    CHECK(reg(UAE_REG_D4) == d4);
    CHECK(memcmp(s_ram + 0xA0000, ref, sizeof(ref)) == 0);
    if (getenv("UAE_TEST_JIT"))
        CHECK(uae_cpu_get_jit_code_size(s_cpu) > 0);
}

/* Device that counts accesses; 16-bit reads return 0x0102. */
static int s_counter_reads;
static uint8_t cnt_r8(void *ud, uint32_t a) { (void)ud; (void)a; s_counter_reads++; return 0x01; }
static uint16_t cnt_r16(void *ud, uint32_t a) { (void)ud; (void)a; s_counter_reads++; return 0x0102; }
static uint32_t cnt_r32(void *ud, uint32_t a) { (void)ud; (void)a; s_counter_reads++; return 0x01020304; }

static void run_until(uint32_t pc)
{
    for (int pass = 0; pass < 50 && reg(UAE_REG_PC) != pc; pass++)
        uae_cpu_execute(s_cpu, 1000000);
}

/*
 * Which memory translated code may touch inline. Runs in every mode; the
 * cases matter under jit_direct_memory:
 *   - a region flagged UAE_MEM_JIT_DIRECT whose host pointer is outside the
 *     JIT window must still be read through its handler;
 *   - a device read by a loop is called once per access;
 *   - x86-64 and Windows ARM64 (the JITs with fault recovery): a translated
 *     read profiled against RAM that later hits the device faults in the
 *     window and is completed through the handler.
 */
static void test_direct_memory(void)
{
    /* LEA $200000,A0; MOVE.L #$4000,D3; MOVEQ #0,D2; MOVEQ #0,D1;
     * loop: MOVE.B (A0)+,D1; ADD.L D1,D2; SUBQ.L #1,D3; BNE loop; exec return */
    static const uint16_t outside[] = {
        0x41F9, 0x0020, 0x0000, 0x263C, 0x0000, 0x4000, 0x7400, 0x7200,
        0x1218, 0xD481, 0x5383, 0x66F8, OP_EXEC_RETURN
    };
    /* LEA $400000,A0; MOVE.L #$4000,D3; MOVEQ #0,D2;
     * loop: MOVE.W (A0),D1; ADD.W D1,D2; SUBQ.L #1,D3; BNE loop; exec return */
    static const uint16_t device[] = {
        0x41F9, 0x0040, 0x0000, 0x263C, 0x0000, 0x4000, 0x7400,
        0x3210, 0xD441, 0x5383, 0x66F8, OP_EXEC_RETURN
    };
    /* MOVEQ #0,D2; MOVEQ #1,D7; LEA $80000,A0;
     * outer: MOVE.L #$4000,D3;
     * loop: MOVE.W (A0),D1; ADD.W D1,D2; SUBQ.L #1,D3; BNE loop;
     * LEA $400000,A0; DBF D7,outer; exec return */
    static const uint16_t ram_then_device[] = {
        0x7400, 0x7E01, 0x41F9, 0x0008, 0x0000, 0x263C, 0x0000, 0x4000,
        0x3210, 0xD441, 0x5383, 0x66F8, 0x41F9, 0x0040, 0x0000, 0x51CF, 0xFFEA,
        OP_EXEC_RETURN
    };
    static uint8_t other[0x10000];
    uint32_t sum = 0;

    printf("[*] JIT-direct region outside the JIT window uses its handler\n");
    for (uint32_t i = 0; i < sizeof(other); i++)
        other[i] = (uint8_t)(i * 37 + 11);
    for (uint32_t i = 0; i < 0x4000; i++)
        sum += other[i];
    boot(UAE_CPU_TYPE_68020, outside, sizeof(outside) / sizeof(outside[0]));
    uae_cpu_map_memory(s_cpu, 0x200000, sizeof(other), other, UAE_MEM_RAM | UAE_MEM_JIT_DIRECT);
    run_until(0x101A);
    CHECK(reg(UAE_REG_PC) == 0x101A);
    CHECK(reg(UAE_REG_D2) == sum);
    uae_cpu_unmap_memory(s_cpu, 0x200000, sizeof(other));

    printf("[*] device reads from a translated loop reach the handler\n");
    boot(UAE_CPU_TYPE_68020, device, sizeof(device) / sizeof(device[0]));
    uae_cpu_map_custom(s_cpu, DEVICE_ADDR, 0x10000, cnt_r8, cnt_r16, cnt_r32,
                       dev_w8, dev_w16, dev_w32, NULL);
    s_counter_reads = 0;
    run_until(0x1018);
    CHECK(reg(UAE_REG_PC) == 0x1018);
    CHECK(s_counter_reads == 0x4000);
    CHECK((reg(UAE_REG_D2) & 0xFFFF) == ((0x4000 * 0x0102) & 0xFFFF));
    uae_cpu_unmap_memory(s_cpu, DEVICE_ADDR, 0x10000);

#if defined(__x86_64__) || defined(_M_X64) || (defined(_WIN32) && defined(_M_ARM64))
    printf("[*] translated RAM read moved onto a device (fault recovery)\n");
    boot(UAE_CPU_TYPE_68020, ram_then_device, sizeof(ram_then_device) / sizeof(ram_then_device[0]));
    uae_cpu_map_custom(s_cpu, DEVICE_ADDR, 0x10000, cnt_r8, cnt_r16, cnt_r32,
                       dev_w8, dev_w16, dev_w32, NULL);
    w16(0x80000, 0x0304);
    s_counter_reads = 0;
    run_until(0x1024);
    CHECK(reg(UAE_REG_PC) == 0x1024);
    CHECK(s_counter_reads == 0x4000);
    CHECK((reg(UAE_REG_D2) & 0xFFFF) == ((0x4000 * 0x0304 + 0x4000 * 0x0102) & 0xFFFF));
    uae_cpu_unmap_memory(s_cpu, DEVICE_ADDR, 0x10000);
#else
    (void)ram_then_device;
#endif
}

/*
 * Direct JIT access goes to base + address for whatever address a translated
 * instruction computes, so the direct-mode run reserves the whole 4 GB guest
 * space without access and commits only the RAM at guest 0.
 */
static uint8_t *reserve_guest_space(void)
{
#if defined(_WIN64)
    void *p = VirtualAlloc(NULL, (SIZE_T)1 << 32, MEM_RESERVE, PAGE_NOACCESS);
    if (!p)
        return NULL;
    if (!VirtualAlloc(p, RAM_SIZE, MEM_COMMIT, PAGE_READWRITE))
        return NULL;
    return (uint8_t *)p;
#elif defined(_WIN32)
    return NULL;
#else
    int flags = MAP_PRIVATE | MAP_ANON;
#ifdef MAP_NORESERVE
    flags |= MAP_NORESERVE;
#endif
    void *p = mmap(NULL, (size_t)1 << 32, PROT_NONE, flags, -1, 0);
    if (p == MAP_FAILED)
        return NULL;
    if (mprotect(p, RAM_SIZE, PROT_READ | PROT_WRITE) != 0)
        return NULL;
    return (uint8_t *)p;
#endif
}

/*
 * fpu_softfloat selects the FPU backend: 1/3 stored as extended keeps the
 * full 64-bit mantissa under SoftFloat and a double's 53 bits natively.
 */
static void test_fpu_backend(void)
{
    /* MOVEA.L #$90000,A0; FMOVE.L #1,FP0; FDIV.L #3,FP0; FMOVE.X FP0,(A0); exec return */
    static const uint16_t code[] = {
        0x207C, 0x0009, 0x0000, 0xF23C, 0x4000, 0x0000, 0x0001,
        0xF23C, 0x4020, 0x0000, 0x0003, 0xF210, 0x6800, OP_EXEC_RETURN
    };
    static const uint8_t softfloat[12] = { 0x3F, 0xFD, 0, 0, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAB };
    static const uint8_t native[12] = { 0x3F, 0xFD, 0, 0, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xA8, 0x00 };

    printf("[*] fpu_softfloat selects SoftFloat or host doubles\n");
    s_fpu_type = UAE_FPU_68040;
    for (int pass = 0; pass < 2; pass++) {
        s_fpu_native = pass == 1;
        boot(UAE_CPU_TYPE_68040, code, sizeof(code) / sizeof(code[0]));
        run_until(0x101C);
        CHECK(reg(UAE_REG_PC) == 0x101C);
        CHECK(memcmp(s_ram + 0x90000, s_fpu_native ? native : softfloat, 12) == 0);
    }
    s_fpu_native = false;
    s_fpu_type = UAE_FPU_NONE;
}

/*
 * FPU loop checked against C doubles (every value is exact in a double, so
 * all backends must agree to the bit): FADD / FMUL / FCMP / FBcc / FSUB,
 * FMOVE between registers, to memory and to integers. Runs on host doubles;
 * under UAE_TEST_JIT it runs with and without jit_fpu. With
 * jit_direct_memory the translated code must differ in size, which shows the
 * FPU instructions were compiled; without it jit_fpu must have no effect.
 * The reference values are doubles held across the execute calls, so on
 * AArch64 they also catch translated code clobbering the host's callee-saved
 * d8-d15.
 */
static uint32_t run_fpu_loop(bool jit_fpu)
{
    static const uint16_t code[] = {
        0xF23C, 0x4000, 0x0000, 0x0000, /* 1000 FMOVE.L #0,FP0              */
        0xF23C, 0x4080, 0x0000, 0x0001, /* 1008 FMOVE.L #1,FP1              */
        0x263C, 0x0000, 0x4000,         /* 1010 MOVE.L #$4000,D3            */
        0x41F9, 0x0008, 0x0000,         /* 1016 LEA $80000,A0               */
        0xF23C, 0x44A2, 0x3F00, 0x0000, /* 101C loop: FADD.S #0.5,FP1       */
        0xF200, 0x0500,                 /* 1024 FMOVE FP1,FP2               */
        0xF23C, 0x4123, 0x0000, 0x0002, /* 1028 FMUL.L #2,FP2               */
        0xF200, 0x0822,                 /* 1030 FADD FP2,FP0                */
        0xF23C, 0x4038, 0x000F, 0x4240, /* 1034 FCMP.L #1000000,FP0         */
        0xF285, 0x000A,                 /* 103C FBOLE skip                  */
        0xF23C, 0x4028, 0x000F, 0x4240, /* 1040 FSUB.L #1000000,FP0         */
        0xF218, 0x7400,                 /* 1048 skip: FMOVE.D FP0,(A0)+     */
        0x5383,                         /* 104C SUBQ.L #1,D3                */
        0x66CC,                         /* 104E BNE loop                    */
        0xF201, 0x6080,                 /* 1050 FMOVE.L FP1,D1              */
        0xF202, 0x6000,                 /* 1054 FMOVE.L FP0,D2              */
        OP_EXEC_RETURN                  /* 1058                             */
    };
    static uint8_t ref[0x4000 * 8];
    double sum = 0.0, x = 1.0;

    for (uint32_t n = 0; n < 0x4000; n++) {
        uint64_t bits;
        x += 0.5;
        sum += x * 2.0;
        if (!(sum <= 1000000.0))
            sum -= 1000000.0;
        memcpy(&bits, &sum, sizeof(bits));
        for (int b = 0; b < 8; b++)
            ref[n * 8 + b] = (uint8_t)(bits >> (56 - 8 * b));
    }

    s_fpu_type = UAE_FPU_68040;
    s_fpu_native = true;
    s_jit_fpu = jit_fpu;
    boot(UAE_CPU_TYPE_68040, code, sizeof(code) / sizeof(code[0]));
    run_until(0x105A);
    CHECK(reg(UAE_REG_PC) == 0x105A);
    CHECK(reg(UAE_REG_D1) == (uint32_t)(int32_t)x);
    CHECK(reg(UAE_REG_D2) == (uint32_t)(int32_t)sum);
    CHECK(memcmp(s_ram + 0x80000, ref, sizeof(ref)) == 0);
    s_jit_fpu = false;
    s_fpu_native = false;
    s_fpu_type = UAE_FPU_NONE;
    return uae_cpu_get_jit_code_size(s_cpu);
}

static void test_fpu_loop(void)
{
    uint32_t without_jit_fpu, with_jit_fpu;

    printf("[*] FPU loop on host doubles matches the C reference\n");
    without_jit_fpu = run_fpu_loop(false);
    if (!getenv("UAE_TEST_JIT"))
        return;
    if (s_jit_direct)
        printf("[*] jit_fpu: the same loop with FPU instructions translated\n");
    else
        printf("[*] jit_fpu without jit_direct_memory changes nothing\n");
    with_jit_fpu = run_fpu_loop(true);
    CHECK(without_jit_fpu > 0);
    CHECK(with_jit_fpu > 0);
    if (s_jit_direct)
        CHECK(with_jit_fpu != without_jit_fpu);
    else
        CHECK(with_jit_fpu == without_jit_fpu);
}

static void test_jit_compiles(void)
{
    /* MOVEQ #0,D0; MOVE.W #999,D1; loop: ADDQ.L #1,D0; DBRA D1,loop; exec return */
    static const uint16_t code[] = { 0x7000, 0x323C, 0x03E7, 0x5280, 0x51C9, 0xFFFC, OP_EXEC_RETURN };
    uae_cpu_host_hooks_t hooks;

    printf("[*] JIT smoke: loop result and translated code\n");
    boot(UAE_CPU_TYPE_68020, code, 7);
    /* Keep only the exec-return trap: without dbf_spin, DBRA compiles natively. */
    memset(&hooks, 0, sizeof(hooks));
    hooks.illegal = on_illegal;
    uae_cpu_set_host_hooks(s_cpu, &hooks);
    for (int pass = 0; pass < 20 && reg(UAE_REG_PC) != 0x100E; pass++)
        uae_cpu_execute(s_cpu, 100000);
    CHECK(reg(UAE_REG_D0) == 1000);
    CHECK((reg(UAE_REG_D1) & 0xFFFF) == 0xFFFF);
    CHECK(reg(UAE_REG_PC) == 0x100E);
    if (getenv("UAE_TEST_JIT"))
        CHECK(uae_cpu_get_jit_code_size(s_cpu) > 0);
    else
        CHECK(uae_cpu_get_jit_code_size(s_cpu) == 0);

    if (!getenv("UAE_TEST_JIT"))
        return;

    printf("[*] JIT smoke: jit_follow_cacr waits for the guest to enable the cache\n");
    s_jit_follow_cacr = true;
    boot(UAE_CPU_TYPE_68020, code, 7);
    s_jit_follow_cacr = false;
    uae_cpu_set_host_hooks(s_cpu, &hooks);
    for (int pass = 0; pass < 20 && reg(UAE_REG_PC) != 0x100E; pass++)
        uae_cpu_execute(s_cpu, 100000);
    CHECK(reg(UAE_REG_D0) == 1000);
    CHECK(uae_cpu_get_jit_code_size(s_cpu) == 0);
}

/*
 * A host that writes guest code reports it with uae_cpu_invalidate_code().
 *
 * The write lands in the middle of a subroutine the loop has already run often
 * enough to be translated, which is the case an invalidation that only matched
 * block start addresses used to miss.
 */
static void test_invalidate_code_range(void)
{
    /* MOVEQ #0,D0; MOVE.W #999,D1; loop: JSR sub; ADD.L D2,D0; DBRA D1,loop */
    static const uint16_t code[] = {
        0x7000, 0x323C, 0x03E7,
        0x4EB9, 0x0000, 0x5000,
        0xD082,
        0x51C9, 0xFFF6,
        OP_EXEC_RETURN
    };
    uae_cpu_host_hooks_t hooks;

    printf("[*] uae_cpu_invalidate_code: a write inside a translated block\n");
    boot(UAE_CPU_TYPE_68020, code, sizeof(code) / sizeof(code[0]));
    memset(&hooks, 0, sizeof(hooks));
    hooks.illegal = on_illegal;
    uae_cpu_set_host_hooks(s_cpu, &hooks);
    /* sub: MOVEQ #0,D3; MOVEQ #1,D2; RTS — the patch target is the second
     * instruction, so the block's start address stays unchanged.
     * 0x5000: clear of HANDLER_ADDR (0x2000) and NESTED_ADDR (0x3000). */
    w16(0x5000, 0x7600);
    w16(0x5002, 0x7401);
    w16(0x5004, 0x4E75);

    run_until(0x1014);
    CHECK(reg(UAE_REG_D0) == 1000);
    if (getenv("UAE_TEST_JIT"))
        CHECK(uae_cpu_get_jit_code_size(s_cpu) > 0);

    /* MOVEQ #1,D2 becomes MOVEQ #2,D2. */
    w16(0x5002, 0x7402);
    uae_cpu_invalidate_code(s_cpu, 0x5002, 2);

    uae_cpu_set_reg(s_cpu, UAE_REG_PC, 0x1000);
    run_until(0x1014);
    CHECK(reg(UAE_REG_D0) == 2000);
}

/*
 * A guest that writes code announces it by flushing its own instruction cache.
 *
 * The 68040 CPUSHA below is the whole notification: the host is never told, so
 * translations survive it unless the core treats the flush as an invalidation.
 * Hosts that report every code write themselves can set
 * jit_ignore_guest_cache_flush.
 */
static void test_guest_cache_flush(void)
{
    static const uint16_t code[] = {
        0x7000, 0x323C, 0x03E7,         /* MOVEQ #0,D0; MOVE.W #999,D1       */
        0x4EB9, 0x0000, 0x5000,         /* loop: JSR sub                     */
        0xD082,                         /* ADD.L D2,D0                       */
        0x51C9, 0xFFF6,                 /* DBRA D1,loop                      */
        0x31FC, 0x7402, 0x5002,         /* MOVE.W #$7402,($2002).W  (patch)  */
        0xF4B8,                         /* CPUSHA IC                         */
        0x7000, 0x323C, 0x0009,         /* MOVEQ #0,D0; MOVE.W #9,D1         */
        0x4EB9, 0x0000, 0x5000,         /* loop2: JSR sub                    */
        0xD082,                         /* ADD.L D2,D0                       */
        0x51C9, 0xFFF6,                 /* DBRA D1,loop2                     */
        OP_EXEC_RETURN
    };
    uae_cpu_host_hooks_t hooks;

    printf("[*] guest instruction-cache flush invalidates translations\n");
    boot(UAE_CPU_TYPE_68040, code, sizeof(code) / sizeof(code[0]));
    memset(&hooks, 0, sizeof(hooks));
    hooks.illegal = on_illegal;
    uae_cpu_set_host_hooks(s_cpu, &hooks);
    w16(0x5000, 0x7600);                /* MOVEQ #0,D3 */
    w16(0x5002, 0x7401);                /* MOVEQ #1,D2 */
    w16(0x5004, 0x4E75);                /* RTS         */

    run_until(0x102E);
    CHECK(reg(UAE_REG_PC) == 0x102E);
    CHECK(reg(UAE_REG_D0) == 20);       /* 10 passes of the patched MOVEQ #2 */
    if (getenv("UAE_TEST_JIT"))
        CHECK(uae_cpu_get_jit_code_size(s_cpu) > 0);
}

/* Writes a big-endian word into a host buffer that is not the JIT window. */
static void poke16(uint8_t *p, uint32_t off, uint16_t v)
{
    p[off] = (uint8_t)(v >> 8);
    p[off + 1] = (uint8_t)v;
}

/*
 * Every jump form, including calls that cross the JIT window boundary.
 *
 * A jump target becomes regs.pc_p, and the 68k PC is recovered from it
 * relative to regs.pc_oldp (m68k_getpc). That arithmetic is only meaningful
 * for a pointer inside the flat JIT window, so jump targets are always
 * window pointers; a target whose bank lives elsewhere is caught by
 * uae_host_jit_pc_translatable() and interpreted a block at a time.
 *
 * The call to the region at 0x200000 is the interesting one: its host buffer
 * is a plain array, so the bank's own pointer is nowhere near the window.
 * Resolving the target through the bank instead would hand the dispatcher a
 * pointer it cannot turn back into a 68k PC.
 */
static void test_jump_targets(void)
{
    static const uint16_t code[] = {
        0x7000,                         /* MOVEQ #0,D0                       */
        0x323C, 0x0063,                 /* MOVE.W #99,D1                     */
        0x4EB9, 0x0000, 0x5000,         /* loop: JSR ($5000).L        +1     */
        0x6100, 0x3FF2,                 /* BSR.W $5000                +1     */
        0x41F9, 0x0000, 0x5000,         /* LEA ($5000).L,A0                  */
        0x4E90,                         /* JSR (A0)                   +1     */
        0x4EB9, 0x0020, 0x0000,         /* JSR ($200000).L            +2     */
        0x4EB9, 0x0000, 0x6000,         /* JSR ($6000).L              +3     */
        0x51C9, 0xFFE0,                 /* DBRA D1,loop                      */
        OP_EXEC_RETURN
    };
    static uint8_t outside[0x10000];
    uae_cpu_host_hooks_t hooks;

    printf("[*] jump targets: every form, including across the JIT window\n");
    boot(UAE_CPU_TYPE_68020, code, sizeof(code) / sizeof(code[0]));
    memset(&hooks, 0, sizeof(hooks));
    hooks.illegal = on_illegal;
    uae_cpu_set_host_hooks(s_cpu, &hooks);

    /* In the window: ADDQ.L #1,D0; RTS */
    w16(0x5000, 0x5280);
    w16(0x5002, 0x4E75);
    /* In the window, and calls out of it: ADDQ.L #1,D0; JSR ($200000).L; RTS */
    w16(0x6000, 0x5280);
    w16(0x6002, 0x4EB9);
    w16(0x6004, 0x0020);
    w16(0x6006, 0x0000);
    w16(0x6008, 0x4E75);
    /* Outside the window: ADDQ.L #2,D0; RTS */
    memset(outside, 0, sizeof(outside));
    poke16(outside, 0, 0x5480);
    poke16(outside, 2, 0x4E75);
    uae_cpu_map_memory(s_cpu, 0x200000, sizeof(outside), outside,
                       UAE_MEM_RAM | UAE_MEM_CACHEABLE | UAE_MEM_JIT_DIRECT);

    run_until(0x102A);
    CHECK(reg(UAE_REG_PC) == 0x102A);
    CHECK(reg(UAE_REG_D0) == 800);      /* 100 passes x (1 + 1 + 1 + 2 + 3) */
    CHECK(reg(UAE_REG_A0) == 0x5000);
    if (getenv("UAE_TEST_JIT"))
        CHECK(uae_cpu_get_jit_code_size(s_cpu) > 0);
    uae_cpu_unmap_memory(s_cpu, 0x200000, sizeof(outside));
}

/*
 * A call into unmapped memory raises a bus error instead of following a
 * pointer nowhere. Under jit_direct_memory the window address for an
 * unmapped bank is reserved but unreadable, so anything that dereferenced
 * the target before deciding it was untranslatable would fault here.
 */
static void test_jump_unmapped(void)
{
    static const uint16_t code[] = {
        0x7000,                         /* MOVEQ #0,D0        */
        0x4EB9, 0x0080, 0x0000,         /* JSR ($800000).L    */
        OP_EXEC_RETURN
    };
    uae_cpu_host_hooks_t hooks;

    printf("[*] a call into unmapped memory raises a bus error\n");
    boot(UAE_CPU_TYPE_68020, code, sizeof(code) / sizeof(code[0]));
    memset(&hooks, 0, sizeof(hooks));
    hooks.illegal = on_illegal;
    hooks.exception = on_exception;
    uae_cpu_set_host_hooks(s_cpu, &hooks);

    uae_cpu_execute(s_cpu, 100000);
    CHECK(R.exceptions[2] == 1);
    CHECK(reg(UAE_REG_PC) == HANDLER_ADDR + 2);
}

int main(void)
{
    /* Unbuffered: a scenario that faults still leaves its name in the log. */
    setvbuf(stdout, NULL, _IONBF, 0);
    uae_cpu_global_init();
    s_cpu = uae_cpu_create(NULL);
    if (getenv("UAE_TEST_JIT") && getenv("UAE_TEST_JIT_DIRECT")) {
        s_ram = reserve_guest_space();
        if (!s_ram) {
            printf("host hooks: cannot reserve the 4 GB guest window\n");
            return 1;
        }
        s_jit_direct = true;
        printf("[*] running with jit_direct_memory\n");
    }

    test_host_trap_opcodes();
    test_unhandled_illegal();
    test_nested_execute();
    test_aline();
    test_fline_fpu_absent();
    test_fline_mmu_absent();
    test_trap_hook();
    test_dbf_spin();
    test_bus_errors();
    test_get_irq();
    test_reserved_opcodes();
    test_mem_flags();
    test_translated_memory_loops();
    test_direct_memory();
    test_fpu_backend();
    test_fpu_loop();
    test_jit_compiles();
    test_invalidate_code_range();
    test_guest_cache_flush();
    test_jump_targets();
    test_jump_unmapped();

    uae_cpu_destroy(s_cpu);
    if (s_failures) {
        printf("host hooks: %d check(s) FAILED\n", s_failures);
        return 1;
    }
    printf("host hooks: all checks passed\n");
    return 0;
}
