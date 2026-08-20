/* isp_dump.c — dump ISP AE state to stdout.
 *
 * Run immediately after killing anyka_ipc (before ak_rtsp reinits the ISP)
 * to capture ground truth of what 3A-converged anyka_ipc had written.
 *
 * Usage:
 *   isp_dump            — dumps all three structs
 *   isp_dump --watch    — loops every 1s (to watch AE converge live)
 *
 * Build: compiled by Makefile as part of the ak_rtsp package.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#define ISP_DEVICE          "/dev/isp-param-0"
#define ISP_IOCTL_CMD       0xc0cc5616  /* _IOWR('V',0x16, isp_cmd_wrapper) */

#define GET_AE_RUN_INFO     0x80044952  /* 36 bytes */
#define GET_EXPOSURE_ATTR   0x8004498b  /* 0x720 bytes */
#define GET_SENSOR_AE_INFO  0x40044981  /* 16 bytes — actual sensor I2C regs */

struct isp_cmd_wrapper {
    uint32_t flag;
    uint32_t inner_cmd;
    void    *payload;
    uint32_t pad[48];
};

static int g_fd = -1;

static int isp_tunnel(uint32_t cmd, void *buf)
{
    struct isp_cmd_wrapper w;
    memset(&w, 0, sizeof(w));
    w.flag      = 1;
    w.inner_cmd = cmd;
    w.payload   = buf;
    return ioctl(g_fd, ISP_IOCTL_CMD, &w);
}

static void hexdump(const char *label, const uint8_t *p, int n)
{
    printf("\n=== %s (%d bytes) ===\n", label, n);
    for (int i = 0; i < n; i++) {
        if (i % 16 == 0) printf("  %04x: ", i);
        printf("%02x ", p[i]);
        if (i % 16 == 15 || i == n-1) {
            /* pad last line */
            for (int j = i%16; j < 15; j++) printf("   ");
            printf(" |");
            int start = (i/16)*16;
            for (int j = start; j <= i; j++)
                printf("%c", (p[j]>=32 && p[j]<127) ? p[j] : '.');
            printf("|\n");
        }
    }
}

static void u32dump(const char *label, const uint8_t *p, int nwords)
{
    printf("\n=== %s (%d × u32) ===\n", label, nwords);
    const uint32_t *v = (const uint32_t *)p;
    for (int i = 0; i < nwords; i++) {
        if (i % 8 == 0) printf("  [%2d]: ", i);
        printf("%8u ", v[i]);
        if (i % 8 == 7 || i == nwords-1) printf("\n");
    }
}

static void dump_once(void)
{
    /* --- get_ae_run_info (36 bytes) --- */
    uint8_t run[36];
    memset(run, 0, sizeof(run));
    if (isp_tunnel(GET_AE_RUN_INFO, run) == 0) {
        printf("\n--- AE run info ---\n");
        printf("  lum_avg   = %u\n",   run[0]);
        printf("  exp_time  = %u  (hardware units)\n", *(uint32_t*)(run+4));
        printf("  snsr_gain = %u  (256=1x)\n",         *(uint32_t*)(run+8));
        printf("  isp_gain  = %u  (256=1x)\n",         *(uint32_t*)(run+12));
        hexdump("get_ae_run_info raw", run, 36);
    } else {
        fprintf(stderr, "get_ae_run_info failed: %s\n", strerror(errno));
    }

    /* --- get_sensor_ae_info (16 bytes) --- */
    uint32_t sae[4];
    memset(sae, 0, sizeof(sae));
    if (isp_tunnel(GET_SENSOR_AE_INFO, sae) == 0) {
        printf("\n--- sensor AE regs (actual I2C register values) ---\n");
        printf("  [0]=%u [1]=%u [2]=%u [3]=%u\n", sae[0], sae[1], sae[2], sae[3]);
    } else {
        fprintf(stderr, "get_sensor_ae_info failed: %s\n", strerror(errno));
    }

    /* --- get_exposure_attr (0x720 bytes) --- */
    static uint8_t expo[0x720];
    memset(expo, 0, sizeof(expo));
    if (isp_tunnel(GET_EXPOSURE_ATTR, expo) == 0) {
        printf("\n--- exposure attr private section (first 7 u32, bytes 0x00..0x1b) ---\n");
        u32dump("private", expo, 7);
        printf("\n--- exposure attr public section (bytes 0x1c..0x1c+0x703) ---\n");
        u32dump("public[0..31]", expo + 0x1c, 32);
        hexdump("public section first 128 bytes", expo + 0x1c, 128);
    } else {
        fprintf(stderr, "get_exposure_attr failed: %s\n", strerror(errno));
    }
}

int main(int argc, char **argv)
{
    int watch = (argc > 1 && strcmp(argv[1], "--watch") == 0);

    g_fd = open(ISP_DEVICE, O_RDWR);
    if (g_fd < 0) {
        fprintf(stderr, "open %s: %s\n", ISP_DEVICE, strerror(errno));
        return 1;
    }
    printf("ISP fd=%d opened\n", g_fd);

    do {
        dump_once();
        if (watch) { printf("\n---\n"); sleep(1); }
    } while (watch);

    close(g_fd);
    return 0;
}
