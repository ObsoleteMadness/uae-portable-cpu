/*
 * UAE Portable 680x0 CPU Core - System Configuration
 */

#ifndef WINUAE_SYSCONFIG_H
#define WINUAE_SYSCONFIG_H

#define WINUAE_FOR_HATARI 1

#define FPUEMU
#define FPU_UAE
#define WITH_SOFTFLOAT
#define MMUEMU
#define FULLMMU

#define CPUEMU_0  /* generic 680x0 emulation */
#define CPUEMU_11 /* 68000/68010 prefetch emulation */
#define CPUEMU_13 /* 68000/68010 cycle-exact cpu */
#define CPUEMU_20 /* 68020 prefetch */
#define CPUEMU_21 /* 68020 "cycle-exact" */
#define CPUEMU_22 /* 68030 prefetch */
#define CPUEMU_23 /* 68030 "cycle-exact" */
#define CPUEMU_24 /* 68060 "cycle-exact" */
#define CPUEMU_25 /* 68040 "cycle-exact" */
#define CPUEMU_31 /* Aranym 68040 MMU */
#define CPUEMU_32 /* 68030 MMU */
#define CPUEMU_33 /* 68060 MMU */
#define CPUEMU_34 /* 68030 MMU + cache */
#define CPUEMU_35 /* 68030 MMU + cache + CE */
#define CPUEMU_40 /* generic 680x0 with JIT direct memory access */
#define CPUEMU_50 /* generic 680x0 with indirect memory access */

#define NOFLAGS_SUPPORT_GENCOMP

#ifndef CYCLE_UNIT
#define CYCLE_UNIT 512
#endif

#define MAX_DPATH 1000
#define CPU_EMU_SIZE 0

#define SIZEOF_VOID_P 8

#endif /* WINUAE_SYSCONFIG_H */
