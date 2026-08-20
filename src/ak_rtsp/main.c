#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>

#include "ak_rtsp.h"
#include "ak_ioctls.h"
#include "isp.h"
#include "ircut.h"
#include "ae.h"
#ifdef AEC_CUSTOM
#include "aec_custom.h"
#endif
#include "night.h"
#include "control.h"
#include "vi.h"
#include "venc.h"
#include "audio.h"
#include "rtsp.h"
#include "version.h"

/* -------------------------------------------------------------------------
 * Frame grab — triggered by SIGUSR1
 * Send with: kill -USR1 $(pidof ak_rtsp)
 * Saves 640×360 NV12 to /mnt/grab_NNNN.yuv; view with:
 *   ffplay -f rawvideo -video_size 640x360 -pixel_format nv12 /mnt/grab_0001.yuv
 * ------------------------------------------------------------------------- */
static volatile sig_atomic_t g_grab_request = 0;
static int                   g_grab_counter = 0;

static void sigusr1_handler(int sig) { (void)sig; g_grab_request = 1; }

/* -------------------------------------------------------------------------
 * Global state definitions (declared extern in ak_rtsp.h)
 * ------------------------------------------------------------------------- */
int g_isp_fd   = -1;
int g_subdev   = -1;
int g_vid_main = -1;
int g_vid_sub  = -1;
int g_vid_td   = -1;
int g_venc_fd  = -1;
int g_chn0_fd  = -1;

void     *g_venc_virt = MAP_FAILED;
uint32_t  g_venc_phys = 0;

void     *g_vi_bufs[VI_BUF_COUNT];
uint32_t  g_vi_buf_sizes[VI_BUF_COUNT];

void     *g_vi_sub_bufs[VI_SUB_BUF_COUNT];
uint32_t  g_vi_sub_buf_sizes[VI_SUB_BUF_COUNT];
uint32_t  g_vi_sub_mmap_offsets[VI_SUB_BUF_COUNT];

void     *g_vi_td_bufs[VI_TD_BUF_COUNT];
uint32_t  g_vi_td_buf_sizes[VI_TD_BUF_COUNT];

/* -------------------------------------------------------------------------
 * Open ISP + VI device nodes only (VENC must NOT be open during STREAMON).
 *
 * The ak_isp kernel driver triggers b2i_pp_chn_init (ISP bridge init) at
 * STREAMON time when no VENC channel is open. If /dev/venc-chn0 is already
 * open, the driver defers b2i_pp_chn_init to CREATE_ENC time instead, at
 * which point some bridge parameter is still zero → kernel division by zero.
 *
 * This matches anyka_ipc's sequence: ISP/VI open → STREAMON (triggers
 * b2i_pp_chn_init with correct slice params) → THEN open VENC devices.
 * ------------------------------------------------------------------------- */
static int open_isp_vi_devices(void)
{
    g_isp_fd   = open(DEV_ISP,        O_RDWR);
    g_subdev   = open(DEV_SUBDEV,     O_RDWR | O_NONBLOCK);
    g_vid_main = open(DEV_VIDEO_MAIN, O_RDWR | O_NONBLOCK);
    g_vid_sub  = open(DEV_VIDEO_SUB,  O_RDWR);  /* blocking: AK VI driver ignores F_SETFL, DQBUF needs blocking open */
    g_vid_td   = open(DEV_VIDEO_TD,   O_RDWR | O_NONBLOCK);

    if (g_isp_fd < 0 || g_subdev < 0 || g_vid_main < 0 || g_vid_sub < 0 || g_vid_td < 0) {
        perror("open_isp_vi_devices");
        return -1;
    }
    printf("[ak_rtsp] ISP/VI devices opened\n");
    return 0;
}

/* Open VENC device nodes — call AFTER vi_start_capture() returns (STREAMON done). */
static int open_venc_devices(void)
{
    g_venc_fd = open(DEV_VENC,      O_RDWR);
    g_chn0_fd = open(DEV_VENC_CHN0, O_RDWR | O_NONBLOCK);

    if (g_venc_fd < 0 || g_chn0_fd < 0) {
        perror("open_venc_devices");
        return -1;
    }
    printf("[ak_rtsp] VENC devices opened\n");
    return 0;
}

/* -------------------------------------------------------------------------
 * Close all device nodes and unmap DMA regions
 * ------------------------------------------------------------------------- */
static void close_devices(void)
{
    int i;
    uint32_t chn_id = 0;

    ioctl(g_chn0_fd, VENC_IOCTL_DEACTIVATE, &chn_id);

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(g_vid_main, VI_IOCTL_STREAMOFF, &type);
    ioctl(g_vid_sub,  VI_IOCTL_STREAMOFF, &type);
    ioctl(g_vid_td,   VI_IOCTL_STREAMOFF, &type);

    for (i = 0; i < VI_BUF_COUNT; i++)
        if (g_vi_bufs[i] && g_vi_bufs[i] != MAP_FAILED)
            munmap(g_vi_bufs[i], g_vi_buf_sizes[i]);

    for (i = 0; i < VI_SUB_BUF_COUNT; i++)
        if (g_vi_sub_bufs[i] && g_vi_sub_bufs[i] != MAP_FAILED)
            munmap(g_vi_sub_bufs[i], g_vi_sub_buf_sizes[i]);

    for (i = 0; i < VI_TD_BUF_COUNT; i++)
        if (g_vi_td_bufs[i] && g_vi_td_bufs[i] != MAP_FAILED)
            munmap(g_vi_td_bufs[i], g_vi_td_buf_sizes[i]);

    audio_stop();

    if (g_venc_virt != MAP_FAILED) munmap(g_venc_virt, ENC_VENC_SIZE);
    if (g_chn0_fd  >= 0) close(g_chn0_fd);
    if (g_venc_fd  >= 0) close(g_venc_fd);
    if (g_vid_td   >= 0) close(g_vid_td);
    if (g_vid_sub  >= 0) close(g_vid_sub);
    if (g_vid_main >= 0) close(g_vid_main);
    if (g_subdev   >= 0) close(g_subdev);
    if (g_isp_fd   >= 0) close(g_isp_fd);
}

/* -------------------------------------------------------------------------
 * Entry point
 *
 * Prerequisites:
 *   Kernel modules loaded by sensor_driver.sh on normal boot:
 *     ak_isp.ko, sensor_gc20c3.ko, ak_venc_adapter.ko, ak_venc_bridge.ko
 *   /tmp/sensor_isp.conf must exist (sensor_driver.sh copies isp_gc20c3.conf).
 *
 * Test procedure (from SD card):
 *   sh /mnt/start_rtsp.sh
 *   Connect VLC to rtsp://CAMERA_IP:554/stream
 *
 * The start_rtsp.sh script handles:
 *   - kill anyka_ipc + watchdog
 *   - rmmod/insmod venc modules (clean kernel state)
 *   - feed /dev/watchdog in background
 *   - run this binary
 *
 * Init sequence:
 *   venc_reset_stale_channel  — clear any state left by a killed anyka_ipc
 *   open_devices              — open all /dev/* nodes
 *   isp_load_sensor_conf      — program GC20C3 I2C registers
 *   isp_pipeline_init         — program 24 ISP image-pipeline modules
 *   venc_reset_stale_channel  — clear any state left by a killed anyka_ipc
 *   open_isp_vi_devices       — open ISP + VI fds (VENC must NOT be open yet)
 *   isp_load_sensor_conf      — program GC20C3 I2C registers
 *   isp_pipeline_init         — program 24 ISP image-pipeline modules
 *   vi_set_channel_attr       — set done_mode/slice/crop/res/buf_size BEFORE REQBUFS
 *   vi_start_capture          — STREAMOFF(stale) → REQBUFS → QUERYBUF → QBUF →
 *                               STREAMON (b2i_pp_chn_init fires, no VENC open) →
 *                               STREAMOFF (ISP stops; bridge ISP-side state kept)
 *   open_venc_devices         — NOW open /dev/venc + /dev/venc-chn0
 *   SET_CHN_STATE=1           — bypass CREATE_POOL/CREATE_ENC paradox
 *   venc_create_pool          — QUERY_DMA + CREATE_POOL ring + ext + mmap
 *   venc_create_encoder       — CREATE_ENC H.264 Main 1920×1080
 *   venc_bind_vi              — BIND_VI_CHN (b2i_pp_chn_init fires again, ISP
 *                               stopped → no interrupt race → no div/0)
 *   venc_activate             — ACTIVATE
 *   vi_start_stream           — re-QBUF → STREAMON (ISP starts; both bridge
 *                               sides connected → frames flow)
 *   audio_start               — open /dev/pcmC0D0c, configure mic capture,
 *                               start its background read() thread
 *                               (non-fatal — video keeps working if this fails)
 *   accept loop               — serve one RTSP client at a time on port 554
 * ------------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    /* Very first thing: tees stdout so the TCP tuning server's debug log
     * view gets the FULL boot log (ISP/VI/VENC init, not just ae/night's
     * own printf calls). See control.c for why this is a stdout tee rather
     * than per-callsite logging changes. */
    control_start();

    int         no_isp_init       = 0;
    const char *grab_then_exit    = NULL;
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-isp-init") == 0) {
            no_isp_init = 1;
        } else if (strcmp(argv[i], "--grab-then-exit") == 0 && i + 1 < argc) {
            grab_then_exit = argv[++i];
        }
    }

    print_build_info();
    signal(SIGUSR1, sigusr1_handler);

    venc_reset_stale_channel();

    /* Step 1: Open and configure the ISP and Video Input (VI) devices.
     * Starts capturing so the ISP-side bridge is initialized cleanly. */
    if (open_isp_vi_devices()    < 0) goto err;

    if (no_isp_init) {
        /* --no-isp-init DOES NOT WORK as a diagnostic.
         *
         * Intent was: skip isp_pipeline_init() to inherit anyka_ipc's converged
         * ISP state and compare image quality.
         *
         * Why it fails: when anyka_ipc exits, the ISP kernel driver resets ALL
         * its software-managed state (exposure attr, AE stats, etc.) to zero.
         * The hardware silicon may retain register values, but the kernel driver
         * structures are zeroed. Result: exp_type=0 (uninitialized zero = AUTO
         * mode), priv[exp]=0, luma=0 permanently, hw_exp=256 stuck, NO FRAMES.
         *
         * Do not attempt to use --no-isp-init as a diagnostic again. */
        printf("[ak_rtsp] --no-isp-init: WARNING — ISP kernel state reset on anyka_ipc exit.\n");
        printf("[ak_rtsp]   Frames will not work. See main.c comment for explanation.\n");
    } else {
        if (isp_load_sensor_conf()   < 0) goto err;
        if (isp_pipeline_init()      < 0) goto err;
    }

    /* Must run before vi_set_channel_attr()/vi_start_capture() (STREAMON) --
     * matches anyka_ipc's own strace-confirmed ordering (it issues every ISP
     * tunnel call, including sensor-fps/frame-rate, before touching any VI
     * channel at all). Doing this after STREAMON left ak_isp.ko's per-channel
     * fps cache zeroed when the first frame interrupt landed, causing a
     * kernel "Division by zero" — see ae_init_isp_params()'s comment in
     * ae.c. */
    if (ae_init_isp_params()     < 0) goto err;

    ircut_init();
    if (vi_set_channel_attr()    < 0) goto err;
    if (vi_start_capture()       < 0) goto err;

#ifdef AEC_CUSTOM
    if (aec_custom_start()       < 0) goto err;
#else
    if (ae_start()               < 0) goto err;
#endif

    /* Step 2: Open and configure VENC.
     * Since bind resolution parameters are now correctly passed, we can safely
     * set up the encoder and connect the bridge while the ISP is active. */
    if (open_venc_devices()      < 0) goto err;

    /* Override the kernel's channel state to 1.
     * Bypasses the CREATE_POOL/CREATE_ENC paradox. */
    if (ioctl(g_chn0_fd, 0x406056c7, 1) < 0)
        perror("VENC_IOCTL_SET_CHN_STATE (non-fatal)");

    if (venc_create_pool()       < 0) goto err;
    if (venc_create_encoder()    < 0) goto err;
    if (venc_bind_vi()           < 0) goto err;
    if (venc_activate()          < 0) goto err;

    night_start();  /* needs AE samples flowing, so start after ae_start() */

    /* Non-fatal: video streaming must keep working even if mic capture
     * fails to init (e.g. an audio device race — a supervisor-respawned
     * anyka_ipc could still be holding it in some future scenario we
     * haven't hit yet). */
    if (audio_start() < 0)
        printf("[ak_rtsp] audio: init failed, continuing video-only\n");

    /* Grab-then-exit: DQBUF blocks (O_NONBLOCK temporarily cleared in
     * vi_grab_sub_frame) until the first sub channel frame arrives. */
    if (grab_then_exit) {
        printf("[ak_rtsp] grab-then-exit: waiting for first sub channel frame...\n");
        vi_grab_sub_frame(grab_then_exit);
        close_devices();
        return 0;
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); goto err; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family      = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port        = htons(RTSP_SERVER_PORT);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind"); close(server_fd); goto err;
    }
    if (listen(server_fd, 5) < 0) {
        perror("listen"); close(server_fd); goto err;
    }

    printf("[ak_rtsp] Listening on port %d...\n", RTSP_SERVER_PORT);

    for (;;) {
        struct pollfd pfd;
        pfd.fd     = server_fd;
        pfd.events = POLLIN;

        /* While waiting for a client, drain the VENC ring so it doesn't wrap.
         * anyka_ipc polls GET_STREAM continuously even without a connected client.
         * Without draining, the ring wraps in ~3s at 2Mbps (ENC_VENC_SIZE=777KB),
         * and the next client would need up to one GOP (2s) to sync to an I-frame. */
        if (g_grab_request) {
            g_grab_request = 0;
            char path[64];
            snprintf(path, sizeof(path), "/mnt/grab_%04d.yuv", ++g_grab_counter);
            vi_grab_sub_frame(path);
        }

        int p = poll(&pfd, 1, 0);
        if (p < 0 && errno != EINTR) { perror("poll"); continue; }
        if (p <= 0) {
            drain_venc_ring();
            continue;
        }

        struct sockaddr_storage client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) { perror("accept"); continue; }
        handle_client(client_fd, &client_addr, addr_len);
    }

    close(server_fd);

err:
    night_stop();
#ifdef AEC_CUSTOM
    aec_custom_stop();
#else
    ae_stop();
#endif
    control_stop();
    close_devices();
    return 1;
}
