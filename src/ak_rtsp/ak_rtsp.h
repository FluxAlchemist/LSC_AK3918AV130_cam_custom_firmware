#pragma once
/* Shared constants, device paths, and global state for the ak_rtsp binary.
 * All globals are defined in main.c and declared extern here so each
 * translation unit can access them without passing them as parameters. */

#include <stdint.h>
#include <stdbool.h>
#include <sys/mman.h>

/* -------------------------------------------------------------------------
 * Device paths
 * ------------------------------------------------------------------------- */
#define DEV_ISP         "/dev/isp-param-0"
#define DEV_SUBDEV      "/dev/v4l-subdev0"
#define DEV_VIDEO_MAIN  "/dev/video-0-0"
#define DEV_VIDEO_SUB   "/dev/video-0-1"
#define DEV_VIDEO_TD    "/dev/video-0-2"
#define DEV_VENC        "/dev/venc"
#define DEV_VENC_CHN0   "/dev/venc-chn0"
#define SENSOR_ISP_CONF "/tmp/sensor_isp.conf"
#define DEV_PCM_CAPTURE  "/dev/pcmC0D0c"  /* confirmed by isp_hook capture */
#define DEV_PCM_PLAYBACK "/dev/pcmC0D0p"  /* AoPlayPcmThread's fd — opened but unused */
#define DEV_PCM_LOOPBACK "/dev/pcmC0D0l"  /* LoopbackThread's fd — opened but unused */

/* -------------------------------------------------------------------------
 * Main channel encoding parameters (from _ht_hw_settings.ini / boot log)
 * ------------------------------------------------------------------------- */
#define ENC_WIDTH       1920
#define ENC_HEIGHT      1080
#define ENC_FPS         15
#define ENC_GOP         30
#define ENC_VENC_SIZE   777600   /* bitstream ring size observed in boot log */
#define ENC_RING_SIZE   4120384  /* H.264 1080p ring pool size from boot log */

#define VI_BUF_COUNT    1        /* Only 1 buffer needed — kernel encode mode */

#define RTSP_SERVER_PORT 554

/* -------------------------------------------------------------------------
 * Global device file descriptors (defined in main.c)
 * ------------------------------------------------------------------------- */
extern int g_isp_fd;
extern int g_subdev;
extern int g_vid_main;
extern int g_vid_sub;
extern int g_vid_td;
extern int g_venc_fd;
extern int g_chn0_fd;

/* -------------------------------------------------------------------------
 * VENC DMA state (defined in main.c, populated by venc_create_pool)
 * ------------------------------------------------------------------------- */
extern void     *g_venc_virt;   /* userspace mmap of bitstream ring */
extern uint32_t  g_venc_phys;   /* physical base address of ring    */

/* -------------------------------------------------------------------------
 * VI capture buffer state (defined in main.c, populated by vi_start_capture)
 * ------------------------------------------------------------------------- */
extern void     *g_vi_bufs[VI_BUF_COUNT];
extern uint32_t  g_vi_buf_sizes[VI_BUF_COUNT];

#define VI_SUB_BUF_COUNT 2
extern void     *g_vi_sub_bufs[VI_SUB_BUF_COUNT];
extern uint32_t  g_vi_sub_buf_sizes[VI_SUB_BUF_COUNT];
extern uint32_t  g_vi_sub_mmap_offsets[VI_SUB_BUF_COUNT]; /* QUERYBUF.m_offset — used in QBUF/re-QBUF */

#define VI_TD_BUF_COUNT  2
extern void     *g_vi_td_bufs[VI_TD_BUF_COUNT];
extern uint32_t  g_vi_td_buf_sizes[VI_TD_BUF_COUNT];
