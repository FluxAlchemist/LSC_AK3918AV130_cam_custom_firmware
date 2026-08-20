/*
 * ARM Linux kuser_helpers atomic stubs for ARMv5TE.
 *
 * ARMv5TE has no LDREX/STREX (those are ARMv6+). The Linux kernel maps a
 * read-only helper page at 0xffff0000 on all ARM Linux systems. We use
 * __kuser_cmpxchg at 0xffff0fc0 to implement the three __sync_* symbols that
 * zig 0.16.0's SmpAllocator emits calls to (it doesn't inline them on ARMv5).
 *
 * See: Documentation/arm/kernel_user_helpers.rst
 * Available since: Linux 2.6.12 (camera runs 3.x — guaranteed present).
 */

/* Clang refuses to redeclare __sync_* builtins in C regardless of -fno-builtin.
 * Solution: define them in a raw .S-style using __asm__ global aliases that
 * point to plain C helper functions with different names. */

#include <stdint.h>

/* int __kuser_cmpxchg(int oldval, int newval, volatile int *ptr)
 * Returns 0 on success (*ptr was oldval and was set to newval). */
typedef int (__kuser_cmpxchg_t)(int, int, volatile int *);
#define kuser_cmpxchg ((__kuser_cmpxchg_t *)0xffff0fc0u)

/* 4-byte CAS — straightforward loop */
static int ak_sync_cas4(volatile int *ptr, int expected, int desired) {
    int cur;
    do {
        cur = *ptr;
        if (cur != expected) return cur;
    } while (kuser_cmpxchg(expected, desired, ptr) != 0);
    return expected;
}

/* Byte CAS: operate on the aligned 4-byte word containing the byte */
static char ak_sync_cas1(volatile char *ptr, char expected, char desired) {
    uintptr_t    addr    = (uintptr_t)ptr;
    int          shift   = (int)((addr & 3u) * 8u);
    int          mask    = 0xff << shift;
    volatile int *wptr   = (volatile int *)(addr & ~(uintptr_t)3u);
    int oldword, newword;
    do {
        oldword = *wptr;
        char cur = (char)(oldword >> shift);
        if (cur != expected) return cur;
        newword = (oldword & ~mask) | (((int)(unsigned char)desired << shift) & mask);
    } while (kuser_cmpxchg(oldword, newword, wptr) != 0);
    return expected;
}

/* Byte TAS: swap unconditionally */
static char ak_sync_tas1(volatile char *ptr, char val) {
    uintptr_t    addr    = (uintptr_t)ptr;
    int          shift   = (int)((addr & 3u) * 8u);
    int          mask    = 0xff << shift;
    volatile int *wptr   = (volatile int *)(addr & ~(uintptr_t)3u);
    int oldword, newword;
    do {
        oldword = *wptr;
        newword = (oldword & ~mask) | (((int)(unsigned char)val << shift) & mask);
    } while (kuser_cmpxchg(oldword, newword, wptr) != 0);
    return (char)(oldword >> shift);
}

/* Expose the above as the __sync_* names the linker needs, bypassing Clang's
 * builtin-redeclaration check by going through __asm__ aliases. */
__asm__(
    ".global __sync_val_compare_and_swap_4\n"
    "__sync_val_compare_and_swap_4 = ak_sync_cas4\n"
    ".global __sync_val_compare_and_swap_1\n"
    "__sync_val_compare_and_swap_1 = ak_sync_cas1\n"
    ".global __sync_lock_test_and_set_1\n"
    "__sync_lock_test_and_set_1 = ak_sync_tas1\n"
);
