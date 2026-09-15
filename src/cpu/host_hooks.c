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
