#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>

#include "ak_rtsp.h"
#include "ak_ioctls.h"
#include "isp.h"

/* -------------------------------------------------------------------------
 * ISP sensor init — parse /tmp/sensor_isp.conf, program GC20C3 via I2C
 *
 * Two-step init:
 *  Step A — Sensor register table: parse the conf file, extract the sensor
 *            section, send ioctl 0x40085301 to /dev/v4l-subdev0.
 *            Without this the GC20C3 won't produce frames.
 *
 *  Step B — ISP image pipeline (24 modules, 0x00..0x17): implemented in
 *            isp_pipeline_init() below. Called after this function.
 *
 * First-test shortcut: run while anyka_ipc has already initialized the ISP
 * (factory RTSP mode), then kill it. ISP hardware registers persist in silicon
 * so isp_pipeline_init() can be skipped on that path.
 * ------------------------------------------------------------------------- */
int isp_load_sensor_conf(void)
{
    int i, ret = -1;
    uint16_t hdr[2];
    void *reg_table = NULL;

    FILE *f = fopen("/tmp/sensor_isp.conf", "rb");
    if (!f) { perror("isp: fopen /tmp/sensor_isp.conf"); return -1; }

    /* skip 512-byte text header (one subfile, mode=0) */
    if (fseek(f, ISP_CONF_HEADER_SIZE, SEEK_SET) != 0) {
        perror("isp: fseek"); goto out;
    }

    /* scan 24 ISP pipeline module blocks to reach the sensor section */
    for (i = 0; i < ISP_MODULE_COUNT; i++) {
        if (fread(hdr, sizeof(uint16_t), 2, f) != 2) {
            fprintf(stderr, "isp: short read at module %d\n", i); goto out;
        }
        /* hdr[0]=module_id (== i), hdr[1]=total block size including this 4-byte header */
        if (hdr[0] != i || hdr[1] < 4) {
            fprintf(stderr, "isp: bad module header %d: id=%u size=%u\n", i, hdr[0], hdr[1]);
            goto out;
        }
        if (fseek(f, hdr[1] - 4, SEEK_CUR) != 0) { goto out; }
    }

    /* sensor section: [u16 0x1c][u16 data_len][data_len bytes: 4-byte I2C reg entries] */
    if (fread(hdr, sizeof(uint16_t), 2, f) != 2 || hdr[0] != ISP_SENSOR_SECTION_ID) {
        fprintf(stderr, "isp: bad sensor section magic 0x%04x\n", hdr[0]); goto out;
    }

    reg_table = malloc(hdr[1]);
    if (!reg_table) { perror("isp: malloc sensor regs"); goto out; }

    if (fread(reg_table, 1, hdr[1], f) != hdr[1]) {
        perror("isp: fread sensor regs"); goto out;
    }

    struct isp_sensor_conf conf = {
        .reg_count = (uint32_t)hdr[1] >> 2,
        .reg_table = (uint32_t)(uintptr_t)reg_table,
    };
    ret = ioctl(g_subdev, ISP_SENSOR_INIT_IOCTL, &conf);
    if (ret < 0) perror("ISP_SENSOR_INIT_IOCTL");
    else printf("[ak_rtsp] GC20C3 sensor programmed: %u regs\n", conf.reg_count);

out:
    free(reg_table);
    fclose(f);
    return ret;
}

/* -------------------------------------------------------------------------
 * ISP image pipeline init — send all 24 module calibration blocks from
 * /tmp/sensor_isp.conf to the ISP hardware via the tunnel ioctl.
 *
 * File layout (starting at byte 512, after the text header):
 *   24 × [u16 module_id][u16 total_block_size][payload...]
 *   total_block_size includes the 4-byte [id][size] header itself.
 *
 * All payloads go through ioctl(isp_fd, ISP_IOCTL_CMD, isp_cmd_wrapper) with
 * the inner_cmd from ISP_INNER_* and payload pointing to block+4 (skipping the
 * 4-byte [id][size] header). Multi-call modules (0x03, 0x0b, 0x13, 0x14) send
 * additional calls at fixed byte offsets within the same block — these offsets
 * were confirmed from isp_set_attr decompilation in anyka_ipc.
 *
 * Reversed from:
 *   isp_module_init    @ 0x00129920 (24-module dispatch loop)
 *   AK_ISP_set_*_attr  @ 0x001456c8..0x00146280 (one function per inner cmd)
 * ------------------------------------------------------------------------- */
static const int g_isp_single_cmd[24] = {
    ISP_INNER_BLC,        /* 0x00 */
    ISP_INNER_LSC,        /* 0x01 */
    ISP_INNER_RAW_LUT,    /* 0x02 */
    0,                    /* 0x03 — NR, handled separately */
    ISP_INNER_3D_NR,      /* 0x04 */
    ISP_INNER_GB,         /* 0x05 */
    ISP_INNER_DEMO,       /* 0x06 */
    ISP_INNER_RGB_GAMMA,  /* 0x07 */
    ISP_INNER_CCM,        /* 0x08 */
    ISP_INNER_FCS,        /* 0x09 */
    ISP_INNER_WDR,        /* 0x0a */
    0,                    /* 0x0b — Sharp, handled separately */
    ISP_INNER_SATURATION, /* 0x0c */
    ISP_INNER_CONTRAST,   /* 0x0d */
    ISP_INNER_RGB2YUV,    /* 0x0e */
    ISP_INNER_EFFECT,     /* 0x0f */
    ISP_INNER_DPC,        /* 0x10 */
    ISP_INNER_LCE,        /* 0x11 */
    ISP_INNER_AF,         /* 0x12 */
    0,                    /* 0x13 — AWB, handled separately */
    0,                    /* 0x14 — AE,  handled separately */
    ISP_INNER_MISC,       /* 0x15 */
    ISP_INNER_Y_GAMMA,    /* 0x16 */
    ISP_INNER_HUE,        /* 0x17 */
};

/* Send all 24 module blocks starting at the file's CURRENT position (caller
 * has already seeked past whatever 512-byte subfile header precedes this
 * module-data region — day and night subfiles share this exact layout, see
 * isp_pipeline_init()/isp_pipeline_init_night()). Factored out so both
 * day and night loading share one implementation instead of two
 * hand-copied ones that could drift apart. On success, the file position
 * is left immediately after the 24th module (i.e. at the sensor-register
 * section's own [u16 0x1c][u16 len] header) so a caller can keep parsing
 * from there (e.g. to locate a subsequent subfile). */
static int isp_send_modules(FILE *f, const char *log_prefix)
{
    int ret = 0;
    int mod;
    for (mod = 0; mod < ISP_MODULE_COUNT; mod++) {
        uint16_t hdr[2];
        if (fread(hdr, sizeof(uint16_t), 2, f) != 2) {
            fprintf(stderr, "%s: short read at module %d\n", log_prefix, mod);
            ret = -1; break;
        }
        if (hdr[0] != mod || hdr[1] < 4) {
            fprintf(stderr, "%s: bad block header mod %d id=%u size=%u\n",
                    log_prefix, mod, hdr[0], hdr[1]);
            ret = -1; break;
        }
        uint32_t block_size = hdr[1];
        uint8_t *block = malloc(block_size);
        if (!block) { perror("isp_send_modules: malloc"); ret = -1; break; }

        memcpy(block, hdr, 4);
        if ((uint32_t)fread(block + 4, 1, block_size - 4, f) != block_size - 4) {
            fprintf(stderr, "%s: short payload mod %d\n", log_prefix, mod);
            free(block); ret = -1; break;
        }

        struct isp_cmd_wrapper w;
        memset(&w, 0, sizeof(w));
        w.flag = 1;

#define ISP_SEND(cmd, off) do { \
    w.inner_cmd = (cmd); w.payload = block + (off); \
    if (ioctl(g_isp_fd, ISP_IOCTL_CMD, &w) < 0) \
        fprintf(stderr, "%s: mod 0x%02x cmd 0x%08x: %s\n", \
                log_prefix, mod, (cmd), strerror(errno)); \
} while (0)

        switch (mod) {
        case 0x03: /* NR: NR1@+4, NR2@+0xbfc, UVNR@+0xfdc */
            ISP_SEND(ISP_INNER_NR1,  4);
            ISP_SEND(ISP_INNER_NR2,  0xbfc);
            ISP_SEND(ISP_INNER_UVNR, 0xfdc);
            break;
        case 0x0b: /* Sharp: SHARP@+4, SHARP_EX@+0xb5fc */
            ISP_SEND(ISP_INNER_SHARP,    4);
            ISP_SEND(ISP_INNER_SHARP_EX, 0xb5fc);
            break;
        case 0x13: /* AWB: WB@+4, AWB_EX@+0x7e4, AWB_CALIB@+0x1be8 */
            ISP_SEND(ISP_INNER_WB,        4);
            ISP_SEND(ISP_INNER_AWB_EX,    0x7e4);
            ISP_SEND(ISP_INNER_AWB_CALIB, 0x1be8);
            break;
        case 0x14: /* AE: EXPOSURE@+4, FRAME_RATE@+0x724 */
            ISP_SEND(ISP_INNER_EXPOSURE,   4);
            ISP_SEND(ISP_INNER_FRAME_RATE, 0x724);
            break;
        default:
            ISP_SEND(g_isp_single_cmd[mod], 4);
            break;
        }

#undef ISP_SEND
        free(block);
    }
    return (ret == 0) ? mod : -1;
}

int isp_pipeline_init(void)
{
    FILE *f = fopen(SENSOR_ISP_CONF, "rb");
    if (!f) { perror("isp_pipeline: fopen"); return -1; }

    if (fseek(f, ISP_CONF_HEADER_SIZE, SEEK_SET) != 0) {
        perror("isp_pipeline: fseek"); fclose(f); return -1;
    }

    int mods_sent = isp_send_modules(f, "isp_pipeline");
    fclose(f);
    if (mods_sent < 0) return -1;
    printf("[ak_rtsp] ISP pipeline init OK (%d modules)\n", mods_sent);
    return 0;
}

/* Skip forward over one already-loaded subfile's module data + sensor
 * register section, landing the file position at the START of whatever
 * follows (either EOF or the next subfile's own 512-byte header). Does not
 * send anything — pure bookkeeping, used to locate the night subfile within
 * sensor_isp.conf without hardcoding a byte offset (module/register sizes
 * are read from the file itself, so this stays correct even if a firmware
 * update changes any module's calibration data size). */
static int isp_skip_subfile_data(FILE *f)
{
    for (int mod = 0; mod < ISP_MODULE_COUNT; mod++) {
        uint16_t hdr[2];
        if (fread(hdr, sizeof(uint16_t), 2, f) != 2) return -1;
        if (hdr[0] != mod || hdr[1] < 4) return -1;
        if (fseek(f, hdr[1] - 4, SEEK_CUR) != 0) return -1;
    }
    uint16_t reg_hdr[2];
    if (fread(reg_hdr, sizeof(uint16_t), 2, f) != 2) return -1;
    if (reg_hdr[0] != 0x1c) return -1;
    if (fseek(f, reg_hdr[1], SEEK_CUR) != 0) return -1;
    return 0;
}

/* Night ISP pipeline init — sensor_isp.conf (isp_gc20c3.conf on the camera)
 * is not a single profile: it's 1-5 concatenated "subfiles", each a
 * 512-byte header (declaring, among other things, a `mode` byte at header
 * offset 31: 0=day, 1=night, 2..4=usr_define_1..3) followed by the exact
 * same 24-module + sensor-register-section layout isp_pipeline_init()
 * already parses for day. Confirmed by walking the real on-camera
 * isp_gc20c3.conf byte-for-byte (2026-07-05): subfile 1 at offset 0 has
 * mode=0 dated 2025-12-9; subfile 2 at offset 94156 has **mode=1, dated
 * 2025-11-10** — a completely separate, real night calibration (different
 * WDR/NR/gamma/CCM/sharpness tables, not just different WB/exposure) that
 * anyka_ipc's own isp_switch()/isp_switch_mode() load and apply via
 * isp_cfg_file_load() when switching modes, and that `ak_rtsp` had never
 * read before this — `night.c` only ever replicated 3 fields (IR-cut,
 * frame-rate/VTS, WB gains) because that's all Ghidra's isp_switch_mode
 * trace had turned up; the actual per-mode *data* this whole time was
 * sitting in the same file `isp_pipeline_init()` already opens, just past
 * the day subfile's data.
 *
 * This walks forward from the day subfile (module data + sensor-register
 * section, sizes read from the file itself, not hardcoded) to find the
 * next subfile's header, validates its `mode` byte is really 1, then sends
 * its 24 modules the same way isp_pipeline_init() does for day. Does NOT
 * reprogram the sensor's own I2C register table (isp_load_sensor_conf()) —
 * the day and night subfiles share the same sensor_id and there's no
 * evidence anyka_ipc reprograms sensor registers on a day/night switch,
 * only the ISP-side calibration. */
int isp_pipeline_init_night(void)
{
    FILE *f = fopen(SENSOR_ISP_CONF, "rb");
    if (!f) { perror("isp_pipeline_night: fopen"); return -1; }

    if (fseek(f, ISP_CONF_HEADER_SIZE, SEEK_SET) != 0) {
        perror("isp_pipeline_night: fseek"); fclose(f); return -1;
    }
    if (isp_skip_subfile_data(f) != 0) {
        fprintf(stderr, "isp_pipeline_night: failed walking past day subfile\n");
        fclose(f); return -1;
    }

    long subfile2_off = ftell(f);
    uint8_t header[ISP_CONF_HEADER_SIZE];
    if (fread(header, 1, sizeof(header), f) != sizeof(header)) {
        fprintf(stderr, "isp_pipeline_night: no second subfile in %s (mode=1/night not present)\n",
                SENSOR_ISP_CONF);
        fclose(f); return -1;
    }
    uint32_t main_version = *(uint32_t *)(header + 0);
    uint8_t  mode         = header[31];
    if (main_version != 7 || mode != 1) {
        fprintf(stderr, "isp_pipeline_night: subfile at offset %ld has main_version=%u mode=%u, "
                "expected version=7 mode=1 — refusing to send unverified data\n",
                subfile2_off, main_version, mode);
        fclose(f); return -1;
    }

    int mods_sent = isp_send_modules(f, "isp_pipeline_night");
    fclose(f);
    if (mods_sent < 0) return -1;
    printf("[ak_rtsp] ISP NIGHT pipeline init OK (%d modules, subfile offset %ld)\n",
           mods_sent, subfile2_off);
    return 0;
}

/* Re-apply AWB_EX (ISP_INNER_AWB_EX, 0x40044973) from sensor_isp.conf.
 *
 * AWB_EX payload is 5124 bytes (0x1404) at offset +0x7e4 within module 0x13's
 * block. Passing any smaller buffer (16 or 256 bytes) causes EFAULT because the
 * ISP kernel driver calls copy_from_user(dst, payload_ptr, 0x1404).
 *
 * Called from ae_setup() after isp_pipeline_init() has already sent it once.
 * We re-send here with the runtime AWB_EX values that anyka_ipc also re-applies
 * after WB init (ae_dump_3.log line 25). */
int isp_reapply_awb_ex(void)
{
    FILE *f = fopen(SENSOR_ISP_CONF, "rb");
    if (!f) { perror("isp_reapply_awb_ex: fopen"); return -1; }

    if (fseek(f, ISP_CONF_HEADER_SIZE, SEEK_SET) != 0) {
        perror("isp_reapply_awb_ex: fseek"); fclose(f); return -1;
    }

    int ret = -1;
    for (int mod = 0; mod < ISP_MODULE_COUNT; mod++) {
        uint16_t hdr[2];
        if (fread(hdr, sizeof(uint16_t), 2, f) != 2) break;
        if (hdr[0] != mod || hdr[1] < 4) break;
        uint32_t block_size = hdr[1];

        if (mod != 0x13) {
            fseek(f, block_size - 4, SEEK_CUR);
            continue;
        }

        /* Found module 0x13 (AWB). Load full block. */
        uint8_t *block = malloc(block_size);
        if (!block) { perror("isp_reapply_awb_ex: malloc"); break; }
        memcpy(block, hdr, 4);
        if ((uint32_t)fread(block + 4, 1, block_size - 4, f) != block_size - 4) {
            fprintf(stderr, "isp_reapply_awb_ex: short read\n");
            free(block); break;
        }

        struct isp_cmd_wrapper w;
        memset(&w, 0, sizeof(w));
        w.flag      = 1;
        w.inner_cmd = ISP_INNER_AWB_EX;
        w.payload   = block + 0x7e4;  /* AWB_EX at +0x7e4 within block */
        ret = ioctl(g_isp_fd, ISP_IOCTL_CMD, &w);
        if (ret < 0)
            fprintf(stderr, "isp_reapply_awb_ex: ioctl failed: %s\n", strerror(errno));
        else
            printf("[ae] AWB_EX OK (5124 bytes from sensor_isp.conf offset +0x7e4)\n");
        free(block);
        break;
    }

    fclose(f);
    return ret;
}

int isp_tunnel(uint32_t inner_cmd, void *payload)
{
    struct isp_cmd_wrapper w;
    memset(&w, 0, sizeof(w));
    w.flag      = 1;
    w.inner_cmd = inner_cmd;
    w.payload   = payload;
    return ioctl(g_isp_fd, ISP_IOCTL_CMD, &w);
}

int isp_set_wb_manual(uint16_t r_gain, uint16_t g_gain, uint16_t b_gain)
{
    /* Struct layout confirmed by Ghidra (ak_vpss_set_mwb_attr, 0x7e0 bytes):
     *   bytes  8-11: wb_type=1 (MANUAL)
     *   bytes 12-13: R_gain u16 (256=1x)
     *   bytes 14-15: G_gain u16 (256=1x)
     *   bytes 16-17: B_gain u16 (256=1x) */
    static uint8_t wb[0x7e0];
    memset(wb, 0, sizeof(wb));
    *(uint32_t *)(wb +  0) = 1;
    *(uint32_t *)(wb +  4) = 2;
    *(uint32_t *)(wb +  8) = 1;    /* wb_type = MANUAL */
    *(uint16_t *)(wb + 12) = r_gain;
    *(uint16_t *)(wb + 14) = g_gain;
    *(uint16_t *)(wb + 16) = b_gain;
    return isp_tunnel(ISP_INNER_SET_WB, wb);
}

/* Saturation/contrast live tuning — GET-modify-SET on the small "effect"
 * struct (ISP_INNER_EFFECT/ISP_INNER_GET_EFFECT, module 0x0f).
 *
 * Confirmed via Ghidra (anyka_ipc's isp_set_effect @ 0x127e40, dispatched
 * from ak_vpss_effect_set — NOTE the dispatch type number is NOT the same
 * as isp_set_effect's own textual case order; ht_video_codec_set_brightness
 * calls type 1, set_saturation calls type 2, set_contrast calls type 3 —
 * verify against ak_vpss_effect_set's actual callers, not just which `case`
 * appears first when reading the decompile, which is exactly the mistake
 * that shipped a mislabeled "brightness" here once already, see git
 * history):
 *
 *   - Saturation (type 2), offset+4, signed 16-bit: anyka_ipc reads the
 *     CURRENT hardware value once to recover the untouched baseline
 *     (current - our last-applied delta), then writes baseline+new_delta
 *     with asymmetric scaling (negative deltas added 1:1, positive deltas
 *     scaled x3), clamped to [0,254].
 *   - Contrast (type 3), BOTH halves of offset+0 (a different word from
 *     saturation): couples a "low" 16-bit field (offset+0) and a "high"
 *     16-bit field (offset+2) via a delta-splitting formula — the same
 *     one-time baseline-cache pattern, but the delta is clamped first
 *     against the low field's [0,255] range, then AGAIN against the high
 *     field's [-128,127] range (recomputing the delta if the high field
 *     would overflow), then both fields are updated together with a
 *     rounding correction (-1 on the low field when the low+delta sum is
 *     odd). Transcribed directly from the decompiled arithmetic below,
 *     including the two-stage clamp and rounding correction — this is not
 *     a simplification, every step matches a real instruction in
 *     isp_set_effect's case 3 block (verified against disassembly for the
 *     signed-divide-by-2 rounding direction, which matches plain C `/2` on
 *     ARM for this specific idiom — add-sign-then-ASR is the standard
 *     round-toward-zero pattern GCC/clang also emit for `x/2`).
 *
 * Both effects share ONE 10-byte baseline cache in real anyka_ipc
 * (`g_yuv_effect_attr`, filled once ever by whichever of saturation/
 * contrast/brightness fires first) — we use separate per-field caches
 * instead since each of our setters is independent and only reads what it
 * needs; functionally equivalent since the struct only changes via these
 * specific writes.
 *
 * Buffer is 64 bytes (generously larger than the 10-byte stack scratch
 * anyka_ipc's own call site uses) so a GET->modify->SET round-trip can't
 * truncate whatever the kernel's real struct size actually is — same
 * defensive sizing pattern as ae.c's 0x720-byte exposure-attr buffer.
 *
 * Brightness (type 1) is NOT here — see isp_set_brightness() further down,
 * a separate, much riskier mechanism (the live AE exposure-attr struct, not
 * this small effect struct). Sharpness (type 4) is in isp_set_sharpness()
 * further down too (a ~46KB blob rescale, not a small struct at all). */
#define EFFECT_BUF_SIZE 64

static int     g_saturation_baseline_valid = 0;
static int16_t g_saturation_baseline = 0;

int isp_set_saturation(int value /* -50..50 */)
{
    uint8_t buf[EFFECT_BUF_SIZE];
    memset(buf, 0, sizeof(buf));
    if (isp_tunnel(ISP_INNER_GET_EFFECT, buf) != 0) return -1;
    int16_t *field = (int16_t *)(buf + 4);
    if (!g_saturation_baseline_valid) {
        g_saturation_baseline = *field;
        g_saturation_baseline_valid = 1;
    }
    int target = (value < 0) ? (g_saturation_baseline + value)
                              : (g_saturation_baseline + value * 3);
    if (target > 254) target = 254;
    if (target < 0)   target = 0;
    *field = (int16_t)target;
    return isp_tunnel(ISP_INNER_EFFECT, buf);
}

static int      g_contrast_baseline_valid = 0;
static uint16_t g_contrast_baseline_lo = 0; /* offset+0, unsigned per the decompile's uVar26 */
static int16_t  g_contrast_baseline_hi = 0; /* offset+2, signed per the decompile's iVar8 */

int isp_set_contrast(int value /* -50..50 */)
{
    uint8_t buf[EFFECT_BUF_SIZE];
    memset(buf, 0, sizeof(buf));
    if (isp_tunnel(ISP_INNER_GET_EFFECT, buf) != 0) return -1;

    if (!g_contrast_baseline_valid) {
        g_contrast_baseline_lo = *(uint16_t *)(buf + 0);
        g_contrast_baseline_hi = *(int16_t  *)(buf + 2);
        g_contrast_baseline_valid = 1;
    }
    int bl = g_contrast_baseline_lo;
    int bh = g_contrast_baseline_hi;

    /* Stage 1: clamp the delta so bl+delta stays in [0,255]. */
    int delta = value;
    if (bl + value < 0)          delta = -bl;
    else if (bl + value >= 256)  delta = 255 - bl;

    /* Stage 2: re-clamp the SAME delta if bh-2*delta would overflow the
     * high field's signed byte-ish range [-128,127] — this can override
     * stage 1's result, exactly as the decompile does (recomputed from bh,
     * not composed with stage 1's clamp). */
    int hi_check = bh - delta * 2;
    if (hi_check < -128)      delta = (bh + 128) / 2;
    else if (hi_check > 127)  delta = (bh - 127) / 2;

    int sum = bl + delta;
    int16_t lo_out = (int16_t)sum;
    if ((sum & 1) != 0 && delta != 0) lo_out = (int16_t)(lo_out - 1);
    int16_t hi_out = (int16_t)(bh - delta * 2);

    *(int16_t *)(buf + 0) = lo_out;
    *(int16_t *)(buf + 2) = hi_out;
    return isp_tunnel(ISP_INNER_EFFECT, buf);
}

/* Brightness live tuning — a completely different mechanism from saturation/
 * contrast: `ht_video_codec_set_brightness` calls `ak_vpss_effect_set(dev, 1,
 * value)` — TYPE 1, which dispatches to a SEPARATE function (internally
 * named "set_brightness", confirmed via its own error-log string, @ 0x126758
 * in anyka_ipc) that reads/writes the LIVE 0x720-byte exposure-attr struct —
 * the SAME struct ae.c's AE thread polls continuously via GET_EXPOSURE — not
 * the small effect struct saturation/contrast use.
 *
 * Verified at the DISASSEMBLY level, not just decompiled C: Ghidra's
 * decompiler badly conflated two different source offsets here (0x13c vs
 * 0x3c relative to a cached copy of the struct's "public" section), enough
 * that a decompile-only reading would have been wrong in a second way beyond
 * the type-0-vs-type-1 mixup already documented above. Confirmed via real
 * ARM load/store instructions:
 *   - A single "primary" target value at absolute struct offset 0x58,
 *     whose BASELINE is read from a DIFFERENT, seemingly-permanent
 *     reference field at offset 0x158 (anyka_ipc never appears to write
 *     0x158 itself — it looks like a static "factory default target"
 *     baked in from sensor_isp.conf, while 0x58 is the "active target" the
 *     running AE algorithm actually consults).
 *   - A 14-entry int32 table at offsets 0x5c..0x90, whose baseline is EACH
 *     ENTRY'S OWN pre-modification value (same self-referential
 *     baseline-once pattern as saturation/contrast) — read and written at
 *     the exact same 14 locations.
 *   - Both are updated as baseline+delta clamped to >=0 (a bitwise
 *     "AND NOT (arithmetic-shift-right-31)" idiom = clamp-negative-to-zero).
 *   - A `(new_primary * <some other field>) / <unidentified>` computation
 *     also appears in the original function but is NOT replicated here:
 *     traced its result to a dead stack slot never read again before the
 *     function returns — confirmed it cannot affect anything sent to
 *     hardware, so omitting it changes nothing observable.
 *
 * SAFETY: ae.c's steady-state loop only ever calls GET_EXPOSURE (read-only,
 * per its own design), never writes back, so
 * there's no write-write race with it. The only real concurrent-access risk
 * would be two of our OWN isp.c setters (or two brightness calls) racing on
 * the same GET-modify-SET sequence — not possible today since control.c
 * handles one client command at a time on a single thread. If that
 * threading model ever changes, this needs a mutex around the sequence. */
#define EXPO_BUF_SIZE               0x720
#define BRIGHTNESS_PRIMARY_OFF      0x58   /* active target, written every call */
#define BRIGHTNESS_PRIMARY_REF_OFF  0x158  /* permanent reference, read-only baseline source */
#define BRIGHTNESS_TABLE_OFF        0x5c   /* 14-entry table, self-referential baseline */
#define BRIGHTNESS_TABLE_COUNT      14

static int     g_brightness_baseline_valid = 0;
static int32_t g_brightness_primary_baseline;
static int32_t g_brightness_table_baseline[BRIGHTNESS_TABLE_COUNT];

static int32_t clamp_nonneg(int32_t v) { return v < 0 ? 0 : v; }

int isp_set_brightness(int value /* -50..50 */)
{
    uint8_t buf[EXPO_BUF_SIZE];
    memset(buf, 0, sizeof(buf));
    if (isp_tunnel(ISP_INNER_GET_EXPOSURE, buf) != 0) return -1;

    if (!g_brightness_baseline_valid) {
        g_brightness_primary_baseline = *(int32_t *)(buf + BRIGHTNESS_PRIMARY_REF_OFF);
        for (int i = 0; i < BRIGHTNESS_TABLE_COUNT; i++)
            g_brightness_table_baseline[i] = *(int32_t *)(buf + BRIGHTNESS_TABLE_OFF + i * 4);
        g_brightness_baseline_valid = 1;
    }

    *(int32_t *)(buf + BRIGHTNESS_PRIMARY_OFF) =
        clamp_nonneg(g_brightness_primary_baseline + value);
    for (int i = 0; i < BRIGHTNESS_TABLE_COUNT; i++)
        *(int32_t *)(buf + BRIGHTNESS_TABLE_OFF + i * 4) =
            clamp_nonneg(g_brightness_table_baseline[i] + value);

    return isp_tunnel(ISP_INNER_SET_EXPOSURE, buf);
}

/* Sharpness live tuning — isp_set_effect type 4, verified via disassembly
 * at anyka_ipc's isp_set_effect case 4 (jump-table-confirmed address
 * 0x12808c — see the jump-table dump used to validate all 6 case addresses
 * directly from raw bytes, since decompiled case *order* on this function
 * already proved unreliable once for case 0 vs 1 above).
 *
 * Unlike saturation/contrast/brightness, this doesn't touch a handful of
 * fields — it rescales almost the entire ~46KB (0xb5f8-byte) sharpness
 * attribute blob (module 0x0b) from a cached pristine baseline, using 4
 * scale factors derived from the 0-100 UI value (v100 = value+50):
 *   iVar7  = (v100 * 1024) / 50   (0..2048, 1024 = neutral, drives the main
 *                                  edge-detection kernel below)
 *   iVar8  = (v100 * 16)   / 50   (0..32,   16   = neutral)
 *   iVar9  = (v100 * 255)  / 50   (0..510,  255  = neutral)
 *   iVar10 = ((50-value) * 255) / 50   (inverse of iVar9: 510..0, 255 = neutral)
 *
 * Implemented here (verified field-by-field against the decompile's
 * unambiguous final write-statement list, not the interleaved intermediate
 * computations — several of those looked like genuine cross-field
 * dependencies on a first read and turned out to be compiler-reused dead
 * registers once traced to their actual write target):
 *   - The main 256-entry x2 edge-detection kernel (offsets 0x678 and
 *     0x878, signed 16-bit): each entry = (baseline * iVar7 * 64) >> 16,
 *     clamped to [-256,255]. This is the dominant "how sharp" control.
 *   - 20 individual scalar fields, each `clamp(baseline * factor >> shift,
 *     max)` with implicit min 0 (these are gain-table entries, always
 *     non-negative by construction):
 *       iVar8, >>4,  clamp 255->15: offsets 0x642, 0x650, 0x65e, 0x668
 *       iVar9, >>8,  clamp 255:     offsets 0x64a, 0x64e, 0x658, 0x65c,
 *                                   0x662, 0x666, 0x66c, 0x670
 *       iVar10,>>8,  clamp 255:     offsets 0x648, 0x64c, 0x656, 0x65a,
 *                                   0x660, 0x664, 0x66a, 0x66e
 *
 * NOT implemented: a repeated 16-group sub-block (stride 0xab4, offsets
 * 0x112c+/0x132c+ each holding another 256-entry kernel plus ~13 more
 * scalar fields, same 4 scale factors and clamp rules) — these are
 * per-lighting-condition ("gain bucket") sharpening tables, a secondary
 * refinement layered on top of the primary kernel above, not the dominant
 * visual effect. Left as a known gap rather than risk further transcription
 * errors on ~200 more field writes in one pass — revisit if the slider
 * turns out to need finer control than the primary kernel alone provides.
 *
 * Baseline blob is fetched fresh via GET_SHARP on first call and cached —
 * same one-time-baseline-then-rescale-from-it pattern as anyka_ipc's own
 * g_sharp_attr cache, so repeated slider drags always rescale from the
 * pristine original rather than compounding onto an already-rescaled
 * buffer. */
#define SHARP_BUF_SIZE 0xb5f8

static int      g_sharp_baseline_valid = 0;
static uint8_t *g_sharp_baseline = NULL; /* calloc'd once, kept for process lifetime */

static int16_t sharp_kernel_scale(int16_t baseline, int32_t iVar7)
{
    int32_t product = (int32_t)iVar7 * baseline * 64;
    int32_t shifted = product >> 16;
    if (shifted >= 0x100) return 0xff;
    if (shifted < -0x100) return (int16_t)-0x100;
    return (int16_t)shifted;
}

static uint16_t sharp_field_scale(uint16_t baseline, int32_t factor, int shift, int32_t max)
{
    int32_t v = ((int32_t)baseline * factor) >> shift;
    return (uint16_t)(v > max ? max : v);
}

int isp_set_sharpness(int value /* -50..50 */)
{
    if (!g_sharp_baseline_valid) {
        g_sharp_baseline = calloc(1, SHARP_BUF_SIZE);
        if (!g_sharp_baseline) return -1;
        if (isp_tunnel(ISP_INNER_GET_SHARP, g_sharp_baseline) != 0) {
            free(g_sharp_baseline);
            g_sharp_baseline = NULL;
            return -1;
        }
        g_sharp_baseline_valid = 1;
    }

    uint8_t *buf = malloc(SHARP_BUF_SIZE);
    if (!buf) return -1;
    memcpy(buf, g_sharp_baseline, SHARP_BUF_SIZE);

    int32_t v100   = value + 50;               /* 0..100 */
    int32_t ivar7  = (v100 * 1024) / 50;        /* 0..2048, 1024 = neutral */
    int32_t ivar8  = (v100 * 16)   / 50;        /* 0..32,   16   = neutral */
    int32_t ivar9  = (v100 * 255)  / 50;        /* 0..510,  255  = neutral */
    int32_t ivar10 = ((50 - value) * 255) / 50; /* 510..0,  255  = neutral */

    int16_t *kernel_a = (int16_t *)(buf + 0x678);
    int16_t *kernel_b = (int16_t *)(buf + 0x878);
    int16_t *base_a   = (int16_t *)(g_sharp_baseline + 0x678);
    int16_t *base_b   = (int16_t *)(g_sharp_baseline + 0x878);
    for (int i = 0; i < 256; i++) {
        kernel_a[i] = sharp_kernel_scale(base_a[i], ivar7);
        kernel_b[i] = sharp_kernel_scale(base_b[i], ivar7);
    }

    static const int iVar8_offs[] = { 0x642, 0x650, 0x65e, 0x668 };
    for (size_t i = 0; i < sizeof(iVar8_offs)/sizeof(iVar8_offs[0]); i++) {
        int off = iVar8_offs[i];
        *(uint16_t *)(buf + off) = sharp_field_scale(*(uint16_t *)(g_sharp_baseline + off), ivar8, 4, 15);
    }

    static const int iVar9_offs[] = { 0x64a, 0x64e, 0x658, 0x65c, 0x662, 0x666, 0x66c, 0x670 };
    for (size_t i = 0; i < sizeof(iVar9_offs)/sizeof(iVar9_offs[0]); i++) {
        int off = iVar9_offs[i];
        *(uint16_t *)(buf + off) = sharp_field_scale(*(uint16_t *)(g_sharp_baseline + off), ivar9, 8, 255);
    }

    static const int iVar10_offs[] = { 0x648, 0x64c, 0x656, 0x65a, 0x660, 0x664, 0x66a, 0x66e };
    for (size_t i = 0; i < sizeof(iVar10_offs)/sizeof(iVar10_offs[0]); i++) {
        int off = iVar10_offs[i];
        *(uint16_t *)(buf + off) = sharp_field_scale(*(uint16_t *)(g_sharp_baseline + off), ivar10, 8, 255);
    }

    int ret = isp_tunnel(ISP_INNER_SHARP, buf);
    free(buf);
    return ret;
}

/* Forget all cached picture-control baselines (saturation/contrast/
 * brightness/sharpness). Call this after any full ISP pipeline reload
 * (isp_pipeline_init()/isp_pipeline_init_night()) — those resend the
 * hardware's raw calibration defaults, which invalidates whatever "original
 * untouched value" each isp_set_*() cached on its first call. Without this,
 * a day/night transition after the user has touched a Picture slider would
 * compute new deltas against a stale baseline instead of the freshly
 * reloaded one. Doesn't undo any currently-applied slider value — the next
 * isp_set_*() call for a given control will just re-cache its baseline and
 * carry on from there. */
void isp_reset_picture_baselines(void)
{
    g_saturation_baseline_valid = 0;
    g_contrast_baseline_valid   = 0;
    g_brightness_baseline_valid = 0;
    g_sharp_baseline_valid      = 0;
    free(g_sharp_baseline);
    g_sharp_baseline = NULL;
}
