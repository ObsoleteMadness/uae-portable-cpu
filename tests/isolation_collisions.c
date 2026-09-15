/*
 * isolation_collisions.c - Host symbols that clash with the core's internals
 *
 * Linked into test_isolated_link together with uaecpu_isolated. Each of
 * these names is a global in the normal uaecpu library, and all of them are
 * common in emulator hosts (Basilisk II defines intlev() and memory_init(),
 * Musashi defines the m68k_* entry points). If isolation leaks any of them,
 * the test executable fails to link with a duplicate-symbol error.
 */

#include <stdarg.h>

int intlev(void) { return 0; }
void memory_init(void) {}
void memory_reset(void) {}
void write_log(const char *format, ...) { (void)format; }
/* Initialised, so a leaked data symbol is a duplicate definition, not a silent merge. */
int regs = 1;
int currprefs = 1;
int cpufunctbl = 1;
void init_m68k(void) {}
void Exception(int nr) { (void)nr; }
#ifndef UAE_TEST_MUSASHI_API
/* Only clashes when the Musashi-compatible API is left out of the library. */
int m68k_execute(int num_cycles) { return num_cycles; }
void m68k_init(void) {}
unsigned int m68k_read_memory_8(unsigned int address) { (void)address; return 0; }
#endif
