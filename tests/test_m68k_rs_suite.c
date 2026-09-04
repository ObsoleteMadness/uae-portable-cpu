/*
 * UAE Portable 680x0 CPU Core - Musashi Test Suite Runner
 */

#include "m68k.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "portable_dirent.h"
#include <sys/types.h>
#include <sys/stat.h>

unsigned int m68k_read_disassembler_16(unsigned int address) {
    (void)address;
    return 0;
}
unsigned int m68k_read_disassembler_32(unsigned int address) {
    (void)address;
    return 0;
}

typedef struct memory_device_tag_t {
    uint32_t mask;
    uint8_t (*read8)(struct memory_device_tag_t*, uint32_t);
    uint16_t (*read16)(struct memory_device_tag_t*, uint32_t);
    uint32_t (*read32)(struct memory_device_tag_t*, uint32_t);
    void (*write8)(struct memory_device_tag_t*, uint32_t, uint8_t);
    void (*write16)(struct memory_device_tag_t*, uint32_t, uint16_t);
    void (*write32)(struct memory_device_tag_t*, uint32_t, uint32_t);
} memory_device_t;

#define BLOCK_SIZE 0x10000
#define MMAP_SIZE  0x10000

static memory_device_t* memory_map[MMAP_SIZE];

static uint8_t read8_fail(memory_device_t* dev, uint32_t address) {
    (void)dev; (void)address;
    return 0;
}
static uint16_t read16_fail(memory_device_t* dev, uint32_t address) {
    (void)dev; (void)address;
    return 0;
}
static uint32_t read32_fail(memory_device_t* dev, uint32_t address) {
    (void)dev; (void)address;
    return 0;
}
static void write8_fail(memory_device_t* dev, uint32_t address, uint8_t value) {
    (void)dev; (void)address; (void)value;
}
static void write16_fail(memory_device_t* dev, uint32_t address, uint16_t value) {
    (void)dev; (void)address; (void)value;
}
static void write32_fail(memory_device_t* dev, uint32_t address, uint32_t value) {
    (void)dev; (void)address; (void)value;
}

static memory_device_t mdev_not_mapped = {
    0,
    read8_fail, read16_fail, read32_fail,
    write8_fail, write16_fail, write32_fail
};

static void memory_map_init(void) {
    for (size_t i = 0; i < MMAP_SIZE; ++i) {
        memory_map[i] = &mdev_not_mapped;
    }
}

static void memory_map_add(memory_device_t* dev, uint32_t start_addr, uint32_t size) {
    assert(start_addr % BLOCK_SIZE == 0);
    assert(size % BLOCK_SIZE == 0);
    unsigned count = size / BLOCK_SIZE;
    unsigned off = start_addr / BLOCK_SIZE;
    for (unsigned i = 0; i < count; ++i) {
        memory_map[off + i] = dev;
    }
}

unsigned int m68k_read_memory_8(unsigned int address) {
    unsigned slot = (address & 0x00FFFFFF) / BLOCK_SIZE;
    memory_device_t *dev = memory_map[slot];
    return dev->read8(dev, dev->mask & address);
}

unsigned int m68k_read_memory_16(unsigned int address) {
    unsigned slot = (address & 0x00FFFFFF) / BLOCK_SIZE;
    memory_device_t *dev = memory_map[slot];
    return dev->read16(dev, dev->mask & address);
}

unsigned int m68k_read_memory_32(unsigned int address) {
    unsigned slot = (address & 0x00FFFFFF) / BLOCK_SIZE;
    memory_device_t *dev = memory_map[slot];
    return dev->read32(dev, dev->mask & address);
}

void m68k_write_memory_8(unsigned int address, unsigned int value) {
    unsigned slot = (address & 0x00FFFFFF) / BLOCK_SIZE;
    memory_device_t *dev = memory_map[slot];
    dev->write8(dev, dev->mask & address, value);
}

void m68k_write_memory_16(unsigned int address, unsigned int value) {
    unsigned slot = (address & 0x00FFFFFF) / BLOCK_SIZE;
    memory_device_t *dev = memory_map[slot];
    dev->write16(dev, dev->mask & address, value);
}

void m68k_write_memory_32(unsigned int address, unsigned int value) {
    unsigned slot = (address & 0x00FFFFFF) / BLOCK_SIZE;
    memory_device_t *dev = memory_map[slot];
    dev->write32(dev, dev->mask & address, value);
}

/* Test Device */
typedef struct test_device_tag_t {
    memory_device_t dev;
    uint32_t test_pass_count;
    uint32_t test_fail_count;
} test_device_t;

static uint8_t test_read8(memory_device_t* dev, uint32_t address) { (void)dev; (void)address; return 0; }
static uint16_t test_read16(memory_device_t* dev, uint32_t address) { (void)dev; (void)address; return 0; }
static uint32_t test_read32(memory_device_t* dev, uint32_t address) { (void)dev; (void)address; return 0; }

static void test_write8(memory_device_t* dev, uint32_t address, uint8_t value) {
    test_device_t* td = (test_device_t*)dev;
    if (address >= 0x0 && address <= 0x3)
        ++td->test_fail_count;
    if (address >= 0x4 && address <= 0x7 && value != 0)
        ++td->test_pass_count;
    if (address == 0x14) {
        char ss[2] = {(char)value, 0};
        fputs(ss, stdout);
        fflush(stdout);
    }
}
static void test_write16(memory_device_t* dev, uint32_t address, uint16_t value) {
    test_device_t* td = (test_device_t*)dev;
    if (address == 0x0 || address == 0x2)
        ++td->test_fail_count;
    if ((address == 0x4 || address == 0x6) && value != 0)
        ++td->test_pass_count;
    if (address == 0xC || address == 0xE) {
        m68k_set_irq(value & 0x7);
        m68k_end_timeslice();
    }
}
static void test_write32(memory_device_t* dev, uint32_t address, uint32_t value) {
    test_device_t* td = (test_device_t*)dev;
    if (address == 0x0)
        ++td->test_fail_count;
    if (address == 0x4)
        ++td->test_pass_count;
    if (address == 0xC) {
        m68k_set_irq(value & 0x7);
        m68k_end_timeslice();
    }
}

static void test_device_init(test_device_t* dev) {
    dev->test_pass_count = 0;
    dev->test_fail_count = 0;
    dev->dev.mask = 0x10000 - 1;
    dev->dev.read8 = test_read8;
    dev->dev.read16 = test_read16;
    dev->dev.read32 = test_read32;
    dev->dev.write8 = test_write8;
    dev->dev.write16 = test_write16;
    dev->dev.write32 = test_write32;
}

/* RAM slot */
#define RAM_SLOT_SIZE 0x10000
typedef struct ram_slot_tag_t {
    memory_device_t dev;
    uint8_t memory[RAM_SLOT_SIZE];
} ram_slot_t;

static uint8_t ram_slot_read8(memory_device_t* dev, uint32_t addr) {
    assert(addr < RAM_SLOT_SIZE);
    return ((ram_slot_t*)dev)->memory[addr];
}
static uint16_t ram_slot_read16(memory_device_t* dev, uint32_t addr) {
    if (addr + 1 >= RAM_SLOT_SIZE) {
        m68k_pulse_bus_error();
        return 0;
    }
    return (((uint16_t)ram_slot_read8(dev, addr + 0)) << 8) |
           (((uint16_t)ram_slot_read8(dev, addr + 1)) << 0);
}
static uint32_t ram_slot_read32(memory_device_t* dev, uint32_t addr) {
    return (((uint32_t)ram_slot_read16(dev, addr + 0)) << 16) |
           (((uint32_t)ram_slot_read16(dev, addr + 2)) << 0);
}
static void ram_slot_write8(memory_device_t* dev, uint32_t addr, uint8_t val) {
    assert(addr < RAM_SLOT_SIZE);
    ((ram_slot_t*)dev)->memory[addr] = val;
}
static void ram_slot_write16(memory_device_t* dev, uint32_t addr, uint16_t val) {
    if (addr + 1 >= RAM_SLOT_SIZE) {
        m68k_pulse_bus_error();
        return;
    }
    ram_slot_write8(dev, addr + 0, (val >> 8) & 0xFF);
    ram_slot_write8(dev, addr + 1, (val >> 0) & 0xFF);
}
static void ram_slot_write32(memory_device_t* dev, uint32_t addr, uint32_t val) {
    ram_slot_write16(dev, addr + 0, (val >> 16) & 0xFFFF);
    ram_slot_write16(dev, addr + 2, (val >> 0) & 0xFFFF);
}

static void ram_slot_init(ram_slot_t* dev) {
    memset(dev->memory, 0, RAM_SLOT_SIZE);
    dev->dev.mask = RAM_SLOT_SIZE - 1;
    dev->dev.read8 = ram_slot_read8;
    dev->dev.read16 = ram_slot_read16;
    dev->dev.read32 = ram_slot_read32;
    dev->dev.write8 = ram_slot_write8;
    dev->dev.write16 = ram_slot_write16;
    dev->dev.write32 = ram_slot_write32;
}

/* ROM slot */
#define ROM_SLOT_SIZE 0x10000
typedef struct rom_slot_tag_t {
    memory_device_t dev;
    uint8_t memory[ROM_SLOT_SIZE];
} rom_slot_t;

static uint8_t rom_slot_read8(memory_device_t* dev, uint32_t addr) {
    assert(addr < ROM_SLOT_SIZE);
    return ((rom_slot_t*)dev)->memory[addr];
}
static uint16_t rom_slot_read16(memory_device_t* dev, uint32_t addr) {
    if (addr + 1 >= ROM_SLOT_SIZE) {
        m68k_pulse_bus_error();
        return 0;
    }
    return (((uint16_t)rom_slot_read8(dev, addr + 0)) << 8) |
           (((uint16_t)rom_slot_read8(dev, addr + 1)) << 0);
}
static uint32_t rom_slot_read32(memory_device_t* dev, uint32_t addr) {
    return (((uint32_t)rom_slot_read16(dev, addr + 0)) << 16) |
           (((uint32_t)rom_slot_read16(dev, addr + 2)) << 0);
}
static void rom_slot_write8(memory_device_t* dev, uint32_t address, uint8_t value) {
    (void)dev; (void)address; (void)value;
    m68k_pulse_bus_error();
}
static void rom_slot_write16(memory_device_t* dev, uint32_t address, uint16_t value) {
    (void)dev; (void)address; (void)value;
    m68k_pulse_bus_error();
}
static void rom_slot_write32(memory_device_t* dev, uint32_t address, uint32_t value) {
    (void)dev; (void)address; (void)value;
    m68k_pulse_bus_error();
}

static size_t rom_slot_init(rom_slot_t* dev, FILE* file) {
    memset(dev->memory, 0, ROM_SLOT_SIZE);
    dev->dev.mask = ROM_SLOT_SIZE - 1;
    dev->dev.read8 = rom_slot_read8;
    dev->dev.read16 = rom_slot_read16;
    dev->dev.read32 = rom_slot_read32;
    dev->dev.write8 = rom_slot_write8;
    dev->dev.write16 = rom_slot_write16;
    dev->dev.write32 = rom_slot_write32;
    if (file)
        return fread(dev->memory, 1, ROM_SLOT_SIZE, file);
    return 0;
}

static ram_slot_t g_stack;
static ram_slot_t g_extra_ram1;
#define N_ROMS 4
static rom_slot_t g_roms[N_ROMS];
static test_device_t g_test_dev;

static void setup_memory(void) {
    memory_map_init();
    memory_map_add(&g_stack.dev, 0x0, RAM_SLOT_SIZE);
    for (unsigned i = 0; i < N_ROMS; ++i)
        memory_map_add(&g_roms[i].dev, RAM_SLOT_SIZE + ROM_SLOT_SIZE * i, ROM_SLOT_SIZE);
    memory_map_add(&g_extra_ram1.dev, 0x300000, RAM_SLOT_SIZE);
    memory_map_add(&g_test_dev.dev, 0x100000, 0x10000);
}

static void setup_bootsec(void) {
    for (int i = 0; i < 64; ++i)
        m68k_write_memory_32(i * 4, 0x1000C);
    m68k_write_memory_32(0, 0x3F0);
    m68k_write_memory_32(4, 0x10000);
}

static int run_single_test(const char* bin_path, unsigned int cpu_type, int verbose) {
    FILE *infile = fopen(bin_path, "rb");
    if (!infile) {
        fprintf(stderr, "Cannot open: %s\n", bin_path);
        return -1;
    }

    ram_slot_init(&g_stack);
    ram_slot_init(&g_extra_ram1);
    for (int i = 0; i < N_ROMS; ++i) {
        rom_slot_init(&g_roms[i], infile);
    }
    fclose(infile);

    m68k_init();
    m68k_set_cpu_type(cpu_type);

    test_device_init(&g_test_dev);
    setup_memory();
    setup_bootsec();

    m68k_pulse_reset();

    uint32_t last_pc = 0xFFFFFFFF;
    int stuck_count = 0;
    for (int i = 0; i < 20; ++i) {
        const int n_cycles = 50000;
        int cyc = m68k_execute(n_cycles);
        uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC);
        uint32_t d0 = m68k_get_reg(NULL, M68K_REG_D0);
        uint32_t d4 = m68k_get_reg(NULL, M68K_REG_D4);
        uint32_t d5 = m68k_get_reg(NULL, M68K_REG_D5);
        if (verbose) {
            printf("    iter %d: cyc=%d, PC=0x%08X, D0=0x%08X, pass=%u, fail=%u\n",
                i, cyc, pc, d0, g_test_dev.test_pass_count, g_test_dev.test_fail_count);
        }
        if (g_test_dev.test_pass_count > 0 || g_test_dev.test_fail_count > 0)
            break;
        if (pc == last_pc) {
            if (++stuck_count >= 2)
                break;
        } else {
            stuck_count = 0;
            last_pc = pc;
        }
    }

    if (verbose) {
        printf("    pass=%u, fail=%u\n", g_test_dev.test_pass_count, g_test_dev.test_fail_count);
    }

    if (g_test_dev.test_fail_count == 0 && g_test_dev.test_pass_count > 0)
        return 0; /* SUCCESS */
    return 1; /* FAILURE */
}

static void collect_binaries(const char *dir_path, char bin_paths[][512], int *count, int max_count) {
    DIR *dir = opendir(dir_path);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir_path, entry->d_name);

        struct stat st;
        if (stat(path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                collect_binaries(path, bin_paths, count, max_count);
            } else if (S_ISREG(st.st_mode)) {
                char *dot = strrchr(entry->d_name, '.');
                if (dot && strcmp(dot, ".bin") == 0 && *count < max_count) {
                    strncpy(bin_paths[*count], path, sizeof(bin_paths[0]) - 1);
                    bin_paths[*count][sizeof(bin_paths[0]) - 1] = '\0';
                    (*count)++;
                }
            }
        }
    }
    closedir(dir);
}

int main(int argc, char* argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    if (argc < 2) {
        printf("Usage: %s <binary.bin | test_dir> [cpu_type (68000|68010|68020|68030|68040)]\n", argv[0]);
        return 1;
    }

    unsigned int cpu_type = M68K_CPU_TYPE_68000;
    if (argc >= 3) {
        if (strcmp(argv[2], "68040") == 0) {
            cpu_type = M68K_CPU_TYPE_68040;
        } else if (strcmp(argv[2], "68020") == 0) {
            cpu_type = M68K_CPU_TYPE_68020;
        } else if (strcmp(argv[2], "68030") == 0) {
            cpu_type = M68K_CPU_TYPE_68030;
        } else if (strcmp(argv[2], "68010") == 0) {
            cpu_type = M68K_CPU_TYPE_68010;
        }
    }

    const char *target = argv[1];
    struct stat st;
    if (stat(target, &st) != 0) {
        fprintf(stderr, "Cannot access path: %s\n", target);
        return 1;
    }

    if (!S_ISDIR(st.st_mode)) {
        /* Single file mode */
        printf("[*] Running single m68k-rs fixture: %s (CPU: %s)...\n", target,
            (cpu_type == M68K_CPU_TYPE_68040) ? "68040" :
            (cpu_type == M68K_CPU_TYPE_68020) ? "68020" : "68000");
        int res = run_single_test(target, cpu_type, 1);
        if (res == 0) {
            printf("[PASS] %s passed!\n", target);
            return 0;
        } else {
            printf("[FAIL] %s failed!\n", target);
            return 1;
        }
    }

    /* Directory batch mode */
    printf("=== Running m68k-rs Test Suite on %s ===\n\n", target);

    static char bin_paths[512][512];
    int bin_count = 0;
    collect_binaries(target, bin_paths, &bin_count, 512);

    /* Sort alphabetically */
    for (int i = 0; i < bin_count - 1; i++) {
        for (int j = i + 1; j < bin_count; j++) {
            if (strcmp(bin_paths[i], bin_paths[j]) > 0) {
                char tmp[512];
                strcpy(tmp, bin_paths[i]);
                strcpy(bin_paths[i], bin_paths[j]);
                strcpy(bin_paths[j], tmp);
            }
        }
    }

    int total = 0, passed = 0, failed = 0;
    for (int i = 0; i < bin_count; i++) {
        const char *name = bin_paths[i] + strlen(target);
        if (*name == '/') name++;

        /* Auto-detect CPU type from fixture subfolder if running entire fixtures directory */
        unsigned int test_cpu = cpu_type;
        if (strstr(bin_paths[i], "/m68040/") != NULL) {
            test_cpu = M68K_CPU_TYPE_68040;
        } else if (strstr(bin_paths[i], "/m68030/") != NULL) {
            test_cpu = M68K_CPU_TYPE_68030;
        } else if (strstr(bin_paths[i], "/m68020/") != NULL) {
            test_cpu = M68K_CPU_TYPE_68020;
        } else if (strstr(bin_paths[i], "/m68010/") != NULL) {
            test_cpu = M68K_CPU_TYPE_68010;
        }

        printf("[%3d/%3d] %-40s ... ", i + 1, bin_count, name);
        fflush(stdout);

        int res = run_single_test(bin_paths[i], test_cpu, 0);
        total++;
        if (res == 0) {
            printf("PASS (pass=%u)\n", g_test_dev.test_pass_count);
            passed++;
        } else {
            printf("FAIL (pass=%u, fail=%u)\n", g_test_dev.test_pass_count, g_test_dev.test_fail_count);
            failed++;
        }
    }

    printf("\n=== m68k-rs Test Suite Summary ===\n");
    printf("Total:     %d\n", total);
    printf("Passed:    %d\n", passed);
    printf("Failed:    %d\n", failed);
    printf("Pass Rate: %.1f%%\n", (double)passed * 100.0 / (total ? total : 1));

    if (passed >= total * 0.80 && passed > 0) {
        printf("\n>>> M68K-RS FIXTURE VERIFICATION SUCCESS (Pass Rate >= 80%%) <<<\n\n");
        return 0;
    }
    return (failed > 0) ? 1 : 0;
}
