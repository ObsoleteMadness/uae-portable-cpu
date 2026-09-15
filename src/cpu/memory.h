/*
 * UAE Portable 680x0 CPU Core - Portable Memory Subsystem
 */

#ifndef UAE_MEMORY_H
#define UAE_MEMORY_H

#include "sysdeps.h"
#include "uae_cpu.h"

#define MEMORY_BANKS 65536
#define MEMORY_RANGE_MASK (~0)

#define S_READ 1
#define S_WRITE 2
#define S_N_ADDR 4

#ifdef JIT
/*
 * JIT memory model (see jit/compemu_support_*.cpp).
 *
 * special_mem collects the jit_read_flag / jit_write_flag of every bank an
 * instruction touched while a block was being profiled, so accesses to
 * indirect (IO) regions compile as helper calls. canbang enables direct
 * host-pointer access through natmem_offset; while it is false every
 * compiled memory access goes through the bank handlers.
 */
extern int special_mem;
extern int special_mem_default;
extern int jit_n_addr_unsafe;
extern int jit_n_addr_bank_unsafe;
extern bool canbang;
extern bool jit_direct_compatible_memory;
extern uae_u8 *natmem_offset;
extern uae_u8 *natmem_reserved;
extern size_t natmem_reserved_size;
#endif

typedef uae_u32 (REGPARAM3 *mem_get_func)(uaecptr) REGPARAM;
typedef void (REGPARAM3 *mem_put_func)(uaecptr, uae_u32) REGPARAM;
typedef uae_u8 *(REGPARAM3 *xlate_func)(uaecptr) REGPARAM;
typedef int (REGPARAM3 *check_func)(uaecptr, uae_u32) REGPARAM;

#define ABFLAG_UNK 0
#define ABFLAG_RAM 1
#define ABFLAG_ROM 2
#define ABFLAG_IO 8
#define ABFLAG_NONE 16
#define ABFLAG_SAFE 32
#define ABFLAG_DIRECTMAP 1024
#define ABFLAG_JIT_UNSAFE_BURST 0x10000

enum {
    CE_MEMBANK_NONE,
    CE_MEMBANK_CHIP16,
    CE_MEMBANK_CHIP32,
    CE_MEMBANK_FAST16,
    CE_MEMBANK_FAST32,
    CE_MEMBANK_FAST32_24
};

typedef struct addrbank {
    /* The accessors must stay first and in this order: the JIT calls them by
     * fixed offset (lput = 3, wput = 4, bput = 5 pointers into the struct),
     * matching the WinUAE / Amiberry addrbank layout. */
    mem_get_func lget, wget, bget;
    mem_put_func lput, wput, bput;
    const char *name;
    xlate_func xlateaddr;
    check_func check;
    uae_u8 *baseaddr;
    uae_u32 mask;
    uae_u32 flags;
    const char *label;       /* Short bank name used in JIT diagnostics */
    uaecptr start;           /* Guest address the host mapping starts at */
    uae_u32 allocated_size;  /* Length of the host mapping in bytes */
    uae_u32 host_flags;      /* uae_mem_flags_t bits given when mapped */
    uae_u32 jit_read_flag;   /* 0 = JIT may read directly, S_READ = via helper */
    uae_u32 jit_write_flag;  /* 0 = JIT may write directly, S_WRITE = via helper */
    void *userdata;

    /* Custom device callbacks if mapped via custom API */
    uae_read8_fn   r8;
    uae_read16_fn  r16;
    uae_read32_fn  r32;
    uae_write8_fn  w8;
    uae_write16_fn w16;
    uae_write32_fn w32;
} addrbank;

extern addrbank *mem_banks[MEMORY_BANKS];

#define bankindex(addr) (((uaecptr)(addr)) >> 16)

#ifdef JIT
/* Host base per 64K bank: baseaddr[bankindex(a)] + a is the host byte for a. */
extern uae_u8 *baseaddr[MEMORY_BANKS];
/* Bank globals the JIT consults for ROM detection and fault probes; unused (zero) here. */
extern addrbank kickmem_bank;
extern addrbank rtarea_bank;
extern addrbank a3000lmem_bank;
extern addrbank a3000hmem_bank;
/* Side-effect-free read of unmapped space for JIT fault handlers: never raises
 * a bus error, returns defvalue. size is sz_byte / sz_word / sz_long. */
extern uae_u32 dummy_get_safe(uaecptr addr, int size, bool inst, uae_u32 defvalue);
#endif
extern addrbank dummy_bank;
extern addrbank musashi_bridge_bank;
extern uae_u8 ce_cachable[65536];
extern uae_u8 ce_banktype[65536];

/* Test device state */
typedef struct {
    uint32_t pass_count;
    uint32_t fail_count;
    int      interrupt_level;
} uae_test_device_t;

extern uae_test_device_t g_test_device;
extern bool g_musashi_mode;

/* Memory Initialization & Management */
void memory_init(void);
void memory_reset(void);
void memory_uninit(void);

void map_banks(addrbank *bank, int start, int size, int realsize);
int  memory_map_ptr(uint32_t start_addr, uint32_t size, uint8_t *host_ptr, uint32_t flags);
int  memory_map_custom(uint32_t start_addr, uint32_t size,
                       uae_read8_fn r8, uae_read16_fn r16, uae_read32_fn r32,
                       uae_write8_fn w8, uae_write16_fn w16, uae_write32_fn w32,
                       void *userdata);
void memory_unmap(uint32_t start_addr, uint32_t size);
uint32_t memory_host_flags(uint32_t addr);

/* Bank Lookup */
#define get_mem_bank(addr) (*mem_banks[((addr) >> 16) & 0xFFFF])

static inline addrbank *get_mem_bank_real(uaecptr addr) {
    return &get_mem_bank(addr);
}

#include "maccess.h"

/* Memory Access Primitives */
static inline uae_u32 get_long(uaecptr addr) {
    addrbank *bank = &get_mem_bank(addr);
    return call_mem_get_func(bank->lget, addr);
}

static inline uae_u32 get_word(uaecptr addr) {
    addrbank *bank = &get_mem_bank(addr);
    return (uae_u32)(uae_u16)call_mem_get_func(bank->wget, addr);
}

static inline uae_u32 get_byte(uaecptr addr) {
    addrbank *bank = &get_mem_bank(addr);
    return (uae_u32)(uae_u8)call_mem_get_func(bank->bget, addr);
}

static inline void put_long(uaecptr addr, uae_u32 l) {
    addrbank *bank = &get_mem_bank(addr);
    call_mem_put_func(bank->lput, addr, l);
}

static inline void put_word(uaecptr addr, uae_u32 w) {
    addrbank *bank = &get_mem_bank(addr);
    call_mem_put_func(bank->wput, addr, (uae_u16)w);
}

static inline void put_byte(uaecptr addr, uae_u32 b) {
    addrbank *bank = &get_mem_bank(addr);
    call_mem_put_func(bank->bput, addr, (uae_u8)b);
}

static inline uae_u32 get_longi(uaecptr addr) {
    return get_long(addr);
}

static inline uae_u32 get_wordi(uaecptr addr) {
    return get_word(addr);
}

static inline uae_u8 *get_real_address(uaecptr addr) {
    addrbank *bank = &get_mem_bank(addr);
    return bank->xlateaddr(addr);
}

static inline int valid_address(uaecptr addr, uae_u32 size) {
    addrbank *bank = &get_mem_bank(addr);
    return bank->check(addr, size);
}

#define put_long_compatible put_long
#define put_word_compatible put_word
#define put_byte_compatible put_byte
#define get_long_compatible get_long
#define get_word_compatible get_word
#define get_byte_compatible get_byte

#ifdef JIT
/* Accessors used by the JIT-profiled opcode tables: record which bank types
 * the instruction touched so compile_block() picks direct or helper access. */
static inline uae_u32 get_long_jit(uaecptr addr) {
    special_mem |= get_mem_bank(addr).jit_read_flag;
    return get_long(addr);
}
static inline uae_u32 get_word_jit(uaecptr addr) {
    special_mem |= get_mem_bank(addr).jit_read_flag;
    return get_word(addr);
}
static inline uae_u32 get_byte_jit(uaecptr addr) {
    special_mem |= get_mem_bank(addr).jit_read_flag;
    return get_byte(addr);
}
static inline void put_long_jit(uaecptr addr, uae_u32 l) {
    special_mem |= get_mem_bank(addr).jit_write_flag;
    put_long(addr, l);
}
static inline void put_word_jit(uaecptr addr, uae_u32 w) {
    special_mem |= get_mem_bank(addr).jit_write_flag;
    put_word(addr, w);
}
static inline void put_byte_jit(uaecptr addr, uae_u32 b) {
    special_mem |= get_mem_bank(addr).jit_write_flag;
    put_byte(addr, b);
}
#else
#define put_long_jit put_long
#define put_word_jit put_word
#define put_byte_jit put_byte
#define get_long_jit get_long
#define get_word_jit get_word
#define get_byte_jit get_byte
#endif

#endif /* UAE_MEMORY_H */
