/*
 * UAE Portable 680x0 CPU Core - Host Hook Dispatch (internal)
 *
 * Emulator-neutral points where an embedding host takes over from the core:
 * host-trap opcodes, Line-A / Line-F interception, TRAP #n, an exception
 * observer, a pulled interrupt level, DBF delay-loop collapsing and
 * instruction-aborting bus errors. The public contract is in uae_cpu.h and
 * HOST_HOOKS.md; this header is what the core itself calls.
 */

#ifndef UAE_HOST_HOOKS_H
#define UAE_HOST_HOOKS_H

#include <stdint.h>
#include <stdbool.h>
#include "uae_cpu.h"

/* The Musashi-compatible m68k_* API can be left out (CMake UAE_CPU_MUSASHI_API=OFF). */
#if !defined(UAE_CPU_MUSASHI_API) || UAE_CPU_MUSASHI_API
#define UAE_CPU_HAS_MUSASHI_API 1
#else
#define UAE_CPU_HAS_MUSASHI_API 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Hooks installed by uae_cpu_set_host_hooks() (all NULL by default). */
extern uae_cpu_host_hooks_t g_host_hooks;

/* Number of uae_host_run()/uae_host_step() calls currently on the stack. */
extern int g_execute_depth;

/* When true, access to an unmapped 64K bank raises a bus error. */
extern bool g_unmapped_bus_error;

/*
 * Set by a core path that has already offered a Line-F word to the host
 * (FPU or MMU emulation) just before it falls back to op_illg(), so the
 * fline hook is not called twice for the same instruction.
 */
extern int g_host_fline_consulted;

/* Replaces the installed host hooks. NULL clears every hook. */
void uae_host_set_hooks(const uae_cpu_host_hooks_t *hooks);

/*
 * Adds [first, last] to the host-reserved opcode ranges and patches the
 * live dispatch table.
 *
 * Returns:
 *   0 on success, -1 if the range is inverted or the range table is full.
 */
int uae_host_reserve_opcodes(uint16_t first, uint16_t last);

/* Drops every reserved range and rebuilds the dispatch table. */
void uae_host_clear_reserved_opcodes(void);

/* Returns non-zero when opcode lies in a host-reserved range. */
int uae_host_opcode_reserved(uint32_t opcode);

/* Routes every reserved opcode to op_illg (newcpu.c; runs on each table build). */
void uae_host_apply_reserved_opcodes(void);

/*
 * Offers an opcode that reached illegal-instruction processing to the
 * illegal, aline or fline hook. Called first thing in op_illg().
 *
 * Returns:
 *   1 if a hook handled it (PC already advanced past the word unless the
 *   hook moved it), 0 to continue with normal exception processing.
 */
int uae_host_dispatch_trap_opcode(uint32_t opcode);

/*
 * Calls the fline hook directly for a core path that knows why the
 * instruction cannot run. Does not touch the PC.
 *
 * Returns:
 *   Non-zero if the hook handled the instruction.
 */
int uae_host_call_fline(uint32_t opcode, uint32_t pc, int reason);

/* Reports an exception to the observer hook before its frame is built. */
void uae_host_note_exception(int nr, uint32_t fault_pc, uint32_t current_pc);

/*
 * Offers a DBF Dn,*-2 loop to the dbf_spin hook (called from generated code).
 *
 * Returns:
 *   1 if the hook collapsed the loop (Dn.W is 0xFFFF and the returned cycles
 *   are credited), 0 to run the loop normally.
 */
int uae_host_dbf_spin(int dreg);

/*
 * Aborts the current instruction with a 680x0 bus error. Only effective
 * inside uae_host_run()/uae_host_step(); outside them it returns.
 */
void uae_host_raise_bus_error(uint32_t addr, bool is_write, int size);

/* Builds the bus error exception after a caught THROW (newcpu.c). */
void uae_host_bus_error_caught(void);

/* Interpreter loop behind uae_cpu_execute() and m68k_execute(). Re-entrant. */
int uae_host_run(int cycles);

/* Single-instruction loop behind uae_cpu_step(). Re-entrant. */
int uae_host_step(void);

/*
 * JIT integration. While uae_host_run() is inside the JIT dispatcher,
 * g_jit_run_active is true and g_jit_run_target is the currcycle value at
 * which the execute call's budget is spent.
 */
extern bool g_jit_run_active;
extern int64_t g_jit_run_target;

/* When true, translation follows the guest CACR cache-enable bit (WinUAE). */
extern bool g_jit_follow_cacr;

/* Applies jit_enabled / jit_cache_size / jit_follow_cacr before the CPU tables are rebuilt. */
void uae_host_configure_jit(bool enabled, uint32_t cache_kb, bool follow_cacr);

/* True when compiled code must call this opcode's C handler (host hooks need it). */
int uae_host_jit_must_interpret(uint32_t opcode);

/* Cycle accounting for do_cycles() while compiled code is running (events.c). */
void uae_host_jit_do_cycles(int cycles);

/* Discards translations overlapping [addr, addr + size). */
void uae_host_invalidate_code(uint32_t addr, uint32_t size);

/* CPU cycles since init, including cycles compiled code has not yet reported. */
uint64_t uae_host_cycles(void);

/* Bytes of host code currently in the translation cache (0 without a JIT). */
uint32_t uae_host_jit_code_size(void);

/* Sets natmem_offset, the flat guest window translated code relies on. */
int uae_host_set_jit_memory_base(uint8_t *base);

/*
 * True when the block at the current PC may be translated: a JIT memory base
 * is set, the PC's bank maps host memory at base + address, and regs.pc_p
 * agrees. Otherwise execute_normal() interprets the block.
 */
int uae_host_jit_pc_translatable(void);

/* Enters the JIT dispatcher until a special condition returns (newcpu.c). */
void uae_host_run_jit(void);

/* uae_mem_flags_t bits of the bank covering addr, 0 when unmapped (memory.c). */
uint32_t memory_host_flags(uint32_t addr);

#ifdef __cplusplus
}
#endif

#endif /* UAE_HOST_HOOKS_H */
