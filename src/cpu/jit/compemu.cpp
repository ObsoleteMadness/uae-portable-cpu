/* Generated compiler handlers: architecture dispatcher (see jit_cxx_prelude.h for linkage). */
#include "jit_cxx_prelude.h"

#if defined(CPU_AARCH64)
#include "arm/compemu_arm.cpp"
#elif defined(CPU_x86_64)
#include "x86/compemu_x86.cpp"
#else
#error "The JIT supports only AArch64 and x86-64 hosts"
#endif
