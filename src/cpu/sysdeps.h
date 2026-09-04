/*
 * UAE Portable 680x0 CPU Core - System & Type Dependencies
 */

#ifndef UAE_SYSDEPS_H
#define UAE_SYSDEPS_H

#include "sysconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

#ifndef NORETURN
#if defined(_MSC_VER)
#define NORETURN __declspec(noreturn)
#elif defined(__GNUC__) || defined(__clang__)
#define NORETURN __attribute__((noreturn))
#else
#define NORETURN
#endif
#endif
#ifndef NOWARN_UNUSED
#define NOWARN_UNUSED(x) x
#endif

#include "uae/types.h"
#include "uae/likely.h"
#include "uae/attributes.h"
#include "uae/string.h"
#include "compat.h"

#ifndef UAE
#define UAE
#endif

#if defined(__aarch64__) || defined(_M_ARM64) || defined(_M_ARM64EC)
#define CPU_AARCH64 1
#define CPU_arm 1
#define ARM_ASSEMBLY 1
#define CPU_64_BIT 1
#elif defined(__arm__) || defined(_M_ARM)
#define CPU_arm 1
#define ARM_ASSEMBLY 1
#elif defined(__x86_64__) || defined(_M_AMD64)
#define CPU_x86_64 1
#define CPU_64_BIT 1
#define X86_64_ASSEMBLY 1
#define SAHF_SETO_PROFITABLE
#elif defined(__i386__) || defined(_M_IX86)
#define CPU_i386 1
#define X86_ASSEMBLY 1
#define SAHF_SETO_PROFITABLE
#endif

#define JITCALL
#define REGPARAM
#define REGPARAM2
#define REGPARAM3

#define STATIC_INLINE static inline

#ifdef _WIN32
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#define stricmp _stricmp
#define strnicmp _strnicmp
#ifndef strdup
#define strdup _strdup
#endif
#else
#define strnicmp strncasecmp
#define stricmp strcasecmp
#endif

#define xmalloc(type, num) ((type *)malloc(sizeof(type) * (num)))
#define xcalloc(type, num) ((type *)calloc(num, sizeof(type)))
#define xfree(p) free(p)

typedef struct TrapContext TrapContext;
#define IRQ_SOURCE_MAX 8

#define uae_atomic int
#define atomic_or(ptr, val)  __sync_or_and_fetch((ptr), (val))
#define atomic_and(ptr, val) __sync_and_and_fetch((ptr), (val))
#define atomic_set(ptr, val) __sync_lock_test_and_set((ptr), (val))
#define ASM_SYM_FOR_FUNC(x)

#define CACHE_ENABLE_DATA 0x01
#define CACHE_ENABLE_DATA_BURST 0x02
#define CACHE_ENABLE_COPYBACK 0x020
#define CACHE_ENABLE_INS 0x80
#define CACHE_ENABLE_INS_BURST 0x40
#define CACHE_ENABLE_BOTH (CACHE_ENABLE_DATA | CACHE_ENABLE_INS)
#define CACHE_ENABLE_ALL (CACHE_ENABLE_BOTH | CACHE_ENABLE_INS_BURST | CACHE_ENABLE_DATA_BURST)
#define CACHE_DISABLE_ALLOCATE 0x08
#define CACHE_DISABLE_MMU 0x10

#ifndef ENUMDECL
#define ENUMDECL typedef enum
#define ENUMNAME(name) name
#endif

#ifndef UVAL64
#define UVAL64(x) ((uae_u64)(x##ULL))
#define SVAL64(x) ((uae_s64)(x##LL))
#endif

#endif /* UAE_SYSDEPS_H */
