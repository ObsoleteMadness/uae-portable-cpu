#ifndef WINUAE_JIT_COMPTBL_H
#define WINUAE_JIT_COMPTBL_H

#if defined(CPU_AARCH64)
#include "arm/comptbl_arm.h"
#elif defined(CPU_x86_64)
#include "x86/comptbl_x86.h"
#endif

#endif /* WINUAE_JIT_COMPTBL_H */
