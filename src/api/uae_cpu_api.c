
#include "sysconfig.h"
#include "sysdeps.h"
#include "uae_glue.h"
#include "options_cpu.h"
#include "events.h"
#include "maccess.h"
#include "memory.h"
#include "newcpu.h"
#include "cpu_prefetch.h"
#include "fpp.h"
#include "disasm.h"
#include "uae_cpu.h"

struct uae_cpu_instance {
    uae_cpu_config_t config;
    struct regstruct regs_backup;
};

static uae_cpu_t s_default_instance;

void uae_cpu_global_init(void) {
    memory_init();
    default_prefs(&currprefs, 68000);
    default_prefs(&changed_prefs, 68000);
    init_m68k();
}

void uae_cpu_global_cleanup(void) {
    memory_uninit();
}

uae_cpu_t* uae_cpu_create(const uae_cpu_config_t *config) {
    uae_cpu_t *cpu = (uae_cpu_t *)calloc(1, sizeof(uae_cpu_t));
    if (!cpu) return NULL;

    if (config) {
        cpu->config = *config;
        uae_cpu_set_config(cpu, config);
    } else {
        cpu->config.cpu_type = UAE_CPU_TYPE_68000;
        cpu->config.address_space_24 = true;
        uae_cpu_set_config(cpu, &cpu->config);
    }

    uae_cpu_reset(cpu);
    return cpu;
}

void uae_cpu_destroy(uae_cpu_t *cpu) {
    if (cpu && cpu != &s_default_instance) {
        free(cpu);
    }
}

void uae_cpu_set_config(uae_cpu_t *cpu, const uae_cpu_config_t *config) {
    if (!config) return;
    if (cpu) cpu->config = *config;

    currprefs.cpu_model = config->cpu_type;
    currprefs.fpu_model = config->fpu_type;
    currprefs.mmu_model = config->mmu_type;
    currprefs.address_space_24 = config->address_space_24;
    currprefs.cpu_compatible = (config->timing_mode >= 1);
    currprefs.cpu_cycle_exact = (config->timing_mode >= 2);
    currprefs.fpu_mode = config->fpu_softfloat ? 0 : 1;
    currprefs.cachesize = config->jit_enabled ? config->jit_cache_size : 0;

    changed_prefs = currprefs;
    fixup_cpu(&currprefs);
    init_m68k();
}

void uae_cpu_get_config(uae_cpu_t *cpu, uae_cpu_config_t *config) {
    if (cpu && config) {
        *config = cpu->config;
    }
}

void uae_cpu_reset(uae_cpu_t *cpu) {
    (void)cpu;
    m68k_reset();
    regs.regs[15] = get_long(regs.vbr + 0);
    regs.isp = regs.regs[15];
    regs.sr = 0x2700;
    MakeFromSR();
    m68k_setpc_normal(get_long(regs.vbr + 4));
    fill_prefetch();
}

int uae_cpu_step(uae_cpu_t *cpu) {
    (void)cpu;
    evt_t start = currcycle;
    if (regs.spcflags) {
        if (do_specialties(0))
            return 0;
    }

    if (g_instr_hook) {
        g_instr_hook(g_instr_userdata, m68k_getpc());
    }

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

    int cycles = (*cpufunctbl[opcode])(opcode) & 0xFFFF;
    cycles = adjust_cycles(cycles);
    do_cycles(cycles);
    regs.instruction_cnt++;

    return (int)((currcycle - start) / CYCLE_UNIT);
}

int uae_cpu_execute(uae_cpu_t *cpu, int cycles) {
    (void)cpu;
    evt_t start = currcycle;
    evt_t target = start + (evt_t)cycles * CYCLE_UNIT;

    while (currcycle < target && !regs.stopped && !regs.halted) {
        if (regs.spcflags) {
            if (do_specialties(0))
                break;
        }

        if (g_instr_hook) {
            g_instr_hook(g_instr_userdata, m68k_getpc());
        }

        uae_u16 opcode;
        if (currprefs.cpu_compatible && currprefs.cpu_model <= 68010) {
            opcode = regs.ir;
        } else {
            opcode = x_get_iword(0);
        }
        int cyc = (*cpufunctbl[opcode])(opcode) & 0xFFFF;
        if (cyc == 0) {
            cyc = (CurrentInstrCycles > 0 ? CurrentInstrCycles : 4) * (CYCLE_UNIT / 2);
        }
        cyc = adjust_cycles(cyc);
        do_cycles(cyc);
        regs.instruction_cnt++;
    }

    return (int)((currcycle - start) / CYCLE_UNIT);
}

bool uae_cpu_is_stopped(uae_cpu_t *cpu) {
    (void)cpu;
    return regs.stopped != 0;
}

bool uae_cpu_is_halted(uae_cpu_t *cpu) {
    (void)cpu;
    return regs.halted != 0;
}

void uae_cpu_set_irq(uae_cpu_t *cpu, int level) {
    (void)cpu;
    pending_irq_level = level & 0x7;
    if (pending_irq_level > 0) {
        set_special(SPCFLAG_INT);
    }
}

int uae_cpu_get_irq(uae_cpu_t *cpu) {
    (void)cpu;
    return pending_irq_level;
}

void uae_cpu_pulse_halt(uae_cpu_t *cpu) {
    (void)cpu;
    regs.halted = 1;
}

void uae_cpu_pulse_bus_error(uae_cpu_t *cpu) {
    (void)cpu;
    BusError68000(0, 0, 0);
}

uint32_t uae_cpu_get_reg(uae_cpu_t *cpu, uae_reg_t reg) {
    (void)cpu;
    switch (reg) {
        case UAE_REG_D0: case UAE_REG_D1: case UAE_REG_D2: case UAE_REG_D3:
        case UAE_REG_D4: case UAE_REG_D5: case UAE_REG_D6: case UAE_REG_D7:
            return regs.regs[reg - UAE_REG_D0];
        case UAE_REG_A0: case UAE_REG_A1: case UAE_REG_A2: case UAE_REG_A3:
        case UAE_REG_A4: case UAE_REG_A5: case UAE_REG_A6: case UAE_REG_A7:
            return regs.regs[8 + (reg - UAE_REG_A0)];
        case UAE_REG_PC:
            return m68k_getpc();
        case UAE_REG_SR:
            MakeSR();
            return regs.sr;
        case UAE_REG_SP:
            return regs.regs[15];
        case UAE_REG_USP:
            return regs.s ? regs.usp : regs.regs[15];
        case UAE_REG_ISP:
            return regs.s ? (regs.m ? regs.isp : regs.regs[15]) : regs.isp;
        case UAE_REG_MSP:
            return regs.msp;
        case UAE_REG_SFC:
            return regs.sfc;
        case UAE_REG_DFC:
            return regs.dfc;
        case UAE_REG_VBR:
            return regs.vbr;
        case UAE_REG_CACR:
            return regs.cacr;
        case UAE_REG_CAAR:
            return regs.caar;
        case UAE_REG_PCR:
            return regs.pcr;
        case UAE_REG_TC:
            return regs.tcr;
        case UAE_REG_ITT0:
            return regs.itt0;
        case UAE_REG_ITT1:
            return regs.itt1;
        case UAE_REG_DTT0:
            return regs.dtt0;
        case UAE_REG_DTT1:
            return regs.dtt1;
        case UAE_REG_MMUSR:
            return regs.mmusr;
        case UAE_REG_URP:
            return regs.urp;
        case UAE_REG_SRP:
            return regs.srp;
        default:
            return 0;
    }
}

void uae_cpu_set_reg(uae_cpu_t *cpu, uae_reg_t reg, uint32_t value) {
    (void)cpu;
    switch (reg) {
        case UAE_REG_D0: case UAE_REG_D1: case UAE_REG_D2: case UAE_REG_D3:
        case UAE_REG_D4: case UAE_REG_D5: case UAE_REG_D6: case UAE_REG_D7:
            regs.regs[reg - UAE_REG_D0] = value;
            break;
        case UAE_REG_A0: case UAE_REG_A1: case UAE_REG_A2: case UAE_REG_A3:
        case UAE_REG_A4: case UAE_REG_A5: case UAE_REG_A6: case UAE_REG_A7:
            regs.regs[8 + (reg - UAE_REG_A0)] = value;
            break;
        case UAE_REG_PC:
            m68k_setpc(value);
            fill_prefetch();
            break;
        case UAE_REG_SR:
            regs.sr = (uae_u16)value;
            MakeFromSR();
            break;
        case UAE_REG_SP:
            regs.regs[15] = value;
            break;
        case UAE_REG_USP:
            if (regs.s) regs.usp = value;
            else regs.regs[15] = value;
            break;
        case UAE_REG_ISP:
            if (regs.s && !regs.m) regs.regs[15] = value;
            else regs.isp = value;
            break;
        case UAE_REG_MSP:
            if (regs.s && regs.m) regs.regs[15] = value;
            else regs.msp = value;
            break;
        case UAE_REG_VBR:
            regs.vbr = value;
            break;
        case UAE_REG_SFC:
            regs.sfc = value;
            break;
        case UAE_REG_DFC:
            regs.dfc = value;
            break;
        case UAE_REG_CACR:
            regs.cacr = value;
            break;
        case UAE_REG_CAAR:
            regs.caar = value;
            break;
        default:
            break;
    }
}

void uae_cpu_get_fp_reg(uae_cpu_t *cpu, int reg_num, uae_fp_reg_t *out_val) {
    (void)cpu;
    if (reg_num >= 0 && reg_num < 8 && out_val) {
        uae_u32 w0, w1, w2;
        fpp_from_exten(&regs.fp[reg_num], &w0, &w1, &w2);
        out_val->exp = (uint16_t)(w0 >> 16);
        out_val->dummy = 0;
        out_val->m[0] = w1;
        out_val->m[1] = w2;
    }
}

void uae_cpu_set_fp_reg(uae_cpu_t *cpu, int reg_num, const uae_fp_reg_t *in_val) {
    (void)cpu;
    if (reg_num >= 0 && reg_num < 8 && in_val) {
        uae_u32 w0 = ((uae_u32)in_val->exp) << 16;
        fpp_to_exten(&regs.fp[reg_num], w0, in_val->m[0], in_val->m[1]);
    }
}

uint32_t uae_cpu_get_fpcr(uae_cpu_t *cpu) {
    (void)cpu;
    return regs.fpcr;
}

void uae_cpu_set_fpcr(uae_cpu_t *cpu, uint32_t value) {
    (void)cpu;
    regs.fpcr = value;
}

uint32_t uae_cpu_get_fpsr(uae_cpu_t *cpu) {
    (void)cpu;
    return regs.fpsr;
}

void uae_cpu_set_fpsr(uae_cpu_t *cpu, uint32_t value) {
    (void)cpu;
    regs.fpsr = value;
}

int uae_cpu_map_memory(uae_cpu_t *cpu, uint32_t start_addr, uint32_t size, uint8_t *host_ptr, uint32_t flags) {
    (void)cpu;
    return memory_map_ptr(start_addr, size, host_ptr, flags);
}

int uae_cpu_map_ram(uae_cpu_t *cpu, uint32_t start_addr, uint32_t size, uint8_t *host_ptr) {
    return uae_cpu_map_memory(cpu, start_addr, size, host_ptr, UAE_MEM_RAM | UAE_MEM_CACHEABLE);
}

int uae_cpu_map_rom(uae_cpu_t *cpu, uint32_t start_addr, uint32_t size, uint8_t *host_ptr) {
    return uae_cpu_map_memory(cpu, start_addr, size, host_ptr, UAE_MEM_ROM | UAE_MEM_CACHEABLE);
}

int uae_cpu_map_custom(uae_cpu_t *cpu, uint32_t start_addr, uint32_t size,
                       uae_read8_fn r8, uae_read16_fn r16, uae_read32_fn r32,
                       uae_write8_fn w8, uae_write16_fn w16, uae_write32_fn w32,
                       void *userdata) {
    (void)cpu;
    return memory_map_custom(start_addr, size, r8, r16, r32, w8, w16, w32, userdata);
}

void uae_cpu_unmap_memory(uae_cpu_t *cpu, uint32_t start_addr, uint32_t size) {
    (void)cpu;
    memory_unmap(start_addr, size);
}

uint32_t uae_cpu_get_pc(uae_cpu_t *cpu) {
    return uae_cpu_get_reg(cpu, UAE_REG_PC);
}

void uae_cpu_set_pc(uae_cpu_t *cpu, uint32_t pc) {
    uae_cpu_set_reg(cpu, UAE_REG_PC, pc);
}

uint8_t uae_cpu_read_byte(uae_cpu_t *cpu, uint32_t addr) {
    (void)cpu;
    return (uint8_t)get_byte(addr);
}

uint16_t uae_cpu_read_word(uae_cpu_t *cpu, uint32_t addr) {
    (void)cpu;
    return (uint16_t)get_word(addr);
}

uint32_t uae_cpu_read_long(uae_cpu_t *cpu, uint32_t addr) {
    (void)cpu;
    return get_long(addr);
}

void uae_cpu_write_byte(uae_cpu_t *cpu, uint32_t addr, uint8_t val) {
    (void)cpu;
    put_byte(addr, val);
}

void uae_cpu_write_word(uae_cpu_t *cpu, uint32_t addr, uint16_t val) {
    (void)cpu;
    put_word(addr, val);
}

void uae_cpu_write_long(uae_cpu_t *cpu, uint32_t addr, uint32_t val) {
    (void)cpu;
    put_long(addr, val);
}

void uae_cpu_set_reset_hook(uae_cpu_t *cpu, uae_reset_hook_fn fn, void *userdata) {
    (void)cpu;
    g_reset_hook = fn;
    g_reset_userdata = userdata;
}

void uae_cpu_set_instr_hook(uae_cpu_t *cpu, uae_instr_hook_fn fn, void *userdata) {
    (void)cpu;
    g_instr_hook = fn;
    g_instr_userdata = userdata;
}

void uae_cpu_set_int_ack_hook(uae_cpu_t *cpu, uae_int_ack_fn fn, void *userdata) {
    (void)cpu;
    g_int_ack_hook = fn;
    g_int_ack_userdata = userdata;
}

void uae_cpu_set_trap_hook(uae_cpu_t *cpu, uae_trap_hook_fn fn, void *userdata) {
    (void)cpu;
    g_trap_hook = fn;
    g_trap_userdata = userdata;
}

int uae_cpu_disassemble(uae_cpu_t *cpu, uint32_t pc, char *output_str, size_t maxlen) {
    (void)cpu;
    uaecptr nextpc = pc;
    m68k_disasm_2(output_str, (int)maxlen, pc, NULL, 0, &nextpc, 1, NULL, NULL, pc, 0);
    return (int)(nextpc - pc);
}
