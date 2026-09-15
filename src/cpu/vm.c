/*
 * Multi-platform virtual memory functions for UAE.
 * Copyright (C) 2015 Frode Solheim
 *
 * Licensed under the terms of the GNU General Public License version 2.
 * See the file 'COPYING' for full license text.
 *
 * Portable-core port of the Amiberry/FS-UAE implementation. Only built with
 * the JIT, which allocates its translation cache and dispatcher stubs here.
 *
 * Apple Silicon: code memory is a MAP_JIT mapping. Pages in it are never
 * writable and executable at the same time for a given thread; the JIT
 * toggles with uae_vm_jit_write_protect() around every store and before it
 * runs translated code.
 */

#include "sysconfig.h"
#include "sysdeps.h"
#include "uae_glue.h"
#include "uae/vm.h"

#include <errno.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
#if defined(__APPLE__) && defined(CPU_AARCH64)
#include <pthread.h>
#endif

#if defined(CPU_x86_64) && !defined(__APPLE__) && !defined(_WIN32)
#define HAVE_MAP_32BIT 1
#endif

/* Upper bound for 32-bit placement probes (keeps allocations below 2 GB). */
static uae_u8 *natmem_reserved_limit(void)
{
    return (uae_u8 *)0x80000000u;
}

/*
 * Converts a UAE protection constant to the host value.
 *
 * Arguments:
 *   protect: One of the UAE_VM_* protection constants.
 *
 * Returns:
 *   PROT_* bits (POSIX) or PAGE_* value (Windows).
 */
static int protect_to_native(int protect)
{
#ifdef _WIN32
    if (protect == UAE_VM_NO_ACCESS) return PAGE_NOACCESS;
    if (protect == UAE_VM_READ) return PAGE_READONLY;
    if (protect == UAE_VM_READ_WRITE) return PAGE_READWRITE;
    if (protect == UAE_VM_READ_EXECUTE) return PAGE_EXECUTE_READ;
    if (protect == UAE_VM_READ_WRITE_EXECUTE) return PAGE_EXECUTE_READWRITE;
    write_log("VM: Invalid protect value %d\n", protect);
    return PAGE_NOACCESS;
#else
    if (protect == UAE_VM_NO_ACCESS) return PROT_NONE;
    if (protect == UAE_VM_READ) return PROT_READ;
    if (protect == UAE_VM_READ_WRITE) return PROT_READ | PROT_WRITE;
    if (protect == UAE_VM_READ_EXECUTE) return PROT_READ | PROT_EXEC;
    if (protect == UAE_VM_READ_WRITE_EXECUTE) return PROT_READ | PROT_WRITE | PROT_EXEC;
    write_log("VM: Invalid protect value %d\n", protect);
    return PROT_NONE;
#endif
}

/* Returns the host page size in bytes. */
int uae_vm_page_size(void)
{
    static int page_size = 0;
    if (page_size == 0) {
#ifdef _WIN32
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        page_size = (int)si.dwPageSize;
#else
        page_size = (int)sysconf(_SC_PAGESIZE);
#endif
    }
    return page_size;
}

/*
 * Allocates committed memory.
 *
 * Arguments:
 *   size: Bytes to allocate.
 *   flags: UAE_VM_32BIT to place it below 4 GB, UAE_VM_JIT for code memory.
 *   protect: Initial UAE_VM_* protection.
 *
 * Returns:
 *   The mapping, or NULL on failure.
 */
void *uae_vm_alloc(size_t size, int flags, int protect)
{
    void *address = NULL;

#ifdef _WIN32
    DWORD va_type = MEM_COMMIT | MEM_RESERVE;
    DWORD va_protect = (DWORD)protect_to_native(protect);
    if (flags & UAE_VM_WRITE_WATCH)
        va_type |= MEM_WRITE_WATCH;
#else
    int mmap_flags = MAP_PRIVATE | MAP_ANON;
    int mmap_prot = protect_to_native(protect);
#if defined(__APPLE__) && defined(CPU_AARCH64) && defined(MAP_JIT)
    if (flags & UAE_VM_JIT)
        mmap_flags |= MAP_JIT;
#endif
#endif

#if !defined(CPU_64_BIT)
    flags &= ~UAE_VM_32BIT;
#endif
    if (flags & UAE_VM_32BIT) {
        /* Probe upward for free space below the 2 GB mark. */
        size_t step = (size_t)uae_vm_page_size();
        uae_u8 *p = (uae_u8 *)0x40000000u;
        uae_u8 *p_end = natmem_reserved_limit() - size;
        if (size > 1024 * 1024) {
            p += 32 * 1024 * 1024;
            step = 1024 * 1024;
        }
#ifdef HAVE_MAP_32BIT
        address = mmap(NULL, size, mmap_prot, mmap_flags | MAP_32BIT, -1, 0);
        if (address == MAP_FAILED)
            address = NULL;
#endif
        while (address == NULL && p <= p_end) {
#ifdef _WIN32
            address = VirtualAlloc(p, size, va_type, va_protect);
#else
            address = mmap(p, size, mmap_prot, mmap_flags, -1, 0);
            if (address == MAP_FAILED) {
                address = NULL;
            } else if ((uintptr_t)address + size > (uintptr_t)0xffffffffu) {
                munmap(address, size);
                address = NULL;
            }
#endif
            p += step;
        }
    } else {
#ifdef _WIN32
        address = VirtualAlloc(NULL, size, va_type, va_protect);
#else
        address = mmap(NULL, size, mmap_prot, mmap_flags, -1, 0);
        if (address == MAP_FAILED)
            address = NULL;
#endif
    }

    if (address == NULL) {
#ifdef _WIN32
        write_log("VM: uae_vm_alloc(%zu, %d, %d) VirtualAlloc failed (%lu)\n",
                  size, flags, protect, (unsigned long)GetLastError());
#else
        int err = errno;
        write_log("VM: uae_vm_alloc(%zu, %d, %d) mmap failed (%d: %s)%s\n",
                  size, flags, protect, err, strerror(err),
                  (flags & UAE_VM_JIT) ? " - on macOS check the com.apple.security.cs.allow-jit entitlement" : "");
#endif
    }
    return address;
}

/*
 * Changes the protection of a mapping.
 *
 * Returns:
 *   True on success.
 */
bool uae_vm_protect(void *address, size_t size, int protect)
{
#ifdef _WIN32
    DWORD old;
    if (VirtualProtect(address, size, (DWORD)protect_to_native(protect), &old) == 0) {
        write_log("VM: uae_vm_protect(%p, %zu, %d) failed (%lu)\n",
                  address, size, protect, (unsigned long)GetLastError());
        return false;
    }
#else
    if (mprotect(address, size, protect_to_native(protect)) != 0) {
        write_log("VM: uae_vm_protect(%p, %zu, %d) failed (%d)\n",
                  address, size, protect, errno);
        return false;
    }
#endif
    return true;
}

/*
 * Releases a mapping made by uae_vm_alloc() or uae_vm_reserve().
 *
 * Returns:
 *   True on success.
 */
bool uae_vm_free(void *address, size_t size)
{
#ifdef _WIN32
    (void)size;
    return VirtualFree(address, 0, MEM_RELEASE) != 0;
#else
    if (munmap(address, size) != 0) {
        write_log("VM: uae_vm_free(%p, %zu) failed (%d)\n", address, size, errno);
        return false;
    }
    return true;
#endif
}

/*
 * Reserves address space without committing it, optionally at a hint.
 *
 * Returns:
 *   The reservation, or NULL.
 */
static void *try_reserve(uintptr_t try_addr, size_t size, int flags)
{
    void *address;
#ifdef _WIN32
    DWORD va_type = MEM_RESERVE;
    if (flags & UAE_VM_WRITE_WATCH)
        va_type |= MEM_WRITE_WATCH;
    address = VirtualAlloc((void *)try_addr, size, va_type, PAGE_NOACCESS);
    if (address == NULL)
        return NULL;
#else
    int mmap_flags = MAP_PRIVATE | MAP_ANON;
#if defined(__APPLE__) && defined(CPU_AARCH64) && defined(MAP_JIT)
    if (flags & UAE_VM_JIT)
        mmap_flags |= MAP_JIT;
#endif
    address = mmap((void *)try_addr, size, PROT_NONE, mmap_flags, -1, 0);
    if (address == MAP_FAILED)
        return NULL;
#endif
#ifdef CPU_64_BIT
    if ((flags & UAE_VM_32BIT) && (uintptr_t)address + size > (uintptr_t)0x100000000ull) {
        uae_vm_free(address, size);
        return NULL;
    }
#endif
    return address;
}

/* Reserves size bytes anywhere (below 4 GB with UAE_VM_32BIT). */
void *uae_vm_reserve(size_t size, int flags)
{
    void *address = NULL;
#ifdef CPU_64_BIT
    if (flags & UAE_VM_32BIT) {
#endif
        uintptr_t try_addr = 0x80000000u;
        while (address == NULL && try_addr >= 0x20000000u) {
            address = try_reserve(try_addr, size, flags);
            try_addr -= 0x4000000u;
        }
#ifdef CPU_64_BIT
    }
#endif
    if (address == NULL && (flags & UAE_VM_32BIT) == 0)
        address = try_reserve(0, size, flags);
    if (address == NULL)
        write_log("VM: Reserve 0x%zx bytes failed\n", size);
    return address;
}

/* Reserves size bytes exactly at want_addr, or returns NULL. */
void *uae_vm_reserve_fixed(void *want_addr, size_t size, int flags)
{
    void *address = try_reserve((uintptr_t)want_addr, size, flags);
    if (address == NULL)
        return NULL;
    if (address != want_addr) {
        uae_vm_free(address, size);
        return NULL;
    }
    return address;
}

/* Commits part of a reservation with the given protection. */
void *uae_vm_commit(void *address, size_t size, int protect)
{
#ifdef _WIN32
    return VirtualAlloc(address, size, MEM_COMMIT, (DWORD)protect_to_native(protect));
#else
    if (!uae_vm_protect(address, size, protect))
        return NULL;
    return address;
#endif
}

/* Returns committed pages to the reserved (inaccessible) state. */
bool uae_vm_decommit(void *address, size_t size)
{
#ifdef _WIN32
    return VirtualFree(address, size, MEM_DECOMMIT) != 0;
#else
    /* Re-mapping drops the old pages so the host can reclaim them. */
    void *result = mmap(address, size, PROT_NONE, MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0);
    if (result == MAP_FAILED) {
        uae_vm_protect(address, size, UAE_VM_NO_ACCESS);
        return false;
    }
    return true;
#endif
}

/*
 * Switches this thread's MAP_JIT pages between write and execute.
 *
 * Arguments:
 *   enable_execute_mode: True to run translated code, false to write it.
 */
void uae_vm_jit_write_protect(bool enable_execute_mode)
{
#if defined(__APPLE__) && defined(CPU_AARCH64)
    if (pthread_jit_write_protect_supported_np())
        pthread_jit_write_protect_np(enable_execute_mode ? 1 : 0);
#else
    (void)enable_execute_mode;
#endif
}
