#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "m68k.h"
#include "uae_cpu.h"

static uint8_t g_ram[0x100000];

unsigned int m68k_read_memory_8(unsigned int address) {
    address &= 0xFFFFFF;
    if (address < sizeof(g_ram))
        return g_ram[address];
    return 0;
}

unsigned int m68k_read_memory_16(unsigned int address) {
    address &= 0xFFFFFF;
    if (address + 1 < sizeof(g_ram))
        return (g_ram[address] << 8) | g_ram[address + 1];
    return 0;
}

unsigned int m68k_read_memory_32(unsigned int address) {
    address &= 0xFFFFFF;
    if (address + 3 < sizeof(g_ram))
        return (g_ram[address] << 24) | (g_ram[address + 1] << 16) |
               (g_ram[address + 2] << 8) | g_ram[address + 3];
    return 0;
}

void m68k_write_memory_8(unsigned int address, unsigned int value) {
    address &= 0xFFFFFF;
    if (address < sizeof(g_ram))
        g_ram[address] = value & 0xFF;
}

void m68k_write_memory_16(unsigned int address, unsigned int value) {
    address &= 0xFFFFFF;
    if (address + 1 < sizeof(g_ram)) {
        g_ram[address]     = (value >> 8) & 0xFF;
        g_ram[address + 1] = value & 0xFF;
    }
}

void m68k_write_memory_32(unsigned int address, unsigned int value) {
    address &= 0xFFFFFF;
    if (address + 3 < sizeof(g_ram)) {
        g_ram[address]     = (value >> 24) & 0xFF;
        g_ram[address + 1] = (value >> 16) & 0xFF;
        g_ram[address + 2] = (value >> 8) & 0xFF;
        g_ram[address + 3] = value & 0xFF;
    }
}

static void test_musashi_api(void) {
    fprintf(stdout, "[*] Testing Musashi C API Compatibility...\n");
    memset(g_ram, 0, sizeof(g_ram));

    // Reset Vector at 0x000000:
    // SSP = 0x00080000
    // PC  = 0x00001000
    m68k_write_memory_32(0x0000, 0x00080000);
    m68k_write_memory_32(0x0004, 0x00001000);

    // Code at 0x00001000:
    // 1000: MOVE.L #0x12345678, D0 -> 203C 1234 5678
    // 1006: MOVE.L #0x00000010, D1 -> 223C 0000 0010
    // 100C: ADD.L D1, D0           -> D081
    // 100E: SUBQ.L #1, D0          -> 5380
    // 1010: NOP                    -> 4E71
    uint32_t pc = 0x1000;
    m68k_write_memory_16(pc + 0,  0x203C);
    m68k_write_memory_16(pc + 2,  0x1234);
    m68k_write_memory_16(pc + 4,  0x5678);
    m68k_write_memory_16(pc + 6,  0x223C);
    m68k_write_memory_16(pc + 8,  0x0000);
    m68k_write_memory_16(pc + 10, 0x0010);
    m68k_write_memory_16(pc + 12, 0xD081);
    m68k_write_memory_16(pc + 14, 0x5380);
    m68k_write_memory_16(pc + 16, 0x4E71);

    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_pulse_reset();

    uint32_t init_pc = m68k_get_reg(NULL, M68K_REG_PC);
    assert(init_pc == 0x1000);

    int cycles_used = m68k_execute(200);
    fprintf(stdout, "    Executed instructions, cycles used: %d\n", cycles_used);

    uint32_t d0 = m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t d1 = m68k_get_reg(NULL, M68K_REG_D1);
    uint32_t end_pc = m68k_get_reg(NULL, M68K_REG_PC);

    fprintf(stdout, "    D0 = 0x%08X (expected 0x12345687)\n", d0);
    fprintf(stdout, "    D1 = 0x%08X (expected 0x00000010)\n", d1);
    fprintf(stdout, "    PC = 0x%08X\n", end_pc);

    assert(d0 == 0x12345687);
    assert(d1 == 0x00000010);
    fprintf(stdout, "    [PASS] Musashi API test passed successfully!\n\n");
}

static void test_uae_cpu_api(void) {
    fprintf(stdout, "[*] Testing uae_cpu Multi-Instance Context API...\n");

    uae_cpu_global_init();

    uae_cpu_config_t config;
    memset(&config, 0, sizeof(config));
    config.cpu_type = UAE_CPU_TYPE_68020;
    config.fpu_type = UAE_FPU_NONE;
    config.mmu_type = UAE_MMU_NONE;

    uae_cpu_t *cpu = uae_cpu_create(&config);
    assert(cpu != NULL);

    uint8_t *ram = (uint8_t *)calloc(1, 0x100000);
    assert(ram != NULL);

    uae_cpu_map_ram(cpu, 0x00000000, 0x100000, ram);

    *(uint32_t *)&ram[0x0000] = __builtin_bswap32(0x00080000);
    *(uint32_t *)&ram[0x0004] = __builtin_bswap32(0x00002000);

    uint32_t pc = 0x2000;
    *(uint16_t *)&ram[pc + 0] = __builtin_bswap16(0x243C); // MOVE.L #imm, D2
    *(uint16_t *)&ram[pc + 2] = __builtin_bswap16(0xCAFE);
    *(uint16_t *)&ram[pc + 4] = __builtin_bswap16(0xBABE);
    *(uint16_t *)&ram[pc + 6] = __builtin_bswap16(0xE19A); // ROL.L #8, D2 -> 0xFEBABECA
    *(uint16_t *)&ram[pc + 8] = __builtin_bswap16(0x4E71); // NOP

    uae_cpu_reset(cpu);
    assert(uae_cpu_get_pc(cpu) == 0x2000);

    int cycles = uae_cpu_execute(cpu, 100);
    fprintf(stdout, "    Executed instructions, cycles used: %d\n", cycles);

    uint32_t d2 = uae_cpu_get_reg(cpu, UAE_REG_D2);
    fprintf(stdout, "    D2 = 0x%08X (expected 0xFEBABECA)\n", d2);
    assert(d2 == 0xFEBABECA);

    uae_cpu_destroy(cpu);
    free(ram);
    uae_cpu_global_cleanup();

    fprintf(stdout, "    [PASS] uae_cpu context API test passed successfully!\n\n");
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    printf("=== UAE Portable CPU Core Smoke Tests ===\n\n");
    test_musashi_api();
    test_uae_cpu_api();
    printf("=== All basic smoke tests PASSED! ===\n");
    return 0;
}
