# Host Hooks

Emulators that embed a 680x0 core rarely just run guest code. They also need
to service calls from guest code in the host, skip hardware the machine
doesn't have, and report crashes. Examples:

- Basilisk II and SheepShaver EmulOps.
- Atari ST NatFeats and Hatari's GEMDOS traps.
- UAE `calltrap`.
- Macintosh Toolbox A-line HLE.
- Paravirtual drivers.
- Test harnesses that signal pass or fail through a trap.

Without supported entry points, every embedder ends up hand-editing
`newcpu.c` and the generated opcode tables. The hooks below are those entry
points. None of them is tied to a particular emulated machine.

All declarations are in [`include/uae_cpu.h`](include/uae_cpu.h). The tests in
[`tests/test_host_hooks.c`](tests/test_host_hooks.c) exercise one scenario per
hook.

```c
uae_cpu_host_hooks_t hooks = {
    .userdata  = my_machine,
    .illegal   = on_illegal,   /* host trap opcodes            */
    .aline     = on_aline,     /* Line-A HLE                   */
    .fline     = on_fline,     /* absent FPU/MMU policy        */
    .exception = on_exception, /* crash reporting              */
    .get_irq   = on_get_irq,   /* pulled interrupt level       */
    .dbf_spin  = on_dbf_spin,  /* busy-wait calibration loops  */
};
uae_cpu_set_host_hooks(cpu, &hooks);
uae_cpu_set_trap_hook(cpu, on_trap, my_machine);   /* TRAP #0-15 */
```

A hook left `NULL` keeps the core's Motorola-accurate behaviour.

---

## Execution model

- **Thread.** Hooks run on the thread that called `uae_cpu_execute()` or
  `uae_cpu_step()`. Only `uae_cpu_set_irq()` and `uae_cpu_signal_irq()` are
  meant to be called from other threads.
- **Registers.** When a hook runs, the status register is already materialised,
  so `uae_cpu_get_reg(cpu, UAE_REG_SR)` is exact. Hooks may change any register,
  including the PC, and may read or write guest memory.
- **Re-entrancy.** A hook may run guest code by calling `uae_cpu_execute()`
  again, for example to call a guest callback and return to the host. The core
  counts nesting in `uae_cpu_execute_depth()`.
- **End of timeslice.** `uae_cpu_end_timeslice()` makes the *innermost* active
  execute call return after the current instruction. Outer calls keep running.
  If no execute call is active, the request is consumed by the next one.
- **Instruction-aborting faults.** Each execute call runs inside the core's
  `TRY` block. `uae_cpu_raise_bus_error()` unwinds out of the memory callback
  or hook with `longjmp`, so do not hold locks or other resources across a call
  that can raise one.

### Resume rule (illegal, aline, fline)

Each of these hooks receives the opcode word and `pc`, the address of that word.

| Hook returns | PC after the hook | Execution continues at |
|--------------|-------------------|------------------------|
| 0 | anything | the core raises the normal exception |
| non-zero | still `pc` | `pc + 2` (after the opcode word) |
| non-zero | moved by the hook | wherever the hook put it |

Instructions with extension words (most FPU and PMMU operations) must be
skipped by the hook: call `uae_cpu_set_pc(cpu, pc + length)`. A hook that runs
guest code in between and then restores the PC to `pc` gets the `pc + 2`
default.

---

## Hooks

### `illegal`: illegal instruction / host-trap opcodes

```c
int (*illegal)(void *userdata, uint16_t opcode, uint32_t pc);
```

This hook sees every opcode that reaches illegal-instruction processing, apart
from Line-A and Line-F words. It runs before vector 4 is taken, and before any
other core policy for the word.

Pair it with `uae_cpu_reserve_opcodes(cpu, first, last)` (up to 16 ranges) to
claim a range for host traps. Reserved words always come to the hooks, even
where the configured CPU would decode them as a valid instruction. For
example, Basilisk II's `0x7100–0x713F` EmulOps are MOVEQ encodings with bit 8
set. `uae_cpu_clear_reserved_opcodes()` restores normal decoding.

### `aline`: Line-A words

```c
int (*aline)(void *userdata, uint16_t opcode, uint32_t pc);
```

Called for `0xAxxx` before vector 10. Use it for trap-dispatch HLE. Return 0
to let the guest's own vector 10 handler run.

### `fline`: Line-F words the CPU cannot execute

```c
int (*fline)(void *userdata, uint16_t opcode, uint32_t pc, uae_fline_reason_t reason);
```

Called before vector 11, or before the 68040/68060 unimplemented-FPU exception.
`reason` says why the CPU cannot run the word:

| `reason` | Raised when |
|----------|-------------|
| `UAE_FLINE_FPU_ABSENT` | FPU instruction, no FPU configured (or disabled through the 68060 PCR) |
| `UAE_FLINE_MMU_ABSENT` | PMMU (coprocessor 0) or 68040/060 `PFLUSH`/`PTEST`/`PLPA` with no MMU configured |
| `UAE_FLINE_UNKNOWN` | Anything else, including unimplemented FPU operations when an FPU is present |

A typical policy is to treat `MMU_ABSENT` as a no-op on a machine whose OS
probes for an MMU and carries on without one, and let everything else trap.

### `exception`: observer

```c
void (*exception)(void *userdata, const uae_cpu_exception_info_t *info);
```

Fires for every exception, interrupts included, before the stack frame is
built. It can't cancel or change the exception. Use it for crash reports,
"guest OS just bombed" detection, or tracing.

| Field | Meaning |
|-------|---------|
| `vector` | Vector number (2 = bus error, 4 = illegal, 10/11 = Line-A/F, 24–31 = interrupts, 32–47 = TRAP) |
| `fault_pc` | Address of the faulting instruction. For interrupts, where the interrupt arrived |
| `current_pc` | PC at dispatch. It may already be past the instruction (DIVU, TRAP) |
| `opcode` | Instruction word being executed |
| `sr` | Status register before the exception |
| `interrupt` | True for vectors 24–31 |

An exception serviced by the host through `illegal`, `aline`, `fline` or the
TRAP hook never reaches the observer.

### TRAP #0–15

```c
void uae_cpu_set_trap_hook(uae_cpu_t *cpu, uae_trap_hook_fn fn, void *userdata);
int  fn(void *userdata, int trap_nr);   /* trap_nr = 0..15 */
```

Called when a `TRAP #n` instruction executes. Return non-zero to service the
call in the host: no exception is taken, and execution continues after the
TRAP. Return 0 for the normal vector `32 + n`.

### `get_irq`: pulled interrupt level

```c
int (*get_irq)(void *userdata);   /* 0..7 */
```

Once installed, this hook owns the interrupt level. The core samples it at the
start of every execute call and whenever it services a pending interrupt
check, which `uae_cpu_signal_irq()` requests. Call `uae_cpu_signal_irq()` from
any thread when the level changes.

Hosts that prefer push semantics can leave `get_irq` NULL and call
`uae_cpu_set_irq(cpu, level)`, which is also safe from other threads.

### `dbf_spin`: busy-wait loops

```c
uint32_t (*dbf_spin)(void *userdata, int dreg, uint16_t count);
```

Many ROMs calibrate timers with `DBF Dn,*-2`, a loop that only burns time.
A fast interpreter finishes it within one host microsecond, and the guest may
then divide by a measured elapsed time of zero.

The hook is called each time a `DBF Dn,*-2` executes, with the current low word
of `Dn`:

- Return 0 to run that one iteration normally. The next iteration is offered
  again.
- Return a non-zero cycle count to finish the loop at once. The result is
  exactly what the loop would have produced (`Dn.W = 0xFFFF`, PC falls
  through), and the returned number of CPU cycles is credited to
  `uae_cpu_get_cycles()`.

It is generated into the fast interpreter tables only. The prefetch,
cycle-exact, MMU and test cores always execute the loop literally.

---

## Control and query functions

| Function | Purpose |
|----------|---------|
| `uae_cpu_end_timeslice(cpu)` | Return from the innermost execute call after the current instruction |
| `uae_cpu_execute_depth(cpu)` | Number of active execute/step calls (0 when not running) |
| `uae_cpu_signal_irq(cpu)` | Re-sample the interrupt level (thread-safe) |
| `uae_cpu_get_cycles(cpu)` | Monotonic 64-bit CPU cycle count since the core was initialised; usable as an emulated clock |
| `uae_cpu_raise_bus_error(cpu, addr, is_write, size)` | Abort the current instruction with a bus error. The frame matches the configured CPU. No effect outside execute calls |
| `uae_cpu_invalidate_code(cpu, addr, size)` | Discard translated code that may overlap the range. Blocks are checksummed before reuse, so any size > 0 is safe. A no-op without a JIT, so hosts can call it unconditionally |
| `uae_cpu_set_jit_memory_base(cpu, base)` | Declare the flat guest window (`host = base + guest`) the JIT translates code from |
| `uae_cpu_get_jit_code_size(cpu)` | Bytes of translated host code in the cache (0 without a JIT) |
| `uae_cpu_get_mem_flags(cpu, addr)` | `uae_mem_flags_t` bits of the region containing `addr`; 0 when unmapped |

### Memory region flags

`uae_cpu_map_memory()` accepts these flags in addition to `UAE_MEM_RAM`,
`UAE_MEM_ROM` and `UAE_MEM_CACHEABLE`:

| Flag | Meaning |
|------|---------|
| `UAE_MEM_JIT_DIRECT` | With `jit_direct_memory`: translated code may read and write the region inline, provided its host pointer is `base + start` for the JIT memory base. Otherwise the flag has no effect |
| `UAE_MEM_JIT_UNSAFE_BURST` | With `jit_direct_memory`: conservative mode for memory maps where a burst (MOVEM, MOVE16) can run off a direct region. Blocks touching handler-backed memory are interpreted, and on x86-64 translated accesses use the handlers |

Custom regions (`uae_cpu_map_custom`) always report `UAE_MEM_IO` and are
never JIT-direct.

Set `uae_cpu_config_t.unmapped_bus_error = true` so that access to an unmapped
64K bank raises a bus error instead of reading as all ones.

---

## Linking next to other cores

The core has many generic global names, for example `intlev`, `memory_init`,
`write_log`, `regs` and `currprefs`. The Musashi-compatible API adds `m68k_*`.
These clash with other emulators and CPU cores in the same binary. Two CMake
options deal with that:

| Option | Default | Effect |
|--------|---------|--------|
| `UAE_CPU_MUSASHI_API` | `ON` | Build `include/m68k.h` (`m68k_*`). Turn it off when the host also links the real Musashi |
| `UAE_CPU_ISOLATE_SYMBOLS` | `OFF` | Also build `uaecpu_isolated` (`uae-portable-cpu::uaecpu_isolated`): a static library whose only global symbols are `uae_cpu_*` (plus `m68k_*` when the Musashi API is on) |

```bash
cmake -B build -DUAE_CPU_MUSASHI_API=OFF -DUAE_CPU_ISOLATE_SYMBOLS=ON
```

Isolation uses a relocatable link. On Apple it uses the linker's
`-exported_symbols_list`; with GNU toolchains it uses `objcopy
--keep-global-symbols`. It needs a single-architecture, non-shared build and
is not available with MSVC. The `isolated_link` test links the hook tests
together with deliberately clashing definitions of those names, so a leaked
symbol breaks the build.

---

## Under the JIT

With `jit_enabled` (see the [README](README.md#3-jit-compiler)), every hook keeps its contract:

| Hook / call | What the JIT does |
|-------------|-------------------|
| `illegal`, `aline`, `fline`, TRAP | The opcode runs through its C handler inside translated code and ends the block, so a hook that moves the PC or runs guest code takes effect at once |
| `uae_cpu_reserve_opcodes()` | Reserved words are never compiled natively and always end a block; reserving rebuilds the translation tables |
| Nested `uae_cpu_execute()` | Runs on the interpreter: a C handler called from translated code never re-enters compiled code |
| `uae_cpu_end_timeslice()` | Translated code returns to the dispatcher at the next instruction boundary it checks, and the execute call returns |
| `dbf_spin` | While the hook is installed, `DBF Dn` is routed to its C handler so the hook is offered; installing or removing it rebuilds the tables |
| `exception` | Fires as in the interpreter; `fault_pc` comes from the instruction being dispatched |
| `get_irq`, `uae_cpu_signal_irq()` | Interrupts are taken at block boundaries |
| `uae_cpu_raise_bus_error()` | Aborts the instruction from inside translated code through the JIT's bus-error recovery. Exception: with `jit_direct_memory` on x86-64 and Windows ARM64, a handler reached through fault recovery (an inlined access that hit an inaccessible part of the window) cannot raise one, and the call is ignored |
| Memory callbacks | Called for every access by default. With `jit_direct_memory`, accesses that profiling saw in `UAE_MEM_JIT_DIRECT` RAM are inlined and never reach a callback |
| `instruction` hook | Only called for code that runs on the interpreter |

## Musashi-compatible API

- `m68k_set_illg_instr_callback()` is wired to the `illegal` hook. The
  callback sees true illegal opcodes, not Line-A/Line-F words, matching Musashi.
  Returning 1 resumes after the opcode word. Note: Musashi itself has already
  advanced the PC when the callback runs; here the PC still points at the opcode.
- `m68k_set_trap_instr_callback()` is called for `TRAP #0–15` with the trap
  number, and honours its return value.
- `m68k_execute()` shares the re-entrant, bus-error-safe loop with
  `uae_cpu_execute()`.
- `m68k_pulse_bus_error()` keeps its previous behaviour: it takes vector 2
  immediately and the instruction continues. Use `uae_cpu_raise_bus_error()`
  to abort the instruction.

## Limitations

- Hook state is global, like the rest of the core: one CPU per process.
- The JIT translates only from the flat window set with
  `uae_cpu_set_jit_memory_base()`. Direct memory access (`jit_direct_memory`)
  needs that window to cover the guest address space, and recovers faults in
  it only on x86-64 and Windows ARM64.
- With `jit_direct_memory` on x86-64 the library installs a SIGSEGV (and, on
  macOS, SIGBUS) handler the first time it builds its tables. Faults outside
  translated code are passed to the handler that was installed before it.
  On Windows (x64 and ARM64) it adds a vectored exception handler instead,
  which passes on every exception it does not handle.
- `uae_cpu_raise_bus_error()` only takes effect inside `uae_cpu_execute()`,
  `uae_cpu_step()` or `m68k_execute()`.
