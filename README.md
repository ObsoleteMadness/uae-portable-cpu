# UAE Portable 680x0 CPU Core

A standalone, embeddable, and portable Motorola 680x0 CPU emulation engine extracted and modernized from **WinUAE** and **Hatari**. 

All Amiga- and Atari ST-specific hardware dependencies, custom chipset logic, floppy disk controllers, and proprietary memory layout assumptions have been completely stripped. The core provides clean, hardware-agnostic interfaces, IEEE-754 SoftFloat FPU support, 68030/68040 MMU support, JIT compilation backends for ARM64 and x86_64, and dual C APIs.

---

## Key Features

- **Broad CPU Support**: Full cycle-accurate emulation for Motorola 68000, 68010, 68EC020, 68020, 68EC030, 68030, 68EC040, 68LC040, 68040, and 68060.
- **FPU & MMU Emulation**:
  - IEEE-754 compliant SoftFloat FPU supporting 68881, 68882, and integrated 68040/060 FPUs.
  - 68030 and 68040 MMU translation support.
- **JIT Compilation**: ARM64 (AArch64) and x86-64 dynamic translation for 68020+ code, built by default on supported hosts. See [JIT Compiler](#3-jit-compiler).
- **Hardware Agnostic**:
  - Zero peripheral baggage: No Amiga chipset (Agnus/Paula/Denise) or Atari ST hardware (FDC, ACIA, Blitter, YM2149).
  - Flexible memory callbacks: Host applications supply simple read/write callbacks or directly mapped address spaces.
- **Dual C APIs**:
  1. **Musashi-Compatible C API (`include/m68k.h`)**: Drop-in replacement for emulators already using Musashi.
  2. **Multi-Instance Context API (`include/uae_cpu.h`)**: Re-entrant, multi-instance CPU context model inspired by `m68k-rs` for running multiple independent 68k cores concurrently.
- **Host Hooks**: Emulator-neutral hooks for host-trap opcodes, Line-A/Line-F interception, TRAP #n, exception observation, pulled interrupts, busy-wait loops and instruction-aborting bus errors. See [HOST_HOOKS.md](HOST_HOOKS.md).
- **Comprehensive Verification**: Validated against the test suites of Musashi, `m68k-rs`, and native UAE CPU tests.

---

## Directory Structure

```text
uae-portable-cpu/
├── include/                  # Public C/C++ API headers
│   ├── m68k.h                # Musashi-compatible C API
│   └── uae_cpu.h             # Multi-instance context API & definitions
├── src/
│   ├── api/                  # API translation layers
│   │   ├── musashi_api.c     # Implementation of Musashi API
│   │   └── uae_cpu_api.c     # Implementation of multi-instance Context API
│   └── cpu/                  # UAE portable CPU core engine
│       ├── build68k.c        # Opcode table preprocessor generator
│       ├── gencpu.c          # C opcode generator tool
│       ├── newcpu.c / .h     # Core CPU execution loop & instruction decode
│       ├── cpummu.c / 030.c  # MMU emulation
│       ├── fpp.c / softfloat # SoftFloat FPU emulation & math tables
│       ├── memory.c / .h     # Memory interface & callback router
│       ├── uae_glue.c / .h   # Minimal portable hardware glue layer
│       ├── host_hooks.c / .h # Host hook dispatch and re-entrant execute loop
│       └── jit/              # JIT compilers
│           ├── compemu*.cpp  # Architecture dispatchers (C++ with C linkage to the core)
│           ├── arm/          # ARM64 (AArch64) JIT backend
│           └── x86/          # x86-64 JIT backend
├── tests/                    # Test suites & fixtures
│   ├── test_basic.c          # Basic API smoke tests
│   ├── test_uae_cpu.c        # Native UAE core unit tests (68000–68060, FPU, MMU)
│   ├── test_musashi_suite.c  # Musashi 68000/68040 test runner
│   ├── test_m68k_rs_suite.c  # m68k-rs opcode coverage test runner
│   └── fixtures/             # Binary test fixtures (Musashi, m68k-rs, UAE)
├── CMakeLists.txt            # CMake build definition
├── WALKTHROUGH.md            # Detailed architecture walkthrough & gap analysis
├── HOST_HOOKS.md             # Host hook contracts for embedding emulators
└── README.md
```

---

## Building and Running Tests

### Prerequisites
- CMake 3.16 or later
- C/C++ compiler (GCC, Clang, or MSVC with C11 and C++17 support)

### Build Instructions

```bash
# Generate build configuration
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Compile library and test executables
cmake --build build -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)

# Run test suite
ctest --test-dir build --output-on-failure

# Install library and CMake config files
cmake --install build --prefix /usr/local
```

### Build Options

| Option | Default | Effect |
| :--- | :--- | :--- |
| `ENABLE_TESTS` | `ON` | Build the test executables and register them with CTest |
| `UAE_CPU_MUSASHI_API` | `ON` | Build the Musashi-compatible `m68k_*` API. Turn off when the host also links Musashi |
| `UAE_CPU_JIT` | `AUTO` | Build the JIT compiler. `AUTO` enables it on AArch64 and x86-64 with GCC or Clang; `ON` fails the configure elsewhere; `OFF` builds the interpreter only |
| `UAE_CPU_ISOLATE_SYMBOLS` | `OFF` | Also build `uaecpu_isolated`, a static library exporting only the public API (Apple ld or GNU ld + objcopy; not MSVC). Adds the `isolated_link` test |

---

## Package Manager & CMake Integration

### Using via CMake `find_package`

Once installed or added via vcpkg, consume `uae-portable-cpu` in your `CMakeLists.txt`:

```cmake
find_package(uae-portable-cpu CONFIG REQUIRED)

add_executable(my_emulator main.c)
target_link_libraries(my_emulator PRIVATE uae-portable-cpu::uaecpu)
```

### Using via vcpkg Manifest Mode (`vcpkg.json`)

Add `uae-portable-cpu` to your project's `vcpkg.json`:

```json
{
  "dependencies": [
    "uae-portable-cpu"
  ]
}
```

Or consume with an overlay port pointing to `ports/uae-portable-cpu`:

```bash
vcpkg install --overlay-ports=ports/uae-portable-cpu uae-portable-cpu
```

---

## Test Verification Matrix

All test suites pass via `ctest` (12 with the JIT built):

| Test Target | Description | Pass Rate | Status |
| :--- | :--- | :--- | :--- |
| **`test_basic`** | Musashi API & Context API smoke tests | 100% | **PASSED** |
| **`test_uae_cpu`** | Native UAE tests (68000–68060 switching, bitfields, CAS, SoftFloat `FMUL.D`, context isolation) | 100% | **PASSED** |
| **`musashi_68000`** | Musashi 68000 test suite | 55 / 60 (91.7%)* | **PASSED** |
| **`musashi_68040`** | Musashi 68040 test suite | 16 / 18 (88.9%)* | **PASSED** |
| **`m68k_rs_coverage`** | `m68k-rs` comprehensive instruction coverage | 25 / 25 (100%) | **PASSED** |
| **`m68k_rs_extra`** | `m68k-rs` extended instruction test fixtures | 102 / 127 (80.3%) | **PASSED** |
| **`host_hooks_jit`**, **`test_uae_cpu_jit`** | The same suites with 68020+ scenarios translated by the JIT | 100% | **PASSED** |
| **`m68k_rs_coverage_jit`**, **`m68k_rs_extra_jit`**, **`musashi_68040_jit`** | Fixture suites under the JIT (m68k-rs code translated from a flat memory map) | Same as interpreter | **PASSED** |
| **`host_hooks`** | Host hook contracts: traps, Line-A/F, TRAP #n, exceptions, IRQ, DBF spin, bus errors, reserved opcodes, memory flags | 100% | **PASSED** |

*\* For detailed analysis of the subtle differences between Musashi's test fixtures and Motorola silicon behavior (such as BCD arithmetic on invalid non-decimal inputs and division overflow CCR flag status), see [WALKTHROUGH.md](WALKTHROUGH.md).*

---

## Usage Examples

### 1. Musashi Drop-In API (`m68k.h`)

```c
#include "m68k.h"
#include <stdio.h>

static unsigned char ram[0x10000];

unsigned int m68k_read_memory_8(unsigned int address) {
    return ram[address & 0xFFFF];
}
unsigned int m68k_read_memory_16(unsigned int address) {
    return (ram[address & 0xFFFF] << 8) | ram[(address + 1) & 0xFFFF];
}
unsigned int m68k_read_memory_32(unsigned int address) {
    return (m68k_read_memory_16(address) << 16) | m68k_read_memory_16(address + 2);
}
void m68k_write_memory_8(unsigned int address, unsigned int value) {
    ram[address & 0xFFFF] = (unsigned char)value;
}
void m68k_write_memory_16(unsigned int address, unsigned int value) {
    ram[address & 0xFFFF] = (unsigned char)(value >> 8);
    ram[(address + 1) & 0xFFFF] = (unsigned char)value;
}
void m68k_write_memory_32(unsigned int address, unsigned int value) {
    m68k_write_memory_16(address, value >> 16);
    m68k_write_memory_16(address + 2, value & 0xFFFF);
}

int main(void) {
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);

    // Set initial SSP and PC vectors
    m68k_write_memory_32(0x0000, 0x00008000);
    m68k_write_memory_32(0x0004, 0x00001000);

    // Place NOP; NOP at 0x1000
    m68k_write_memory_16(0x1000, 0x4E71);
    m68k_write_memory_16(0x1002, 0x4E71);

    m68k_pulse_reset();
    int cycles_used = m68k_execute(100);

    printf("Executed %d cycles, PC = 0x%08X\n", cycles_used, m68k_get_reg(NULL, M68K_REG_PC));
    return 0;
}
```

### 2. Multi-Instance Context API (`uae_cpu.h`)

```c
#include "uae_cpu.h"
#include <stdio.h>

static uint8_t mem[0x10000];

static uint32_t mem_read(void *userdata, uint32_t addr, int size) {
    (void)userdata;
    addr &= 0xFFFF;
    if (size == 1) return mem[addr];
    if (size == 2) return (mem[addr] << 8) | mem[(addr + 1) & 0xFFFF];
    return (mem[addr] << 24) | (mem[(addr + 1) & 0xFFFF] << 16) |
           (mem[(addr + 2) & 0xFFFF] << 8) | mem[(addr + 3) & 0xFFFF];
}

static void mem_write(void *userdata, uint32_t addr, uint32_t val, int size) {
    (void)userdata;
    addr &= 0xFFFF;
    if (size == 1) {
        mem[addr] = (uint8_t)val;
    } else if (size == 2) {
        mem[addr] = (uint8_t)(val >> 8);
        mem[(addr + 1) & 0xFFFF] = (uint8_t)val;
    } else {
        mem[addr] = (uint8_t)(val >> 24);
        mem[(addr + 1) & 0xFFFF] = (uint8_t)(val >> 16);
        mem[(addr + 2) & 0xFFFF] = (uint8_t)(val >> 8);
        mem[(addr + 3) & 0xFFFF] = (uint8_t)val;
    }
}

int main(void) {
    uae_cpu_config_t cfg = {
        .model = UAE_CPU_68020,
        .fpu_model = UAE_FPU_68882,
        .read_func = mem_read,
        .write_func = mem_write,
        .userdata = NULL
    };

    uae_cpu_t *cpu = uae_cpu_create(&cfg);

    // Setup reset vectors
    mem_write(NULL, 0x0000, 0x00008000, 4);
    mem_write(NULL, 0x0004, 0x00001000, 4);

    // MOVE.L #$12345678, D0 (0x203C 0x1234 0x5678)
    mem_write(NULL, 0x1000, 0x203C, 2);
    mem_write(NULL, 0x1002, 0x12345678, 4);

    uae_cpu_reset(cpu);
    uae_cpu_execute(cpu, 20);

    printf("D0 = 0x%08X, PC = 0x%08X\n",
           uae_cpu_get_dreg(cpu, 0),
           uae_cpu_get_pc(cpu));

    uae_cpu_destroy(cpu);
    return 0;
}
```

### 3. JIT Compiler

When the library is built with `UAE_CPU_JIT`, 68020 and later CPUs (without MMU or prefetch emulation) can run through the WinUAE / Amiberry dynamic translator:

```c
static uint8_t guest[16 * 1024 * 1024];      /* guest address 0 .. 16 MB */

uae_cpu_config_t cfg = {
    .cpu_type = UAE_CPU_TYPE_68040,
    .fpu_type = UAE_FPU_68040,
    .fpu_softfloat = true,
    .jit_enabled = true,
    .jit_cache_size = 16384,                 /* KB; 0 selects 8192 */
};
uae_cpu_t *cpu = uae_cpu_create(&cfg);
uae_cpu_map_ram(cpu, 0, sizeof(guest), guest);
uae_cpu_set_jit_memory_base(cpu, guest);     /* host byte for guest a is guest + a */
```

With `.jit_direct_memory = true`, map the regions translated code may access inline with `uae_cpu_map_memory(cpu, start, size, base + start, UAE_MEM_RAM | UAE_MEM_JIT_DIRECT)` inside a reservation covering the address space (see below).

How it behaves:

- **Flat code window.** Translated code derives the 68k PC from host pointers, so the JIT only translates code in regions mapped with `uae_cpu_map_memory()` whose host pointer equals `base + guest address`. Declare that base with `uae_cpu_set_jit_memory_base()`. A RAM buffer mapped at guest 0 qualifies, as does one reservation covering the whole address space. Code anywhere else (custom devices, Musashi-style callbacks) runs through the interpreter inside the JIT dispatcher.
- **Memory access** from translated code goes through the region handlers by default, so custom devices, ROM write protection and `uae_cpu_raise_bus_error()` behave as in the interpreter.
- **Direct memory access** (`jit_direct_memory`). Translated code reads and writes RAM inline as `base + address` instead of calling a handler. This is how WinUAE and Amiberry run fast, and it is several times faster than handler calls. It applies to regions mapped with `UAE_MEM_JIT_DIRECT` whose host pointer is `base + start`; ROM writes still go through the handler. Each block is profiled in the interpreter before it is compiled: accesses that touched only such regions are inlined, all others keep the handler call. An instruction that later reaches a different address still goes to `base + address`, so the window must cover every address translated code can reach. In practice that means one 4 GB reservation with RAM, ROM and video memory committed at their guest addresses and the rest left inaccessible:
  - on x86-64 a fault in an inaccessible part of the window is recovered: the access completes through the region's handler and the block is profiled again. A handler reached this way cannot raise a bus error;
  - on AArch64 there is no recovery; such a fault reaches the host's SIGSEGV/SIGBUS handler.
  `UAE_MEM_JIT_UNSAFE_BURST` on any region switches to a conservative mode: blocks that touch handler-backed memory are interpreted, and on x86-64 translated accesses use the handlers again.
- **Cache gating.** WinUAE only translates while the guest has enabled the CPU cache through `CACR`. By default the library translates whenever the JIT is on. Set `jit_follow_cacr` to restore the WinUAE behaviour.
- **Execute budget.** `uae_cpu_execute(cpu, cycles)` stops translated code when the budget is spent. `uae_cpu_get_cycles()` includes work compiled blocks have not reported yet. Cycle counts under the JIT are block estimates, not per-instruction timing.
- **Hooks.** Every host hook works under the JIT; see [HOST_HOOKS.md](HOST_HOOKS.md#under-the-jit).
- **Self-modifying code.** Call `uae_cpu_invalidate_code()` after the host writes guest code; translated blocks are also checksummed before reuse.

Verification: the m68k-rs fixtures give the same per-fixture results with the interpreter, the JIT and the JIT with direct memory access on the same memory map. `host_hooks_jit` and `host_hooks_jit_direct` check translated byte/word/long access against a C reference. The direct variant also checks that devices and regions outside the window keep their handlers, and exercises the x86-64 fault recovery. `uae_cpu_bench` measures throughput; see [Benchmarks](#benchmarks).

Limitations: no JIT with MSVC; direct memory access needs a window covering the guest address space and has no fault recovery on AArch64; one CPU per process.

#### Benchmarks

`uae_cpu_bench` (built with the tests, source in [bench/](bench/uae_cpu_bench.c)) times small 68020 programs in each execution mode: `interpreter`, `jit` (translated code calls the region handlers) and `jit-direct` (`jit_direct_memory`, RAM accessed inline). It reports the best of several runs and the speedup over the interpreter:

| Workload | Exercises |
|----------|-----------|
| `arith` | Register arithmetic, no memory access |
| `bytemix` | Byte reads mixed into a checksum |
| `memcopy` | 64 KB long copy plus a byte sum |
| `device` | Word reads from a custom-mapped device (must reach the handler in every mode) |

```sh
./build/uae_cpu_bench                          # all workloads, best of 3
./build/uae_cpu_bench --repeat 5 memcopy       # one workload
./build/uae_cpu_bench --csv before.csv         # record, then compare with a later build
```

Every run is checked against a result computed in C, and a wrong result makes the program exit with status 1. CTest runs it as `bench_smoke` (`--quick --repeat 1`), which checks results only. CI publishes the timings in each job summary and as a CSV artifact. Compare timings between builds on the same machine; shared CI runners are too noisy for absolute thresholds.

### 4. Host Hooks

Hosts that service guest calls themselves (trap tables, HLE, paravirtual devices) install hooks instead of patching the core:

```c
static int on_illegal(void *ud, uint16_t opcode, uint32_t pc) {
    if (opcode == 0x7100) {              /* host-defined trap */
        uae_cpu_end_timeslice(ud);       /* return to the host */
        return 1;                        /* handled: resume at pc + 2 */
    }
    return 0;                            /* take vector 4 */
}

uae_cpu_host_hooks_t hooks = { .userdata = cpu, .illegal = on_illegal };
uae_cpu_set_host_hooks(cpu, &hooks);
uae_cpu_reserve_opcodes(cpu, 0x7100, 0x713F);
```

See [HOST_HOOKS.md](HOST_HOOKS.md) for every hook and its contract.

---

## References & Acknowledgments

- **WinUAE**: Bernd Schmidt, Toni Wilen, and the UAE development community.
- **Hatari**: The Hatari emulator team.
- **Musashi**: Karl Stenerud (for the Musashi 680x0 emulator and test suite).
- **m68k-rs**: For modern 68k emulator testing fixtures and API design inspirations.
