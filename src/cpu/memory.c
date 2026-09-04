/*
 * UAE Portable 680x0 CPU Core - Portable Memory Implementation
 */

#include "sysconfig.h"
#include "sysdeps.h"
#include "uae_glue.h"
#include "maccess.h"
#include "memory.h"
#include "newcpu.h"
#include "m68k.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

addrbank *mem_banks[MEMORY_BANKS];
addrbank dummy_bank;
addrbank musashi_bridge_bank;
addrbank test_device_bank;
uae_u8 ce_cachable[65536] = {0};
uae_u8 ce_banktype[65536] = {0};

uae_test_device_t g_test_device = {0, 0, 0};
bool g_musashi_mode = false;

/* Weak default callbacks for Musashi API (overridden when using Musashi API) */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak)) unsigned int m68k_read_memory_8(unsigned int address) { (void)address; return 0; }
__attribute__((weak)) unsigned int m68k_read_memory_16(unsigned int address) { (void)address; return 0; }
__attribute__((weak)) unsigned int m68k_read_memory_32(unsigned int address) { (void)address; return 0; }
__attribute__((weak)) void m68k_write_memory_8(unsigned int address, unsigned int value) { (void)address; (void)value; }
__attribute__((weak)) void m68k_write_memory_16(unsigned int address, unsigned int value) { (void)address; (void)value; }
__attribute__((weak)) void m68k_write_memory_32(unsigned int address, unsigned int value) { (void)address; (void)value; }
#elif defined(_MSC_VER)
#pragma comment(linker, "/alternatename:m68k_read_memory_8=default_m68k_read_memory_8")
#pragma comment(linker, "/alternatename:m68k_read_memory_16=default_m68k_read_memory_16")
#pragma comment(linker, "/alternatename:m68k_read_memory_32=default_m68k_read_memory_32")
#pragma comment(linker, "/alternatename:m68k_write_memory_8=default_m68k_write_memory_8")
#pragma comment(linker, "/alternatename:m68k_write_memory_16=default_m68k_write_memory_16")
#pragma comment(linker, "/alternatename:m68k_write_memory_32=default_m68k_write_memory_32")

unsigned int default_m68k_read_memory_8(unsigned int address) { (void)address; return 0; }
unsigned int default_m68k_read_memory_16(unsigned int address) { (void)address; return 0; }
unsigned int default_m68k_read_memory_32(unsigned int address) { (void)address; return 0; }
void default_m68k_write_memory_8(unsigned int address, unsigned int value) { (void)address; (void)value; }
void default_m68k_write_memory_16(unsigned int address, unsigned int value) { (void)address; (void)value; }
void default_m68k_write_memory_32(unsigned int address, unsigned int value) { (void)address; (void)value; }
#endif

/* Dummy Bank Handlers (Default / Unmapped) */
static uae_u32 REGPARAM3 dummy_lget(uaecptr addr) REGPARAM {
    if (g_musashi_mode) {
        return m68k_read_memory_32(addr);
    }
    return 0xFFFFFFFF;
}

static uae_u32 REGPARAM3 dummy_wget(uaecptr addr) REGPARAM {
    if (g_musashi_mode) {
        return m68k_read_memory_16(addr);
    }
    return 0xFFFF;
}

static uae_u32 REGPARAM3 dummy_bget(uaecptr addr) REGPARAM {
    if (g_musashi_mode) {
        return m68k_read_memory_8(addr);
    }
    return 0xFF;
}

static void REGPARAM3 dummy_lput(uaecptr addr, uae_u32 l) REGPARAM {
    if (g_musashi_mode) {
        m68k_write_memory_32(addr, l);
    }
}

static void REGPARAM3 dummy_wput(uaecptr addr, uae_u32 w) REGPARAM {
    if (g_musashi_mode) {
        m68k_write_memory_16(addr, w);
    }
}

static void REGPARAM3 dummy_bput(uaecptr addr, uae_u32 b) REGPARAM {
    if (g_musashi_mode) {
        m68k_write_memory_8(addr, b);
    }
}

static int REGPARAM3 dummy_check(uaecptr addr, uae_u32 size) REGPARAM {
    (void)addr; (void)size;
    return 0;
}

static uae_u8 *REGPARAM3 dummy_xlate(uaecptr addr) REGPARAM {
    (void)addr;
    return NULL;
}

/* Musashi Direct Bridge Handlers */
static uae_u32 REGPARAM3 musashi_lget(uaecptr addr) REGPARAM {
    return m68k_read_memory_32(addr);
}
static uae_u32 REGPARAM3 musashi_wget(uaecptr addr) REGPARAM {
    return m68k_read_memory_16(addr);
}
static uae_u32 REGPARAM3 musashi_bget(uaecptr addr) REGPARAM {
    return m68k_read_memory_8(addr);
}
static void REGPARAM3 musashi_lput(uaecptr addr, uae_u32 l) REGPARAM {
    m68k_write_memory_32(addr, l);
}
static void REGPARAM3 musashi_wput(uaecptr addr, uae_u32 w) REGPARAM {
    m68k_write_memory_16(addr, w);
}
static void REGPARAM3 musashi_bput(uaecptr addr, uae_u32 b) REGPARAM {
    m68k_write_memory_8(addr, b);
}

/* Direct RAM Bank Handlers */
static uae_u32 REGPARAM3 ram_lget(uaecptr addr) REGPARAM {
    addrbank *bank = &get_mem_bank(addr);
    uae_u8 *p = bank->baseaddr + (addr & bank->mask);
    return do_get_mem_long((uae_u32 *)p);
}

static uae_u32 REGPARAM3 ram_wget(uaecptr addr) REGPARAM {
    addrbank *bank = &get_mem_bank(addr);
    uae_u8 *p = bank->baseaddr + (addr & bank->mask);
    return do_get_mem_word((uae_u16 *)p);
}

static uae_u32 REGPARAM3 ram_bget(uaecptr addr) REGPARAM {
    addrbank *bank = &get_mem_bank(addr);
    return bank->baseaddr[addr & bank->mask];
}

static void REGPARAM3 ram_lput(uaecptr addr, uae_u32 l) REGPARAM {
    addrbank *bank = &get_mem_bank(addr);
    if (!(bank->flags & ABFLAG_ROM)) {
        uae_u8 *p = bank->baseaddr + (addr & bank->mask);
        do_put_mem_long((uae_u32 *)p, l);
    }
}

static void REGPARAM3 ram_wput(uaecptr addr, uae_u32 w) REGPARAM {
    addrbank *bank = &get_mem_bank(addr);
    if (!(bank->flags & ABFLAG_ROM)) {
        uae_u8 *p = bank->baseaddr + (addr & bank->mask);
        do_put_mem_word((uae_u16 *)p, w);
    }
}

static void REGPARAM3 ram_bput(uaecptr addr, uae_u32 b) REGPARAM {
    addrbank *bank = &get_mem_bank(addr);
    if (!(bank->flags & ABFLAG_ROM)) {
        bank->baseaddr[addr & bank->mask] = (uae_u8)b;
    }
}

static int REGPARAM3 ram_check(uaecptr addr, uae_u32 size) REGPARAM {
    (void)addr; (void)size;
    return 1;
}

static uae_u8 *REGPARAM3 ram_xlate(uaecptr addr) REGPARAM {
    addrbank *bank = &get_mem_bank(addr);
    return bank->baseaddr + (addr & bank->mask);
}

/* Custom Bank Handlers */
static uae_u32 REGPARAM3 custom_lget(uaecptr addr) REGPARAM {
    addrbank *bank = &get_mem_bank(addr);
    if (bank->r32)
        return bank->r32(bank->userdata, addr);
    if (bank->r16) {
        uae_u32 hi = bank->r16(bank->userdata, addr);
        uae_u32 lo = bank->r16(bank->userdata, addr + 2);
        return (hi << 16) | (lo & 0xFFFF);
    }
    if (bank->r8) {
        uae_u32 b0 = bank->r8(bank->userdata, addr);
        uae_u32 b1 = bank->r8(bank->userdata, addr + 1);
        uae_u32 b2 = bank->r8(bank->userdata, addr + 2);
        uae_u32 b3 = bank->r8(bank->userdata, addr + 3);
        return (b0 << 24) | (b1 << 16) | (b2 << 8) | b3;
    }
    return 0xFFFFFFFF;
}

static uae_u32 REGPARAM3 custom_wget(uaecptr addr) REGPARAM {
    addrbank *bank = &get_mem_bank(addr);
    if (bank->r16)
        return bank->r16(bank->userdata, addr);
    if (bank->r8) {
        uae_u32 hi = bank->r8(bank->userdata, addr);
        uae_u32 lo = bank->r8(bank->userdata, addr + 1);
        return (hi << 8) | (lo & 0xFF);
    }
    return 0xFFFF;
}

static uae_u32 REGPARAM3 custom_bget(uaecptr addr) REGPARAM {
    addrbank *bank = &get_mem_bank(addr);
    if (bank->r8)
        return bank->r8(bank->userdata, addr);
    return 0xFF;
}

static void REGPARAM3 custom_lput(uaecptr addr, uae_u32 l) REGPARAM {
    addrbank *bank = &get_mem_bank(addr);
    if (bank->w32) {
        bank->w32(bank->userdata, addr, l);
    } else if (bank->w16) {
        bank->w16(bank->userdata, addr, (l >> 16) & 0xFFFF);
        bank->w16(bank->userdata, addr + 2, l & 0xFFFF);
    } else if (bank->w8) {
        bank->w8(bank->userdata, addr, (l >> 24) & 0xFF);
        bank->w8(bank->userdata, addr + 1, (l >> 16) & 0xFF);
        bank->w8(bank->userdata, addr + 2, (l >> 8) & 0xFF);
        bank->w8(bank->userdata, addr + 3, l & 0xFF);
    }
}

static void REGPARAM3 custom_wput(uaecptr addr, uae_u32 w) REGPARAM {
    addrbank *bank = &get_mem_bank(addr);
    if (bank->w16) {
        bank->w16(bank->userdata, addr, w & 0xFFFF);
    } else if (bank->w8) {
        bank->w8(bank->userdata, addr, (w >> 8) & 0xFF);
        bank->w8(bank->userdata, addr + 1, w & 0xFF);
    }
}

static void REGPARAM3 custom_bput(uaecptr addr, uae_u32 b) REGPARAM {
    addrbank *bank = &get_mem_bank(addr);
    if (bank->w8)
        bank->w8(bank->userdata, addr, b & 0xFF);
}

/* Test Device Bank (Musashi / m68k-rs Test Device protocol) */
static void handle_test_device_write(uaecptr offset, uae_u32 val) {
    switch (offset & 0xFF) {
        case 0x00:
            g_test_device.fail_count++;
            break;
        case 0x04:
            g_test_device.pass_count++;
            break;
        case 0x0C:
            g_test_device.interrupt_level = val & 0x7;
            pending_irq_level = val & 0x7;
            break;
        case 0x14:
            putchar(val & 0xFF);
            fflush(stdout);
            break;
        default:
            break;
    }
}

static uae_u32 REGPARAM3 testdev_lget(uaecptr addr) REGPARAM {
    (void)addr; return 0;
}
static uae_u32 REGPARAM3 testdev_wget(uaecptr addr) REGPARAM {
    (void)addr; return 0;
}
static uae_u32 REGPARAM3 testdev_bget(uaecptr addr) REGPARAM {
    (void)addr; return 0;
}
static void REGPARAM3 testdev_lput(uaecptr addr, uae_u32 l) REGPARAM {
    handle_test_device_write(addr & 0xFF, l);
}
static void REGPARAM3 testdev_wput(uaecptr addr, uae_u32 w) REGPARAM {
    handle_test_device_write(addr & 0xFF, w);
}
static void REGPARAM3 testdev_bput(uaecptr addr, uae_u32 b) REGPARAM {
    handle_test_device_write(addr & 0xFF, b);
}

void memory_init(void) {
    memset(&dummy_bank, 0, sizeof(dummy_bank));
    dummy_bank.name = "dummy";
    dummy_bank.lget = dummy_lget;
    dummy_bank.wget = dummy_wget;
    dummy_bank.bget = dummy_bget;
    dummy_bank.lput = dummy_lput;
    dummy_bank.wput = dummy_wput;
    dummy_bank.bput = dummy_bput;
    dummy_bank.xlateaddr = dummy_xlate;
    dummy_bank.check = dummy_check;
    dummy_bank.flags = ABFLAG_NONE;

    memset(&musashi_bridge_bank, 0, sizeof(musashi_bridge_bank));
    musashi_bridge_bank.name = "musashi";
    musashi_bridge_bank.lget = musashi_lget;
    musashi_bridge_bank.wget = musashi_wget;
    musashi_bridge_bank.bget = musashi_bget;
    musashi_bridge_bank.lput = musashi_lput;
    musashi_bridge_bank.wput = musashi_wput;
    musashi_bridge_bank.bput = musashi_bput;
    musashi_bridge_bank.xlateaddr = dummy_xlate;
    musashi_bridge_bank.check = dummy_check;
    musashi_bridge_bank.flags = ABFLAG_NONE;

    memset(&test_device_bank, 0, sizeof(test_device_bank));
    test_device_bank.name = "testdev";
    test_device_bank.lget = testdev_lget;
    test_device_bank.wget = testdev_wget;
    test_device_bank.bget = testdev_bget;
    test_device_bank.lput = testdev_lput;
    test_device_bank.wput = testdev_wput;
    test_device_bank.bput = testdev_bput;
    test_device_bank.xlateaddr = dummy_xlate;
    test_device_bank.check = dummy_check;
    test_device_bank.flags = ABFLAG_IO;

    for (int i = 0; i < MEMORY_BANKS; i++) {
        mem_banks[i] = &dummy_bank;
        ce_cachable[i] = 0;
        ce_banktype[i] = CE_MEMBANK_NONE;
    }

    g_test_device.pass_count = 0;
    g_test_device.fail_count = 0;
    g_test_device.interrupt_level = 0;
}

void memory_reset(void) {
    g_test_device.pass_count = 0;
    g_test_device.fail_count = 0;
    g_test_device.interrupt_level = 0;
}

void memory_uninit(void) {
    for (int i = 0; i < MEMORY_BANKS; i++) {
        addrbank *bank = mem_banks[i];
        if (bank && bank != &dummy_bank &&
            bank != &musashi_bridge_bank &&
            bank != &test_device_bank) {
            for (int j = i + 1; j < MEMORY_BANKS; j++) {
                if (mem_banks[j] == bank) {
                    mem_banks[j] = &dummy_bank;
                }
            }
            free(bank);
        }
        mem_banks[i] = &dummy_bank;
    }
}

void map_banks(addrbank *bank, int start, int size, int realsize) {
    (void)realsize;
    for (int i = 0; i < size; i++) {
        int bank_idx = (start + i) & 0xFFFF;
        mem_banks[bank_idx] = bank;
    }
}

int memory_map_ptr(uint32_t start_addr, uint32_t size, uint8_t *host_ptr, uint32_t flags) {
    int start_bank = (start_addr >> 16) & 0xFFFF;
    int num_banks = ((size + 0xFFFF) >> 16);

    addrbank *bank = (addrbank *)calloc(1, sizeof(addrbank));
    if (!bank) return -1;

    bank->name = (flags & UAE_MEM_ROM) ? "ROM" : "RAM";
    bank->lget = ram_lget;
    bank->wget = ram_wget;
    bank->bget = ram_bget;
    bank->lput = ram_lput;
    bank->wput = ram_wput;
    bank->bput = ram_bput;
    bank->xlateaddr = ram_xlate;
    bank->check = ram_check;
    bank->baseaddr = host_ptr;
    bank->mask = size - 1;
    bank->flags = ABFLAG_RAM | ((flags & UAE_MEM_ROM) ? ABFLAG_ROM : 0);

    for (int i = 0; i < num_banks; i++) {
        int idx = (start_bank + i) & 0xFFFF;
        mem_banks[idx] = bank;
        ce_cachable[idx] = (flags & UAE_MEM_CACHEABLE) ? 1 : 0;
        ce_banktype[idx] = CE_MEMBANK_FAST32;
    }
    return 0;
}

int memory_map_custom(uint32_t start_addr, uint32_t size,
                      uae_read8_fn r8, uae_read16_fn r16, uae_read32_fn r32,
                      uae_write8_fn w8, uae_write16_fn w16, uae_write32_fn w32,
                      void *userdata) {
    int start_bank = (start_addr >> 16) & 0xFFFF;
    int num_banks = ((size + 0xFFFF) >> 16);

    addrbank *bank = (addrbank *)calloc(1, sizeof(addrbank));
    if (!bank) return -1;

    bank->name = "custom";
    bank->lget = custom_lget;
    bank->wget = custom_wget;
    bank->bget = custom_bget;
    bank->lput = custom_lput;
    bank->wput = custom_wput;
    bank->bput = custom_bput;
    bank->xlateaddr = dummy_xlate;
    bank->check = dummy_check;
    bank->flags = ABFLAG_IO;
    bank->r8 = r8;
    bank->r16 = r16;
    bank->r32 = r32;
    bank->w8 = w8;
    bank->w16 = w16;
    bank->w32 = w32;
    bank->userdata = userdata;

    for (int i = 0; i < num_banks; i++) {
        int idx = (start_bank + i) & 0xFFFF;
        mem_banks[idx] = bank;
    }
    return 0;
}

void memory_unmap(uint32_t start_addr, uint32_t size) {
    int start_bank = (start_addr >> 16) & 0xFFFF;
    int num_banks = ((size + 0xFFFF) >> 16);
    addrbank *bank_to_free = NULL;
    if (num_banks > 0) {
        int idx = start_bank & 0xFFFF;
        addrbank *b = mem_banks[idx];
        if (b && b != &dummy_bank && b != &musashi_bridge_bank && b != &test_device_bank) {
            bank_to_free = b;
        }
    }
    for (int i = 0; i < num_banks; i++) {
        int idx = (start_bank + i) & 0xFFFF;
        mem_banks[idx] = &dummy_bank;
        ce_cachable[idx] = 0;
        ce_banktype[idx] = CE_MEMBANK_NONE;
    }
    if (bank_to_free) {
        for (int i = 0; i < MEMORY_BANKS; i++) {
            if (mem_banks[i] == bank_to_free) {
                bank_to_free = NULL;
                break;
            }
        }
        if (bank_to_free) {
            free(bank_to_free);
        }
    }
}
