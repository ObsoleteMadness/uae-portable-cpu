/*
 * UAE Portable 680x0 CPU Core - Host Hook Dispatch
 *
 * Hosts that run their own code from inside the guest (native trap tables,
 * paravirtual calls, OS-level HLE, test harnesses) need a few supported
 * places to take control from the CPU core. This file owns that state and
 * the interpreter loop that makes those hooks safe to use:
 *
 *   - The loop is re-entrant: a hook may call uae_cpu_execute() to run guest
 *     code and return to the host (g_execute_depth tracks the nesting).
 *   - The loop runs inside a TRY block, so a memory callback can abort the
 *     current instruction with a real 680x0 bus error.
 *   - An end-of-timeslice request (SPCFLAG_BRK) only unwinds the innermost
 *     loop, because the innermost do_specialties() consumes it.
 */

#include "sysconfig.h"
#include "sysdeps.h"
#include "uae_glue.h"
#include "options_cpu.h"
#include "events.h"
#include "maccess.h"
#include "memory.h"
#include "newcpu.h"
#include "cpu_prefetch.h"
#include "readcpu.h"
#include "mmu_common.h"
#include "host_hooks.h"

#include <limits.h>
#include <string.h>

uae_cpu_host_hooks_t g_host_hooks;
int g_execute_depth = 0;
bool g_unmapped_bus_error = false;
int g_host_fline_consulted = 0;
bool g_jit_run_active = false;
bool g_jit_follow_cacr = false;
bool g_jit_in_fault_recovery = false;
int64_t g_jit_run_target = 0;

#ifdef JIT
extern void compiler_init(void);
extern uae_u32 get_jitted_size(void);
extern void set_cache_state(int enabled);
/* Cache size currently allocated by alloc_cache(), in KB. */
static int s_jit_cache_kb = 0;
/* Whether the JIT tables were built with DBF routed to its C handler. */
static bool s_jit_tables_dbf_hook = false;

/* Rebuilds the CPU and compiler tables after host-visible opcode policy changed. */
static void uae_host_jit_rebuild(void)
{
    if (!currprefs.cachesize)
        return;
    if (flush_icache)
        flush_icache(3);
    init_m68k();
}
#endif

#define UAE_HOST_MAX_RESERVED_RANGES 16

static struct {
    uint16_t first;
    uint16_t last;
} s_reserved[UAE_HOST_MAX_RESERVED_RANGES];
static int s_reserved_count = 0;

/*
 * Replaces the installed hook table.
 *
 * Arguments:
 *   hooks: New hooks, copied by value; NULL clears every hook.
 */
void uae_host_set_hooks(const uae_cpu_host_hooks_t *hooks)
{
    if (hooks)
        g_host_hooks = *hooks;
    else
        memset(&g_host_hooks, 0, sizeof(g_host_hooks));
#ifdef JIT
    /* DBF compiles natively unless the host wants to see delay loops. */
    if (currprefs.cachesize && (g_host_hooks.dbf_spin != NULL) != s_jit_tables_dbf_hook)
        uae_host_jit_rebuild();
#endif
}

/*
 * Reserves an opcode range for the host. Reserved words always go through
 * op_illg(), even when the configured CPU would decode them as a valid
 * instruction, so a host can claim any encoding for its traps.
 *
 * Arguments:
 *   first: First opcode word in the range.
 *   last: Last opcode word in the range (inclusive).
 *
 * Returns:
 *   0 on success, -1 if last < first or the range table is full.
 */
int uae_host_reserve_opcodes(uint16_t first, uint16_t last)
{
    if (last < first || s_reserved_count >= UAE_HOST_MAX_RESERVED_RANGES)
        return -1;
    s_reserved[s_reserved_count].first = first;
    s_reserved[s_reserved_count].last = last;
    s_reserved_count++;
#ifdef JIT
    /* Compiled blocks must also stop compiling these words. */
    if (currprefs.cachesize) {
        uae_host_jit_rebuild();
        return 0;
    }
#endif
    /* Patch the live table now; later table builds re-apply the ranges. */
    uae_host_apply_reserved_opcodes();
    return 0;
}

/*
 * Drops every reserved range. init_m68k() rebuilds the dispatch table from
 * the CPU tables so the formerly reserved words decode normally again.
 */
void uae_host_clear_reserved_opcodes(void)
{
    s_reserved_count = 0;
    init_m68k();
}

/*
 * Tests whether an opcode word is host-reserved.
 *
 * Arguments:
 *   opcode: 16-bit instruction word.
 *
 * Returns:
 *   Non-zero when opcode lies in any reserved range.
 */
int uae_host_opcode_reserved(uint32_t opcode)
{
    for (int i = 0; i < s_reserved_count; i++) {
        if (opcode >= s_reserved[i].first && opcode <= s_reserved[i].last)
            return 1;
    }
    return 0;
}

/*
 * Classifies a Line-F word that reached op_illg() without a more specific
 * caller, so the host can apply a policy per missing coprocessor.
 *
 * Arguments:
 *   opcode: 0xFxxx instruction word.
 *
 * Returns:
 *   A uae_fline_reason_t value.
 */
static int fline_reason_for(uint32_t opcode)
{
    int cpid = (opcode >> 9) & 7;

    if (!currprefs.mmu_model) {
        /* Coprocessor 0 is the 68851/68030 PMMU. */
        if (cpid == 0)
            return UAE_FLINE_MMU_ABSENT;
        /* 68040/68060 PFLUSH / PTEST / PLPA live at 0xF5xx. */
        if (currprefs.cpu_model >= 68040 && (opcode & 0xFF00) == 0xF500)
            return UAE_FLINE_MMU_ABSENT;
    }
    /* Coprocessor 1 is the 68881/68882 (or on-chip) FPU. */
    if (cpid == 1 && currprefs.fpu_model == 0)
        return UAE_FLINE_FPU_ABSENT;
    return UAE_FLINE_UNKNOWN;
}

/*
 * Calls the fline hook with the status register materialised.
 *
 * Arguments:
 *   opcode: 0xFxxx instruction word.
 *   pc: Address of the instruction word.
 *   reason: uae_fline_reason_t explaining why the CPU cannot run it.
 *
 * Returns:
 *   Non-zero if the hook handled the instruction. The PC is left as the hook
 *   set it; callers decide how to resume.
 */
int uae_host_call_fline(uint32_t opcode, uint32_t pc, int reason)
{
    if (!g_host_hooks.fline)
        return 0;
    MakeSR();
    return g_host_hooks.fline(g_host_hooks.userdata, (uint16_t)opcode, pc,
                              (uae_fline_reason_t)reason) != 0;
}

/*
 * Offers an opcode that reached illegal-instruction processing to the host.
 *
 * Line-A words go to aline, Line-F words to fline, everything else to
 * illegal. All three share one resume rule: when a hook handles the word
 * and leaves the PC where it was, execution continues after the 16-bit
 * opcode word; when the hook moved the PC (nested call, multi-word skip),
 * execution continues there.
 *
 * Arguments:
 *   opcode: Instruction word (already byte-order corrected by op_illg_1).
 *
 * Returns:
 *   1 if a hook handled the opcode, 0 to raise the normal exception.
 */
int uae_host_dispatch_trap_opcode(uint32_t opcode)
{
    uaecptr pc = m68k_getpc();
    int handled = 0;

    /* Translated code does not maintain instruction_pc; exceptions raised for
     * this word must report it as the faulting instruction. */
    regs.instruction_pc = pc;
    opcode &= 0xffff;
    switch (opcode & 0xF000) {
    case 0xA000:
        if (g_host_hooks.aline) {
            MakeSR();
            handled = g_host_hooks.aline(g_host_hooks.userdata, (uint16_t)opcode, pc) != 0;
        }
        break;
    case 0xF000:
        /* FPU/MMU emulation already asked the host about this instruction. */
        if (g_host_fline_consulted) {
            g_host_fline_consulted = 0;
            return 0;
        }
        handled = uae_host_call_fline(opcode, pc, fline_reason_for(opcode));
        break;
    default:
        if (g_host_hooks.illegal) {
            MakeSR();
            handled = g_host_hooks.illegal(g_host_hooks.userdata, (uint16_t)opcode, pc) != 0;
        }
        break;
    }

    if (!handled)
        return 0;
    /* Resume after the opcode word unless the hook redirected execution. */
    if (m68k_getpc() == pc)
        m68k_incpc_normal(2);
    return 1;
}

/*
 * Reports an exception to the observer before the core builds its frame.
 *
 * Arguments:
 *   nr: Vector number.
 *   fault_pc: Address of the faulting instruction (current PC for interrupts).
 *   current_pc: PC at dispatch time.
 */
void uae_host_note_exception(int nr, uint32_t fault_pc, uint32_t current_pc)
{
    uae_cpu_exception_info_t info;

    if (!g_host_hooks.exception)
        return;
    MakeSR();
    info.vector = nr;
    info.fault_pc = fault_pc;
    info.current_pc = current_pc;
    info.opcode = (uint16_t)regs.opcode;
    info.sr = (uint16_t)regs.sr;
    /* Vectors 24..31 are the spurious interrupt and the seven autovectors. */
    info.interrupt = nr >= 24 && nr < 32;
    g_host_hooks.exception(g_host_hooks.userdata, &info);
}

/*
 * Lets the host collapse a DBF Dn,*-2 busy-wait. Generated only into the
 * fast (non-prefetch, non-cycle-exact) opcode tables.
 *
 * A calibration loop that decrements Dn.W to -1 has exactly one
 * architectural result: Dn.W = 0xFFFF and fall-through. Completing it in one
 * step is safe; the hook decides how much emulated time it represents.
 *
 * Arguments:
 *   dreg: Data register index (0..7) holding the loop count.
 *
 * Returns:
 *   1 if collapsed, 0 to execute the loop normally.
 */
int uae_host_dbf_spin(int dreg)
{
    uae_u32 d;
    uint32_t credit;

    if (!g_host_hooks.dbf_spin || currprefs.cpu_cycle_exact)
        return 0;

    d = m68k_dreg(regs, dreg);
    credit = g_host_hooks.dbf_spin(g_host_hooks.userdata, dreg, (uint16_t)d);
    if (!credit)
        return 0;

    /* DBF from N runs N+1 decrements and exits with the low word at -1. */
    m68k_dreg(regs, dreg) = (d & ~0xffffu) | 0xffffu;
    /* do_cycles() takes an int in CYCLE_UNIT ticks; clamp to avoid overflow. */
    if (credit > (uint32_t)(INT_MAX / CYCLE_UNIT))
        credit = (uint32_t)(INT_MAX / CYCLE_UNIT);
    do_cycles((int)credit * CYCLE_UNIT);
    return 1;
}

/*
 * Aborts the current instruction with a bus error, using the same path as
 * the core's own hardware bus errors so the frame matches the CPU model.
 *
 * Arguments:
 *   addr: Faulting guest address.
 *   is_write: True for a write access.
 *   size: Access width in bytes (1, 2 or 4).
 */
void uae_host_raise_bus_error(uint32_t addr, bool is_write, int size)
{
    int sz = size >= 4 ? sz_long : (size == 2 ? sz_word : sz_byte);
    /* Inside a signal handler with compiled-code register state: no unwind. */
    if (g_jit_in_fault_recovery)
        return;
    hardware_exception2(addr, 0, !is_write, false, sz);
}

/*
 * Runs instructions until the cycle budget is spent, the CPU stops or halts,
 * or a special condition asks the loop to return (end of timeslice).
 *
 * Arguments:
 *   cycles: Budget in CPU cycles.
 *
 * Returns:
 *   Cycles actually consumed.
 */
int uae_host_run(int cycles)
{
    evt_t start = currcycle;
    evt_t target = start + (evt_t)cycles * CYCLE_UNIT;
    volatile int done = 0;

    g_execute_depth++;
    /* A pulled interrupt level is sampled at least once per call. */
    if (g_host_hooks.get_irq)
        set_special(SPCFLAG_INT);

#ifdef JIT
    /* The outermost execute call runs translated code. Nested calls (from
     * inside a hook) stay on the interpreter: compiled code must not be
     * re-entered from a C handler it called. */
    if (currprefs.cachesize && g_execute_depth == 1 && currprefs.cpu_model >= 68020 &&
        !currprefs.cpu_compatible && !currprefs.mmu_model) {
        evt_t budget = target - currcycle;
        g_jit_run_target = target;
        pissoff_value = pissoff = budget > INT_MAX ? INT_MAX : (int)budget;
        g_jit_run_active = true;
        TRY(prb) {
            uae_host_run_jit();
        } CATCH(prb) {
            uae_host_bus_error_caught();
        } ENDTRY
        /* Credit whatever the last compiled chain consumed. */
        currcycle += pissoff_value - pissoff;
        pissoff_value = pissoff = 0;
        g_jit_run_active = false;
        g_execute_depth--;
        return (int)((currcycle - start) / CYCLE_UNIT);
    }
#endif

    while (!done) {
        TRY(prb) {
            while (currcycle < target && !regs.stopped && !regs.halted) {
                if (regs.spcflags) {
                    if (do_specialties(0))
                        break;
                }

                if (g_instr_hook)
                    g_instr_hook(g_instr_userdata, m68k_getpc());

                uae_u16 opcode;
                if (currprefs.cpu_compatible && currprefs.cpu_model <= 68010)
                    opcode = regs.ir;
                else
                    opcode = x_get_iword(0);
                /* Exception reporting and bus error frames read these. */
                regs.instruction_pc = m68k_getpc();
                regs.opcode = opcode;

                int cyc = (*cpufunctbl[opcode])(opcode) & 0xFFFF;
                if (cyc == 0)
                    cyc = (CurrentInstrCycles > 0 ? CurrentInstrCycles : 4) * (CYCLE_UNIT / 2);
                cyc = adjust_cycles(cyc);
                do_cycles(cyc);
                regs.instruction_cnt++;
            }
            done = 1;
        } CATCH(prb) {
            /* A memory access threw: build the bus error frame and keep going. */
            uae_host_bus_error_caught();
        } ENDTRY
    }

    g_execute_depth--;
    return (int)((currcycle - start) / CYCLE_UNIT);
}

/*
 * Executes at most one instruction (fewer if a special condition exits first).
 *
 * Returns:
 *   Cycles consumed, or 0 when a special condition returned before executing.
 */
int uae_host_step(void)
{
    evt_t start = currcycle;
    volatile int exited = 0;

    g_execute_depth++;
    TRY(prb) {
        if (regs.spcflags && do_specialties(0)) {
            exited = 1;
        } else {
            if (g_instr_hook)
                g_instr_hook(g_instr_userdata, m68k_getpc());

            uae_u16 opcode;
            if (currprefs.cpu_compatible) {
                if (currprefs.cpu_model <= 68010)
                    opcode = regs.ir;
                else
                    opcode = regs.irc;
            } else {
                opcode = get_iword(0);
            }
            regs.opcode = opcode;
            regs.instruction_pc = m68k_getpc();

            int cyc = (*cpufunctbl[opcode])(opcode) & 0xFFFF;
            cyc = adjust_cycles(cyc);
            do_cycles(cyc);
            regs.instruction_cnt++;
        }
    } CATCH(prb) {
        uae_host_bus_error_caught();
    } ENDTRY
    g_execute_depth--;

    if (exited)
        return 0;
    return (int)((currcycle - start) / CYCLE_UNIT);
}

/*
 * Applies the JIT settings from uae_cpu_config_t. Must run before init_m68k()
 * builds the tables: build_comp() needs the compiler initialised and the
 * translation cache allocated.
 *
 * By default compiled code accesses memory through the bank handlers
 * (canbang = false), so it honours custom devices, ROM write protection and
 * bus errors for any memory map. direct_memory turns on canbang with fully
 * trusted access: accesses the profiler saw hitting UAE_MEM_JIT_DIRECT
 * regions inside the JIT window compile to inline host-pointer access
 * (see memory_jit_sync_all), everything else keeps the handler call.
 *
 * WinUAE only translates while the guest has enabled the CPU cache through
 * CACR. That suits an Amiga (Kickstart enables it) but not a generic host,
 * so by default translation is always on and jit_follow_cacr restores the
 * WinUAE behaviour.
 *
 * Arguments:
 *   enabled: uae_cpu_config_t.jit_enabled.
 *   cache_kb: Translation cache size in KB; 0 selects 8192.
 *   follow_cacr: uae_cpu_config_t.jit_follow_cacr.
 *   direct_memory: uae_cpu_config_t.jit_direct_memory.
 *   jit_fpu: uae_cpu_config_t.jit_fpu. FPU instructions are translated only
 *            with an FPU, the host-double backend (currprefs.fpu_mode 0, set
 *            by the caller first) and direct_memory: compiled FPU code works
 *            on regs.fp[].fp, which SoftFloat does not use, and moves
 *            extended/double values through the JIT memory base, as in
 *            WinUAE. Both backends refuse to compile FPU instructions that
 *            profiling saw touch handler-backed memory.
 */
void uae_host_configure_jit(bool enabled, uint32_t cache_kb, bool follow_cacr, bool direct_memory,
                            bool jit_fpu)
{
    g_jit_follow_cacr = follow_cacr;
#ifdef JIT
    int want = 0;

    /* The dispatcher only exists for 68020+ without MMU or prefetch emulation. */
    if (enabled && currprefs.cpu_model >= 68020 && !currprefs.mmu_model && !currprefs.cpu_compatible) {
        want = cache_kb ? (int)cache_kb : 8192;
        if (want < MIN_JIT_CACHE)
            want = MIN_JIT_CACHE;
        if (want > MAX_JIT_CACHE)
            want = MAX_JIT_CACHE;
    }

    compiler_init();
    canbang = direct_memory;
    jit_direct_compatible_memory = direct_memory;

    /* 0 = trust (inline where profiling allows), 1 = always call the handlers. */
    changed_prefs.comptrustbyte = changed_prefs.comptrustword = direct_memory ? 0 : 1;
    changed_prefs.comptrustlong = changed_prefs.comptrustnaddr = direct_memory ? 0 : 1;
    changed_prefs.compnf = true;
    changed_prefs.compfpu = jit_fpu && direct_memory && want && currprefs.fpu_model &&
                            currprefs.fpu_mode == 0;
    changed_prefs.comp_hardflush = false;
    changed_prefs.comp_constjump = true;
    changed_prefs.fpu_strict = currprefs.fpu_strict;
    changed_prefs.cachesize = want;

    /* check_prefs_changed_comp() (re)allocates the cache when the size differs
     * from the current one, so present the previously allocated size. */
    currprefs.cachesize = s_jit_cache_kb;
    check_prefs_changed_comp(false);
    /* check_prefs_changed_comp() derives this from the trust level it had on
     * entry; recompute it for the level just applied. */
    special_mem_default = currprefs.comptrustbyte ? (S_READ | S_WRITE | S_N_ADDR) : 0;
    memory_jit_sync_all();
    s_jit_cache_kb = currprefs.cachesize;
    s_jit_tables_dbf_hook = g_host_hooks.dbf_spin != NULL;
    pissoff_value = pissoff = 0;
    /* Without CACR gating the translator is enabled as soon as there is a cache. */
    if (currprefs.cachesize)
        set_cache_state(follow_cacr ? ((regs.cacr & (currprefs.cpu_model >= 68040 ? 0x8000 : 1)) != 0) : 1);
#else
    (void)enabled;
    (void)cache_kb;
    (void)follow_cacr;
    (void)direct_memory;
    (void)jit_fpu;
    currprefs.cachesize = 0;
#endif
}

/*
 * Decides which opcodes compiled code must hand to the C opcode handlers.
 *
 * Arguments:
 *   opcode: 16-bit opcode word.
 *
 * Returns:
 *   Non-zero for host-reserved opcodes, and for DBF Dn while a dbf_spin
 *   hook is installed (the C handler is where the hook is offered).
 */
int uae_host_jit_must_interpret(uint32_t opcode)
{
#ifdef JIT
    if ((opcode & 0xFFF8) == 0x51C8 && g_host_hooks.dbf_spin) {
        s_jit_tables_dbf_hook = true;
        return 1;
    }
#endif
    return uae_host_opcode_reserved(opcode);
}

/*
 * do_cycles() while the JIT dispatcher is running.
 *
 * Compiled blocks subtract their cycles from countdown (pissoff) instead of
 * calling do_cycles(), and call do_nothing() -> do_cycles(0) when countdown
 * expires or a special flag is set. Fold what they consumed into currcycle,
 * reload the countdown with the remaining budget, and ask the dispatcher to
 * return once the budget is gone.
 *
 * Arguments:
 *   cycles: Interpreter cycles to add (0 from compiled code).
 */
void uae_host_jit_do_cycles(int cycles)
{
#ifdef JIT
    evt_t remaining;

    currcycle += (evt_t)cycles + (pissoff_value - pissoff);
    remaining = g_jit_run_target - currcycle;
    if (remaining <= 0) {
        set_special(SPCFLAG_BRK);
        remaining = 0;
    }
    pissoff_value = pissoff = remaining > INT_MAX ? INT_MAX : (int)remaining;
#else
    currcycle += cycles;
#endif
}

/*
 * Discards translations that may cover modified guest code.
 *
 * Uses the configured flush (lazy by default: affected blocks are
 * checksummed before reuse) and asks compiled code to stop building the
 * current block.
 *
 * Arguments:
 *   addr: Start of the modified range (unused: flushes are cache-wide).
 *   size: Length of the modified range; 0 is a no-op.
 */
void uae_host_invalidate_code(uint32_t addr, uint32_t size)
{
    (void)addr;
#ifdef JIT
    if (size == 0 || !currprefs.cachesize || !flush_icache)
        return;
    flush_icache(3);
    set_special(SPCFLAG_END_COMPILE);
#else
    (void)size;
#endif
}

/* CPU cycles since init, including compiled work not yet folded into currcycle. */
uint64_t uae_host_cycles(void)
{
    evt_t c = currcycle;
#ifdef JIT
    if (g_jit_run_active)
        c += pissoff_value - pissoff;
#endif
    return c > 0 ? (uint64_t)(c / CYCLE_UNIT) : 0;
}

/* Bytes of translated host code in the cache. */
uint32_t uae_host_jit_code_size(void)
{
#ifdef JIT
    return currprefs.cachesize ? (uint32_t)get_jitted_size() : 0;
#else
    return 0;
#endif
}

/*
 * Declares the flat guest window used to translate code (natmem_offset).
 *
 * Arguments:
 *   base: Host address of guest address 0, or NULL to stop translating.
 *
 * Returns:
 *   0 on success, -1 when the library was built without a JIT.
 */
int uae_host_set_jit_memory_base(uint8_t *base)
{
#ifdef JIT
    if (natmem_offset != base) {
        natmem_offset = base;
        /* Which regions may be accessed inline depends on the base. */
        memory_jit_sync_all();
        /* Translations embed host PCs derived from the old base. */
        if (currprefs.cachesize && flush_icache)
            flush_icache(3);
    }
    return 0;
#else
    (void)base;
    return -1;
#endif
}

int uae_host_jit_pc_translatable(void)
{
#ifdef JIT
    uaecptr pc;
    addrbank *bank;

    if (!natmem_offset)
        return 0;
    pc = m68k_getpc();
    bank = &get_mem_bank(pc);
    return bank->baseaddr != NULL &&
           bank->baseaddr - bank->start == natmem_offset &&
           regs.pc_p == natmem_offset + pc;
#else
    return 0;
#endif
}
