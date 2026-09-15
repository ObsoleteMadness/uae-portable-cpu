/*
 * UAE Portable 680x0 CPU Core
 *
 * Public C API Header
 * Copyright (C) 2026 uae-portable-cpu contributors
 * Based on UAE / WinUAE / Hatari CPU emulation core
 */

#ifndef UAE_PORTABLE_CPU_H
#define UAE_PORTABLE_CPU_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CPU Type enumeration */
typedef enum {
    UAE_CPU_TYPE_68000   = 68000,
    UAE_CPU_TYPE_68010   = 68010,
    UAE_CPU_TYPE_68EC020 = 68019,
    UAE_CPU_TYPE_68020   = 68020,
    UAE_CPU_TYPE_68EC030 = 68029,
    UAE_CPU_TYPE_68030   = 68030,
    UAE_CPU_TYPE_68EC040 = 68039,
    UAE_CPU_TYPE_68LC040 = 68041,
    UAE_CPU_TYPE_68040   = 68040,
    UAE_CPU_TYPE_68060   = 68060
} uae_cpu_type_t;

/* FPU Type enumeration */
typedef enum {
    UAE_FPU_NONE   = 0,
    UAE_FPU_68881  = 68881,
    UAE_FPU_68882  = 68882,
    UAE_FPU_68040  = 68040,
    UAE_FPU_68060  = 68060
} uae_fpu_type_t;

/* MMU Type enumeration */
typedef enum {
    UAE_MMU_NONE   = 0,
    UAE_MMU_68851  = 68851,
    UAE_MMU_68030  = 68030,
    UAE_MMU_68040  = 68040,
    UAE_MMU_68060  = 68060
} uae_mmu_type_t;

typedef enum {
    UAE_MEM_RAM       = 0x01,
    UAE_MEM_ROM       = 0x02,
    UAE_MEM_IO        = 0x04,
    UAE_MEM_CACHEABLE = 0x08,
    UAE_MEM_JIT_DIRECT = 0x10,       /* With jit_direct_memory: translated code may access the region inline */
    UAE_MEM_JIT_UNSAFE_BURST = 0x20  /* With jit_direct_memory: conservative mode for maps where a burst
                                      * (MOVEM, MOVE16) can run off a direct region; see README */
} uae_mem_flags_t;

/* Registers enumeration */
typedef enum {
    UAE_REG_D0, UAE_REG_D1, UAE_REG_D2, UAE_REG_D3,
    UAE_REG_D4, UAE_REG_D5, UAE_REG_D6, UAE_REG_D7,
    UAE_REG_A0, UAE_REG_A1, UAE_REG_A2, UAE_REG_A3,
    UAE_REG_A4, UAE_REG_A5, UAE_REG_A6, UAE_REG_A7,
    UAE_REG_PC,
    UAE_REG_SR,
    UAE_REG_SP,
    UAE_REG_USP,
    UAE_REG_ISP,
    UAE_REG_MSP,
    UAE_REG_SFC,
    UAE_REG_DFC,
    UAE_REG_VBR,
    UAE_REG_CACR,
    UAE_REG_CAAR,
    UAE_REG_PCR,
    UAE_REG_TC,
    UAE_REG_ITT0,
    UAE_REG_ITT1,
    UAE_REG_DTT0,
    UAE_REG_DTT1,
    UAE_REG_MMUSR,
    UAE_REG_URP,
    UAE_REG_SRP
} uae_reg_t;

/* 80-bit Extended Precision Floating Point Register */
typedef struct {
    uint16_t exp;
    uint16_t dummy;
    uint32_t m[2];
} uae_fp_reg_t;

/* Configuration structure */
typedef struct {
    uae_cpu_type_t cpu_type;
    uae_fpu_type_t fpu_type;
    uae_mmu_type_t mmu_type;
    bool fpu_softfloat;      /* true = SoftFloat (80-bit extended precision); false = host doubles
                              * (faster, double precision; required for jit_fpu) */
    bool address_space_24;   /* true = 24-bit addressing (e.g. 68000, 68EC020) */
    int timing_mode;         /* 0 = fast/standard, 1 = prefetch, 2 = cycle-exact */
    bool jit_enabled;        /* true = run 68020+ code through the JIT when the library has one */
    uint32_t jit_cache_size; /* JIT code cache size in KB (0 = 8192) */
    bool unmapped_bus_error; /* true = access to an unmapped address raises a bus error */
    bool jit_follow_cacr;    /* true = translate only while the guest has the CPU cache enabled
                              * (CACR, WinUAE behaviour); false = always translate */
    bool jit_direct_memory;  /* true = translated code reads and writes UAE_MEM_JIT_DIRECT regions
                              * inline through the JIT memory base instead of calling the region
                              * handlers; see uae_cpu_set_jit_memory_base() */
    bool jit_fpu;            /* true = also translate FPU instructions (WinUAE "JIT FPU"). Takes
                              * effect only with jit_enabled, jit_direct_memory, an FPU and
                              * fpu_softfloat = false: translated FPU code works on host doubles
                              * and moves values through the JIT memory base */
} uae_cpu_config_t;

/* Memory Read/Write Callbacks for custom mapped devices */
typedef uint8_t  (*uae_read8_fn)(void *userdata, uint32_t address);
typedef uint16_t (*uae_read16_fn)(void *userdata, uint32_t address);
typedef uint32_t (*uae_read32_fn)(void *userdata, uint32_t address);
typedef void     (*uae_write8_fn)(void *userdata, uint32_t address, uint8_t value);
typedef void     (*uae_write16_fn)(void *userdata, uint32_t address, uint16_t value);
typedef void     (*uae_write32_fn)(void *userdata, uint32_t address, uint32_t value);

/* Reset / Instruction / Trap Hook Callbacks */
typedef void (*uae_reset_hook_fn)(void *userdata);
typedef void (*uae_instr_hook_fn)(void *userdata, uint32_t pc);
typedef int  (*uae_int_ack_fn)(void *userdata, int int_level);
typedef int  (*uae_trap_hook_fn)(void *userdata, int trap_nr);

/*
 * Host hooks (full contracts in HOST_HOOKS.md).
 *
 * Hooks run on the CPU thread inside uae_cpu_execute() / uae_cpu_step(), with
 * the status register already materialised. A hook may read and write
 * registers and memory, call uae_cpu_end_timeslice(), and run guest code
 * re-entrantly with uae_cpu_execute(). A NULL hook keeps the core's
 * Motorola-accurate behaviour.
 *
 * Resume rule for illegal / aline / fline: return non-zero to mark the
 * instruction handled. If the hook left the PC at `pc`, execution continues
 * at pc + 2; if it moved the PC, execution continues there.
 */

/* Why a Line-F instruction reached the host. */
typedef enum {
    UAE_FLINE_UNKNOWN    = 0, /* Unrecognised or unimplemented Line-F word */
    UAE_FLINE_FPU_ABSENT = 1, /* FPU instruction with no FPU configured (or FPU disabled) */
    UAE_FLINE_MMU_ABSENT = 2  /* MMU instruction with no MMU configured */
} uae_fline_reason_t;

/* Exception details passed to the observer before the stack frame is built. */
typedef struct {
    int      vector;      /* 680x0 vector number */
    uint32_t fault_pc;    /* Faulting instruction address (current PC for interrupts) */
    uint32_t current_pc;  /* PC at dispatch; may already be past the instruction */
    uint16_t opcode;      /* Instruction word being executed */
    uint16_t sr;          /* Status register before the exception */
    bool     interrupt;   /* True for vectors 24..31 */
} uae_cpu_exception_info_t;

typedef struct {
    void *userdata;

    /* Illegal instruction (not Line-A / Line-F), before vector 4. */
    int (*illegal)(void *userdata, uint16_t opcode, uint32_t pc);

    /* Line-A word, before vector 10. */
    int (*aline)(void *userdata, uint16_t opcode, uint32_t pc);

    /* Line-F word the CPU cannot execute, before vector 11 or the 68040/060
     * unimplemented-FPU exception. For multi-word instructions the hook must
     * move the PC past the whole instruction. */
    int (*fline)(void *userdata, uint16_t opcode, uint32_t pc, uae_fline_reason_t reason);

    /* Observes every exception, interrupts included; cannot cancel it. */
    void (*exception)(void *userdata, const uae_cpu_exception_info_t *info);

    /* Pulled interrupt level 0..7. Sampled at the start of every execute call
     * and whenever the core re-checks interrupts (see uae_cpu_signal_irq). */
    int (*get_irq)(void *userdata);

    /* DBF Dn,*-2 delay loop (fast interpreter only, never cycle-exact). Called
     * each time the DBF executes with the current Dn.W count. Return 0 to run
     * that one iteration normally (the hook is offered the next one too);
     * otherwise the loop completes at once (Dn.W = 0xFFFF, PC falls through)
     * and the returned number of CPU cycles is credited. */
    uint32_t (*dbf_spin)(void *userdata, int dreg, uint16_t count);
} uae_cpu_host_hooks_t;

/*
 * Opaque CPU handle. The core is single-instance: CPU state, the memory map,
 * hooks and the JIT cache are global, so every handle refers to the same CPU
 * and uae_cpu_create() does not give an independent core. Drive the CPU from
 * one thread; only the calls marked thread-safe may be made from others.
 */
typedef struct uae_cpu_instance uae_cpu_t;

/* Global Init / Cleanup */
void       uae_cpu_global_init(void);
void       uae_cpu_global_cleanup(void);

/* Instance Lifecycle */
uae_cpu_t* uae_cpu_create(const uae_cpu_config_t *config);
void       uae_cpu_destroy(uae_cpu_t *cpu);

/* Configuration & State */
void       uae_cpu_reset(uae_cpu_t *cpu);
void       uae_cpu_set_config(uae_cpu_t *cpu, const uae_cpu_config_t *config);
void       uae_cpu_get_config(uae_cpu_t *cpu, uae_cpu_config_t *config);

/* Execution */
int        uae_cpu_step(uae_cpu_t *cpu);
int        uae_cpu_execute(uae_cpu_t *cpu, int cycles);
bool       uae_cpu_is_stopped(uae_cpu_t *cpu);
bool       uae_cpu_is_halted(uae_cpu_t *cpu);

/* Interrupts & Control */
void       uae_cpu_set_irq(uae_cpu_t *cpu, int level);   /* Thread-safe */
int        uae_cpu_get_irq(uae_cpu_t *cpu);
void       uae_cpu_pulse_halt(uae_cpu_t *cpu);
void       uae_cpu_pulse_bus_error(uae_cpu_t *cpu);

/* Registers */
uint32_t   uae_cpu_get_reg(uae_cpu_t *cpu, uae_reg_t reg);
void       uae_cpu_set_reg(uae_cpu_t *cpu, uae_reg_t reg, uint32_t value);
void       uae_cpu_get_fp_reg(uae_cpu_t *cpu, int reg_num, uae_fp_reg_t *out_val);
void       uae_cpu_set_fp_reg(uae_cpu_t *cpu, int reg_num, const uae_fp_reg_t *in_val);
uint32_t   uae_cpu_get_fpcr(uae_cpu_t *cpu);
void       uae_cpu_set_fpcr(uae_cpu_t *cpu, uint32_t value);
uint32_t   uae_cpu_get_fpsr(uae_cpu_t *cpu);
void       uae_cpu_set_fpsr(uae_cpu_t *cpu, uint32_t value);

/* Memory Mapping */
int  uae_cpu_map_memory(uae_cpu_t *cpu, uint32_t start_addr, uint32_t size, uint8_t *host_ptr, uint32_t flags);
int  uae_cpu_map_ram(uae_cpu_t *cpu, uint32_t start_addr, uint32_t size, uint8_t *host_ptr);
int  uae_cpu_map_rom(uae_cpu_t *cpu, uint32_t start_addr, uint32_t size, uint8_t *host_ptr);
int  uae_cpu_map_custom(uae_cpu_t *cpu, uint32_t start_addr, uint32_t size,
                        uae_read8_fn r8, uae_read16_fn r16, uae_read32_fn r32,
                        uae_write8_fn w8, uae_write16_fn w16, uae_write32_fn w32,
                        void *userdata);
void uae_cpu_unmap_memory(uae_cpu_t *cpu, uint32_t start_addr, uint32_t size);

/* PC convenience */
uint32_t   uae_cpu_get_pc(uae_cpu_t *cpu);
void       uae_cpu_set_pc(uae_cpu_t *cpu, uint32_t pc);

/* Direct Bus Access */
uint8_t  uae_cpu_read_byte(uae_cpu_t *cpu, uint32_t addr);
uint16_t uae_cpu_read_word(uae_cpu_t *cpu, uint32_t addr);
uint32_t uae_cpu_read_long(uae_cpu_t *cpu, uint32_t addr);
void     uae_cpu_write_byte(uae_cpu_t *cpu, uint32_t addr, uint8_t val);
void     uae_cpu_write_word(uae_cpu_t *cpu, uint32_t addr, uint16_t val);
void     uae_cpu_write_long(uae_cpu_t *cpu, uint32_t addr, uint32_t val);

/* Hooks */
void uae_cpu_set_reset_hook(uae_cpu_t *cpu, uae_reset_hook_fn fn, void *userdata);
void uae_cpu_set_instr_hook(uae_cpu_t *cpu, uae_instr_hook_fn fn, void *userdata);
void uae_cpu_set_int_ack_hook(uae_cpu_t *cpu, uae_int_ack_fn fn, void *userdata);
/* TRAP #0-15: fn(userdata, trap_nr) returns non-zero to service the TRAP in the
 * host (no exception is taken; execution continues after the TRAP). */
void uae_cpu_set_trap_hook(uae_cpu_t *cpu, uae_trap_hook_fn fn, void *userdata);

/* Host hooks */
void     uae_cpu_set_host_hooks(uae_cpu_t *cpu, const uae_cpu_host_hooks_t *hooks); /* NULL clears */
/* Route [first, last] to the illegal/aline/fline hooks even if the CPU model
 * decodes them. Returns 0, or -1 if the range is inverted or 16 are in use. */
int      uae_cpu_reserve_opcodes(uae_cpu_t *cpu, uint16_t first, uint16_t last);
void     uae_cpu_clear_reserved_opcodes(uae_cpu_t *cpu);

/* Execution control, callable from inside hooks */
void     uae_cpu_end_timeslice(uae_cpu_t *cpu); /* Innermost execute returns after this instruction */
int      uae_cpu_execute_depth(uae_cpu_t *cpu); /* Active execute/step calls; 0 when not running */
void     uae_cpu_signal_irq(uae_cpu_t *cpu);    /* Thread-safe: re-sample the interrupt level */
uint64_t uae_cpu_get_cycles(uae_cpu_t *cpu);    /* Monotonic CPU cycles since the core was initialised */

/* Abort the current instruction with a bus error. Call from memory callbacks
 * or hooks during execution; it has no effect outside uae_cpu_execute/step. */
void     uae_cpu_raise_bus_error(uae_cpu_t *cpu, uint32_t addr, bool is_write, int size);

/* Discard translated code that may overlap [addr, addr + size). Blocks are
 * checksummed before reuse, so any size > 0 is safe. No-op without a JIT. */
void     uae_cpu_invalidate_code(uae_cpu_t *cpu, uint32_t addr, uint32_t size);

/*
 * Declares a flat guest window for the JIT: for guest code the JIT may
 * translate, the host byte for guest address a must be base + a. Translated
 * code derives the 68k PC from host pointers through this base, so the JIT
 * only compiles code in regions mapped with uae_cpu_map_memory() whose host
 * pointer equals base + start (for example one reservation covering the whole
 * guest address space, or a RAM buffer mapped at guest address 0). Code
 * anywhere else, and all code while no base is set, runs through the
 * interpreter. Returns 0, or -1 when the library has no JIT.
 *
 * With jit_direct_memory, translated code also reads and writes
 * UAE_MEM_JIT_DIRECT regions whose host pointer equals base + start at
 * base + address, without calling handlers. Each block is profiled in the
 * interpreter before it is compiled: accesses that touched only such regions
 * are inlined, the rest keep the handler call. A later access by the same
 * instruction to a different address still goes to base + address, so the
 * window must cover every address translated code can reach (typically one
 * 4 GB reservation with RAM, ROM and video memory committed in place). If
 * such an access faults in an uncommitted part of the window, the x86-64
 * and Windows ARM64 JITs recover and complete it through the region's
 * handler; on AArch64 Linux and macOS the fault reaches the host's
 * SIGSEGV/SIGBUS handler.
 */
int      uae_cpu_set_jit_memory_base(uae_cpu_t *cpu, uint8_t *base);

/* Bytes of translated host code currently in the JIT cache; 0 when the JIT
 * is off or not built. */
uint32_t uae_cpu_get_jit_code_size(uae_cpu_t *cpu);

/* uae_mem_flags_t bits of the region containing addr; 0 when unmapped. */
uint32_t uae_cpu_get_mem_flags(uae_cpu_t *cpu, uint32_t addr);

/* Disassembler */
int  uae_cpu_disassemble(uae_cpu_t *cpu, uint32_t pc, char *output_str, size_t maxlen);

#ifdef __cplusplus
}
#endif

#endif /* UAE_PORTABLE_CPU_H */
