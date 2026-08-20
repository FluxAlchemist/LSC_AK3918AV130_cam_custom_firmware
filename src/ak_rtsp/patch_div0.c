/* Silences the "Division by zero in kernel." dmesg spam (harmless __div0
 * traps hit by ak_isp.ko's on-chip AE/streaming-setup code dividing by a
 * momentarily-zero cached fps value during stream-start/night-mode-switch
 * reconfiguration windows) by patching __div0() itself, rather than chasing
 * each of the several division call sites inside the closed-source
 * ak_isp.ko individually.
 *
 * __div0() (arch/arm/kernel/traps.c) is core kernel code at a FIXED virtual
 * address (0xc000d3b0 -- confirmed identical across every captured
 * backtrace on this device) that does nothing but printk() + dump_stack().
 * Its caller (the ARM software-divide runtime's zero-check, Ldiv0) doesn't
 * use any return value from it and keeps its own safe fallback-and-continue
 * behavior regardless -- __div0 is purely the logging path. Overwriting its
 * first instruction ("mov r12, sp" -> "mov pc, lr") makes it return
 * immediately: same safe recovery, no more log spam.
 *
 * Patched live via /dev/mem, not flashed -- a plain reboot fully reverts it,
 * so a wrong kernel build/address can't do lasting damage. The expected-byte
 * check below refuses to touch anything if the target doesn't look exactly
 * like __div0's known prologue, precisely to guard against that.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#define DIV0_VADDR   0xc000d3b0u
#define PAGE_OFFSET  0xc0000000u   /* lowmem virtual base, from boot log     */
#define PHYS_BASE    0x80000000u  /* RAM physical base, from boot log/u-boot */
#define DIV0_PADDR   (DIV0_VADDR - PAGE_OFFSET + PHYS_BASE)
#define PAGE_SIZE    4096u

/* "mov r12, sp" -- __div0()'s real first instruction, Ghidra-confirmed. */
static const uint8_t expected[4] = {0x0d, 0xc0, 0xa0, 0xe1};
/* "mov pc, lr" -- return immediately, skipping printk()+dump_stack(). */
static const uint8_t patched[4]  = {0x0e, 0xf0, 0xa0, 0xe1};

int main(int argc, char **argv)
{
    int revert = (argc > 1 && strcmp(argv[1], "revert") == 0);

    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("open /dev/mem"); return 1; }

    off_t page_addr = DIV0_PADDR & ~(off_t)(PAGE_SIZE - 1);
    off_t page_off  = DIV0_PADDR &  (off_t)(PAGE_SIZE - 1);

    void *map = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, page_addr);
    if (map == MAP_FAILED) { perror("mmap"); close(fd); return 1; }

    volatile uint8_t *p = (volatile uint8_t *)map + page_off;

    printf("__div0 @ 0x%08x (phys 0x%08lx): current bytes %02x %02x %02x %02x\n",
           DIV0_VADDR, (long)DIV0_PADDR, p[0], p[1], p[2], p[3]);

    if (revert) {
        if (memcmp((void *)p, patched, 4) != 0) {
            fprintf(stderr, "not currently patched (unexpected bytes) -- refusing to touch\n");
            munmap(map, PAGE_SIZE); close(fd); return 1;
        }
        memcpy((void *)p, expected, 4);
        printf("reverted __div0 to its original prologue\n");
    } else {
        if (memcmp((void *)p, expected, 4) != 0) {
            fprintf(stderr, "unexpected bytes at __div0 -- refusing to patch (wrong kernel build, or already patched?)\n");
            munmap(map, PAGE_SIZE); close(fd); return 1;
        }
        memcpy((void *)p, patched, 4);
        printf("patched __div0 to return immediately -- no more printk/dump_stack spam\n");
    }

    munmap(map, PAGE_SIZE);
    close(fd);
    return 0;
}
