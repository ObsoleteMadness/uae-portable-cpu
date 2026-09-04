
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
#include "m68k.h"

static int s_cpu_type = M68K_CPU_TYPE_68000;
static int (*s_int_ack_cb)(int) = NULL;
static void (*s_bkpt_ack_cb)(unsigned int) = NULL;
static void (*s_reset_cb)(void) = NULL;
static void (*s_pc_changed_cb)(unsigned int) = NULL;
static int  (*s_tas_cb)(void) = NULL;
static int  (*s_illg_cb)(int) = NULL;
static int  (*s_trap_cb)(int) = NULL;
static void (*s_fc_cb)(unsigned int) = NULL;
static void (*s_instr_hook_cb)(unsigned int) = NULL;

static int musashi_int_ack_bridge(void *userdata, int int_level) {
    (void)userdata;
    if (s_int_ack_cb) {
        return s_int_ack_cb(int_level);
    }
    return M68K_INT_ACK_AUTOVECTOR;
}

static void musashi_reset_bridge(void *userdata) {
    (void)userdata;
    if (s_reset_cb) {
        s_reset_cb();
    }
}

static void musashi_instr_bridge(void *userdata, uae_u32 pc) {
    (void)userdata;
    if (s_instr_hook_cb) {
        s_instr_hook_cb(pc);
    }
}

static int musashi_trap_bridge(void *userdata, int trap_nr) {
    (void)userdata;
    if (s_trap_cb) {
        return s_trap_cb(trap_nr);
    }
    return 0;
}

void m68k_init(void) {
    memory_init();
    g_musashi_mode = true;
    default_prefs(&currprefs, 68000);
    default_prefs(&changed_prefs, 68000);

    g_int_ack_hook = musashi_int_ack_bridge;
    g_reset_hook = musashi_reset_bridge;
    g_instr_hook = musashi_instr_bridge;
    g_trap_hook = musashi_trap_bridge;

    init_m68k();
}

void m68k_set_cpu_type(unsigned int cpu_type) {
    s_cpu_type = cpu_type;
    currprefs.cpu_compatible = false;
    switch (cpu_type) {
        case M68K_CPU_TYPE_68000:
            currprefs.cpu_model = 68000;
            currprefs.address_space_24 = true;
            currprefs.fpu_model = 0;
            currprefs.mmu_model = 0;
            break;
        case M68K_CPU_TYPE_68010:
            currprefs.cpu_model = 68010;
            currprefs.address_space_24 = true;
            currprefs.fpu_model = 0;
            currprefs.mmu_model = 0;
            break;
        case M68K_CPU_TYPE_68EC020:
            currprefs.cpu_model = 68020;
            currprefs.address_space_24 = true;
            currprefs.fpu_model = 0;
            currprefs.mmu_model = 0;
            break;
        case M68K_CPU_TYPE_68020:
            currprefs.cpu_model = 68020;
            currprefs.address_space_24 = false;
            currprefs.fpu_model = 68881;
            currprefs.mmu_model = 0;
            break;
        case M68K_CPU_TYPE_68EC030:
            currprefs.cpu_model = 68030;
            currprefs.address_space_24 = true;
            currprefs.fpu_model = 68882;
            currprefs.mmu_model = 0;
            break;
        case M68K_CPU_TYPE_68030:
            currprefs.cpu_model = 68030;
            currprefs.address_space_24 = false;
            currprefs.fpu_model = 68882;
            currprefs.mmu_model = 0;
            break;
        case M68K_CPU_TYPE_68EC040:
            currprefs.cpu_model = 68040;
            currprefs.address_space_24 = false;
            currprefs.fpu_model = 0;
            currprefs.mmu_model = 0;
            break;
        case M68K_CPU_TYPE_68LC040:
            currprefs.cpu_model = 68040;
            currprefs.address_space_24 = false;
            currprefs.fpu_model = 0;
            currprefs.mmu_model = 0;
            break;
        case M68K_CPU_TYPE_68040:
            currprefs.cpu_model = 68040;
            currprefs.address_space_24 = false;
            currprefs.fpu_model = 68040;
            currprefs.mmu_model = 0;
            break;
        default:
            currprefs.cpu_model = 68000;
            currprefs.address_space_24 = true;
            break;
    }
    changed_prefs = currprefs;
    fixup_cpu(&currprefs);
    init_m68k();
}

void m68k_pulse_reset(void) {
    m68k_reset();
    regs.regs[15] = get_long(regs.vbr + 0); /* Initial SSP */
    regs.isp = regs.regs[15];
    regs.sr = 0x2700;
    MakeFromSR();
    m68k_setpc_normal(get_long(regs.vbr + 4)); /* Initial PC */
    fill_prefetch();
}

int m68k_execute(int num_cycles) {
    evt_t start = currcycle;
    evt_t target = start + (evt_t)num_cycles * CYCLE_UNIT;

    while (currcycle < target && !regs.stopped && !regs.halted) {
        if (regs.spcflags) {
            if (do_specialties(0))
                break;
        }

        if (s_instr_hook_cb) {
            s_instr_hook_cb(m68k_getpc());
        }

        uae_u16 opcode = x_get_iword(0);
        int cycles = (*cpufunctbl[opcode])(opcode) & 0xFFFF;
        if (cycles == 0) {
            cycles = (CurrentInstrCycles > 0 ? CurrentInstrCycles : 4) * (CYCLE_UNIT / 2);
        }
        cycles = adjust_cycles(cycles);
        do_cycles(cycles);
        regs.instruction_cnt++;
    }

    return (int)((currcycle - start) / CYCLE_UNIT);
}

int m68k_cycles_run(void) {
    return (int)(currcycle / CYCLE_UNIT);
}

int m68k_cycles_remaining(void) {
    return 0;
}

void m68k_modify_timeslice(int cycles) {
    (void)cycles;
}

void m68k_end_timeslice(void) {
    set_special(SPCFLAG_BRK);
}

void m68k_set_irq(unsigned int int_level) {
    pending_irq_level = int_level & 0x7;
    if (pending_irq_level > 0) {
        set_special(SPCFLAG_INT);
    }
}

void m68k_set_virq(unsigned int level, unsigned int active) {
    if (active)
        pendingInterrupts |= (1 << (level & 0x7));
    else
        pendingInterrupts &= ~(1 << (level & 0x7));

    if (pendingInterrupts)
        set_special(SPCFLAG_INT);
}

unsigned int m68k_get_virq(unsigned int level) {
    return (pendingInterrupts & (1 << (level & 0x7))) ? 1 : 0;
}

void m68k_pulse_halt(void) {
    regs.halted = 1;
}

void m68k_pulse_bus_error(void) {
    BusError68000(0, 0, 0);
}

unsigned int m68k_context_size(void) {
    return (unsigned int)sizeof(struct regstruct);
}

unsigned int m68k_get_context(void *dst) {
    if (dst) {
        memcpy(dst, &regs, sizeof(struct regstruct));
    }
    return (unsigned int)sizeof(struct regstruct);
}

void m68k_set_context(void *src) {
    if (src) {
        memcpy(&regs, src, sizeof(struct regstruct));
        MakeFromSR();
    }
}

void m68k_state_register(const char *type, int index) {
    (void)type; (void)index;
}

unsigned int m68k_get_reg(void *context, m68k_register_t reg) {
    struct regstruct *r = context ? (struct regstruct *)context : &regs;
    switch (reg) {
        case M68K_REG_D0: case M68K_REG_D1: case M68K_REG_D2: case M68K_REG_D3:
        case M68K_REG_D4: case M68K_REG_D5: case M68K_REG_D6: case M68K_REG_D7:
            return r->regs[reg - M68K_REG_D0];
        case M68K_REG_A0: case M68K_REG_A1: case M68K_REG_A2: case M68K_REG_A3:
        case M68K_REG_A4: case M68K_REG_A5: case M68K_REG_A6: case M68K_REG_A7:
            return r->regs[8 + (reg - M68K_REG_A0)];
        case M68K_REG_PC:
            return m68k_getpc();
        case M68K_REG_SR:
            MakeSR();
            return r->sr;
        case M68K_REG_SP:
            return r->regs[15];
        case M68K_REG_USP:
            return r->s ? r->usp : r->regs[15];
        case M68K_REG_ISP:
            return r->s ? (r->m ? r->isp : r->regs[15]) : r->isp;
        case M68K_REG_MSP:
            return r->msp;
        case M68K_REG_SFC:
            return r->sfc;
        case M68K_REG_DFC:
            return r->dfc;
        case M68K_REG_VBR:
            return r->vbr;
        case M68K_REG_CACR:
            return r->cacr;
        case M68K_REG_CAAR:
            return r->caar;
        case M68K_REG_PPC:
            return r->instruction_pc;
        case M68K_REG_IR:
            return r->opcode;
        case M68K_REG_CPU_TYPE:
            return s_cpu_type;
        default:
            return 0;
    }
}

void m68k_set_reg(m68k_register_t reg, unsigned int value) {
    switch (reg) {
        case M68K_REG_D0: case M68K_REG_D1: case M68K_REG_D2: case M68K_REG_D3:
        case M68K_REG_D4: case M68K_REG_D5: case M68K_REG_D6: case M68K_REG_D7:
            regs.regs[reg - M68K_REG_D0] = value;
            break;
        case M68K_REG_A0: case M68K_REG_A1: case M68K_REG_A2: case M68K_REG_A3:
        case M68K_REG_A4: case M68K_REG_A5: case M68K_REG_A6: case M68K_REG_A7:
            regs.regs[8 + (reg - M68K_REG_A0)] = value;
            break;
        case M68K_REG_PC:
            m68k_setpc_normal(value);
            fill_prefetch();
            break;
        case M68K_REG_SR:
            regs.sr = (uae_u16)value;
            MakeFromSR();
            break;
        case M68K_REG_SP:
            regs.regs[15] = value;
            break;
        case M68K_REG_USP:
            if (regs.s) regs.usp = value;
            else regs.regs[15] = value;
            break;
        case M68K_REG_ISP:
            if (regs.s && !regs.m) regs.regs[15] = value;
            else regs.isp = value;
            break;
        case M68K_REG_MSP:
            if (regs.s && regs.m) regs.regs[15] = value;
            else regs.msp = value;
            break;
        case M68K_REG_VBR:
            regs.vbr = value;
            break;
        case M68K_REG_SFC:
            regs.sfc = value;
            break;
        case M68K_REG_DFC:
            regs.dfc = value;
            break;
        case M68K_REG_CACR:
            regs.cacr = value;
            break;
        case M68K_REG_CAAR:
            regs.caar = value;
            break;
        default:
            break;
    }
}

void m68k_set_int_ack_callback(int (*callback)(int int_level)) {
    s_int_ack_cb = callback;
}
void m68k_set_bkpt_ack_callback(void (*callback)(unsigned int data)) {
    s_bkpt_ack_cb = callback;
}
void m68k_set_reset_instr_callback(void (*callback)(void)) {
    s_reset_cb = callback;
}
void m68k_set_pc_changed_callback(void (*callback)(unsigned int new_pc)) {
    s_pc_changed_cb = callback;
}
void m68k_set_tas_instr_callback(int (*callback)(void)) {
    s_tas_cb = callback;
}
void m68k_set_illg_instr_callback(int (*callback)(int)) {
    s_illg_cb = callback;
}
void m68k_set_trap_instr_callback(int (*callback)(int)) {
    s_trap_cb = callback;
}
void m68k_set_fc_callback(void (*callback)(unsigned int new_fc)) {
    s_fc_cb = callback;
}
void m68k_set_instr_hook_callback(void (*callback)(unsigned int pc)) {
    s_instr_hook_cb = callback;
}

unsigned int m68k_is_valid_instruction(unsigned int instruction, unsigned int cpu_type) {
    (void)cpu_type;
    return cpufunctbl[instruction & 0xFFFF] != NULL;
}

unsigned int m68k_disassemble(char *str_buff, unsigned int pc, unsigned int cpu_type) {
    (void)cpu_type;
    uaecptr nextpc = pc;
    m68k_disasm_2(str_buff, 256, pc, NULL, 0, &nextpc, 1, NULL, NULL, pc, 0);
    return (unsigned int)(nextpc - pc);
}

unsigned int m68k_disassemble_raw(char* str_buff, unsigned int pc, const unsigned char* opdata, const unsigned char* argdata, unsigned int cpu_type) {
    (void)opdata; (void)argdata;
    return m68k_disassemble(str_buff, pc, cpu_type);
}
