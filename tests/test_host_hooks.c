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

static uint8_t s_ram[RAM_SIZE];
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
    cfg.fpu_type = UAE_FPU_NONE;
    cfg.fpu_softfloat = true;
    cfg.unmapped_bus_error = true;
    uae_cpu_set_config(s_cpu, &cfg);

    memset(s_ram, 0, sizeof(s_ram));
    memset(&R, 0, sizeof(R));
    uae_cpu_map_ram(s_cpu, 0, RAM_SIZE, s_ram);

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

int main(void)
{
    uae_cpu_global_init();
    s_cpu = uae_cpu_create(NULL);

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

    uae_cpu_destroy(s_cpu);
    if (s_failures) {
        printf("host hooks: %d check(s) FAILED\n", s_failures);
        return 1;
    }
    printf("host hooks: all checks passed\n");
    return 0;
}
