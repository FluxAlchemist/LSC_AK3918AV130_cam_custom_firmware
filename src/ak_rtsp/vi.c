#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "ak_rtsp.h"
#include "ak_ioctls.h"
#include "vi.h"

/* -------------------------------------------------------------------------
 * VI channel attribute setup — ground truth from strace of anyka_ipc.
 *
 * Confirmed order from fast_ioctl_raw.log (factory RTSP run):
 *
 * Main channel (fd=video-0-0):
 *   1. CROPCAP x2 (query bounds — confirms 1920x1080 sensor)
 *   2. VI_IOCTL_TUNNEL inner=0x106, value=0   (VI_INNER_CHN_ENABLE)
 *   3. VI_IOCTL_TUNNEL inner=0x101, value=0, width=1920, height=1080
 *      (VI_INNER_CHN_PRES — uses vi_chn_res_hint, offsets 20/24 = w/h)
 *   4. VI_IOCTL_TUNNEL inner=0x103, value=1   (done_mode = SLICE)
 *   5. VI_IOCTL_TUNNEL inner=0x102, value=3   (slice_num = 3)
 *   6. VI_IOCTL_TUNNEL inner=0x104, value=4   (block_num = 4)
 *   7. CROPCAP again (one more)
 *   8. S_CROP  (0,0 → 1920x1080)  ← full sensor, NOT output res
 *   9. S_FMT   (1920x1080, NV12)
 *  10. VI_IOCTL_TUNNEL inner=0x109, value=3112960 (buf_size, after S_FMT)
 *
 * Sub channel (fd=video-0-1):
 *   1. VI_IOCTL_TUNNEL inner=0x103, value=0   (done_mode = FRAME)
 *   2. CROPCAP
 *   3. S_CROP  (0,0 → 1920x1080)  ← full sensor
 *   4. S_FMT   (640x360, NV12)
 *   5. VI_IOCTL_TUNNEL inner=0x109, value=348160 (buf_size)
 *
 * TD channel (fd=video-0-2):
 *   1. VI_IOCTL_TUNNEL inner=0x103, value=0   (done_mode = FRAME)
 *   2. CROPCAP
 *   3. S_CROP  (0,0 → 1920x1080)  ← full sensor
 *   4. S_FMT   (320x180, NV12)
 *   5. VI_IOCTL_TUNNEL inner=0x109, value=90112 (buf_size, page-aligned)
 *
 * buf_sizes: (w*h*3/2 rounded up to 4096-byte pages)
 *   main = 3112960 (1920*1080*1.5, no rounding needed at 4096 boundary)
 *   sub  = 348160  (640*360*1.5 = 345600, ceil to 85 pages * 4096)
 *   TD   = 90112   (320*180*1.5 = 86400, ceil to 22 pages * 4096)
 * ------------------------------------------------------------------------- */

#define SUB_WIDTH   640
#define SUB_HEIGHT  360
#define TD_WIDTH    320
#define TD_HEIGHT   180

/* Page-aligned NV12 frame sizes (confirmed from strace QUERYBUF output) */
#define MAIN_BUF_SIZE  3112960
#define SUB_BUF_SIZE   348160
#define TD_BUF_SIZE    90112

/* Exact NV12 pixel sizes (width*height*3/2) — used in QBUF.length field.
 * strace shows QBUF uses exact size, NOT the page-aligned buf_size. */
#define MAIN_FRAME_BYTES  (ENC_WIDTH * ENC_HEIGHT * 3 / 2)
#define SUB_FRAME_BYTES   (SUB_WIDTH * SUB_HEIGHT * 3 / 2)
#define TD_FRAME_BYTES    (TD_WIDTH  * TD_HEIGHT  * 3 / 2)

static int setup_channel_attrs(int fd, int is_main)
{
    struct vi_cmd_wrapper    cmd;
    struct vi_chn_res_hint   pres;
    struct vi_fmt_wrapper    fmt;
    struct vi_crop_req       crop;
    uint32_t                 crop_cap[11];

    if (is_main) {
        /* --- Main channel: CROPCAP x2, 0x106, 0x101, 0x103, 0x102, 0x104 --- */
        memset(crop_cap, 0, sizeof(crop_cap));
        crop_cap[0] = 1;
        ioctl(fd, VI_IOCTL_G_CROP_CAP, crop_cap);  /* error non-fatal */
        ioctl(fd, VI_IOCTL_G_CROP_CAP, crop_cap);

        /* inner_cmd 0x106 — channel enable/prepare, value=0 */
        memset(&cmd, 0, sizeof(cmd));
        cmd.flag = 1; cmd.inner_cmd = VI_INNER_CHN_ENABLE; cmd.value = 0;
        if (ioctl(fd, VI_IOCTL_TUNNEL, &cmd) < 0) {
            perror("VI 0x106 (CHN_ENABLE)"); return -1;
        }

        /* inner_cmd 0x101 — pre-resolution hint, width/height at offsets 20/24 */
        memset(&pres, 0, sizeof(pres));
        pres.flag      = 1;
        pres.inner_cmd = VI_INNER_CHN_PRES;
        pres.value     = 0;
        pres.width     = ENC_WIDTH;
        pres.height    = ENC_HEIGHT;
        if (ioctl(fd, VI_IOCTL_TUNNEL, &pres) < 0) {
            perror("VI 0x101 (CHN_PRES)"); return -1;
        }

        /* done_mode=1 (SLICE), slice_num=3, block_num=4 */
        memset(&cmd, 0, sizeof(cmd));
        cmd.flag = 1; cmd.inner_cmd = VI_INNER_DONE_MODE; cmd.value = 1;
        if (ioctl(fd, VI_IOCTL_TUNNEL, &cmd) < 0) {
            perror("VI done_mode=1"); return -1;
        }

        memset(&cmd, 0, sizeof(cmd));
        cmd.flag = 1; cmd.inner_cmd = VI_INNER_SLICE_NUM; cmd.value = 3;
        if (ioctl(fd, VI_IOCTL_TUNNEL, &cmd) < 0) {
            perror("VI slice_num=3"); return -1;
        }

        memset(&cmd, 0, sizeof(cmd));
        cmd.flag = 1; cmd.inner_cmd = VI_INNER_BLOCK_NUM; cmd.value = 4;
        if (ioctl(fd, VI_IOCTL_TUNNEL, &cmd) < 0) {
            perror("VI block_num=4"); return -1;
        }

        /* One more CROPCAP query, then S_CROP full sensor, S_FMT output res */
        memset(crop_cap, 0, sizeof(crop_cap));
        crop_cap[0] = 1;
        ioctl(fd, VI_IOCTL_G_CROP_CAP, crop_cap);

        memset(&crop, 0, sizeof(crop));
        crop.flag = 1; crop.left = 0; crop.top = 0;
        crop.width = ENC_WIDTH; crop.height = ENC_HEIGHT;
        if (ioctl(fd, VI_IOCTL_S_CROP, &crop) < 0) {
            perror("VI S_CROP (main)"); return -1;
        }

        memset(&fmt, 0, sizeof(fmt));
        fmt.flag = 1; fmt.width = ENC_WIDTH; fmt.height = ENC_HEIGHT;
        fmt.pixelformat = VI_PIXFMT_NV12; fmt.field = 4;
        if (ioctl(fd, VI_IOCTL_S_FMT, &fmt) < 0) {
            perror("VI S_FMT (main)"); return -1;
        }

        memset(&cmd, 0, sizeof(cmd));
        cmd.flag = 1; cmd.inner_cmd = VI_INNER_BUF_SIZE; cmd.value = MAIN_BUF_SIZE;
        if (ioctl(fd, VI_IOCTL_TUNNEL, &cmd) < 0) {
            perror("VI buf_size (main)"); return -1;
        }

        printf("[ak_rtsp] VI main: mode=SLICE s=3 b=4 crop=1920x1080 fmt=1920x1080 buf=%u\n",
               MAIN_BUF_SIZE);

    } else if (fd == g_vid_sub) {
        /* --- Sub channel: done_mode=FRAME, CROPCAP, S_CROP full, S_FMT 640x360 --- */
        memset(&cmd, 0, sizeof(cmd));
        cmd.flag = 1; cmd.inner_cmd = VI_INNER_DONE_MODE; cmd.value = 0;
        ioctl(fd, VI_IOCTL_TUNNEL, &cmd);  /* error non-fatal for sub/TD */

        memset(crop_cap, 0, sizeof(crop_cap));
        crop_cap[0] = 1;
        ioctl(fd, VI_IOCTL_G_CROP_CAP, crop_cap);

        memset(&crop, 0, sizeof(crop));
        crop.flag = 1; crop.left = 0; crop.top = 0;
        crop.width = ENC_WIDTH; crop.height = ENC_HEIGHT;  /* full sensor */
        if (ioctl(fd, VI_IOCTL_S_CROP, &crop) < 0) {
            perror("VI S_CROP (sub)"); return -1;
        }

        memset(&fmt, 0, sizeof(fmt));
        fmt.flag = 1; fmt.width = SUB_WIDTH; fmt.height = SUB_HEIGHT;
        fmt.pixelformat = VI_PIXFMT_NV12; fmt.field = 4;
        if (ioctl(fd, VI_IOCTL_S_FMT, &fmt) < 0) {
            perror("VI S_FMT (sub)"); return -1;
        }

        memset(&cmd, 0, sizeof(cmd));
        cmd.flag = 1; cmd.inner_cmd = VI_INNER_BUF_SIZE; cmd.value = SUB_BUF_SIZE;
        if (ioctl(fd, VI_IOCTL_TUNNEL, &cmd) < 0) {
            perror("VI buf_size (sub)"); return -1;
        }

        printf("[ak_rtsp] VI sub:  mode=FRAME crop=1920x1080 fmt=%ux%u buf=%u\n",
               SUB_WIDTH, SUB_HEIGHT, SUB_BUF_SIZE);

    } else {
        /* --- TD channel: done_mode=FRAME, CROPCAP, S_CROP full, S_FMT 320x180 --- */
        memset(&cmd, 0, sizeof(cmd));
        cmd.flag = 1; cmd.inner_cmd = VI_INNER_DONE_MODE; cmd.value = 0;
        ioctl(fd, VI_IOCTL_TUNNEL, &cmd);

        memset(crop_cap, 0, sizeof(crop_cap));
        crop_cap[0] = 1;
        ioctl(fd, VI_IOCTL_G_CROP_CAP, crop_cap);

        memset(&crop, 0, sizeof(crop));
        crop.flag = 1; crop.left = 0; crop.top = 0;
        crop.width = ENC_WIDTH; crop.height = ENC_HEIGHT;  /* full sensor */
        if (ioctl(fd, VI_IOCTL_S_CROP, &crop) < 0) {
            perror("VI S_CROP (TD)"); return -1;
        }

        memset(&fmt, 0, sizeof(fmt));
        fmt.flag = 1; fmt.width = TD_WIDTH; fmt.height = TD_HEIGHT;
        fmt.pixelformat = VI_PIXFMT_NV12; fmt.field = 4;
        if (ioctl(fd, VI_IOCTL_S_FMT, &fmt) < 0) {
            perror("VI S_FMT (TD)"); return -1;
        }

        memset(&cmd, 0, sizeof(cmd));
        cmd.flag = 1; cmd.inner_cmd = VI_INNER_BUF_SIZE; cmd.value = TD_BUF_SIZE;
        if (ioctl(fd, VI_IOCTL_TUNNEL, &cmd) < 0) {
            perror("VI buf_size (TD)"); return -1;
        }

        printf("[ak_rtsp] VI TD:   mode=FRAME crop=1920x1080 fmt=%ux%u buf=%u\n",
               TD_WIDTH, TD_HEIGHT, TD_BUF_SIZE);
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * Grab one raw NV12 frame from the sub channel and write it to a file.
 *
 * The sub channel (640×360) is already streaming after vi_start_capture().
 * This does a plain DQBUF (waiting up to 200ms via poll), writes the full
 * NV12 frame (345600 bytes) to the given path, then re-queues the buffer.
 *
 * View the saved file on PC with:
 *   ffplay -f rawvideo -video_size 640x360 -pixel_format nv12 <file>
 *   ffmpeg -f rawvideo -video_size 640x360 -pix_fmt nv12 -i <file> out.png
 *
 * Trigger via SIGUSR1 on the camera:
 *   kill -USR1 $(pidof ak_rtsp)
 * ------------------------------------------------------------------------- */
int vi_grab_sub_frame(const char *path)
{
    struct vi_buffer buf;
    int dqbuf_ok;

    /* Try standard DQBUF first. */
    memset(&buf, 0, sizeof(buf));
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    dqbuf_ok = (ioctl(g_vid_sub, VI_IOCTL_DQBUF, &buf) == 0);
    if (dqbuf_ok) {
        printf("[grab] DQBUF OK buf[%u]\n", buf.index);
    } else {
        /* The AK VI driver never sets sub-channel buffers to V4L2 DONE state —
         * DQBUF always returns EAGAIN regardless of blocking mode or how many
         * frames have elapsed.  Diagnose the buffer states via QUERYBUF, then
         * fall back to a direct DMA-buffer peek: the ISP writes frames into the
         * mmap'd buffer continuously after STREAMON; we just wait a few frame
         * periods and read buf[0] directly. */
        int i;
        for (i = 0; i < VI_SUB_BUF_COUNT; i++) {
            struct vi_buffer qb;
            memset(&qb, 0, sizeof(qb));
            qb.index  = i;
            qb.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            qb.memory = V4L2_MEMORY_MMAP;
            if (ioctl(g_vid_sub, VI_IOCTL_QUERYBUF, &qb) == 0)
                printf("[grab] QUERYBUF sub[%d] flags=0x%x bytesused=%u\n",
                       i, qb.flags, qb.bytesused);
        }
        fprintf(stderr, "[grab] DQBUF errno=%d — using direct DMA peek on buf[0]\n", errno);
        /* Wait 4 frames (267ms at 15fps) so the ISP has written valid data */
        usleep(267000);
        buf.index = 0;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        perror("[grab] fopen");
    } else {
        size_t n = fwrite(g_vi_sub_bufs[buf.index], 1, SUB_FRAME_BYTES, f);
        fclose(f);
        if (n == SUB_FRAME_BYTES)
            printf("[ak_rtsp] grab: %s  (%u bytes, buf[%u])\n",
                   path, SUB_FRAME_BYTES, buf.index);
        else
            fprintf(stderr, "[grab] short write: %zu/%u bytes\n", n, SUB_FRAME_BYTES);
    }

    /* Re-queue only if we actually DQBUFed (direct-peek path leaves buffer QUEUED) */
    if (dqbuf_ok) {
        buf.m_offset = g_vi_sub_mmap_offsets[buf.index];
        buf.length   = SUB_FRAME_BYTES;
        if (ioctl(g_vid_sub, VI_IOCTL_QBUF, &buf) < 0)
            perror("[grab] re-QBUF (non-fatal)");
    }

    return f ? 0 : -1;
}

int vi_set_channel_attr(void)
{
    printf("[ak_rtsp] vi_set_channel_attr: entry (main=%d sub=%d td=%d)\n",
           g_vid_main, g_vid_sub, g_vid_td);
    fflush(stdout);

    if (setup_channel_attrs(g_vid_main, 1) < 0) return -1;
    if (setup_channel_attrs(g_vid_sub,  0) < 0) return -1;
    if (setup_channel_attrs(g_vid_td,   0) < 0) return -1;

    return 0;
}

/* -------------------------------------------------------------------------
 * Allocate VI buffers and STREAMON all three channels.
 *
 * Confirmed buffer counts from strace (fast_ioctl_raw.log):
 *   main (video-0-0): REQBUFS count=1, QUERYBUF length=1037653
 *   sub  (video-0-1): REQBUFS count=2, QUERYBUF length=348160 each
 *   TD   (video-0-2): REQBUFS count=2, QUERYBUF length=90112  each
 *
 * QBUF length field: exact NV12 pixel size (w*h*3/2), NOT page-aligned size.
 *   main: 3110400, sub: 345600, TD: 86400
 * (anyka_ipc passes exact frame bytes in QBUF even though QUERYBUF returns
 *  a larger page-aligned value — confirmed from strace L455/L492/L495)
 *
 * STREAMON order: main → sub → TD (all before any VENC open).
 * anyka_ipc NEVER calls DQBUF on main in steady state — the b2i bridge feeds
 * the VENC ring buffer directly. Only sub and TD are polled by the app.
 * We don't poll sub/TD either (no RTSP encode needed from them), but we must
 * STREAMON them so the kernel ISP driver initialises all channel contexts.
 * ------------------------------------------------------------------------- */
/* mmap_offsets: if non-NULL, receives the QUERYBUF.m_offset for each buffer.
 * These are the values the driver expects back in QBUF.m_offset (confirmed from
 * anyka_ipc strace: re-QBUF after DQBUF passes m.offset=0 for buf[0], which
 * matches the QUERYBUF offset, NOT the virtual address from mmap()). */
static int setup_channel_bufs(int fd, int count,
                               void **bufs, uint32_t *sizes,
                               uint32_t *mmap_offsets,
                               uint32_t qbuf_frame_bytes,
                               const char *name)
{
    struct vi_reqbufs reqbufs;
    struct vi_buffer  buf;
    uint32_t          local_offsets[8]; /* fallback if caller passes NULL */
    uint32_t         *offsets = mmap_offsets ? mmap_offsets : local_offsets;
    int               i;

    memset(&reqbufs, 0, sizeof(reqbufs));
    reqbufs.count  = count;
    reqbufs.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    reqbufs.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VI_IOCTL_REQBUFS, &reqbufs) < 0) {
        perror("VI_IOCTL_REQBUFS"); return -1;
    }
    printf("[ak_rtsp] VI %s REQBUFS: got %u buffers\n", name, reqbufs.count);

    for (i = 0; i < count; i++) {
        memset(&buf, 0, sizeof(buf));
        buf.index  = i;
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd, VI_IOCTL_QUERYBUF, &buf) < 0) {
            perror("VI_IOCTL_QUERYBUF"); return -1;
        }
        sizes[i]   = buf.length;
        offsets[i] = buf.m_offset;  /* save mmap offset for QBUF */
        bufs[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                       MAP_SHARED, fd, buf.m_offset);
        if (bufs[i] == MAP_FAILED) {
            perror("mmap vi buf"); return -1;
        }
        printf("[ak_rtsp] VI %s buf[%d]: size=%u mmap_off=0x%x virt=%p\n",
               name, i, buf.length, buf.m_offset, bufs[i]);
    }

    for (i = 0; i < count; i++) {
        memset(&buf, 0, sizeof(buf));
        buf.index    = i;
        buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory   = V4L2_MEMORY_MMAP;
        buf.m_offset = offsets[i];   /* mmap offset from QUERYBUF, NOT virtual addr */
        buf.length   = qbuf_frame_bytes;
        if (ioctl(fd, VI_IOCTL_QBUF, &buf) < 0) {
            perror("VI_IOCTL_QBUF"); return -1;
        }
    }
    printf("[ak_rtsp] VI %s buffers queued\n", name);

    return 0;
}

int vi_start_capture(void)
{
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    /* Flush any stale stream state on all three channels */
    ioctl(g_vid_main, VI_IOCTL_STREAMOFF, &type);
    ioctl(g_vid_sub,  VI_IOCTL_STREAMOFF, &type);
    ioctl(g_vid_td,   VI_IOCTL_STREAMOFF, &type);

    /* Allocate + queue buffers for all three channels (REQBUFS/QUERYBUF/QBUF) */
    if (setup_channel_bufs(g_vid_main, VI_BUF_COUNT,
                            g_vi_bufs, g_vi_buf_sizes, NULL,
                            MAIN_FRAME_BYTES, "Main") < 0) return -1;

    if (setup_channel_bufs(g_vid_sub, VI_SUB_BUF_COUNT,
                            g_vi_sub_bufs, g_vi_sub_buf_sizes, g_vi_sub_mmap_offsets,
                            SUB_FRAME_BYTES, "Sub") < 0) return -1;

    if (setup_channel_bufs(g_vid_td, VI_TD_BUF_COUNT,
                            g_vi_td_bufs, g_vi_td_buf_sizes, NULL,
                            TD_FRAME_BYTES, "TD") < 0) return -1;

    /* STREAMON in order: main, sub, TD — all before any VENC open.
     * This fires b2i_pp_chn_init in ak_isp.ko for each channel.
     * With no VENC fd open, the init runs cleanly (no div/0 race). */
    if (ioctl(g_vid_main, VI_IOCTL_STREAMON, &type) < 0) {
        perror("VI_IOCTL_STREAMON (main)"); return -1;
    }
    printf("[ak_rtsp] VI main STREAMON OK\n");

    if (ioctl(g_vid_sub, VI_IOCTL_STREAMON, &type) < 0) {
        perror("VI_IOCTL_STREAMON (sub)"); return -1;
    }
    printf("[ak_rtsp] VI sub STREAMON OK\n");

    if (ioctl(g_vid_td, VI_IOCTL_STREAMON, &type) < 0) {
        perror("VI_IOCTL_STREAMON (TD)"); return -1;
    }
    printf("[ak_rtsp] VI TD STREAMON OK (all three channels running, b2i_pp_chn_init fired)\n");

    return 0;
}
