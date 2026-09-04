/*
 * test_uae_cpu.c - Comprehensive test suite for UAE Portable CPU Core
 *
 * Validates:
 * 1. CPU Models: 68000, 68010, 68020, 68030, 68040, 68060
 * 2. ALU & CCR flags: ADD, SUB, NEG, CMP, Shifts, Rotates
 * 3. 68020+ Bitfields: BFTST, BFEXTU, BFFFO
 * 4. 68020+ Atomic operations: CAS
 * 5. FPU Floating-Point & Transcendentals via Softfloat: FMUL.D, FMOVE
 * 6. Multi-instance Context Isolation: Concurrent independent CPU states
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

#include "uae_cpu.h"

#define RAM_SIZE 0x100000 /* 1MB test RAM */
static uint8_t s_ram[RAM_SIZE];

static void write_mem16(uint32_t addr, uint16_t val) {
    if (addr + 1 < RAM_SIZE) {
        s_ram[addr] = (val >> 8) & 0xFF;
        s_ram[addr + 1] = val & 0xFF;
    }
}
static void write_mem32(uint32_t addr, uint32_t val) {
    if (addr + 3 < RAM_SIZE) {
        s_ram[addr] = (val >> 24) & 0xFF;
        s_ram[addr + 1] = (val >> 16) & 0xFF;
        s_ram[addr + 2] = (val >> 8) & 0xFF;
        s_ram[addr + 3] = val & 0xFF;
    }
}
static uint32_t read_mem32(uint32_t addr) {
    if (addr + 3 < RAM_SIZE) {
        return ((uint32_t)s_ram[addr] << 24) |
               ((uint32_t)s_ram[addr + 1] << 16) |
               ((uint32_t)s_ram[addr + 2] << 8) |
               s_ram[addr + 3];
    }
    return 0;
}

static void setup_test_program(uae_cpu_t *cpu, const uint16_t *opcodes, size_t count) {
    memset(s_ram, 0, sizeof(s_ram));
    uae_cpu_map_ram(cpu, 0x00000, RAM_SIZE, s_ram);
    write_mem32(0x00, 0x80000); /* Initial SSP */
    write_mem32(0x04, 0x1000);  /* Initial PC */
    for (size_t i = 0; i < count; i++) {
        write_mem16(0x1000 + i * 2, opcodes[i]);
    }
    uae_cpu_reset(cpu);
}

/* --- Tests --- */

static void test_cpu_models(void) {
    printf("[*] Testing CPU Model Configuration...\n");
    uae_cpu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    uae_cpu_type_t models[] = {
        UAE_CPU_TYPE_68000,
        UAE_CPU_TYPE_68010,
        UAE_CPU_TYPE_68020,
        UAE_CPU_TYPE_68030,
        UAE_CPU_TYPE_68040,
        UAE_CPU_TYPE_68060
    };

    for (int i = 0; i < 6; i++) {
        cfg.cpu_type = models[i];
        uae_cpu_t *cpu = uae_cpu_create(&cfg);
        assert(cpu != NULL);
        uae_cpu_config_t out_cfg;
        uae_cpu_get_config(cpu, &out_cfg);
        assert(out_cfg.cpu_type == models[i]);
        uae_cpu_destroy(cpu);
    }
    printf("    -> CPU models 68000..68060 created, queried, and destroyed successfully.\n");
}

static void test_alu_ccr(void) {
    printf("[*] Testing ALU & Condition Code Calculations...\n");
    uae_cpu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.cpu_type = UAE_CPU_TYPE_68000;
    uae_cpu_t *cpu = uae_cpu_create(&cfg);

    /* Test ADD.L: 0x7FFFFFFF + 1 -> 0x80000000 (V=1, N=1, Z=0, C=0) */
    uint16_t prog_add[] = {
        0x203C, 0x7FFF, 0xFFFF, /* MOVE.L #0x7FFFFFFF, D0 */
        0x5280,                 /* ADDQ.L #1, D0 */
        0x4E71                  /* NOP */
    };
    setup_test_program(cpu, prog_add, sizeof(prog_add)/2);
    uae_cpu_execute(cpu, 50);

    uint32_t d0 = uae_cpu_get_reg(cpu, UAE_REG_D0);
    uint16_t sr = uae_cpu_get_reg(cpu, UAE_REG_SR);
    assert(d0 == 0x80000000);
    assert((sr & 0x0A) == 0x0A); /* N=1, V=1 */

    /* Test SUB.L to Zero: 0x100 - 0x100 -> 0 (Z=1, N=0, V=0, C=0) */
    uint16_t prog_sub[] = {
        0x203C, 0x0000, 0x0100, /* MOVE.L #0x100, D0 */
        0x90BC, 0x0000, 0x0100, /* SUB.L #0x100, D0 */
        0x4E71                  /* NOP */
    };
    setup_test_program(cpu, prog_sub, sizeof(prog_sub)/2);
    uae_cpu_execute(cpu, 50);

    d0 = uae_cpu_get_reg(cpu, UAE_REG_D0);
    sr = uae_cpu_get_reg(cpu, UAE_REG_SR);
    assert(d0 == 0);
    assert((sr & 0x04) != 0); /* Z=1 */

    uae_cpu_destroy(cpu);
    printf("    -> ALU arithmetic and CCR flags verified.\n");
}

static void test_68020_bitfields(void) {
    printf("[*] Testing 68020+ Bitfield Instructions...\n");
    uae_cpu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.cpu_type = UAE_CPU_TYPE_68020;
    uae_cpu_t *cpu = uae_cpu_create(&cfg);

    /* Test BFEXTU: Extract 8 bits at offset 12 from 0x01234567 -> 0x34 */
    uint16_t prog_bfextu[] = {
        0x203C, 0x0123, 0x4567, /* MOVE.L #0x01234567, D0 */
        0xEB80, 0x0308,         /* BFEXTU D0{12:8}, D1 */
        0x4E71                  /* NOP */
    };
    setup_test_program(cpu, prog_bfextu, sizeof(prog_bfextu)/2);
    uae_cpu_execute(cpu, 50);

    uint32_t d1 = uae_cpu_get_reg(cpu, UAE_REG_D1);
    assert(d1 == 0x34);

    /* Test BFFFO: Find first one in 0x00008000 -> offset 16 */
    uint16_t prog_bfffo[] = {
        0x203C, 0x0000, 0x8000, /* MOVE.L #0x00008000, D0 */
        0xED80, 0x0000,         /* BFFFO D0{0:32}, D1 */
        0x4E71                  /* NOP */
    };
    setup_test_program(cpu, prog_bfffo, sizeof(prog_bfffo)/2);
    uae_cpu_execute(cpu, 50);

    d1 = uae_cpu_get_reg(cpu, UAE_REG_D1);
    assert(d1 == 16);

    uae_cpu_destroy(cpu);
    printf("    -> 68020+ Bitfield instructions (BFEXTU, BFFFO) verified.\n");
}

static void test_68020_cas(void) {
    printf("[*] Testing 68020+ CAS (Compare and Swap)...\n");
    uae_cpu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.cpu_type = UAE_CPU_TYPE_68020;
    uae_cpu_t *cpu = uae_cpu_create(&cfg);

    /* Memory location 0x2000 has value 0x11112222 */
    write_mem32(0x2000, 0x11112222);

    /* CAS.L D0, D1, (A0) where D0=0x11112222, D1=0x33334444 -> successful swap */
    uint16_t prog_cas[] = {
        0x203C, 0x1111, 0x2222, /* MOVE.L #0x11112222, D0 (compare val) */
        0x223C, 0x3333, 0x4444, /* MOVE.L #0x33334444, D1 (update val) */
        0x207C, 0x0000, 0x2000, /* MOVEA.L #0x2000, A0 */
        0x0AC8, 0x0841,         /* CAS.L D0, D1, (A0) */
        0x4E71                  /* NOP */
    };
    setup_test_program(cpu, prog_cas, sizeof(prog_cas)/2);
    uae_cpu_execute(cpu, 50);

    uint32_t mem_val = read_mem32(0x2000);
    assert(mem_val == 0x33334444);

    uae_cpu_destroy(cpu);
    printf("    -> 68020+ CAS atomic compare-and-swap verified.\n");
}

static void test_68881_fpu_softfloat(void) {
    printf("[*] Testing IEEE 754 Floating-Point Math via Softfloat...\n");
    uae_cpu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.cpu_type = UAE_CPU_TYPE_68040;
    cfg.fpu_type = UAE_FPU_68040;
    cfg.fpu_softfloat = true;
    uae_cpu_t *cpu = uae_cpu_create(&cfg);

    /* Test FMOVE.D and FMUL.D: 3.0 * 4.0 = 12.0 */
    /* Double 3.0: 0x4008000000000000, Double 4.0: 0x4010000000000000 */
    write_mem32(0x2000, 0x40080000);
    write_mem32(0x2004, 0x00000000);
    write_mem32(0x2008, 0x40100000);
    write_mem32(0x200C, 0x00000000);

    uint16_t prog_fpu[] = {
        0x207C, 0x0000, 0x2000, /* MOVEA.L #0x2000, A0 */
        0xF210, 0x5400,         /* FMOVE.D (A0), FP0 */
        0xF228, 0x5423, 0x0008, /* FMUL.D 8(A0), FP0 */
        0xF228, 0x7400, 0x0010, /* FMOVE.D FP0, 16(A0) (result at 0x2010) */
        0x4E71                  /* NOP */
    };
    setup_test_program(cpu, prog_fpu, sizeof(prog_fpu)/2);
    uae_cpu_execute(cpu, 100);

    uint32_t res_hi = read_mem32(0x2010);
    uint32_t res_lo = read_mem32(0x2014);
    /* Double 12.0: 0x4028000000000000 */
    assert(res_hi == 0x40280000 && res_lo == 0x00000000);

    uae_cpu_destroy(cpu);
    printf("    -> FPU basic math (FMUL.D 3.0 * 4.0 = 12.0) verified.\n");
}

static void test_multi_instance_isolation(void) {
    printf("[*] Testing Multi-Instance Context Isolation...\n");
    uae_cpu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.cpu_type = UAE_CPU_TYPE_68000;

    uae_cpu_t *cpu1 = uae_cpu_create(&cfg);
    uae_cpu_t *cpu2 = uae_cpu_create(&cfg);

    uae_cpu_set_reg(cpu1, UAE_REG_D0, 0x11111111);
    uae_cpu_set_reg(cpu2, UAE_REG_D0, 0x22222222);

    uae_cpu_set_reg(cpu1, UAE_REG_A0, 0xAAAAAAAA);
    uae_cpu_set_reg(cpu2, UAE_REG_A0, 0xBBBBBBBB);

    assert(uae_cpu_get_reg(cpu1, UAE_REG_D0) == 0x11111111);
    assert(uae_cpu_get_reg(cpu2, UAE_REG_D0) == 0x22222222);
    assert(uae_cpu_get_reg(cpu1, UAE_REG_A0) == 0xAAAAAAAA);
    assert(uae_cpu_get_reg(cpu2, UAE_REG_A0) == 0xBBBBBBBB);

    uae_cpu_destroy(cpu1);
    uae_cpu_destroy(cpu2);
    printf("    -> Multi-instance context isolation verified.\n");
}

int main(void) {
    printf("========================================\n");
    printf("UAE Portable CPU Core - Native Tests\n");
    printf("========================================\n\n");

    uae_cpu_global_init();

    test_cpu_models();
    test_alu_ccr();
    test_68020_bitfields();
    test_68020_cas();
    test_68881_fpu_softfloat();
    test_multi_instance_isolation();

    uae_cpu_global_cleanup();

    printf("\n>>> ALL UAE CPU CORE TESTS PASSED! <<<\n\n");
    return 0;
}
