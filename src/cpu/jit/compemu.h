#ifndef WINUAE_JIT_COMPEMU_H
#define WINUAE_JIT_COMPEMU_H

#if defined(CPU_AARCH64)
#include "arm/compemu_arm.h"
#elif defined(CPU_x86_64)
#include "x86/compemu_x86.h"
#endif

/* Set by host fault handlers to request a guest exception from compiled code. */
extern volatile int jit_exception_pending;

/* Both 64-bit backends can longjmp out of compiled code on a guest bus error. */
#if (defined(CPU_AARCH64) || defined(CPU_x86_64)) && !defined(JIT_HAS_BUS_ERROR_RECOVERY)
#define JIT_HAS_BUS_ERROR_RECOVERY 1
#include <setjmp.h>
extern jmp_buf jit_bus_error_jmpbuf;
extern volatile bool jit_in_compiled_code;
#endif

#endif /* WINUAE_JIT_COMPEMU_H */
