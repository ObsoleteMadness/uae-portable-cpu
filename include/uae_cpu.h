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
    UAE_MEM_CACHEABLE = 0x08
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
    bool fpu_softfloat;      /* true = use SoftFloat, false = host native */
    bool address_space_24;   /* true = 24-bit addressing (e.g. 68000, 68EC020) */
    int timing_mode;         /* 0 = fast/standard, 1 = prefetch, 2 = cycle-exact */
    bool jit_enabled;        /* true = enable dynamic translation if available */
    uint32_t jit_cache_size; /* JIT code cache size in KB (e.g. 8192) */
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

/* Opaque CPU Instance Handle */
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
void       uae_cpu_set_irq(uae_cpu_t *cpu, int level);
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
void uae_cpu_set_trap_hook(uae_cpu_t *cpu, uae_trap_hook_fn fn, void *userdata);

/* Disassembler */
int  uae_cpu_disassemble(uae_cpu_t *cpu, uint32_t pc, char *output_str, size_t maxlen);

#ifdef __cplusplus
}
#endif

#endif /* UAE_PORTABLE_CPU_H */
