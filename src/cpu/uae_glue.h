/*
 * UAE Portable 680x0 CPU Core - Glue Header
 */

#ifndef UAE_GLUE_H
#define UAE_GLUE_H

#include "sysconfig.h"
#include "sysdeps.h"
#include "options_cpu.h"
#include "events.h"
#include "memory.h"

#ifdef __cplusplus
extern "C" {
#endif

extern int pendingInterrupts;
extern int pending_irq_level;
extern FILE *TraceFile;
extern uint64_t CyclesGlobalClockCounter;
extern int BlitterPhase;
extern int WaitStateCycles;
extern bool CpuRunFuncNoret;
extern bool CpuRunCycleExact;
extern bool MFP_UpdateNeeded;
extern int savestate_state;
extern int bVdiAesIntercept;
extern int VDI_OldPC;
extern int CART_VDI_OPCODE_ADDR;
extern int CurrentInstrCycles;

/* Interrupt & reset functions called by CPU core */
extern uae_u32 hsync_counter;
extern uae_u32 vsync_counter;
extern uae_u8 agnus_hpos;

int  intlev(void);
void customreset(void);
void custom_reset(bool hardreset, bool keyboardreset);
void reset_frame_rate_hack(void);

/* Tracing and Logging */
#define LOG_TRACE_LEVEL(x) 0
#define TRACE_CPU_VIDEO_CYCLES 0
#define TRACE_CPU_DISASM 0
#define TRACE_CPU_PAIRING 0
#define TRACE_CPU_ALL 0
#define TRACE_CPU_EXCEPTION 0
#define TRACE_VIDEO_HBL 0
#define TRACE_VIDEO_VBL 0
#define LOG_TRACE(type, ...) do {} while(0)
#define LOG_TRACE_DIRECT_INIT() do {} while(0)
#define LOG_TRACE_DIRECT(...) do {} while(0)
#define LOG_DEBUG 0
#define LOG_INFO 1
#define LOG_WARN 2
#define LOG_ERROR 3
#define Log_Printf(level, ...) do {} while(0)
#define f_out(...) do {} while(0)

/* ST Compatibility Helpers */
#define STMemory_ReadLong(addr) get_long(addr)
#define STMemory_ReadWord(addr) get_word(addr)
#define STMemory_ReadByte(addr) get_byte(addr)
#define get_long_debug(addr) get_long(addr)
#define get_word_debug(addr) get_word(addr)
#define get_byte_debug(addr) get_byte(addr)

#define M68000_IsVerboseBusError(pc, fault) 0
#define M68000_AddCycles_CE(cycles) do_cycles((int)(cycles))
#define M68000_AddCycles(cycles) do_cycles((int)(cycles))
#define M68000_AddCyclesWithPairing(cycles) do_cycles((int)(cycles))
#define CycInt_Process() do {} while(0)
#define CycInt_Process_stop(s) do {} while(0)
#define CPU_IACK_CYCLES_START 0
#define CPU_IACK_CYCLES_MFP 0
#define CPU_IACK_CYCLES_MFP_CE 0
#define CPU_IACK_CYCLES_VIDEO 0
#define CPU_IACK_CYCLES_VIDEO_CE 0

#define memory_clear() do {} while(0)
#define M68000_PatchCpuTables() do {} while(0)
#define M68000_RestoreDebugger() do {} while(0)

#ifndef MAX_LINEWIDTH
#define MAX_LINEWIDTH 256
#endif

#define VDI_AES_Entry() 0
#define Bios() 0
#define XBios() 0
#define MFP_DelayIRQ() do {} while(0)
#define M68000_Update_intlev() do {} while(0)
#define DebugCpu_Check() do {} while(0)
#define MFP_UpdateIRQ_All(x) do {} while(0)

#define debug_safe_addr(addr, size) 1
#define get_byte_cache_debug(addr, cached) get_byte(addr)
#define get_word_cache_debug(addr, cached) get_word(addr)
#define get_long_cache_debug(addr, cached) get_long(addr)
#define Symbols_GetByCpuAddress(addr, type) NULL
#define SYMTYPE_TEXT 0
#define Profile_CpuAddr_HasData(addr) 0
#define Profile_CpuAddr_DataStr(buf, size, addr) do {} while(0)

static inline void Video_GetPosition(int *f, int *h, int *l) {
    if (f) *f = 0;
    if (h) *h = 0;
    if (l) *l = 0;
}

static inline void Blitter_HOG_CPU_mem_access_before(int x) { (void)x; }
static inline void Blitter_HOG_CPU_mem_access_after(int x) { (void)x; }

/* Wait cycle callbacks for CE modes */
uae_u32 wait_cpu_cycle_read(uaecptr addr, int mode);
void    wait_cpu_cycle_write(uaecptr addr, int mode, uae_u32 v);
uae_u32 wait_cpu_cycle_read_ce020(uaecptr addr, int mode);
void    wait_cpu_cycle_write_ce020(uaecptr addr, int mode, uae_u32 v);

/* Logging */
void write_log(const TCHAR *format, ...);
void error_log(const TCHAR *format, ...);
TCHAR* buf_out(TCHAR *buffer, int *bufsize, const TCHAR *format, ...);

/* User hooks */
typedef void (*uae_reset_cb)(void *userdata);
typedef void (*uae_instr_cb)(void *userdata, uae_u32 pc);
typedef int  (*uae_int_ack_cb)(void *userdata, int int_level);
typedef int  (*uae_trap_cb)(void *userdata, int trap_nr);

extern uae_reset_cb   g_reset_hook;
extern void          *g_reset_userdata;
extern uae_instr_cb   g_instr_hook;
extern void          *g_instr_userdata;
extern uae_int_ack_cb g_int_ack_hook;
extern void          *g_int_ack_userdata;
extern uae_trap_cb    g_trap_hook;
extern void          *g_trap_userdata;

/* DSP / UI Stubs */
#define Dialog_HaltDlg() do {} while(0)
#define bDspEnabled 0
#define DSP_Run(c) do {} while(0)

typedef struct {
    uint32_t I_Cache_hit;
    uint32_t I_Cache_miss;
    uint32_t D_Cache_hit;
    uint32_t D_Cache_miss;
} cpu_instruction_stats_t;
extern cpu_instruction_stats_t CpuInstruction;

void default_prefs(struct uae_prefs *p, int cpu_type);
void fixup_cpu(struct uae_prefs *p);

#ifdef __cplusplus
}
#endif

#endif /* UAE_GLUE_H */
