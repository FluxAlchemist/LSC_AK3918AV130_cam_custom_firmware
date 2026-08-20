/*
 * ARMv5TE atomic stubs via Linux kuser_helpers page.
 *
 * ARMv5TE has no LDREX/STREX. The Linux kernel maps a helper page at
 * 0xffff0000 on all ARM Linux kernels (present since 2.6.12; camera runs 3.x).
 * __kuser_cmpxchg at 0xffff0fc0 provides a kernel-mediated CAS with full
 * memory barriers. We use it to implement the three __sync_* symbols that
 * zig 0.16.0's SmpAllocator requires.
 *
 * kuser_cmpxchg calling convention:
 *   r0 = oldval, r1 = newval, r2 = volatile int *ptr
 *   Returns 0 in r0 on success; non-zero on failure.
 *   Clobbers: r0, r1, r2, r3, r12 (ip), flags, memory.
 */

.syntax unified
.arm
.text

@ -----------------------------------------------------------------------------
@ int __sync_val_compare_and_swap_4(volatile int *ptr, int expected, int desired)
@   Returns the value *ptr held before the swap (expected if success, other if fail).
@ -----------------------------------------------------------------------------
.global __sync_val_compare_and_swap_4
.type   __sync_val_compare_and_swap_4, %function
__sync_val_compare_and_swap_4:
    push    {r4, r5, r6, lr}
    mov     r4, r0              @ r4 = ptr
    mov     r5, r1              @ r5 = expected
    mov     r6, r2              @ r6 = desired
.Lcas4_loop:
    ldr     r3, [r4]            @ r3 = *ptr
    cmp     r3, r5
    bne     .Lcas4_mismatch     @ *ptr != expected, return *ptr
    mov     r0, r5              @ kuser arg0: oldval
    mov     r1, r6              @ kuser arg1: newval
    mov     r2, r4              @ kuser arg2: ptr
    ldr     r3, =0xffff0fc0
    blx     r3                  @ clobbers r0-r3, r12
    cmp     r0, #0
    bne     .Lcas4_loop         @ CAS race-lost, retry
    mov     r0, r5              @ success: return expected (was old value)
    pop     {r4, r5, r6, pc}
.Lcas4_mismatch:
    mov     r0, r3              @ return current *ptr
    pop     {r4, r5, r6, pc}
.ltorg
.size __sync_val_compare_and_swap_4, . - __sync_val_compare_and_swap_4


@ -----------------------------------------------------------------------------
@ char __sync_val_compare_and_swap_1(volatile char *ptr, char expected, char desired, ...)
@   Byte CAS via word-level kuser_cmpxchg. Variadic suffix ignored.
@ -----------------------------------------------------------------------------
.global __sync_val_compare_and_swap_1
.type   __sync_val_compare_and_swap_1, %function
__sync_val_compare_and_swap_1:
    push    {r4, r5, r6, r7, r8, lr}
    bic     r4, r0, #3          @ r4 = word-aligned ptr
    and     r5, r0, #3
    lsl     r5, r5, #3          @ r5 = bit shift (0/8/16/24)
    mov     r6, #0xff
    lsl     r6, r6, r5          @ r6 = byte mask
    and     r7, r1, #0xff       @ r7 = expected byte
    and     r8, r2, #0xff       @ r8 = desired byte
.Lcas1_loop:
    ldr     r3, [r4]            @ r3 = current word
    lsr     r0, r3, r5
    and     r0, r0, #0xff       @ r0 = current byte
    cmp     r0, r7
    bne     .Lcas1_done         @ mismatch, r0 already has current byte
    bic     r1, r3, r6          @ word with byte cleared
    lsl     r2, r8, r5
    orr     r1, r1, r2          @ new word with desired byte set
    mov     r0, r3              @ kuser arg0: old word
    mov     r2, r4              @ kuser arg2: word ptr
    ldr     r3, =0xffff0fc0
    blx     r3
    cmp     r0, #0
    bne     .Lcas1_loop         @ race, retry
    mov     r0, r7              @ success: return expected byte
.Lcas1_done:
    pop     {r4, r5, r6, r7, r8, pc}
.ltorg
.size __sync_val_compare_and_swap_1, . - __sync_val_compare_and_swap_1


@ -----------------------------------------------------------------------------
@ char __sync_lock_test_and_set_1(volatile char *ptr, char val, ...)
@   Atomic byte swap. Returns the old value of *ptr.
@ -----------------------------------------------------------------------------
.global __sync_lock_test_and_set_1
.type   __sync_lock_test_and_set_1, %function
__sync_lock_test_and_set_1:
    push    {r4, r5, r6, r7, r8, lr}
    bic     r4, r0, #3          @ r4 = word-aligned ptr
    and     r5, r0, #3
    lsl     r5, r5, #3          @ r5 = bit shift
    mov     r6, #0xff
    lsl     r6, r6, r5          @ r6 = byte mask
    and     r7, r1, #0xff       @ r7 = new byte value
.Ltas1_loop:
    ldr     r8, [r4]            @ r8 = old word (preserved across blx)
    bic     r1, r8, r6          @ word with byte cleared
    lsl     r2, r7, r5
    orr     r1, r1, r2          @ new word with val byte set
    mov     r0, r8              @ kuser arg0: old word
    mov     r2, r4              @ kuser arg2: word ptr
    ldr     r3, =0xffff0fc0
    blx     r3
    cmp     r0, #0
    bne     .Ltas1_loop         @ race, retry
    lsr     r0, r8, r5          @ extract old byte from saved word
    and     r0, r0, #0xff
    pop     {r4, r5, r6, r7, r8, pc}
.ltorg
.size __sync_lock_test_and_set_1, . - __sync_lock_test_and_set_1
