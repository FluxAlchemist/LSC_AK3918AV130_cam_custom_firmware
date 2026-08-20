#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stddef.h>

#include "ak_rtsp.h"
#include "ak_ioctls.h"
#include "venc.h"

/* DEBUG_IOCTL_DUMP (opt-in, off by default — see Makefile `debug-dump`
 * target): dumps our own VENC_IOCTL_CREATE_ENC payload to
 * /tmp/ak_rtsp_ioctl_dump.log in the exact same format isp_hook.c uses for
 * stock anyka_ipc's capture (ExtractedData/isp_hook_n.log), via the shared
 * venc_dump.h — so the two can be diffed directly instead of eyeballed, and
 * so a struct-packing/offset bug would show up as an obvious mismatch
 * before ever touching hardware. Not compiled into the normal build: this
 * links a real libc so it uses plain write(2), unlike isp_hook.c's raw
 * syscalls (that file must stay libc-free to LD_PRELOAD onto anyka_ipc
 * regardless of which libc it's linked against). */
#ifdef DEBUG_IOCTL_DUMP
#define AKDUMP_WRITE(fd, ptr, len) write((fd), (ptr), (len))
#include "venc_dump.h"
#endif

/* -------------------------------------------------------------------------
 * Reset stale venc channel state left by a previously killed anyka_ipc.
 *
 * When anyka_ipc is SIGKILLed while streaming, venc_chn_dev_close calls
 * venc_destroy_chn but that function returns early (error 0x300000e) because
 * chn_attr[2] (stream-active flag) is still 1. This leaves:
 *   chn_dev+0x28 = valid (stale) chn_attr pointer (vfree never called)
 *   chn_dev+0x30 = 1  (encoder-created flag, NOT reset)
 *
 * Consequence: our subsequent CREATE_ENC sees chn_dev+0x30=1 and silently
 * returns 0 without creating the encoder. Later ioctls dereference the NULL
 * chn_attr (set to NULL by a previous successful close) and crash.
 *
 * Fix: open the device, call DEACTIVATE to zero out chn_attr[2], then close.
 * The close re-triggers venc_destroy_chn; now chn_attr[2]==0 → takes the
 * success path → vfree(chn_attr) + chn_dev+0x30=0 + chn_dev+0x34=1.
 * A subsequent open gets a fully clean channel.
 *
 * Safe to call unconditionally: if the channel is already clean (no stale
 * anyka_ipc state), DEACTIVATE returns an error (ignored) and close is a
 * no-op with respect to chn_dev+0x30.
 * ------------------------------------------------------------------------- */
void venc_reset_stale_channel(void)
{
    int fd = open(DEV_VENC_CHN0, O_RDWR | O_NONBLOCK);
    if (fd < 0) return;

    uint32_t chn_id = 0;
    int ret = ioctl(fd, VENC_IOCTL_DEACTIVATE, &chn_id);
    printf("[ak_rtsp] venc_reset_stale: DEACTIVATE ret=%d errno=%d\n", ret, errno);

    close(fd);
    printf("[ak_rtsp] venc channel reset done\n");
}

/* -------------------------------------------------------------------------
 * Query the kernel for exact DMA block sizes the AL encoder needs.
 *
 * ioctl(g_venc_fd, VENC_IOCTL_QUERY_DMA, buf) on the GLOBAL /dev/venc fd.
 * This mirrors ak_venc_open_ex → venc_get_req_dma_block @ 0x001d0c88.
 *
 * Input struct (84 bytes = uint32_t[21]):
 *   offset  0 (uint32): profile_idc  (77=Main)
 *   offset  4 (uint32): chroma_mode  (1 = YUV420)
 *   offset  8 (uint16): width
 *   offset 10 (uint16): height
 *   offset 12 (uint16): max_fps = 60
 *   offset 14 (uint16): smart_mode  (0 = off)
 *   offset 28 (uint8):  1  (hardcoded in venc_get_req_dma_block, always)
 *   offset 29 (uint8):  bEnableMMA = 1
 *   offset 32 (uint8):  MVVRange_factor = 40
 *
 * Output (kernel overwrites the same struct):
 *   buf[0]    = N  (number of DMA blocks required)
 *   buf[1..N] = block sizes in bytes
 *
 * SIDE EFFECT: this call also caches the H264 encoder config (profile, width,
 * height) in kernel channel state. CREATE_ENC reads from that cached state
 * and ignores the codec_type_code field in venc_create_enc_req — so calling
 * this BEFORE CREATE_POOL/CREATE_ENC is required.
 *
 * Ring pool size = Σ ((block_size + 31) & ~31) over all N blocks.
 * For H264 Main 1920×1080 with MMA: 8 blocks, total = 4120384 (ENC_RING_SIZE).
 * ------------------------------------------------------------------------- */
static uint32_t venc_query_ring_pool_size(void)
{
    uint32_t buf[21];
    memset(buf, 0, sizeof(buf));

    buf[0] = H264_PROFILE_IDC_MAIN;          /* profile_idc = Main */
    buf[1] = 1;                               /* chroma_mode = YUV420 */
    ((uint16_t *)buf)[4] = ENC_WIDTH;         /* width  */
    ((uint16_t *)buf)[5] = ENC_HEIGHT;        /* height */
    ((uint16_t *)buf)[6] = 60;                /* max_fps = 60 */
    ((uint16_t *)buf)[7] = 0;                 /* smart_mode = 0 */
    ((uint8_t  *)buf)[28] = 1;               /* hardcoded 1 */
    ((uint8_t  *)buf)[29] = 1;               /* bEnableMMA = 1 */
    ((uint8_t  *)buf)[32] = 40;              /* MVVRange_factor = 40 */

    if (ioctl(g_venc_fd, VENC_IOCTL_QUERY_DMA, buf) < 0) {
        perror("VENC_IOCTL_QUERY_DMA");
        printf("[ak_rtsp] QUERY_DMA failed; falling back to ENC_RING_SIZE=%u\n", ENC_RING_SIZE);
        return ENC_RING_SIZE;
    }

    uint32_t count = buf[0];
    printf("[ak_rtsp] QUERY_DMA: %u DMA blocks required\n", count);
    if (count == 0 || count > 20) {
        printf("[ak_rtsp] QUERY_DMA: unexpected count %u; falling back to ENC_RING_SIZE\n", count);
        return ENC_RING_SIZE;
    }

    uint32_t pool_size = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t sz      = buf[1 + i];
        uint32_t aligned = (sz + 31u) & ~31u;
        printf("[ak_rtsp]   block[%u] = %u bytes (aligned %u)\n", i, sz, aligned);
        pool_size += aligned;
    }
    printf("[ak_rtsp] QUERY_DMA ring pool size = %u bytes\n", pool_size);
    return pool_size;
}

/* -------------------------------------------------------------------------
 * Create venc DMA pool + mmap the bitstream buffer.
 *
 * Order matches ak_venc_open_ex exactly:
 *   1. QUERY_DMA → kernel caches H264 config + returns ring pool size
 *   2. CREATE_POOL ring (size from query)
 *   3. CREATE_POOL ext (size = ENC_VENC_SIZE bitstream ring)
 *   4. mmap ext pool into userspace
 * ------------------------------------------------------------------------- */
int venc_create_pool(void)
{
    printf("[ak_rtsp] sizeof(venc_dma_pool_desc)=%d\n", (int)sizeof(struct venc_dma_pool_desc));

    uint32_t ring_size = venc_query_ring_pool_size();

    struct venc_dma_pool_desc pool;

    memset(&pool, 0, sizeof(pool));
    pool.create      = 1;
    pool.is_external = 0;
    pool.pool_size   = ring_size;
    if (ioctl(g_chn0_fd, VENC_IOCTL_CREATE_POOL, &pool) < 0) {
        perror("VENC_IOCTL_CREATE_POOL ring");
        return -1;
    }

    memset(&pool, 0, sizeof(pool));
    pool.create      = 1;
    pool.is_external = 1;
    pool.pool_size   = ENC_VENC_SIZE;
    if (ioctl(g_chn0_fd, VENC_IOCTL_CREATE_POOL, &pool) < 0) {
        perror("VENC_IOCTL_CREATE_POOL ext");
        return -1;
    }

    /* phys_base  (+0x10) = kernel virtual address of the DMA region (0xc...).
     * phys_base2 (+0x14) = true physical address of the DMA region  (0x81...).
     *
     * GET_STREAM returns frame_offset as a KERNEL VIRTUAL address inside the
     * ring buffer, so frame pointer arithmetic must use phys_base (kv) as the
     * base, not phys_base2 (physical):
     *
     *   data_virt = g_venc_virt + (frame_offset - g_venc_phys)
     *             = mmap_base   + (kv_offset    - kv_base)
     *
     * Confirmed from run log: frame_offset=0xc62120a0, kv_base=0xc61f6000
     *   → ring offset = 0x1c0a0 = 115,872 bytes  (within 777,600-byte ring ✓)
     * Using phys_base2 (0x81ad0000) gives a wild pointer → segfault. */
    g_venc_phys = pool.phys_base;   /* kernel virtual base — used for frame_offset arithmetic */

    g_venc_virt = mmap(NULL, ENC_VENC_SIZE, PROT_READ | PROT_WRITE,
                       MAP_SHARED, g_chn0_fd, 0);
    if (g_venc_virt == MAP_FAILED) {
        perror("mmap venc");
        return -1;
    }
    printf("[ak_rtsp] venc DMA virt=%p kv_base=0x%08x phys=0x%08x\n",
           g_venc_virt, g_venc_phys, pool.phys_base2);
    return 0;
}

/* -------------------------------------------------------------------------
 * Create H.264 encoder.
 *
 * IMPORTANT: codec_type_code MUST be H264_PROFILE_IDC_MAIN (77), not 0x42.
 * Passing 0x42 accidentally triggers the H265 code path inside
 * AL_SchedulerCpu_CreateChannel → over-allocates DMA → ring pool crash.
 * The encoder takes profile/width/height from the kernel channel state cached
 * by VENC_IOCTL_QUERY_DMA, not from this struct.
 * ------------------------------------------------------------------------- */
int venc_create_encoder(void)
{
    struct venc_create_enc_req req;
    memset(&req, 0, sizeof(req));

    req.chn_id               = 0;
    req.enc.codec_type_code  = H264_PROFILE_IDC_MAIN;
    req.enc.chroma_mode      = 1;    /* CHROMA_4_2_0 */
    req.enc.width            = ENC_WIDTH;
    req.enc.height           = ENC_HEIGHT;
    /* CORRECTED (2026-07-04, isp_hook.c CREATE_ENC byte-exact capture —
     * ExtractedData/isp_hook_n.log, "CREATE_ENC #1 chn_id=0"): stock uses
     * VENC_RC_VBRP (5), NOT VENC_RC_AVBR (0). This was the actual root cause
     * of the earlier "qp_or_kbps=99 is initqp" reasoning below being wrong —
     * that was inferred indirectly from anyka_ipc's own debug log text
     * ("initqp:99, minqp:28, maxqp:43"), which doesn't say which wire field
     * each value lands in for which rc_mode. Under the WRONG assumed mode
     * (AVBR), qp_or_kbps carries initqp; under the REAL mode (VBR+) it
     * carries minqp instead, and qp2/max_kbps_vbrp/initqp_vbrp are real,
     * separate fields — previously left unset (0). The raw hex dump
     * decodes stock's byte-exact values directly: minqp=28, maxqp=43,
     * max_kbps=1536, initqp=99 — the exact _ht_hw_settings.ini values, just
     * in the fields VBR+ actually uses. This does not touch the `rc`
     * (0x40-0x5f) sub-struct at all — still the hardware-confirmed-safe
     * values below, per the crash history below. */
    req.enc.rc_mode          = VENC_RC_VBRP;
    req.enc.fps              = ENC_FPS;
    req.enc.goplen           = ENC_GOP;
    req.enc.max_fps          = 0;    /* stock sends 0 for both channels (isp_hook_n.log) */
    req.enc.enc_level        = 40;   /* H.264 Level 4.0 */
    req.enc.qp_or_kbps       = 28;   /* VBR+: minqp (stock isp_hook_n.log capture) */
    req.enc.qp2              = 43;   /* VBR+: maxqp */
    req.enc.max_kbps_vbrp    = 1536; /* VBR+: max_kbps */
    req.enc.initqp_vbrp      = 99;   /* VBR+: initqp — the "99" belongs here, not qp_or_kbps */
    req.smart.smart_mode     = 0;
    req.smart.smart_goplen   = 100;
    req.smart.smart_quality  = 50;
    /* REVERTED (2026-07-04) — hardware-tested and CRASHES THE KERNEL.
     * A Ghidra trace of venc_check_and_format_rc_param (0x1d0a58) and its
     * caller ht_video_codec_start_encode (0xbef60) appeared to show stock
     * sending an all-zero rc struct (cub_size/minqp/maxqp/delta/I_P_B_pic_size
     * all 0, only rc_flag=1). Tried it on hardware with UART attached this
     * time — confirmed kernel Oops, not a graceful failure:
     *   dma_pool_alloc_obj+0x440 [ak_venc_adapter]
     *     <- venc_dma_pool_malloc
     *     <- AL_SchedulerCpu_CreateChannel   (called from CREATE_ENC)
     *     <- AL_RC_MulDivR                    ("rate control mul-div-round")
     * AL_* is Allegro DVT/Envive H.264 IP naming — AL_RC_MulDivR is doing a
     * scaled (a*b)/c division as part of rate-control channel setup, and it
     * divides by zero. This is direct proof the kernel DOES read/use these
     * rc fields (not a struct-offset misread as first suspected) — almost
     * certainly maxqp-minqp==0 (both zero) used as a QP-range divisor.
     * Reverted to the values confirmed safe on hardware (they stream, just
     * with the macroblocking artifact this was trying to fix). Do NOT set
     * minqp==maxqp (or both zero) here again. The full crash dump and
     * follow-up plan (byte-exact runtime capture via the LD_PRELOAD hook
     * instead of further guessing from Ghidra) is what led to the values
     * below. */
    /* TRIED AND FAILED AGAIN (2026-07-04): setting rc.flag=0/rc.flag2=0/
     * rc.srd_threshold=0 (MMA/MVVRange disabled, minqp/maxqp left at the
     * known-safe 28/43) CRASHES THE KERNEL — same NULL-deref bug as the
     * all-zero rc struct attempt above, confirmed via UART:
     *   dma_pool_alloc_obj+0x440 [ak_venc_adapter]
     *     <- venc_dma_pool_malloc <- AL_SchedulerCpu_CreateChannel
     * A Ghidra trace through AKV_Encoder_Open_Ext (0x2eb7c) beforehand
     * appeared to show flag/flag2/srd_threshold are independently gated
     * ("if nonzero, use; else skip") and don't feed any division — that
     * trace was real but incomplete; it examined the wrong function
     * relative to the actual crash site. This is now the THIRD time
     * (across multiple sessions) that a plausible-looking Ghidra reasoning
     * chain about this rc struct has been contradicted by hardware. A full
     * disassembly-level pass ("RC struct div/0 — full disassembly-level
     * root cause") found the panic is actually a NULL pointer dereference
     * in a DMA-pool free-list search (dma_pool_alloc_obj), not a literal
     * integer division — and the causal link from these specific ioctl
     * fields to that pool exhaustion could NOT be conclusively closed this
     * round. Do not retry
     * flag/flag2/srd_threshold changes without new evidence (e.g. the
     * isp_hook.c CREATE_ENC byte capture) — Ghidra reasoning alone has now
     * failed twice for this struct. Reverted to hardware-confirmed-safe. */
    req.rc_flag              = 1;
    req.rc.cub_size          = 0;
    req.rc.minqp             = 28;
    req.rc.maxqp             = 43;
    req.rc.srd_threshold     = 128;
    req.rc.flag              = 1;   /* bEnableMMA = TRUE */
    req.rc.flag2             = 40;  /* MVVRange_factor = 40 */

    /* Same log block format as anyka_ipc's own ht_video_codec.c:749-760
     * ("===========channel %d venc param===========" / width / height /
     * fps / goplen / target_kbps / max_kbps / minqp / maxqp / initqp /
     * enc_level / "===...==="), so our own debug log is directly
     * line-for-line comparable against anyka_ipc_hooked.log without having
     * to mentally map field names each time. target_kbps and max_kbps are
     * the same field in our VBR+ payload (req.enc.max_kbps_vbrp) — stock's
     * two separate ini values (target_kbps/max_kbps) are also both 1536,
     * so printing the one field twice matches stock's printed values
     * exactly even though our wire struct only carries one of them. */
    printf("===========channel %d venc param===========\n", req.chn_id);
    printf("width:%d\n",        req.enc.width);
    printf("height:%d\n",       req.enc.height);
    printf("fps:%d\n",          req.enc.fps);
    printf("goplen:%d\n",       req.enc.goplen);
    printf("target_kbps:%d\n",  req.enc.max_kbps_vbrp);
    printf("max_kbps:%d\n",     req.enc.max_kbps_vbrp);
    printf("minqp:%d\n",        req.enc.qp_or_kbps);
    printf("maxqp:%d\n",        req.enc.qp2);
    printf("initqp:%d\n",       req.enc.initqp_vbrp);
    printf("enc_level:%d\n",    req.enc.enc_level);
    printf("============================================\n");

    printf("[ak_rtsp] calling CREATE_ENC...\n");
    int ret = ioctl(g_chn0_fd, VENC_IOCTL_CREATE_ENC, &req);
#ifdef DEBUG_IOCTL_DUMP
    {
        int f = open("/tmp/ak_rtsp_ioctl_dump.log", O_WRONLY|O_CREAT|O_APPEND, 0644);
        if (f >= 0) {
            akdump_create_enc(f, "AK_RTSP", &req, (uint32_t)ret);
            close(f);
        }
    }
#endif
    if (ret < 0) {
        perror("VENC_IOCTL_CREATE_ENC");
        return -1;
    }
    printf("[ak_rtsp] CREATE_ENC OK\n");
    return 0;
}

/* -------------------------------------------------------------------------
 * Bind venc channel 0 to VI channel 0 (kernel encode mode).
 *
 * encode_mode=1: kernel handles ISP→VENC DMA internally, no userspace
 * involvement needed per frame.
 *
 * Reversed from ak_venc_bind_vi_chn @ 0x001d65c0.
 * IMPORTANT: must be called BEFORE CREATE_POOL/CREATE_ENC (see
 * smolrtsp_integration.md §C for the ordering constraint explanation).
 * ------------------------------------------------------------------------- */
int venc_bind_vi(void)
{
    struct venc_bind_vi_req bind;
    memset(&bind, 0, sizeof(bind));
    bind.venc_chn_id   = 0;
    bind.vi_chn_packed = 0;  /* (dev_idx=0 << 8) | vi_chn=0 */
    bind.param0        = ENC_WIDTH;
    bind.param1        = ENC_HEIGHT;
    bind.encode_mode   = 1;  /* kernel mode */

    printf("[ak_rtsp] calling BIND_VI_CHN...\n");
    if (ioctl(g_chn0_fd, VENC_IOCTL_BIND_VI_CHN, &bind) < 0) {
        perror("VENC_IOCTL_BIND_VI_CHN");
        return -1;
    }
    printf("[ak_rtsp] BIND_VI_CHN OK\n");
    return 0;
}

/* -------------------------------------------------------------------------
 * Activate the encoder — starts frame production.
 * Reversed from ak_venc_bind_activate @ 0x001d68a8.
 * ------------------------------------------------------------------------- */
int venc_activate(void)
{
    uint32_t chn_id = 0;
    printf("[ak_rtsp] calling ACTIVATE...\n");
    if (ioctl(g_chn0_fd, VENC_IOCTL_ACTIVATE, &chn_id) < 0) {
        perror("VENC_IOCTL_ACTIVATE");
        return -1;
    }
    printf("[ak_rtsp] ACTIVATE OK\n");
    return 0;
}
