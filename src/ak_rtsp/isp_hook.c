/* isp_hook.c — LD_PRELOAD ioctl interceptor for anyka_ipc ISP/VENC state capture.
 *
 * Logs to /tmp/isp_hook.log:
 *   - get_ae_run_info (0x80044952): windowed min/max/reversal-count summary,
 *     one line per ~1s (AE_WINDOW_N=15 samples @ ~15Hz) — format mirrors
 *     ak_rtsp/ae.c's ae_loop() exactly (lum[min max last], hw_exp[min max],
 *     reversals, <-- HUNTING flag) so the two logs are directly comparable:
 *     does stock anyka_ipc's own on-chip AE hunt on the same high-contrast
 *     scene ak_rtsp does? A prior 1-in-30 raw-sample throttle was too coarse
 *     to see fast hunting (same mistake fixed in ak_rtsp/ae.c earlier).
 *   - set/get_exposure_attr (0x8004498a/8b) every N calls — shows private+public fields
 *   - ISP_INNER_SENSOR_SETREG (0x4004496b) every call — shows which GC20C3 I2C
 *     registers anyka_ipc writes for gain/exposure, with exact values
 *   - VENC_IOCTL_CREATE_ENC (0x406056e0, added 2026-07-04): the raw 96-byte
 *     venc_create_enc_req payload anyka_ipc actually sends the kernel,
 *     labeled per-field to match ak_ioctls.h's struct layout, plus a raw hex
 *     word dump as a fallback in case the field offsets/labels are wrong.
 *     This exists because a pure Ghidra decompiled-stack trace of the
 *     equivalent userspace formatter (venc_check_and_format_rc_param) led to
 *     a value (all-zero rc struct) that panicked the kernel on real hardware.
 *     This capture reads the actual bytes off the
 *     wire instead of re-deriving them from decompiled stack layout.
 *     The actual field dump (added 2026-07-05) now lives in the shared
 *     venc_dump.h, also used by ak_rtsp/venc.c's DEBUG_IOCTL_DUMP build, so
 *     "what stock sent" and "what we sent" are always directly diffable.
 *   - AKPCM audio ioctl family (2026-07-04):
 *     generic catch-all for every ioctl whose magic/type byte is 'P' (0x50) —
 *     this covers all the akpcm (mic capture, /dev/akpcm_cdev1) ioctls
 *     reverse-engineered from libplat_ai.so (IOC_GET_PARS=0x401c50f2,
 *     IOC_PREPARE=0x804050f0, plus the no-arg 0x50e0/0x50e1/0x50e2 ones),
 *     decoded generically via the standard Linux _IOC(dir,type,nr,size)
 *     layout rather than hardcoded per-cmd, so any akpcm ioctl not yet
 *     identified (set_volume/set_nr/set_aec/etc., same ioctl family)
 *     gets logged too. No collision with the
 *     ISP wrapper or VENC ioctls above (both use magic 'V'=0x56). Logs a raw
 *     word dump of the payload (like CREATE_ENC's raw[] fallback) since the
 *     exact struct field layout isn't pinned down yet — this capture is
 *     what's needed to nail it down.
 *   - PCM open/read/write (2026-07-04): the AKPCM ioctl catch-all above already shows every ioctl on
 *     every fd, but not which fd is capture/playback/loopback, and nothing
 *     shows whether anyka_ipc's own read()/write() on these devices actually
 *     succeed. open() is hooked to tag any fd opened from a path containing
 *     "pcm" by its last path character (c/p/l, matching DEV_PCM_CAPTURE/
 *     _PLAYBACK/_LOOPBACK's naming), and read()/write() are hooked to log
 *     every call on a tagged fd. This is the direct way to answer: does
 *     anyka_ipc call SET_MODE on the playback fd at all, does it succeed
 *     there (unlike our crash), and does its own playback write()/capture
 *     read() actually get data through.
 *
 * No libc dependency — raw ARM EABI syscalls only, works with any libc on camera.
 *
 * Build:
 *   make isp_hook.so
 *
 * Usage (Telnet):
 *   rm -f /tmp/isp_hook.log
 *   killall -9 anyka_ipc nvtservice rtspserver watchdog 2>/dev/null; sleep 1
 *   LD_PRELOAD=/mnt/isp_hook.so /usr/sbin/main.sh &   # or /usr/bin/anyka_ipc
 *   sleep 20
 *   cat /tmp/isp_hook.log
 */
#include <stdint.h>
#include <stdarg.h>

/* ---- raw ARM EABI syscalls -------------------------------------------- */
static long _sc3(long n, long a, long b, long c)
{
    register long r0 asm("r0") = a;
    register long r1 asm("r1") = b;
    register long r2 asm("r2") = c;
    register long r7 asm("r7") = n;
    asm volatile ("svc #0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory", "cc");
    return r0;
}
static long _sc2(long n, long a, long b)
{
    register long r0 asm("r0") = a;
    register long r1 asm("r1") = b;
    register long r7 asm("r7") = n;
    asm volatile ("svc #0" : "+r"(r0) : "r"(r1), "r"(r7) : "memory", "cc");
    return r0;
}
#define SYS_IOCTL  54
#define SYS_OPEN    5
#define SYS_READ    3
#define SYS_WRITE   4
#define SYS_CLOSE   6
#define O_WRONLY    1
#define O_CREAT   0100
#define O_APPEND 02000

/* ---- ISP inner commands ----------------------------------------------- */
#define ISP_IOCTL_CMD      0xc0cc5616U
#define GET_EXPOSURE       0x8004498bU   /* read  0x720-byte exposure attr */
#define SET_EXPOSURE       0x8004498aU   /* write 0x720-byte exposure attr */
#define GET_AE_RUN_INFO    0x80044952U   /* read  36-byte AE stats         */
#define GET_3D_NR_STAT     0x80044970U   /* read  0x608-byte scene stats — polled by all 3A threads at 15Hz; may tick ISP AE */
#define GET_AE_STAT_INFO   0x80044991U   /* read  0xc0-byte AE stat info   */
#define SET_WB_ATTR        0x80044986U   /* write WB R/G/B gains           */
#define GET_WB_ATTR        0x80044987U   /* read  WB attr                  */
#define SENSOR_SETREG      0x4004496bU   /* write sensor I2C reg (8 bytes) */
#define SENSOR_GETREG      0x8004496cU   /* read  sensor I2C reg (8 bytes) */

/* VENC — different ioctl code entirely (not the ISP tunnel), payload is the
 * raw venc_create_enc_req struct directly (not wrapped in isp_wrapper).
 * Offsets match ak_rtsp/ak_ioctls.h's venc_create_enc_req/venc_enc_cfg/
 * venc_smart_cfg/venc_rc_cfg exactly. */
#define VENC_IOCTL_CREATE_ENC 0x406056e0U  /* _IOWR('V',0xe0, venc_create_enc_req[96]) */

struct isp_wrapper {
    uint32_t flag;
    uint32_t inner_cmd;
    void    *payload;
};

/* sensor setreg payload: {reg_addr u32, reg_value u32} */
struct sensor_reg { uint32_t addr; uint32_t val; };

/* ---- ISP inner_cmd name table -----------------------------------------
 * Every inner_cmd sent by isp_module_init's 24-module pipeline plus the
 * other individually-confirmed cmds, so the catch-all logger below prints a
 * readable name instead of a bare hex value the reader has to re-look-up
 * every time. Kept in sync with ak_rtsp/ak_ioctls.h's ISP_INNER_* defines —
 * duplicated here rather than #included because this file is intentionally
 * standalone (no libc, cross-compiles for any target libc on the camera).
 * Add 2026-07-05: request from repeatedly having to re-derive names for
 * cmds that show up in every capture. */
static const char *isp_cmd_name(uint32_t cmd)
{
    switch (cmd) {
    case 0x40044902U: return "BLC";
    case 0x40044904U: return "LSC";
    case 0x4004490aU: return "RAW_LUT";
    case 0x4004490cU: return "NR1";
    case 0x4004492bU: return "NR2";
    case 0x4004497dU: return "UVNR";
    case 0x4004492fU: return "3D_NR";
    case 0x40044906U: return "GB";
    case 0x4004490eU: return "DEMO";
    case 0x4004491aU: return "RGB_GAMMA";
    case 0x40044912U: return "CCM";
    case 0x40044936U: return "FCS";
    case 0x4004491bU: return "WDR";
    case 0x40044925U: return "SHARP";
    case 0x40044927U: return "SHARP_EX";
    case 0x4004493bU: return "SATURATION";
    case 0x40044939U: return "CONTRAST";
    case 0x4004493fU: return "RGB2YUV";
    case 0x40044941U: return "EFFECT";
    case 0x40044910U: return "DPC";
    case 0x8004498eU: return "LCE";
    case 0x40044960U: return "AF";
    case 0x40044973U: return "AWB_EX";
    case 0x80044988U: return "AWB_CALIB";
    case 0x4004496eU: return "MISC";
    case 0x40044975U: return "Y_GAMMA";
    case 0x40044977U: return "HUE";
    case 0x40044981U: return "GET_SENSOR_AE";
    case 0x80044984U: return "AE_SUSPEND";
    case 0x40044979U: return "FLIP_MIRROR"; /* not sent by ak_rtsp */
    default:          return (const char *)0;
    }
}

/* ---- minimal text formatting -------------------------------------------
 * REVERTED to self-contained (2026-07-05): briefly shared this with
 * ak_rtsp/venc.c's DEBUG_IOCTL_DUMP build via venc_dump.h, but the very
 * first hardware re-test after that refactor came back with EVERY log type
 * except the one CREATE_ENC line missing (no AE window, no SETREG, no WB —
 * all of which had worked repeatedly before, same script, same procedure).
 * Could not find the bug by static re-reading — reverting to this exact
 * known-good inline shape rather than risk another blind hardware
 * round-trip chasing a shared-header bug. venc_dump.h still exists and is
 * still used by ak_rtsp/venc.c's DEBUG_IOCTL_DUMP build (verified working
 * there) — just not included here anymore. If byte-exact comparison
 * between the two logs is needed again, diff the two files by hand; don't
 * re-attempt sharing the implementation without a way to bisect on
 * hardware faster than one full reflash-and-wait-for-3A-convergence cycle
 * per attempt. */
static void w_str(int fd, const char *s)
{
    const char *e = s; while (*e) e++;
    _sc3(SYS_WRITE, fd, (long)s, e - s);
}
static void w_u32(int fd, uint32_t v)
{
    /* Stack-local, NOT static/shared: ioctl() is hit concurrently from
     * multiple anyka_ipc threads (~15Hz ISP/AE poll thread vs.
     * ht_msg_service_th's venc reconfigure calls) — a shared static buffer
     * here previously let concurrent callers clobber each other's digits
     * mid-format (found 2026-07-04). Keep this fix even though the
     * shared-header refactor above it got reverted. */
    char nb[16];
    int i = 15; nb[i] = '\0';
    if (!v) { nb[--i] = '0'; }
    else { while (v) { nb[--i] = '0' + (v % 10); v /= 10; } }
    w_str(fd, nb + i);
}
static void w_hex(int fd, uint32_t v)   /* 0x prefix + 8 hex digits */
{
    char h[11]; h[0]='0'; h[1]='x'; h[10]='\0';
    for (int i = 9; i >= 2; i--) {
        int d = v & 0xf;
        h[i] = d < 10 ? '0'+d : 'a'+(d-10);
        v >>= 4;
    }
    w_str(fd, h);
}
static void w_frac(int fd, uint32_t v, uint32_t d)  /* v/d with 1 decimal */
{
    w_u32(fd, v/d); w_str(fd, "."); w_u32(fd, (v%d)*10/d);
}

/* ---- throttle for high-frequency calls -------------------------------- */
static unsigned g_expo_cnt  = 0;
static unsigned g_3dnr_cnt  = 0;
static unsigned g_other_cnt = 0;
#define NR_EVERY   30   /* log get_3d_nr_stat every N calls */
#define EXPO_EVERY  1   /* log every set/get_exposure_attr (infrequent) */

/* ---- windowed AE stats — mirrors ak_rtsp/ae.c's ae_loop() exactly, so the
 * two logs are directly comparable: does stock anyka_ipc's own AE hunt on
 * the same high-contrast scene where our on-chip-AE-only ak_rtsp does?
 * Previously this was a raw 1-in-30 (~2s) sample, which is too coarse to
 * see fast hunting — the same mistake we made in ak_rtsp before adding
 * windowed diagnostics there. */
#define AE_WINDOW_N   15   /* ~1s window at anyka_ipc's ~15Hz AE poll rate */
static int      g_win_n = 0;
static int      g_lum_min = 256, g_lum_max = -1, g_lum_last = -1, g_lum_prev = -1;
static uint32_t g_exp_min = 0xffffffffu, g_exp_max = 0;
static int      g_reversals = 0;
static int      g_dir = 0; /* -1 falling, 0 unknown/flat, +1 rising */
static uint32_t g_last_sgain = 0, g_last_ispgain = 0;

/* ---- PCM fd tagging (open/read/write hooks) ----------------------------
 * See the file header comment for why: the AKPCM ioctl catch-all
 * doesn't say which fd is which stream, and doesn't show read()/write()
 * results at all. */
#define MAX_PCM_FDS 8
static int  g_pcm_fd[MAX_PCM_FDS];
static char g_pcm_tag[MAX_PCM_FDS];  /* 'c'/'p'/'l' from DEV_PCM_*'s last path char, else '?' */
static int  g_pcm_fd_n = 0;

static int str_has(const char *s, const char *needle)
{
    for (; *s; s++) {
        const char *a = s, *b = needle;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

static char pcm_tag_for_path(const char *path)
{
    const char *e = path;
    while (*e) e++;
    if (e == path) return '?';
    char last = *(e - 1);
    return (last == 'c' || last == 'p' || last == 'l') ? last : '?';
}

static char pcm_tag_of_fd(int fd)
{
    for (int i = 0; i < g_pcm_fd_n; i++)
        if (g_pcm_fd[i] == fd) return g_pcm_tag[i];
    return 0;
}

int open(const char *path, int flags, ...)
{
    int mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    int fd = (int)_sc3(SYS_OPEN, (long)path, (long)flags, (long)mode);
    if (fd >= 0 && path && str_has(path, "pcm")) {
        char tag = pcm_tag_for_path(path);
        if (g_pcm_fd_n < MAX_PCM_FDS) {
            g_pcm_fd[g_pcm_fd_n]  = fd;
            g_pcm_tag[g_pcm_fd_n] = tag;
            g_pcm_fd_n++;
        }
        int f = (int)_sc3(SYS_OPEN, (long)"/tmp/isp_hook.log",
                          O_WRONLY|O_CREAT|O_APPEND, 0644);
        if (f >= 0) {
            char tstr[2] = { tag, 0 };
            w_str(f, "PCM_OPEN path="); w_str(f, path);
            w_str(f, " tag=");          w_str(f, tstr);
            w_str(f, " flags=");        w_hex(f, (uint32_t)flags);
            w_str(f, " -> fd=");        w_u32(f, (uint32_t)fd);
            w_str(f, "\n");
            _sc2(SYS_CLOSE, f, 0);
        }
    }
    return fd;
}

long read(int fd, void *buf, unsigned long count)
{
    long ret = _sc3(SYS_READ, fd, (long)buf, (long)count);
    char tag = pcm_tag_of_fd(fd);
    if (tag) {
        int f = (int)_sc3(SYS_OPEN, (long)"/tmp/isp_hook.log",
                          O_WRONLY|O_CREAT|O_APPEND, 0644);
        if (f >= 0) {
            char tstr[2] = { tag, 0 };
            w_str(f, "PCM_READ fd=");  w_u32(f, (uint32_t)fd);
            w_str(f, " tag=");         w_str(f, tstr);
            w_str(f, " count=");       w_u32(f, (uint32_t)count);
            w_str(f, " ret=");
            if (ret < 0) { w_str(f, "-"); w_u32(f, (uint32_t)(-ret)); }
            else w_u32(f, (uint32_t)ret);
            w_str(f, "\n");
            _sc2(SYS_CLOSE, f, 0);
        }
    }
    return ret;
}

long write(int fd, const void *buf, unsigned long count)
{
    long ret = _sc3(SYS_WRITE, fd, (long)buf, (long)count);
    char tag = pcm_tag_of_fd(fd);
    if (tag) {
        int f = (int)_sc3(SYS_OPEN, (long)"/tmp/isp_hook.log",
                          O_WRONLY|O_CREAT|O_APPEND, 0644);
        if (f >= 0) {
            char tstr[2] = { tag, 0 };
            w_str(f, "PCM_WRITE fd="); w_u32(f, (uint32_t)fd);
            w_str(f, " tag=");         w_str(f, tstr);
            w_str(f, " count=");       w_u32(f, (uint32_t)count);
            w_str(f, " ret=");
            if (ret < 0) { w_str(f, "-"); w_u32(f, (uint32_t)(-ret)); }
            else w_u32(f, (uint32_t)ret);
            w_str(f, "\n");
            _sc2(SYS_CLOSE, f, 0);
        }
    }
    return ret;
}

/* ---- hook ------------------------------------------------------------- */
static volatile int _g_init = 0;

int ioctl(int fd, unsigned int req, void *arg)
{
    /* First-call marker: if /tmp/isp_hook_loaded appears, the hook loaded. */
    if (!_g_init) {
        _g_init = 1;
        int f = (int)_sc3(SYS_OPEN, (long)"/tmp/isp_hook.pid",
                          O_WRONLY|O_CREAT|4, 0644);  /* 4 = O_TRUNC */
        if (f >= 0) { w_str(f, "ok\n"); _sc2(SYS_CLOSE, f, 0); }
    }

    int ret = (int)_sc3(SYS_IOCTL, fd, (long)req, (long)arg);

    /* -- VENC CREATE_ENC: capture the exact wire-level rc/enc struct anyka_ipc
     * sends, byte-for-byte, so we stop guessing this from Ghidra decompiled
     * stack traces (which already crashed hardware once). Not throttled —
     * this only fires a handful of times per boot (once per venc channel). */
    if (req == VENC_IOCTL_CREATE_ENC && arg) {
        /* Unconditional hit counter, logged before any field parsing, so a
         * future capture can prove whether every real CREATE_ENC call (we
         * expect ~146/session for the main channel per
         * ht_video_codec_start_encode's write_log count) actually reaches
         * this hook, even if something below this line fails. */
        static unsigned g_create_enc_seen = 0;
        g_create_enc_seen++;
        const uint8_t  *b = (const uint8_t *)arg;
#define RD32(off) (*(const uint32_t*)(b+(off)))
#define RD16(off) (*(const uint16_t*)(b+(off)))
#define RD8(off)  (*(const uint8_t *)(b+(off)))
        int f = (int)_sc3(SYS_OPEN, (long)"/tmp/isp_hook.log",
                          O_WRONLY|O_CREAT|O_APPEND, 0644);
        if (f >= 0) {
            w_str(f,"#"); w_u32(f, g_create_enc_seen);
            w_str(f," STOCK CREATE_ENC chn_id=");   w_u32(f, RD32(0x00));
            w_str(f," codec=");               w_hex(f, RD32(0x04));
            w_str(f," chroma=");              w_u32(f, RD32(0x08));
            w_str(f," w=");                   w_u32(f, RD16(0x0c));
            w_str(f," h=");                   w_u32(f, RD16(0x0e));
            w_str(f," rc_mode=");             w_u32(f, RD32(0x18));
            w_str(f," fps=");                 w_u32(f, RD16(0x1c));
            w_str(f," goplen=");              w_u32(f, RD16(0x1e));
            w_str(f," max_fps=");             w_u32(f, RD32(0x20));
            w_str(f," enc_level=");           w_u32(f, RD32(0x24));
            w_str(f," qp_or_kbps=");          w_u32(f, RD16(0x28));
            w_str(f," qp2=");                 w_u32(f, RD16(0x2a));
            w_str(f,"\n  smart_mode=");       w_u32(f, RD16(0x34));
            w_str(f," smart_goplen=");        w_u32(f, RD16(0x36));
            w_str(f," smart_quality=");       w_u32(f, RD16(0x38));
            w_str(f," rc_flag=");             w_u32(f, RD32(0x3c));
            w_str(f,"\n  rc.cub_size=");       w_u32(f, RD32(0x40));
            w_str(f," rc.minqp=");            w_u32(f, RD16(0x44));
            w_str(f," rc.maxqp=");            w_u32(f, RD16(0x46));
            w_str(f," rc.delta=");            w_u32(f, RD32(0x48));
            w_str(f," rc.I_pic_size=");       w_u32(f, RD32(0x4c));
            w_str(f," rc.P_pic_size=");       w_u32(f, RD32(0x50));
            w_str(f," rc.B_pic_size=");       w_u32(f, RD32(0x54));
            w_str(f," rc.flag=");             w_u32(f, RD32(0x58));
            w_str(f," rc.flag2=");            w_u32(f, RD8(0x5c));
            w_str(f," rc.srd_threshold=");    w_u32(f, RD8(0x5d));
            w_str(f,"\n  raw[0..23]:");
            for (int i = 0; i < 24; i++) { w_str(f," "); w_hex(f, RD32(i*4)); }
            w_str(f," ret="); w_u32(f, (uint32_t)ret);
            w_str(f,"\n");
            _sc2(SYS_CLOSE, f, 0);
        }
#undef RD32
#undef RD16
#undef RD8
        return ret;
    }

    /* -- AKPCM (audio input, /dev/akpcm_cdev1) ioctl family --
     * Type byte (bits 8-15) == 'P' (0x50) for every akpcm cmd, per
     * (recovered from libplat_ai.so:
     * IOC_GET_PARS=0x401c50f2 dir=R size=28, IOC_PREPARE=0x804050f0
     * dir=W size=64, plus 0x50e0/0x50e1/0x50e2 dir=none size=0). Decoded
     * generically here instead of listing individual cmd numbers so any
     * akpcm ioctl not yet identified is captured too. Not gated on `arg`
     * being non-NULL up front — the size=0 cmds are sometimes called with
     * arg==0 or a small immediate value cast to a pointer, both worth
     * logging as-is. */
    if (((req >> 8) & 0xffU) == 0x50U) {
        uint32_t dir = (req >> 30) & 0x3U;
        uint32_t sz  = (req >> 16) & 0x3fffU;
        uint32_t nr  = req & 0xffU;
        int f = (int)_sc3(SYS_OPEN, (long)"/tmp/isp_hook.log",
                          O_WRONLY|O_CREAT|O_APPEND, 0644);
        if (f >= 0) {
            char tag = pcm_tag_of_fd(fd);
            char tstr[2] = { tag ? tag : '?', 0 };
            w_str(f, "AKPCM fd="); w_u32(f, (uint32_t)fd);
            w_str(f, " tag=");     w_str(f, tstr);
            w_str(f, " req=");     w_hex(f, req);
            w_str(f, " nr=");      w_hex(f, nr);
            w_str(f, " dir=");     w_str(f, dir == 0 ? "none" : dir == 1 ? "R" : dir == 2 ? "W" : "RW");
            w_str(f, " size=");    w_u32(f, sz);
            if (sz > 0 && arg) {
                uint32_t nwords = sz >> 2;
                if (nwords > 16) nwords = 16;   /* cap at 64 bytes */
                const uint32_t *p = (const uint32_t *)arg;
                w_str(f, " payload:");
                for (uint32_t i = 0; i < nwords; i++) { w_str(f, " "); w_hex(f, p[i]); }
            } else {
                w_str(f, " arg="); w_hex(f, (uint32_t)(long)arg);
            }
            w_str(f, " ret="); w_u32(f, (uint32_t)ret);
            w_str(f, "\n");
            _sc2(SYS_CLOSE, f, 0);
        }
        return ret;
    }

    /* -- ALSA-magic ('A'=0x41) catch-all — added 2026-07-04.
     * Our AKPCM capture (magic 'P') never
     * showed a call that actually "starts" the capture stream — configure
     * succeeds but read() then fails. Theory: a real "start streaming"
     * trigger might use the standard Linux ALSA ioctl magic ('A') instead
     * of this driver's custom 'P' family, which our type=='P' filter above
     * would have silently missed (it never reached this far down). Decoded
     * generically exactly like the 'P' block, for the same reason. */
    if (((req >> 8) & 0xffU) == 0x41U) {
        uint32_t dir = (req >> 30) & 0x3U;
        uint32_t sz  = (req >> 16) & 0x3fffU;
        uint32_t nr  = req & 0xffU;
        int f = (int)_sc3(SYS_OPEN, (long)"/tmp/isp_hook.log",
                          O_WRONLY|O_CREAT|O_APPEND, 0644);
        if (f >= 0) {
            w_str(f, "ALSA fd="); w_u32(f, (uint32_t)fd);
            w_str(f, " req=");    w_hex(f, req);
            w_str(f, " nr=");     w_hex(f, nr);
            w_str(f, " dir=");    w_str(f, dir == 0 ? "none" : dir == 1 ? "R" : dir == 2 ? "W" : "RW");
            w_str(f, " size=");   w_u32(f, sz);
            if (sz > 0 && arg) {
                uint32_t nwords = sz >> 2;
                if (nwords > 16) nwords = 16;
                const uint32_t *p = (const uint32_t *)arg;
                w_str(f, " payload:");
                for (uint32_t i = 0; i < nwords; i++) { w_str(f, " "); w_hex(f, p[i]); }
            } else {
                w_str(f, " arg="); w_hex(f, (uint32_t)(long)arg);
            }
            w_str(f, " ret="); w_u32(f, (uint32_t)ret);
            w_str(f, "\n");
            _sc2(SYS_CLOSE, f, 0);
        }
        return ret;
    }

    if (req != ISP_IOCTL_CMD || !arg) {
        /* -- Known-fd catch-all — added 2026-07-04.
         * anyka_ipc's three akpcm fds were 6/8/9 in the last successful
         * capture (fd numbers can drift slightly run to run depending on what
         * else the process has opened by then, but stay in this general low
         * range since they're opened early in startup). This catches anything
         * at all on those specific fds regardless of ioctl magic, as a last
         * resort in case the "start capture" trigger uses a magic byte that
         * isn't 'P' or 'A'. Deliberately narrow (a handful of fd numbers, not
         * every fd) to avoid flooding the log with unrelated network/tty/file
         * ioctls from the rest of this large, busy process. MUST run only
         * here (req != ISP_IOCTL_CMD), never before the ISP wrapper check
         * above — the ISP fd is also opened early and could easily land in
         * this same fd range, and an unconditional catch would have
         * swallowed every ISP_IOCTL_CMD call before the detailed AE/WB/
         * SETREG decoders below ever saw it. */
        if (fd >= 6 && fd <= 10) {
            int f = (int)_sc3(SYS_OPEN, (long)"/tmp/isp_hook.log",
                              O_WRONLY|O_CREAT|O_APPEND, 0644);
            if (f >= 0) {
                w_str(f, "FD6-10 fd="); w_u32(f, (uint32_t)fd);
                w_str(f, " req=");      w_hex(f, req);
                w_str(f, " arg=");      w_hex(f, (uint32_t)(long)arg);
                w_str(f, " ret=");      w_u32(f, (uint32_t)ret);
                w_str(f, "\n");
                _sc2(SYS_CLOSE, f, 0);
            }
        }
        return ret;
    }
    const struct isp_wrapper *w = (const struct isp_wrapper *)arg;
    if (!w->payload) return ret;

    /* -- AE run info (luma stats, hw exposure) — windowed, see comment above -- */
    if (w->inner_cmd == GET_AE_RUN_INFO && ret == 0) {
        const uint8_t *p = (const uint8_t *)w->payload;
        int      lum    = p[0];
        uint32_t hw_exp = *(uint32_t*)(p+4);
        g_last_sgain   = *(uint32_t*)(p+8);
        g_last_ispgain = *(uint32_t*)(p+12);

        if (lum < g_lum_min) g_lum_min = lum;
        if (lum > g_lum_max) g_lum_max = lum;
        if (hw_exp < g_exp_min) g_exp_min = hw_exp;
        if (hw_exp > g_exp_max) g_exp_max = hw_exp;
        g_lum_last = lum;

        if (g_lum_prev >= 0 && lum != g_lum_prev) {
            int new_dir = (lum > g_lum_prev) ? 1 : -1;
            if (g_dir != 0 && new_dir != g_dir) g_reversals++;
            g_dir = new_dir;
        }
        g_lum_prev = lum;
        g_win_n++;

        if (g_win_n < AE_WINDOW_N) return ret;

        int f = (int)_sc3(SYS_OPEN, (long)"/tmp/isp_hook.log",
                          O_WRONLY|O_CREAT|O_APPEND, 0644);
        if (f >= 0) {
            w_str(f,"AE window n="); w_u32(f, (uint32_t)g_win_n);
            w_str(f," lum[min="); w_u32(f, (uint32_t)g_lum_min);
            w_str(f," max=");     w_u32(f, (uint32_t)g_lum_max);
            w_str(f," last=");    w_u32(f, (uint32_t)g_lum_last);
            w_str(f,"] hw_exp[min="); w_u32(f, g_exp_min);
            w_str(f," max=");         w_u32(f, g_exp_max);
            w_str(f,"] reversals="); w_u32(f, (uint32_t)g_reversals);
            w_str(f," sgain=");    w_frac(f, g_last_sgain, 256); w_str(f,"x");
            w_str(f," ispgain=");  w_frac(f, g_last_ispgain, 256); w_str(f,"x");
            if (g_reversals >= 4 || g_lum_max - g_lum_min >= 100) w_str(f, "  <-- HUNTING");
            w_str(f,"\n");
            _sc2(SYS_CLOSE, f, 0);
        }

        g_win_n = 0; g_lum_min = 256; g_lum_max = -1; g_lum_prev = -1;
        g_exp_min = 0xffffffffu; g_exp_max = 0; g_reversals = 0; g_dir = 0;
        return ret;
    }

    /* -- set/get exposure attr (private+public section dump) -- */
    if ((w->inner_cmd == SET_EXPOSURE || w->inner_cmd == GET_EXPOSURE)) {
        if ((g_expo_cnt++ % EXPO_EVERY) != 0) return ret;
        const uint32_t *priv = (const uint32_t *)w->payload;
        const uint32_t *pub  = (const uint32_t *)((const uint8_t*)w->payload + 0x1c);
        const char *dir = (w->inner_cmd == SET_EXPOSURE) ? "SET_EXPO" : "GET_EXPO";
        int f = (int)_sc3(SYS_OPEN, (long)"/tmp/isp_hook.log",
                          O_WRONLY|O_CREAT|O_APPEND, 0644);
        if (f >= 0) {
            w_str(f, dir);
            w_str(f," priv[exp=");    w_u32(f,priv[1]);
            w_str(f," type=");        w_u32(f,priv[2]);
            w_str(f," sgain=");       w_frac(f,priv[3],1024); w_str(f,"x");
            w_str(f," isp=");         w_frac(f,priv[4],256);  w_str(f,"x]");
            w_str(f," pub[emin=");    w_u32(f,pub[0]);
            w_str(f," emax=");        w_u32(f,pub[1]);
            w_str(f," smin=");        w_frac(f,pub[2],256); w_str(f,"x");
            w_str(f," smax=");        w_frac(f,pub[3],256); w_str(f,"x");
            w_str(f," imin=");        w_frac(f,pub[4],256); w_str(f,"x");
            w_str(f," imax=");        w_frac(f,pub[5],256); w_str(f,"x]\n");
            _sc2(SYS_CLOSE, f, 0);
        }
        return ret;
    }

    /* -- SENSOR_SETREG: log EVERY write — these are the actual I2C register
     * writes the ISP SDK makes to the GC20C3 sensor for gain/exp control.
     * This shows us the exact register addresses and values anyka_ipc uses. */
    if (w->inner_cmd == SENSOR_SETREG) {
        const struct sensor_reg *r = (const struct sensor_reg *)w->payload;
        int f = (int)_sc3(SYS_OPEN, (long)"/tmp/isp_hook.log",
                          O_WRONLY|O_CREAT|O_APPEND, 0644);
        if (f >= 0) {
            w_str(f,"SETREG addr="); w_hex(f,r->addr);
            w_str(f," val=");        w_hex(f,r->val);
            w_str(f," ("); w_u32(f,r->val); w_str(f,")\n");
            _sc2(SYS_CLOSE, f, 0);
        }
    }

    /* -- SENSOR_GETREG: log reads too (less frequent) -- */
    if (w->inner_cmd == SENSOR_GETREG && ret == 0) {
        const struct sensor_reg *r = (const struct sensor_reg *)w->payload;
        int f = (int)_sc3(SYS_OPEN, (long)"/tmp/isp_hook.log",
                          O_WRONLY|O_CREAT|O_APPEND, 0644);
        if (f >= 0) {
            w_str(f,"GETREG addr="); w_hex(f,r->addr);
            w_str(f," val=");        w_hex(f,r->val);
            w_str(f," ("); w_u32(f,r->val); w_str(f,")\n");
            _sc2(SYS_CLOSE, f, 0);
        }
        return ret;
    }

    /* -- get_3d_nr_stat_info: polled by 3A at 15Hz — key diagnostic!
     * If anyka_ipc calls this but we don't, that's the missing tick for ISP AE. */
    if (w->inner_cmd == GET_3D_NR_STAT && ret == 0) {
        if ((g_3dnr_cnt++ % NR_EVERY) == 0) {
            const uint8_t *p = (const uint8_t *)w->payload;
            int f = (int)_sc3(SYS_OPEN, (long)"/tmp/isp_hook.log",
                              O_WRONLY|O_CREAT|O_APPEND, 0644);
            if (f >= 0) {
                /* log first 8 u32s of the 0x608-byte stat struct */
                const uint32_t *v = (const uint32_t *)p;
                w_str(f,"3DNR_STAT[0..7]:");
                for (int i = 0; i < 8; i++) {
                    w_str(f," "); w_u32(f, v[i]);
                }
                w_str(f,"\n");
                _sc2(SYS_CLOSE, f, 0);
            }
        }
        return ret;
    }

    /* -- get_ae_stat_info (0x80044991) -- */
    if (w->inner_cmd == GET_AE_STAT_INFO && ret == 0) {
        int f = (int)_sc3(SYS_OPEN, (long)"/tmp/isp_hook.log",
                          O_WRONLY|O_CREAT|O_APPEND, 0644);
        if (f >= 0) {
            w_str(f,"AE_STAT_INFO ret=0\n");
            _sc2(SYS_CLOSE, f, 0);
        }
        return ret;
    }

    /* -- set/get WB attr — log 32 u32s to see full struct --- */
    if (w->inner_cmd == SET_WB_ATTR || w->inner_cmd == GET_WB_ATTR) {
        const uint32_t *v = (const uint32_t *)w->payload;
        int f = (int)_sc3(SYS_OPEN, (long)"/tmp/isp_hook.log",
                          O_WRONLY|O_CREAT|O_APPEND, 0644);
        if (f >= 0) {
            w_str(f, w->inner_cmd == SET_WB_ATTR ? "SET_WB" : "GET_WB");
            w_str(f," [0..15]:");
            for (int i = 0; i < 16; i++) { w_str(f," "); w_u32(f, v[i]); }
            w_str(f,"\n");
            w_str(f,"       [16..31]:");
            for (int i = 16; i < 32; i++) { w_str(f," "); w_u32(f, v[i]); }
            w_str(f,"\n");
            _sc2(SYS_CLOSE, f, 0);
        }
        return ret;
    }

    /* -- catch-all: log OTHER ISP inner_cmds with their first 4 payload bytes.
     * Skip VI inner_cmds (0x100-0x10f range) — VI reuses the same ioctl code. */
    {
        uint32_t cmd = w->inner_cmd;
        if (cmd >= 0x100 && cmd <= 0x10f) return ret;
        /* Log every unknown cmd (not throttled — they're infrequent) */
        int f = (int)_sc3(SYS_OPEN, (long)"/tmp/isp_hook.log",
                          O_WRONLY|O_CREAT|O_APPEND, 0644);
        if (f >= 0) {
            const uint32_t *pay = (const uint32_t *)w->payload;
            const char *name = isp_cmd_name(cmd);
            if (name) { w_str(f, name); w_str(f, " cmd="); }
            else      { w_str(f, "UNKNOWN cmd="); }
            w_hex(f, cmd);
            w_str(f," payload[0..3]=");
            for (int i = 0; i < 4; i++) { w_str(f," "); w_hex(f, pay[i]); }
            w_str(f," ret="); w_u32(f, (uint32_t)ret);
            w_str(f,"\n");
            _sc2(SYS_CLOSE, f, 0);
        }
    }

    return ret;
}
