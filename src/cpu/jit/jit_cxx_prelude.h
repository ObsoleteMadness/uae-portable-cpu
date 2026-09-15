/*
 * UAE Portable 680x0 CPU Core - JIT C++ prelude
 *
 * The JIT translation units are C++ (inherited from WinUAE / ARAnyM /
 * Amiberry) while the CPU core they plug into is C. Every JIT unit includes
 * this header first:
 *
 *   1. C++ standard headers are pulled in outside any linkage block, because
 *      templates cannot have C linkage. Their include guards then turn the
 *      C headers they wrap into no-ops later on.
 *   2. The core headers are included with C linkage. Functions declared there
 *      and defined in the JIT (compiler_init, build_comp, compile_block, ...)
 *      or defined in the core and called from the JIT (execute_normal,
 *      Exception, memory accessors, ...) then link against newcpu.c.
 */

#ifndef UAE_JIT_CXX_PRELUDE_H
#define UAE_JIT_CXX_PRELUDE_H

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cmath>
#include <csetjmp>
#include <csignal>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

extern "C" {
#include "sysconfig.h"
#include "sysdeps.h"
#include "uae_glue.h"
#include "options_cpu.h"
#include "events.h"
#include "memory.h"
#include "readcpu.h"
#include "newcpu.h"
#include "cpu_prefetch.h"
#include "fpp.h"
#include "uae/vm.h"
#include "jit/comptbl.h"
#include "jit/compemu.h"
#if defined(CPU_AARCH64)
/* Defined in fpp_native.c; the FPU compiler redeclares it without linkage. */
void fp_to_exten(fpdata *fpd, uae_u32 wrd1, uae_u32 wrd2, uae_u32 wrd3);
#endif
/* Defined in the JIT, called from host_hooks.c (uae_cpu_get_jit_code_size). */
uae_u32 get_jitted_size(void);
}

#endif /* UAE_JIT_CXX_PRELUDE_H */
