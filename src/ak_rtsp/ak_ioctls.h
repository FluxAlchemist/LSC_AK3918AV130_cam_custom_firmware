#pragma once
#include <stdint.h>

/* -------------------------------------------------------------------------
 * AK3918AV130 confirmed kernel ioctl codes (reversed from anyka_ipc).
 * All magic byte is 'V' (0x56).
 * ------------------------------------------------------------------------- */

/* /dev/isp-param-N — all ISP commands tunneled through one entry point */
#define ISP_IOCTL_CMD              0xc0cc5616  /* _IOWR('V',0x16, isp_cmd_wrapper)        */

/* ISP inner commands — all go through ISP_IOCTL_CMD via isp_cmd_wrapper.
 * Confirmed by Ghidra decompilation and isp_hook LD_PRELOAD capture. */

/* Sensor I2C register access */
#define ISP_INNER_SENSOR_SETREG    0x4004496b  /* AK_ISP_sensor_setreg     write I2C reg  */
#define ISP_INNER_SENSOR_GETREG    0x8004496c  /* AK_ISP_sensor_getreg     read  I2C reg  */

/* Sensor frame rate */
#define ISP_INNER_SET_SENSOR_FPS   0x4004497a  /* Ak_ISP_Set_Sensor_Fps    @ 0x00145ce4   */
#define ISP_INNER_GET_SENSOR_FPS   0x8004497b  /* Ak_ISP_Get_Sensor_Fps    @ 0x00145cf4   */

/* AE — exposure attribute (0x720-byte struct) */
#define ISP_INNER_GET_AE_RUN_INFO  0x80044952  /* AK_ISP_get_ae_run_info   36 B stat      */
#define ISP_INNER_GET_EXPOSURE     0x8004498b  /* AK_ISP_get_exposure_attr 0x720 B read   */
#define ISP_INNER_SET_EXPOSURE     0x8004498a  /* AK_ISP_set_exposure_attr 0x720 B write  */
#define ISP_INNER_GET_SENSOR_AE    0x40044981  /* AK_ISP_get_sensor_ae_info 16 B          */
#define ISP_INNER_AE_SUSPEND       0x80044984  /* Ak_ISP_Set_Ae_Suspend: 1=stop kernel AE, 0=resume */

/* WB — white balance (0x7e0-byte struct) */
#define ISP_INNER_SET_WB           0x80044986  /* AK_ISP_set_wb_attr       @ 0x00145758   */
#define ISP_INNER_GET_WB           0x80044987  /* AK_ISP_get_wb_attr                      */

/* Frame rate config */
#define ISP_INNER_SET_FRAME_RATE   0x4004494e  /* AK_ISP_set_frame_rate    @ 0x00145730   */
#define ISP_INNER_GET_FRAME_RATE   0x8004494f  /* AK_ISP_get_frame_rate    @ 0x00145748   */

/* Scene statistics */
#define ISP_INNER_GET_3D_NR_STAT   0x80044970  /* AK_ISP_get_3d_nr_stat_info 0x608 B      */
#define ISP_INNER_GET_AE_STAT      0x80044991  /* AK_ISP_get_ae_stat_info  0xc0 B         */

/* 3D NR reference frame dimensions */
#define ISP_INNER_3D_NR_REF        0x40044967  /* AK_ISP_set_3d_nr_ref_attr {w,h}         */

/* OSD / overlay (sub-routed; payload[0] = secondary inner_cmd) */
#define ISP_INNER_OSD_USER_PARAMS  0x4004496d  /* Ak_ISP_Set_User_Params   @ 0x001463b4   */

/* ISP pipeline module init — called by isp_module_init for each of 24 modules.
 * Module 0 (BLC) = 0x40044902; other modules follow non-sequential numbering.
 * All are called by our isp_pipeline_init() from sensor_isp.conf. */
#define ISP_INNER_SET_BLC          0x40044902  /* AK_ISP_set_blc_attr      @ 0x00145868   */

/* /dev/venc — control device (shared) */
#define VENC_IOCTL_QUERY_CHN    0x406056ce  /* _IOWR('V',0xce,...) query chn by name */
#define VENC_IOCTL_QUERY_DMA    0x805456ec  /* _IOWR('V',0xec,...) DMA block reqs    */

/* /dev/venc-chnN — per-channel device */
#define VENC_IOCTL_CREATE_POOL    0x402056ed  /* _IOW('V',0xed, venc_dma_pool_desc) */
#define VENC_IOCTL_DELETE_POOL    0x402056ee  /* _IOW('V',0xee, venc_dma_pool_desc) */
#define VENC_IOCTL_CREATE_ENC     0x406056e0  /* _IOWR('V',0xe0, venc_create_enc_req[96]) */
#define VENC_IOCTL_BIND_VI_CHN    0x401456e1  /* _IOW('V',0xe1, venc_bind_vi_req[20]) */
#define VENC_IOCTL_ACTIVATE       0x400456e2  /* _IOW('V',0xe2, uint32) start encoder */
#define VENC_IOCTL_DEACTIVATE     0x400456f5  /* _IOW('V',0xf5, uint32) stop  encoder */
#define VENC_IOCTL_GET_STREAM     0x402056ea  /* _IOW('V',0xea, venc_stream_req[32]) */
#define VENC_IOCTL_RELEASE_STREAM 0x402056ff  /* _IOW('V',0xff, venc_stream_req[32]) */

/* /dev/v4l-subdev0 — sensor subdev (sensor I2C register programming) */
#define ISP_SENSOR_INIT_IOCTL 0x40085301  /* _IOW('S',1,8) — program sensor via I2C reg table */

/* /dev/video-0-N — VI channel devices (standard V4L2) */
#define VI_IOCTL_REQBUFS   0xc0145608  /* VIDIOC_REQBUFS  — allocate capture buffers */
#define VI_IOCTL_QUERYBUF  0xc0445609  /* VIDIOC_QUERYBUF — get mmap offset+size     */
#define VI_IOCTL_QBUF      0xc044560f  /* VIDIOC_QBUF     — queue buffer to driver   */
#define VI_IOCTL_DQBUF     0xc0445611  /* VIDIOC_DQBUF    — dequeue filled buffer     */
#define VI_IOCTL_STREAMON  0x40045612  /* VIDIOC_STREAMON — start ISP pipeline        */
#define VI_IOCTL_STREAMOFF 0x40045613  /* VIDIOC_STREAMOFF— stop  ISP pipeline        */

/* VI channel attribute configuration — must be called BEFORE REQBUFS.
 * Reversed from vi_dev_set_chn_mode   @ 0x00142650
 *               vi_dev_set_chn_crop   @ 0x001429a4
 *               vi_dev_set_chn_res    @ 0x00142b2c
 *               vi_dev_set_chn_buf_size @ 0x00142308
 * Confirmed by boot_dmesg_stream.log lines 399-408.                          */

/* Tunnel ioctl shared by ISP and VI devices (same ioctl code 0xc0cc5616),
 * but when used on a video fd the 'value' field is a direct uint32 scalar,
 * NOT a pointer. The kernel vi driver reads it as an integer. */
#define VI_IOCTL_TUNNEL    0xc0cc5616  /* _IOWR('V',0x16,204) — vi scalar tunnel  */

/* Inner command codes for VI_IOCTL_TUNNEL (on /dev/video-0-N).
 * Confirmed by strace of anyka_ipc factory-RTSP run (fast_ioctl_raw.log).
 * Order on main channel: 0x106, 0x101, 0x103, 0x102, 0x104, then CROP/FMT/0x109.
 * Sub and TD channels only need: 0x103 (done_mode=0), then CROP/FMT/0x109. */
#define VI_INNER_CHN_ENABLE 0x106  /* first call on main, value=0; purpose unknown but required */
#define VI_INNER_CHN_PRES   0x101  /* pre-resolution hint on main; uses vi_chn_res_hint struct  */
#define VI_INNER_SLICE_NUM  0x102  /* set slice_num (only when done_mode != 0)                  */
#define VI_INNER_DONE_MODE  0x103  /* done_mode: 0=FRAME (sub/TD), 1=SLICE (main)               */
#define VI_INNER_BLOCK_NUM  0x104  /* set block_num (only when done_mode != 0)                  */
#define VI_INNER_BUF_SIZE   0x109  /* set per-channel DMA buffer size (bytes), after S_FMT      */

/* VI_IOCTL_TUNNEL wrapper — 204 bytes, flag=1, inner_cmd, scalar value, rest 0.
 * Reversed from vi_dev_set_chn_mode / vi_dev_set_chn_buf_size local struct. */
struct vi_cmd_wrapper {
    uint32_t flag;       /* always 1 */
    uint32_t inner_cmd;  /* VI_INNER_* */
    uint32_t value;      /* direct integer (NOT a pointer) */
    uint8_t  padding[192];
};

/* Extended tunnel wrapper for VI_INNER_CHN_PRES (0x101) — main channel only.
 * Confirmed from strace: strace maps the 204-byte ioctl as v4l2_streamparm so
 *   offset 20 → v4l2_captureparm.extendedmode = width
 *   offset 24 → v4l2_captureparm.readbuffers  = height
 * In our struct layout that is padding[8..11]=width, padding[12..15]=height. */
struct vi_chn_res_hint {
    uint32_t flag;       /* 1 */
    uint32_t inner_cmd;  /* VI_INNER_CHN_PRES = 0x101 */
    uint32_t value;      /* 0 */
    uint32_t pad0[2];    /* 0 */
    uint32_t width;      /* e.g. 1920 */
    uint32_t height;     /* e.g. 1080 */
    uint8_t  pad1[176];  /* zeros */
};

/* VIDIOC_S_FMT equivalent — sets channel output resolution + pixel format.
 * ioctl(video_fd, 0xc0cc5605, &vi_fmt_wrapper)
 * Reversed from vi_dev_set_chn_res @ 0x00142b2c. */
#define VI_IOCTL_S_FMT     0xc0cc5605  /* _IOWR('V',5,204) — VIDIOC_S_FMT      */
#define VI_PIXFMT_NV12     0x3231564eu /* 'NV12' fourcc = little-endian "NV12"  */

struct vi_fmt_wrapper {
    uint32_t flag;         /* 1 */
    uint32_t width;
    uint32_t height;
    uint32_t pixelformat;  /* VI_PIXFMT_NV12 */
    uint32_t field;        /* 4 — value observed in anyka_ipc */
    uint8_t  padding[184];
};

/* Crop capability query + set.
 * Reversed from vi_dev_set_chn_crop @ 0x001429a4.
 * Step 1: ioctl(video_fd, VI_IOCTL_G_CROP_CAP, vi_crop_cap[11])
 *   buf[0] = 1, rest zeroed; output: crop capability rectangle
 * Step 2: ioctl(video_fd, VI_IOCTL_S_CROP, &vi_crop_req)              */
#define VI_IOCTL_G_CROP_CAP 0x802c563a  /* _IOR('V',58,44)    — VIDIOC_CROPCAP */
#define VI_IOCTL_S_CROP     0x4014563c  /* _IOW('V',0x3c,20)  — SET_CROP       */

struct vi_crop_req {       /* 20 bytes — VI_IOCTL_S_CROP */
    uint32_t flag;         /* 1 */
    uint32_t left;
    uint32_t top;
    uint32_t width;
    uint32_t height;
};

/* -------------------------------------------------------------------------
 * H264 internal profile_idc values stored in auStack_f4[0] after
 * venc_check_and_format_param, and used in the VENC_IOCTL_QUERY_DMA request
 * struct (offset 0).  These are standard H.264 profile_idc values.
 *
 * User-facing profile selector (param_1[6]) → internal profile_idc:
 *   0 → 0x4D (77)  = Main
 *   1 → 0x64 (100) = High
 *   2 → 0x42 (66)  = Baseline
 *   3 → 0x242      = Constrained High
 *
 * WARNING: do NOT pass these to venc_create_enc_req.enc.codec_type_code!
 * ak_venc_open_ex passes codec_type_code=0 for CREATE_ENC; the encoder uses
 * the H264 config cached by VENC_IOCTL_QUERY_DMA instead. Passing a non-zero
 * codec_type_code (e.g. 0x42) can accidentally trigger H265 code paths inside
 * AL_SchedulerCpu_CreateChannel, causing the "CU size only for H265" warning
 * and over-allocating DMA memory that exhausts the ring pool.
 * ------------------------------------------------------------------------- */
#define H264_PROFILE_IDC_MAIN        77   /* standard H.264 Main profile_idc    */
#define H264_PROFILE_IDC_HIGH       100   /* standard H.264 High profile_idc    */
#define H264_PROFILE_IDC_BASELINE    66   /* standard H.264 Baseline profile_idc */
#define H264_PROFILE_IDC_CONSTHI   0x242  /* Constrained High                   */
#define JPEG_CODEC_CODE         0x4000000 /* JPEG mode (enc_out_type=1)         */

/* Kernel RC mode (venc_enc_cfg.rc_mode) — different from user-facing br_mode */
#define VENC_RC_AVBR  0  /* br_mode=2 AVBR */
#define VENC_RC_DEF   1  /* default/CBR variant */
#define VENC_RC_CBR   2  /* br_mode=1 CBR  */
#define VENC_RC_VBRP  5  /* br_mode=4 VBR+ */

/* -------------------------------------------------------------------------
 * ISP tunnel wrapper
 * ------------------------------------------------------------------------- */
struct isp_cmd_wrapper {
    int   flag;         /* always 1 */
    int   inner_cmd;    /* actual ISP command code (ISP_INNER_*) */
    void *payload;
    char  padding[192];
} __attribute__((packed));

/* -------------------------------------------------------------------------
 * DMA pool (CREATE/DELETE_POOL)
 *
 * After VENC_IOCTL_CREATE_POOL with is_external=1, the kernel writes back:
 *   phys_base  (+0x10) — physical base address of the allocated DMA region
 *   phys_base2 (+0x14) — second physical address (purpose TBD)
 *
 * virt_base is obtained by mmap(NULL, pool_size, PROT_RW, MAP_SHARED, chn_fd, 0)
 * immediately after a successful external-pool create.
 *
 * Frame pointer arithmetic:
 *   data_virt   = virt_base  + (frame_offset - phys_base)
 *   frame_offset = phys_base + (data_virt   - virt_base)   [for release]
 * ------------------------------------------------------------------------- */
struct venc_dma_pool_desc {
    int32_t      create;      /* +0x00  always 1 */
    int32_t      is_external; /* +0x04  0=ring buffer, 1=external/mapped */
    uint8_t      _pad0[8];    /* +0x08..+0x0f  (zeroed on input, unknown output) */
    uint32_t     phys_base;   /* +0x10  OUTPUT after is_external=1 create: DMA phys addr */
    uint32_t     phys_base2;  /* +0x14  OUTPUT: secondary phys addr (purpose TBD) */
    uint8_t      _pad1[4];    /* +0x18..+0x1b */
    uint32_t     pool_size;   /* +0x1c  input: bytes to allocate */
};

/* -------------------------------------------------------------------------
 * CREATE_ENC ioctl (VENC_IOCTL_CREATE_ENC) — 96 bytes total
 * Reversed from ak_venc_open_ex @ 0x001d4a40 and
 * venc_check_and_format_param  @ 0x001d025c
 * venc_check_and_format_rc_param @ 0x001d0a58
 * ------------------------------------------------------------------------- */

/* Encoder config sub-struct, 48 bytes (struct offset +0x04..+0x33) */
struct venc_enc_cfg {
    uint32_t codec_type_code; /* +0x00 H264_PROFILE_* or H265_CODEC_CODE */
    uint32_t chroma_mode;     /* +0x04 0=yuv420, 1=yuv422, 2=? */
    uint16_t width;           /* +0x08 */
    uint16_t height;          /* +0x0a */
    uint8_t  _gap0[8];        /* +0x0c..+0x13 */
    uint32_t rc_mode;         /* +0x14 VENC_RC_* */
    uint16_t fps;             /* +0x18 */
    uint16_t goplen;          /* +0x1a */
    uint32_t max_fps;         /* +0x1c (computed from resolution, see venc_max_fps()) */
    uint32_t enc_level;       /* +0x20 H264 level*10, e.g. 40 = Level 4.0 */
    uint16_t qp_or_kbps;     /* +0x24 meaning depends on rc_mode:
                               *   AVBR → initqp
                               *   CBR  → max_kbps
                               *   VBR+ → minqp */
    uint16_t qp2;            /* +0x26 AVBR: unused; CBR: initqp; VBR+: maxqp */
    uint16_t max_kbps_vbrp;  /* +0x28 VBR+ only: max_kbps */
    uint16_t initqp_vbrp;    /* +0x2a VBR+ only: initqp */
    uint8_t  _gap1[4];       /* +0x2c..+0x2f */
};

/* Smart-encoding params, 8 bytes (struct offset +0x34..+0x3b) */
struct venc_smart_cfg {
    int16_t  smart_mode;         /* +0x00 0=off */
    uint16_t smart_goplen;
    uint16_t smart_quality;
    uint16_t smart_static_value;
};

/* RC (rate control) params, 32 bytes (struct offset +0x40..+0x5f) */
struct venc_rc_cfg {
    uint32_t cub_size;      /* +0x00 0-2 */
    int16_t  minqp;         /* +0x04 10-51 */
    uint16_t maxqp;         /* +0x06 10-62 */
    int32_t  delta;         /* +0x08 -12..+12 */
    uint32_t I_pic_size;    /* +0x0c */
    uint32_t P_pic_size;    /* +0x10 */
    uint32_t B_pic_size;    /* +0x14 */
    uint32_t flag;          /* +0x18 */
    uint8_t  flag2;         /* +0x1c */
    uint8_t  srd_threshold; /* +0x1d 1-254 */
    uint8_t  _pad[2];       /* +0x1e..+0x1f */
};

/* Full CREATE_ENC request, 96 bytes */
struct venc_create_enc_req {
    int32_t         chn_id;    /* +0x00 */
    struct venc_enc_cfg enc;   /* +0x04 (48 bytes) */
    struct venc_smart_cfg smart; /* +0x34 (8 bytes) */
    int32_t         rc_flag;   /* +0x3c 0=no RC, 1=use RC */
    struct venc_rc_cfg  rc;    /* +0x40 (32 bytes) */
};  /* total: 4+48+8+4+32 = 96 = 0x60 bytes */

/* Helper: compute max_fps for venc_enc_cfg from resolution (H264) */
static inline uint32_t venc_max_fps(int width, int height)
{
    int px = width * height;
    if (px < 0x38401)  return 60;
    if (px < 0x2d9001) return 40;
    if (px < 0x4ce301) return 20;
    if (px < 0x7e9001) return 10;
    if (px < 0xf00001) return  5;
    if (px < 0x2000001) return 1;
    return 1;
}

/* -------------------------------------------------------------------------
 * BIND_VI_CHN ioctl — 20 bytes
 * Reversed from ak_venc_bind_vi_chn @ 0x001d65c0,
 * called from app_video_bind_vi_venc @ 0x0011696c.
 *
 * encode_mode: 1 = kernel mode (ISP feeds encoder directly, no userspace DQ loop)
 *              0 = user mode   (userspace must DQBUF VI frames and push them)
 * vi_chn_packed: low byte = vi_chn (0=main, 1=sub), high byte = device index (0)
 * ------------------------------------------------------------------------- */
struct venc_bind_vi_req {
    uint32_t venc_chn_id;   /* +0x00  venc channel id (0, 1, ...) */
    uint32_t vi_chn_packed; /* +0x04  (dev_idx << 8) | vi_chn */
    uint32_t param0;        /* +0x08  reserved / extra bind param */
    uint32_t param1;        /* +0x0c  reserved */
    uint32_t encode_mode;   /* +0x10  1=kernel_mode, 0=user_mode */
};

/* -------------------------------------------------------------------------
 * V4L2 buffer structs for VI REQBUFS / QUERYBUF / QBUF
 * These match the standard Linux v4l2_requestbuffers / v4l2_buffer layout.
 * The ak_vi driver uses standard V4L2 buffer types.
 * ------------------------------------------------------------------------- */
#define V4L2_BUF_TYPE_VIDEO_CAPTURE 1
#define V4L2_MEMORY_MMAP            1

struct vi_reqbufs {          /* 20 bytes — VIDIOC_REQBUFS */
    uint32_t count;          /* number of buffers to allocate   */
    uint32_t type;           /* V4L2_BUF_TYPE_VIDEO_CAPTURE = 1 */
    uint32_t memory;         /* V4L2_MEMORY_MMAP = 1            */
    uint32_t capabilities;   /* output: driver buffer caps      */
    uint8_t  flags;
    uint8_t  reserved[3];
};

struct vi_buffer {           /* 68 bytes — VIDIOC_QUERYBUF / VIDIOC_QBUF */
    uint32_t index;          /* +0x00  buffer index (0..count-1) */
    uint32_t type;           /* +0x04  V4L2_BUF_TYPE_VIDEO_CAPTURE */
    uint32_t bytesused;      /* +0x08 */
    uint32_t flags;          /* +0x0c */
    uint32_t field;          /* +0x10 */
    uint32_t timestamp[2];   /* +0x14  struct timeval (8 bytes) */
    uint8_t  timecode[16];   /* +0x1c  struct v4l2_timecode */
    uint32_t sequence;       /* +0x2c */
    uint32_t memory;         /* +0x30  V4L2_MEMORY_MMAP */
    uint32_t m_offset;       /* +0x34  QUERYBUF: mmap offset; QBUF: pass virt addr */
    uint32_t length;         /* +0x38  buffer size in bytes */
    uint32_t reserved2;      /* +0x3c */
    uint32_t reserved;       /* +0x40 */
};

/* -------------------------------------------------------------------------
 * GET_STREAM / RELEASE_STREAM ioctl — 32 bytes
 * ------------------------------------------------------------------------- */
struct venc_stream_req {
    int32_t chn_id;        /* in:  channel id */
    int32_t len;           /* in:  max buf len; out: actual frame len */
    int32_t ts_lo;         /* out: timestamp low  32 bits */
    int32_t ts_hi;         /* out: timestamp high 32 bits */
    int32_t seq_no;        /* out: sequence number */
    int32_t frame_type_raw;/* out: 1=P 2=I 3=B (0 on release) */
    int32_t frame_offset;  /* in/out: physical DMA offset in ring */
    int32_t _pad;
};

#define FRAME_TYPE_P 1
#define FRAME_TYPE_I 2
#define FRAME_TYPE_B 3

/* -------------------------------------------------------------------------
 * ISP sensor register programming
 *
 * Sensor conf file format (/tmp/sensor_isp.conf, same as isp_gc20c3.conf):
 *   Byte 0..511  : 512-byte text header block (mode=0 → 1 subfile)
 *                  [+0x00] int  version = 7
 *                  [+0x04] char[16] version_str "6.114-20250904"
 *                  [+0x14] int  sensor_id  (GC20C3 = 0x20c3)
 *                  [+0x18] u16  year, [+0x1a] month, day, hour, min, sec
 *                  [+0x1f] u8   style_id (0)
 *   Byte 512+    : 24 ISP pipeline module blocks
 *                  each block: [u16 module_id][u16 total_block_size][payload...]
 *                  module_ids 0..0x17, sizes from Isp_Struct_len[]
 *   After 24 mods: sensor register section
 *                  [u16 0x1c][u16 data_len][data_len bytes: 4-byte reg entries]
 *
 * Sensor register ioctl goes to /dev/v4l-subdev0.
 * The kernel driver (sensor_gc20c3.ko) writes I2C registers to the GC20C3.
 *
 * Reversed from:
 *   isp_match_sensor_cfgfile @ 0x0012c4f4
 *   isp_cfg_file_load        @ 0x0012bac4
 *   isp_module_init          @ 0x00129920 (sVar3==0x18 branch)
 *   vi_dev_load_sensor_conf  @ 0x00145400
 *   isp_sensor_ioctl         @ 0x001451ec
 * ------------------------------------------------------------------------- */
struct isp_sensor_conf {
    uint32_t reg_count; /* = data_len >> 2 (number of 4-byte register entries) */
    uint32_t reg_table; /* userspace pointer to register data */
};

#define ISP_CONF_HEADER_SIZE   512  /* bytes to skip: one 512-byte text header block */
#define ISP_MODULE_COUNT       24   /* ISP pipeline modules (ids 0x00..0x17) */
#define ISP_SENSOR_SECTION_ID  0x1c /* magic id of the sensor register section */

/* -------------------------------------------------------------------------
 * ISP image-pipeline inner command codes (tunneled via 0xc0cc5616 on isp_fd)
 *
 * All fully confirmed from Ghidra decompilation of AK_ISP_set_*_attr functions.
 * All are magic-byte 'I' (0x49). Modules with multiple calls send them in order.
 *
 * Usage:
 *   struct isp_cmd_wrapper w = {0};
 *   w.flag      = 1;
 *   w.inner_cmd = ISP_INNER_xxx;
 *   w.payload   = data_ptr;           // pointer into ISP data blob (skip 4-byte header)
 *   ioctl(g_isp_fd, ISP_IOCTL_CMD, &w);
 * ------------------------------------------------------------------------- */

/* module 0x00 — BLC (Black Level Correction) */
#define ISP_INNER_BLC           0x40044902  /* AK_ISP_set_blc_attr       @ 0x00145860 */

/* module 0x01 — LSC (Lens Shading Correction) */
#define ISP_INNER_LSC           0x40044904  /* AK_ISP_set_lsc_attr       @ 0x00145890 */

/* module 0x02 — Raw LUT */
#define ISP_INNER_RAW_LUT       0x4004490a  /* AK_ISP_set_raw_lut_attr   @ 0x001458c0 */

/* module 0x03 — Noise Reduction (3 sub-calls in order) */
#define ISP_INNER_NR1           0x4004490c  /* AK_ISP_set_nr1_attr       @ 0x00145920 */
#define ISP_INNER_NR2           0x4004492b  /* AK_ISP_set_nr2_attr       @ 0x00145bc0 */
#define ISP_INNER_UVNR          0x4004497d  /* AK_ISP_set_uvnr_attr      @ 0x00145bf0 */

/* module 0x04 — 3D Noise Reduction (7180 bytes) */
#define ISP_INNER_3D_NR         0x4004492f  /* AK_ISP_set_3d_nr_attr     @ 0x00145ad0 */

/* module 0x05 — Green Balance */
#define ISP_INNER_GB            0x40044906  /* AK_ISP_set_gb_attr        @ 0x00145950 */

/* module 0x06 — Demosaic */
#define ISP_INNER_DEMO          0x4004490e  /* AK_ISP_set_demo_attr      @ 0x00145980 */

/* module 0x07 — RGB Gamma */
#define ISP_INNER_RGB_GAMMA     0x4004491a  /* AK_ISP_set_rgb_gamma_attr @ 0x001459e0 */

/* module 0x08 — CCM (Color Correction Matrix) */
#define ISP_INNER_CCM           0x40044912  /* AK_ISP_set_ccm_attr       @ 0x001459b0 */

/* module 0x09 — FCS (False Color Suppression) */
#define ISP_INNER_FCS           0x40044936  /* AK_ISP_set_fcs_attr       @ 0x00145b90 */

/* module 0x0a — WDR (Wide Dynamic Range) */
#define ISP_INNER_WDR           0x4004491b  /* AK_ISP_set_wdr_attr       @ 0x00146250 */

/* module 0x0b — Sharpness (2 sub-calls in order) */
#define ISP_INNER_SHARP         0x40044925  /* AK_ISP_set_sharp_attr     @ 0x00145b30 */
#define ISP_INNER_GET_SHARP     0x80044926  /* AK_ISP_get_sharp_attr     @ 0x00145b48 */
#define ISP_INNER_SHARP_EX      0x40044927  /* AK_ISP_set_sharp_ex_attr  @ 0x00145b60 */

/* module 0x0c — Saturation */
#define ISP_INNER_SATURATION    0x4004493b  /* AK_ISP_set_saturation_attr @ 0x00145aa0 */

/* module 0x0d — Contrast */
#define ISP_INNER_CONTRAST      0x40044939  /* AK_ISP_set_contrast_attr  @ 0x00145a10 */

/* module 0x0e — RGB→YUV */
#define ISP_INNER_RGB2YUV       0x4004493f  /* AK_ISP_set_rgb2yuv_attr   @ 0x00146230 */

/* module 0x0f — YUV Effect */
#define ISP_INNER_EFFECT        0x40044941  /* AK_ISP_set_effect_attr    @ 0x00145c50 */
#define ISP_INNER_GET_EFFECT    0x80044942  /* AK_ISP_get_effect_attr    @ 0x00145c68 */

/* module 0x10 — DPC (Defective Pixel Correction) */
#define ISP_INNER_DPC           0x40044910  /* AK_ISP_set_dpc_attr       @ 0x001458f0 */

/* module 0x11 — LCE (Local Contrast Enhancement) */
#define ISP_INNER_LCE           0x8004498e  /* AK_ISP_set_lce_attr       @ 0x00145a40 */

/* module 0x12 — AF (Auto Focus) */
#define ISP_INNER_AF            0x40044960  /* AK_ISP_set_af_attr        @ 0x00145818 */

/* module 0x13 — AWB/WB (3 sub-calls in order) */
#define ISP_INNER_WB            0x80044986  /* AK_ISP_set_wb_attr        @ 0x00145758 */
#define ISP_INNER_AWB_EX        0x40044973  /* AK_ISP_set_awb_ex_attr    @ 0x00145788 */
#define ISP_INNER_AWB_CALIB     0x80044988  /* AK_ISP_set_awb_calib_info @ 0x001457b8 */

/* module 0x14 — AE/Exposure (2 sub-calls in order) */
#define ISP_INNER_EXPOSURE      0x8004498a  /* AK_ISP_set_exposure_attr  @ 0x001456c8 */
#define ISP_INNER_FRAME_RATE    0x4004494e  /* AK_ISP_set_frame_rate     @ 0x00145728 */

/* module 0x15 — Misc */
#define ISP_INNER_MISC          0x4004496e  /* AK_ISP_set_misc_attr      @ 0x00145c80 */

/* module 0x16 — Y Gamma */
#define ISP_INNER_Y_GAMMA       0x40044975  /* AK_ISP_set_Y_gamma_attr   @ 0x00145a70 */

/* module 0x17 — Hue */
#define ISP_INNER_HUE           0x40044977  /* AK_ISP_set_hue_attr       @ 0x00145c20 */

static inline int venc_max_frame_size(int width, int height)
{
    int px = width * height;
    if (px < 921600)  return 262144;  /* 256 KB */
    if (px < 3000000) return 524288;  /* 512 KB */
    return 1048576;                    /* 1 MB   */
}

/* -------------------------------------------------------------------------
 * AKPCM audio ioctls (/dev/pcmC0D0c capture, /dev/pcmC?D?p playback,
 * /dev/pcmC?D?l loopback) — magic byte 'P' (0x50).
 *
 * A later capture session (2026-07-04) SUPERSEDES the
 * original capture below: isp_hook.c was extended with an
 * open()/read()/write() hook that tags each fd by its actual device path
 * (previously fds were guessed by cross-referencing thread names, which
 * turns out to have mislabeled capture vs playback's period_size).
 * This second, path-tagged capture is authoritative; treat any values
 * here that conflict with the original §11 numbers (still noted below where
 * relevant) as corrected by §23, not as a second data point of equal
 * weight.
 *
 * Confirmed via /c/.../references/ak3918ev200 (a different, OLDER Anyka SoC
 * generation's open-source RE project, src/lib/audio/akpcm_device.c): the
 * same nr family (0x10/0x30/0xe0/0xe1/0xe2/0xe3) exists there too, under the
 * names SET_SOURCE/SET_MODE/GET_PARS/AUDIO_PORT_SYNC/POWER_CTRL/RESET_BUF —
 * confirms this is a shared "ak_pcm" driver lineage across Anyka SoC
 * generations, and gave us the real name for 0xe3 (previously totally
 * unknown to us). Do NOT copy that project's exact mode/state/source
 * numeric *values* (5/4/7/2/4 etc.) — those don't match anything we've
 * observed on AV130 and are presumably chip-generation-specific; only the nr
 * numbers and rough semantic shape transfer. Also: their code re-issues the
 * 0xe1 "port sync" ioctl before every single read()/write() — our own
 * AV130 capture does NOT do this (0xe1 is called exactly once, before 1442
 * consecutive successful reads, confirmed by grep over the full capture) —
 * so that specific behavior is generation-specific too and must NOT be
 * replicated here.
 *
 * fds are identified by isp_hook.c tagging the fd by its open() path
 * directly (authoritative, not a guess):
 *   /dev/pcmC0D0c (CAPTURE — this is the one we use) — period_size=512 BYTES
 *   /dev/pcmC0D0p (playback, out of scope)           — period_size=1024 BYTES
 *   /dev/pcmC0D0l (AEC reference tap, out of scope)   — period_size=1024 BYTES
 * (§11's original capture had these two period_size numbers swapped between
 * capture and playback — a mislabeling artifact of the old thread-name
 * cross-referencing method, corrected here.) The high-frequency per-frame
 * nr=0xe6 timestamp ioctl fires on playback+loopback ONLY, never on
 * capture — not defined here since audio.c never needs it.
 * ------------------------------------------------------------------------- */

/* GET_PARS — MISNAMED, despite the _IOR-shaped encoding this is actually a
 * commit/set operation (confirmed 2026-07-04 via the Ghidra ak_pcm.ko
 * relocation fix — capture_ioctl's real
 * dispatch, decompiled directly off the fixed module, does a copy_FROM_user
 * of this 28-byte struct into the kernel, not a copy_to_user). Layout
 * (7×u32, byte offsets from struct start):
 *   [0]=format(0=linear PCM) [1]=rate(Hz) [2]=channels [3]=bits
 *   [4]=period_size(BYTES, not samples) [5]=period_count [6]=reserved/unused
 * Validated on the way in (decompiled, not inferred):
 *   rate != 0; period_size != 0 && period_size % 64 == 0; period_count in
 *   [1, 0x50]. On success, computes the hardware rate divisor and sets the
 *   ready bit at +0x304 (bit 0x2) that capture_read() requires (§15) — this
 *   is the actual, disassembly-confirmed fix for the long-standing capture
 *   read() EPERM. Every previous session called this with a zeroed struct,
 *   which fails validation (rate=0) every time — exactly the EINVAL always
 *   observed — and never reaches the code that sets the ready bit,
 *   regardless of what SET_MODE/SET_RATE/SET_STATE/SET_PERIODS did. Must be
 *   called with real values (e.g. rate=8000, period_size=512,
 *   period_count=16 — the real captured defaults, §23) to succeed. */
#define AKPCM_IOC_GET_PARS      0x401c50e0  /* _IOR('P',0xe0,28) */

struct akpcm_pars {
    uint32_t format;
    uint32_t rate;
    uint32_t channels;
    uint32_t bits;
    uint32_t period_size;
    uint32_t period_count;
    uint32_t reserved;
};

/* SET_RATE — write the target sample rate in Hz (observed value 8000). */
#define AKPCM_IOC_SET_RATE      0x800450ea  /* _IOW('P',0xea,4) */

/* Best-guess name "STATE" — nr=0x10, size=4, W/R pair. Semantics still not
 * confirmed — values are NOT a simple "0=reset/4=start" lifecycle as
 * originally guessed in §11 (that guess drove the STATE(0)->STATE(4)
 * sequence this file used to document, which is now known wrong — see
 * §23). Real, path-tag-confirmed observed sequences:
 *   capture: SET(2) -> ... -> SET(2) again -> [reads start succeeding]
 *   playback: SET(0) -> ... -> SET(4) (later, well after config)
 * Replay the exact values per stream, don't assume meaning beyond that. */
#define AKPCM_IOC_SET_STATE     0x80045010  /* _IOW('P',0x10,4) */
#define AKPCM_IOC_GET_STATE     0x40045010  /* _IOR('P',0x10,4) */

/* Best-guess name "PERIODS" — nr=0x30, size=4, W/R pair. Real, path-tag-
 * confirmed observed value for capture's *initial* config is 2 (not 4 as
 * §11 originally documented — see §23); playback's initial value is 4.
 * Both fds' periods value changes again later during steady-state read/
 * write (not proven required for the initial unlock — not replicated here). */
#define AKPCM_IOC_SET_PERIODS   0x80045030  /* _IOW('P',0x30,4) */
#define AKPCM_IOC_GET_PERIODS   0x40045030  /* _IOR('P',0x30,4) */

/* Best-guess name "CAPS" — nr=0x81, GET only, observed value 1. */
#define AKPCM_IOC_GET_CAPS      0x40045081  /* _IOR('P',0x81,4) */

/* dir=NONE (no size in the request itself) — the 3rd ioctl() arg is used
 * directly as an integer value, NOT a pointer (confirmed: isp_hook.c's
 * generic dir==NONE branch logs the raw arg value itself, and it printed a
 * plain small integer here, not a heap/stack address). Observed value: 0.
 * ak3918ev200's akpcm_device.c calls the same nr "AUDIO_PORT_SYNC"/"kick the
 * port" rather than "mode", called before every read()/write() there — but
 * our own AV130 capture calls it exactly ONCE, before 1442 consecutive
 * successful reads with no further calls to it, so whatever it does here it
 * is NOT a per-transfer requirement on this chip generation. */
#define AKPCM_IOC_SET_MODE      0x000050e1  /* _IO('P',0xe1) */

/* dir=NONE, arg passed as a literal integer (same calling convention as
 * SET_MODE above). Completely unidentified until Session 8 (§23) — found by
 * cross-referencing against ak3918ev200's akpcm_device.c, which names the
 * same nr "RESET_BUF" (called with no meaningful arg there). Our own AV130
 * capture calls it exactly once, on the capture fd only (never seen on
 * playback), positioned between GET_PARS and SET_MODE, with arg=0xffffffff
 * (-1) — not NULL/0 as ev200's usage might suggest, so replay the exact
 * captured arg rather than assuming NULL is equivalent. This was entirely
 * missing from our own capture sequence prior to §23 — a strong candidate
 * for (part of) the actual fix for capture's read() EPERM. */
#define AKPCM_IOC_RESET_BUF     0x000050e3  /* _IO('P',0xe3) */
