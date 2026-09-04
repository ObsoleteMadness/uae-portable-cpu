/*
 * UAE Portable 680x0 CPU Core - Glue Implementation
 */

#include "sysconfig.h"
#include "sysdeps.h"
#include "uae_glue.h"
#include "memory.h"
#include "newcpu.h"
#include <stdio.h>
#include <stdarg.h>

struct uae_prefs currprefs;
struct uae_prefs changed_prefs;

int pendingInterrupts = 0;
int pending_irq_level = 0;
FILE *TraceFile = NULL;
uint64_t CyclesGlobalClockCounter = 0;
int BlitterPhase = 0;
int WaitStateCycles = 0;
bool CpuRunFuncNoret = false;
bool CpuRunCycleExact = false;
bool MFP_UpdateNeeded = false;
int savestate_state = 0;
int bVdiAesIntercept = 0;
int VDI_OldPC = 0;
int CART_VDI_OPCODE_ADDR = 0;
int CurrentInstrCycles = 0;

uae_reset_cb   g_reset_hook = NULL;
void          *g_reset_userdata = NULL;
uae_instr_cb   g_instr_hook = NULL;
void          *g_instr_userdata = NULL;
uae_int_ack_cb g_int_ack_hook = NULL;
void          *g_int_ack_userdata = NULL;
uae_trap_cb    g_trap_hook = NULL;
void          *g_trap_userdata = NULL;

int intlev(void)
{
    if (pending_irq_level > 0)
        return pending_irq_level;
    for (int i = 7; i >= 1; i--) {
        if (pendingInterrupts & (1 << i))
            return i;
    }
    return 0;
}

void customreset(void)
{
    pendingInterrupts = 0;
    if (g_reset_hook) {
        g_reset_hook(g_reset_userdata);
    }
}

uae_u32 wait_cpu_cycle_read(uaecptr addr, int mode)
{
    uae_u32 v = 0;
    if (mode < 0)
        v = get_long(addr);
    else if (mode > 0)
        v = get_word(addr);
    else
        v = get_byte(addr);
    return v;
}

void wait_cpu_cycle_write(uaecptr addr, int mode, uae_u32 v)
{
    if (mode < 0)
        put_long(addr, v);
    else if (mode > 0)
        put_word(addr, v);
    else
        put_byte(addr, v);
}

uae_u32 wait_cpu_cycle_read_ce020(uaecptr addr, int mode)
{
    return wait_cpu_cycle_read(addr, mode);
}

void wait_cpu_cycle_write_ce020(uaecptr addr, int mode, uae_u32 v)
{
    wait_cpu_cycle_write(addr, mode, v);
}

void fixup_cpu(struct uae_prefs *p)
{
    if (p->cpu_model <= 68010) {
        p->address_space_24 = true;
    }
    if (p->cpu_model >= 68040) {
        if (p->fpu_model)
            p->fpu_model = p->cpu_model;
    }
}

void default_prefs(struct uae_prefs *p, int cpu_type)
{
    memset(p, 0, sizeof(*p));
    p->cpu_model = cpu_type ? cpu_type : 68000;
    p->cpu_compatible = false;
    p->cpu_cycle_exact = false;
    p->fpu_model = 0;
    p->mmu_model = 0;
    p->fpu_mode = 0; /* 0 = softfloat */
    p->fpu_strict = true;
    p->fpu_no_unimplemented = false;
    p->address_space_24 = (p->cpu_model <= 68010);
    p->cpu_frequency = 0;
    p->cpu_clock_multiplier = 0;
    fixup_cpu(p);
}

void write_log(const TCHAR *format, ...)
{
    static int s_debug_checked = 0;
    static int s_debug = 0;
    if (!s_debug_checked) {
        s_debug = getenv("UAE_DEBUG") != NULL;
        s_debug_checked = 1;
    }
    if (s_debug) {
        va_list ap;
        va_start(ap, format);
        vfprintf(stderr, format, ap);
        va_end(ap);
    }
}

void error_log(const TCHAR *format, ...)
{
    static int s_debug_checked = 0;
    static int s_debug = 0;
    if (!s_debug_checked) {
        s_debug = getenv("UAE_DEBUG") != NULL;
        s_debug_checked = 1;
    }
    if (s_debug) {
        va_list ap;
        va_start(ap, format);
        vfprintf(stderr, format, ap);
        va_end(ap);
    }
}

TCHAR* buf_out(TCHAR *buffer, int *bufsize, const TCHAR *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int len = vsnprintf(buffer, *bufsize, format, ap);
    va_end(ap);
    if (len > 0) {
        buffer += len;
        *bufsize -= len;
    }
    return buffer;
}

cpu_instruction_stats_t CpuInstruction = {0, 0, 0, 0};

uae_u8 agnus_hpos = 0;
uae_u32 hsync_counter = 0;
uae_u32 vsync_counter = 0;

void custom_reset(bool hardreset, bool keyboardreset) {
    (void)hardreset; (void)keyboardreset;
}
void reset_frame_rate_hack(void) {}

void BusError68000(uaecptr addr, int ws, int fc)
{
    (void)addr; (void)ws; (void)fc;
    Exception(2);
}

void do_cycles_ce(int cycles) { currcycle += cycles; }
void do_cycles_ce020(int cycles) { currcycle += cycles; }
void do_cycles_ce_hatari_blitter(int cycles) { currcycle += cycles; }
void do_cycles_slow(int cycles) { currcycle += cycles; }
void do_cycles_normal(int cycles) { currcycle += cycles; }
bool is_cycle_ce(uaecptr addr) { (void)addr; return false; }

bool fp_init_native(void) { return true; }
void fpux_restore(int *status) { (void)status; }

evt_t currcycle = 0;
evt_t nextevent = 0;

uae_u32 get_iword_debug(int o) { return get_word(m68k_getpc() + o); }
uae_u32 get_ilong_debug(int o) { return get_long(m68k_getpc() + o); }

int save_state(const TCHAR *filename, const TCHAR *description) { (void)filename; (void)description; return 0; }
void save_u32(uae_u32 val) { (void)val; }
void save_u16(uae_u16 val) { (void)val; }
uae_u32 restore_u32(void) { return 0; }
uae_u16 restore_u16(void) { return 0; }

int cctrue(int cc)
{
    uae_u32 cznv = regflags.cznv;

    switch (cc) {
    case 0:  return 1;                              /*              T  */
    case 1:  return 0;                              /*              F  */
    case 2:  return (cznv & (FLAGVAL_C | FLAGVAL_Z)) == 0;              /* !CFLG && !ZFLG       HI */
    case 3:  return (cznv & (FLAGVAL_C | FLAGVAL_Z)) != 0;              /*  CFLG || ZFLG        LS */
    case 4:  return (cznv & FLAGVAL_C) == 0;                    /* !CFLG            CC */
    case 5:  return (cznv & FLAGVAL_C) != 0;                    /*  CFLG            CS */
    case 6:  return (cznv & FLAGVAL_Z) == 0;                    /* !ZFLG            NE */
    case 7:  return (cznv & FLAGVAL_Z) != 0;                    /*  ZFLG            EQ */
    case 8:  return (cznv & FLAGVAL_V) == 0;                    /* !VFLG            VC */
    case 9:  return (cznv & FLAGVAL_V) != 0;                    /*  VFLG            VS */
    case 10: return (cznv & FLAGVAL_N) == 0;                    /* !NFLG            PL */
    case 11: return (cznv & FLAGVAL_N) != 0;                    /*  NFLG            MI */
#if FLAGBIT_N > FLAGBIT_V
    case 12: return (((cznv << (FLAGBIT_N - FLAGBIT_V)) ^ cznv) & FLAGVAL_N) == 0;  /*  NFLG == VFLG        GE */
    case 13: return (((cznv << (FLAGBIT_N - FLAGBIT_V)) ^ cznv) & FLAGVAL_N) != 0;  /*  NFLG != VFLG        LT */
    case 14: cznv &= (FLAGVAL_N | FLAGVAL_Z | FLAGVAL_V);               /* !ZFLG && (NFLG == VFLG)   GT */
        return (((cznv << (FLAGBIT_N - FLAGBIT_V)) ^ cznv) & (FLAGVAL_N | FLAGVAL_Z)) == 0;
    case 15: cznv &= (FLAGVAL_N | FLAGVAL_Z | FLAGVAL_V);               /* ZFLG || (NFLG != VFLG)   LE */
        return (((cznv << (FLAGBIT_N - FLAGBIT_V)) ^ cznv) & (FLAGVAL_N | FLAGVAL_Z)) != 0;
#else
    case 12: return (((cznv << (FLAGBIT_V - FLAGBIT_N)) ^ cznv) & FLAGVAL_V) == 0;  /*  NFLG == VFLG        GE */
    case 13: return (((cznv << (FLAGBIT_V - FLAGBIT_N)) ^ cznv) & FLAGVAL_V) != 0;  /*  NFLG != VFLG        LT */
    case 14: cznv &= (FLAGVAL_N | FLAGVAL_Z | FLAGVAL_V);               /* !ZFLG && (NFLG == VFLG)   GT */
        return (((cznv << (FLAGBIT_V - FLAGBIT_N)) ^ cznv) & (FLAGVAL_V | FLAGVAL_Z)) == 0;
    case 15: cznv &= (FLAGVAL_N | FLAGVAL_Z | FLAGVAL_V);               /* ZFLG || (NFLG != VFLG)   LE */
        return (((cznv << (FLAGBIT_V - FLAGBIT_N)) ^ cznv) & (FLAGVAL_V | FLAGVAL_Z)) != 0;
#endif
    }
    return 0;
}
