#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <stdbool.h>
#include <smolrtsp.h>
#include <smolrtsp/types/rtcp.h>
#include <interface99.h>
#include <datatype99.h>

#include "ak_rtsp.h"
#include "ak_ioctls.h"
#include "audio.h"
#include "rtsp.h"

/* -------------------------------------------------------------------------
 * Client connection context conforming to SmolRTSP_Controller interface
 * ------------------------------------------------------------------------- */
typedef struct {
    int socket_fd;
    struct sockaddr_storage addr;
    socklen_t addr_len;

    uint64_t session_id;
    bool is_playing;

    bool is_tcp;
    uint8_t rtp_channel;
    uint8_t rtcp_channel;

    int rtp_udp_fd;
    int rtcp_udp_fd;
    uint16_t client_rtp_port;
    uint16_t client_rtcp_port;

    SmolRTSP_RtpTransport *rtp_transport;
    SmolRTSP_NalTransport *nal_transport;

    /* Audio track (streamid=1). Plain RTP, no NAL wrapping — payload is raw
     * L16 samples. NULL until SETUP is called for the audio URI; audio
     * frames are only sent once this is non-NULL. Separate UDP fds from the
     * video track since each track gets its own client_port pair. */
    SmolRTSP_RtpTransport *audio_rtp_transport;
    int audio_rtp_udp_fd;
    int audio_rtcp_udp_fd;

    /* time(NULL) value of the next scheduled RTCP Sender Report — see
     * send_rtcp_reports() below. 0 means "not yet scheduled". */
    time_t next_rtcp_sr_sec;
} Client;

declImpl(SmolRTSP_Controller, Client);

static void Client_drop(VSelf) {
    VSELF(Client);
    if (self->nal_transport) {
        VTABLE(SmolRTSP_NalTransport, SmolRTSP_Droppable).drop(self->nal_transport);
    } else if (self->rtp_transport) {
        VTABLE(SmolRTSP_RtpTransport, SmolRTSP_Droppable).drop(self->rtp_transport);
    }
    if (self->rtp_udp_fd >= 0) close(self->rtp_udp_fd);
    if (self->rtcp_udp_fd >= 0) close(self->rtcp_udp_fd);

    if (self->audio_rtp_transport)
        VTABLE(SmolRTSP_RtpTransport, SmolRTSP_Droppable).drop(self->audio_rtp_transport);
    if (self->audio_rtp_udp_fd >= 0) close(self->audio_rtp_udp_fd);
    if (self->audio_rtcp_udp_fd >= 0) close(self->audio_rtcp_udp_fd);
}

impl(SmolRTSP_Droppable, Client);

/* Generic per-track transport setup (used for both the video and audio
 * tracks — see setup_video_transport()/setup_audio_transport() below).
 * Creates the RtpTransport and its UDP fds (for UDP) but does NOT wrap it
 * in a NalTransport — that's video-specific and done by the caller. */
static int setup_rtp_transport(Client *self, SmolRTSP_Context *ctx, const SmolRTSP_Request *req,
                                uint8_t payload_ty, uint32_t clock_rate,
                                SmolRTSP_RtpTransport **out_transport,
                                int *out_rtp_fd, int *out_rtcp_fd) {
    CharSlice99 transport_val;
    if (!SmolRTSP_HeaderMap_find(&req->header_map, SMOLRTSP_HEADER_TRANSPORT, &transport_val)) {
        smolrtsp_respond(ctx, SMOLRTSP_STATUS_BAD_REQUEST, "Transport not present");
        return -1;
    }

    SmolRTSP_TransportConfig config;
    if (smolrtsp_parse_transport(&config, transport_val) == -1) {
        smolrtsp_respond(ctx, SMOLRTSP_STATUS_BAD_REQUEST, "Malformed Transport");
        return -1;
    }

    if (config.lower == SmolRTSP_LowerTransport_TCP) {
        ifLet(config.interleaved, SmolRTSP_ChannelPair_Some, interleaved) {
            self->is_tcp = true;
            self->rtp_channel = interleaved->rtp_channel;
            self->rtcp_channel = interleaved->rtcp_channel;

            printf("[ak_rtsp] Transport: TCP interleaved (RTP ch %d, RTCP ch %d)\n",
                   (int)interleaved->rtp_channel, (int)interleaved->rtcp_channel);

            SmolRTSP_Transport t = smolrtsp_transport_tcp(
                SmolRTSP_Context_get_writer(ctx), interleaved->rtp_channel, 0);

            *out_transport = SmolRTSP_RtpTransport_new(t, payload_ty, clock_rate);

            smolrtsp_header(ctx, SMOLRTSP_HEADER_TRANSPORT,
                "RTP/AVP/TCP;unicast;interleaved=%d-%d",
                (int)interleaved->rtp_channel, (int)interleaved->rtcp_channel);
            return 0;
        }
        smolrtsp_respond(ctx, SMOLRTSP_STATUS_BAD_REQUEST, "interleaved not found");
        return -1;
    } else {
        ifLet(config.client_port, SmolRTSP_PortPair_Some, client_port) {
            self->is_tcp = false;
            self->client_rtp_port = client_port->rtp_port;
            self->client_rtcp_port = client_port->rtcp_port;

            char ip_str[64] = "unknown";
            if (self->addr.ss_family == AF_INET) {
                inet_ntop(AF_INET, &((struct sockaddr_in *)&self->addr)->sin_addr,
                          ip_str, sizeof(ip_str));
            }
            printf("[ak_rtsp] Transport: UDP client %s RTP:%u RTCP:%u\n",
                   ip_str, client_port->rtp_port, client_port->rtcp_port);

            void *ip = smolrtsp_sockaddr_ip((struct sockaddr *)&self->addr);
            *out_rtp_fd = smolrtsp_dgram_socket(self->addr.ss_family, ip,
                                                 client_port->rtp_port);
            if (*out_rtp_fd < 0) {
                printf("[ak_rtsp] Failed to create UDP RTP socket\n");
                smolrtsp_respond_internal_error(ctx);
                return -1;
            }

            *out_rtcp_fd = smolrtsp_dgram_socket(self->addr.ss_family, ip,
                                                  client_port->rtcp_port);

            struct sockaddr_storage local_addr;
            socklen_t local_len = sizeof(local_addr);
            uint16_t local_rtp_port = 0, local_rtcp_port = 0;

            if (getsockname(*out_rtp_fd, (struct sockaddr *)&local_addr, &local_len) == 0)
                local_rtp_port = ntohs(((struct sockaddr_in *)&local_addr)->sin_port);
            if (*out_rtcp_fd >= 0 &&
                getsockname(*out_rtcp_fd, (struct sockaddr *)&local_addr, &local_len) == 0)
                local_rtcp_port = ntohs(((struct sockaddr_in *)&local_addr)->sin_port);

            printf("[ak_rtsp] Local UDP: RTP:%u RTCP:%u\n", local_rtp_port, local_rtcp_port);

            SmolRTSP_Transport t = smolrtsp_transport_udp(*out_rtp_fd);
            *out_transport = SmolRTSP_RtpTransport_new(t, payload_ty, clock_rate);

            smolrtsp_header(ctx, SMOLRTSP_HEADER_TRANSPORT,
                "RTP/AVP/UDP;unicast;client_port=%u-%u;server_port=%u-%u",
                client_port->rtp_port, client_port->rtcp_port,
                local_rtp_port, local_rtcp_port);
            return 0;
        }
        smolrtsp_respond(ctx, SMOLRTSP_STATUS_BAD_REQUEST, "client_port not found");
        return -1;
    }
}

static int setup_video_transport(Client *self, SmolRTSP_Context *ctx, const SmolRTSP_Request *req) {
    if (setup_rtp_transport(self, ctx, req, 96, 90000,
                             &self->rtp_transport, &self->rtp_udp_fd, &self->rtcp_udp_fd) < 0)
        return -1;
    self->nal_transport = SmolRTSP_NalTransport_new(self->rtp_transport);
    return 0;
}

static int setup_audio_transport(Client *self, SmolRTSP_Context *ctx, const SmolRTSP_Request *req) {
    return setup_rtp_transport(self, ctx, req, 97, AUDIO_SAMPLE_RATE,
                                &self->audio_rtp_transport,
                                &self->audio_rtp_udp_fd, &self->audio_rtcp_udp_fd);
}

static void Client_options(VSelf, SmolRTSP_Context *ctx, const SmolRTSP_Request *req) {
    VSELF(Client);
    (void)self; (void)req;
    smolrtsp_header(ctx, SMOLRTSP_HEADER_PUBLIC, "DESCRIBE, SETUP, PLAY, TEARDOWN");
    smolrtsp_respond_ok(ctx);
}

static void Client_describe(VSelf, SmolRTSP_Context *ctx, const SmolRTSP_Request *req) {
    VSELF(Client);
    (void)self; (void)req;
    char sdp_buf[768];
    /* Audio track uses payload type 97 with L16 (linear PCM): anyka_ipc's own
     * capture stream is raw 8kHz/mono/16-bit, not G.711, so RFC 3551 L16
     * needs no fmtp at all. PT 97 (dynamic) matches the convention anyka_ipc's
     * own RTSP SDP builder uses for all of its audio codec options ("Session 3"
     * correction), even though it's not required for L16 specifically. */
    snprintf(sdp_buf, sizeof(sdp_buf),
        "v=0\r\n"
        "o=- %llu %llu IN IP4 0.0.0.0\r\n"
        "s=ak_rtsp\r\n"
        "t=0 0\r\n"
        "m=video 0 RTP/AVP 96\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "a=rtpmap:96 H264/90000\r\n"
        "a=fmtp:96 packetization-mode=1\r\n"
        "a=control:streamid=0\r\n"
        "m=audio 0 RTP/AVP 97\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "a=rtpmap:97 L16/%d/%d\r\n"
        "a=control:streamid=1\r\n",
        (unsigned long long)time(NULL), (unsigned long long)time(NULL),
        AUDIO_SAMPLE_RATE, AUDIO_CHANNELS);

    smolrtsp_header(ctx, SMOLRTSP_HEADER_CONTENT_TYPE, "application/sdp");
    smolrtsp_body(ctx, CharSlice99_from_str(sdp_buf));
    smolrtsp_respond_ok(ctx);
}

static void Client_setup(VSelf, SmolRTSP_Context *ctx, const SmolRTSP_Request *req) {
    VSELF(Client);

    /* Which track this SETUP is for is carried in the request URI, matching
     * the "a=control:streamid=N" values handed out in Client_describe above
     * (clients build the SETUP URI as <base>/<control-value>). */
    const char *uri = CharSlice99_alloca_c_str(req->start_line.uri);
    bool is_audio = strstr(uri, "streamid=1") != NULL;

    if (is_audio) {
        if (setup_audio_transport(self, ctx, req) < 0) return;
    } else {
        if (setup_video_transport(self, ctx, req) < 0) return;
    }

    /* Session ID must stay the same across both tracks' SETUP calls within
     * one session — only generate it once. */
    if (self->session_id == 0) self->session_id = (uint64_t)rand();
    smolrtsp_header(ctx, SMOLRTSP_HEADER_SESSION, "%llu", (unsigned long long)self->session_id);
    smolrtsp_respond_ok(ctx);
}

static void Client_play(VSelf, SmolRTSP_Context *ctx, const SmolRTSP_Request *req) {
    VSELF(Client);
    (void)req;
    self->is_playing = true;
    smolrtsp_header(ctx, SMOLRTSP_HEADER_RANGE, "npt=now-");
    smolrtsp_respond_ok(ctx);
}

static void Client_teardown(VSelf, SmolRTSP_Context *ctx, const SmolRTSP_Request *req) {
    VSELF(Client);
    (void)req;
    self->is_playing = false;
    smolrtsp_respond_ok(ctx);
}

static void Client_unknown(VSelf, SmolRTSP_Context *ctx, const SmolRTSP_Request *req) {
    VSELF(Client);
    (void)self; (void)req;
    smolrtsp_respond(ctx, SMOLRTSP_STATUS_METHOD_NOT_ALLOWED, "Method Not Allowed");
}

static SmolRTSP_ControlFlow Client_before(VSelf, SmolRTSP_Context *ctx,
                                           const SmolRTSP_Request *req) {
    VSELF(Client);
    (void)self; (void)ctx;
    printf("[ak_rtsp] %s %s CSeq=%u\n",
           CharSlice99_alloca_c_str(req->start_line.method),
           CharSlice99_alloca_c_str(req->start_line.uri),
           req->cseq);
    return SmolRTSP_ControlFlow_Continue;
}

static void Client_after(VSelf, ssize_t ret, SmolRTSP_Context *ctx,
                          const SmolRTSP_Request *req) {
    VSELF(Client);
    (void)self; (void)ctx; (void)req;
    if (ret < 0) printf("[ak_rtsp] Failed to respond to client\n");
}

impl(SmolRTSP_Controller, Client);

/* -------------------------------------------------------------------------
 * H.264 frame packetization
 *
 * Walks Annex-B start codes, wraps each NALU in SmolRTSP_NalUnit, sets the
 * marker bit on the last VCL NALU of each access unit, and hands it to
 * SmolRTSP_NalTransport_send_packet for FU-A fragmentation if needed.
 * Timestamps: VENC gives milliseconds → multiply by 90 for 90 kHz RTP clock.
 * ------------------------------------------------------------------------- */
static void send_frame_nalus(Client *client, uint8_t *data, size_t len,
                              uint32_t timestamp)
{
    size_t i = 0;
    while (i + 3 < len) {
        size_t start_code_len = 0;
        if (data[i] == 0x00 && data[i+1] == 0x00 &&
            data[i+2] == 0x00 && data[i+3] == 0x01)
            start_code_len = 4;
        else if (data[i] == 0x00 && data[i+1] == 0x00 && data[i+2] == 0x01)
            start_code_len = 3;

        if (start_code_len > 0) {
            size_t nalu_start = i + start_code_len;
            size_t next_nalu_start = len;
            size_t j = nalu_start;
            while (j + 2 < len) {
                if (data[j] == 0x00 && data[j+1] == 0x00 && data[j+2] == 0x01) {
                    next_nalu_start = (j > 0 && data[j-1] == 0x00) ? j - 1 : j;
                    break;
                }
                j++;
            }

            size_t nalu_len = next_nalu_start - nalu_start;
            if (nalu_len > 0) {
                uint8_t nalu_type = data[nalu_start] & 0x1F;
                SmolRTSP_H264NalHeader h264_hdr =
                    SmolRTSP_H264NalHeader_parse(data[nalu_start]);
                SmolRTSP_NalUnit nalu = {
                    .header  = SmolRTSP_NalHeader_H264(h264_hdr),
                    .payload = U8Slice99_new(data + nalu_start + 1, nalu_len - 1),
                };
                bool is_vcl = (nalu_type >= 1 && nalu_type <= 5);
                if (SmolRTSP_NalTransport_send_packet(
                        client->nal_transport,
                        SmolRTSP_RtpTimestamp_Raw(timestamp),
                        is_vcl, nalu) == -1)
                    perror("send RTP/NAL");
            }
            i = next_nalu_start;
        } else {
            i++;
        }
    }
}

/* Drain any frames queued in the VENC ring without sending them.
 * Called while waiting for a client in accept() so the ring doesn't wrap
 * and the next client gets a fresh stream starting near an I-frame. */
void drain_venc_ring(void)
{
    struct venc_stream_req req;
    memset(&req, 0, sizeof(req));
    req.chn_id = 0;
    req.len    = venc_max_frame_size(ENC_WIDTH, ENC_HEIGHT);

    if (ioctl(g_chn0_fd, VENC_IOCTL_GET_STREAM, &req) == 0)
        ioctl(g_chn0_fd, VENC_IOCTL_RELEASE_STREAM, &req);
}

static int send_video_frame(Client *client)
{
    struct venc_stream_req req;
    int max_len = venc_max_frame_size(ENC_WIDTH, ENC_HEIGHT);

    memset(&req, 0, sizeof(req));
    req.chn_id = 0;
    req.len    = max_len;

    if (ioctl(g_chn0_fd, VENC_IOCTL_GET_STREAM, &req) < 0) {
        if (errno == EAGAIN || errno == EFAULT || errno == EINTR) {
            usleep(2000);
            return 0;
        }
        /* Unexpected error — drop this client, encoder keeps running */
        perror("VENC_IOCTL_GET_STREAM");
        fprintf(stderr, "[ak_rtsp] GET_STREAM errno=%d, dropping client\n", errno);
        return -1;
    }

    void *data = (char *)g_venc_virt + (req.frame_offset - g_venc_phys);
    uint64_t ms_ts = (uint64_t)req.ts_hi << 32 | (uint64_t)(uint32_t)req.ts_lo;
    uint32_t rtp_ts = (uint32_t)(ms_ts * 90);

    static int frame_count = 0;
    /* Always log I-frames; log a heartbeat every ~10s (150 frames at 15fps) */
    if (req.frame_type_raw == FRAME_TYPE_I || frame_count % 150 == 0)
        printf("[ak_rtsp] frame#%d type=%s len=%d ts=%u\n",
               frame_count,
               req.frame_type_raw == FRAME_TYPE_I ? "I" :
               req.frame_type_raw == FRAME_TYPE_P ? "P" : "B",
               req.len, rtp_ts);
    frame_count++;

    /* Live active-bitrate log — accumulates encoded bytes over a window
     * using the encoder's own ms_ts (not wall-clock time, so it stays
     * accurate even if the process stalls briefly for some other reason).
     *
     * Window length is 2 full GOPs, not a flat 1s: with VBR+ rate control
     * on a static/low-motion scene, I-frames are far larger than P-frames
     * (confirmed on hardware: ~134KB I-frame vs. ~7-9KB P-frames on an
     * unchanging scene) and a 1s window doesn't line up with the 2s GOP
     * cycle at ENC_GOP=30/ENC_FPS=15 — so a plain 1s average alternated
     * between an ~1850kbps "I-frame window" and an ~520kbps "P-frame-only
     * window", both individually correct but neither representative of the
     * actual sustained rate, and both looking alarmingly far from the
     * 1536kbps target ceiling on their own. Averaging over 2 full GOPs
     * guarantees at least one I-frame lands in every window, so the
     * reported number reflects the real blended rate the target ceiling is
     * actually meant to bound. */
    static uint64_t bitrate_window_start_ms = 0;
    static uint64_t bitrate_window_bytes    = 0;
    static int      bitrate_window_frames   = 0;
    const uint64_t bitrate_window_target_ms = (uint64_t)(ENC_GOP * 2) * 1000 / ENC_FPS;
    if (bitrate_window_start_ms == 0) bitrate_window_start_ms = ms_ts;
    bitrate_window_bytes += (uint64_t)req.len;
    bitrate_window_frames++;
    uint64_t window_elapsed_ms = ms_ts - bitrate_window_start_ms;
    if (window_elapsed_ms >= bitrate_window_target_ms) {
        double kbps = (double)(bitrate_window_bytes * 8) / (double)window_elapsed_ms;
        printf("[ak_rtsp] bitrate: %.0f kbps (%d frames, %llu bytes over %llums, ~2 GOPs)\n",
               kbps, bitrate_window_frames,
               (unsigned long long)bitrate_window_bytes,
               (unsigned long long)window_elapsed_ms);
        bitrate_window_start_ms = ms_ts;
        bitrate_window_bytes    = 0;
        bitrate_window_frames   = 0;
    }

    send_frame_nalus(client, (uint8_t *)data, req.len, rtp_ts);

    if (ioctl(g_chn0_fd, VENC_IOCTL_RELEASE_STREAM, &req) < 0) {
        perror("VENC_IOCTL_RELEASE_STREAM");
        return -1;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Audio: drain whatever periods audio.c's capture thread has ready and send
 * them as plain RTP (no NAL framing — this isn't H.264).
 *
 * Timestamp: a running sample counter (RFC 3551's intended scheme for a
 * fixed-rate PCM stream like L16), NOT wall-clock time. Originally used
 * SmolRTSP_RtpTimestamp_SysClockUs(clock_gettime(CLOCK_MONOTONIC)), which
 * hardware-confirmed broke badly: CLOCK_MONOTONIC is time-since-boot, while
 * the video track's timestamp (rtsp.c's send_video_frame, ms_ts*90) comes
 * from the VENC hardware encoder's own internal clock, which resets near
 * zero when the encoder channel initializes — a totally different epoch.
 * Whatever gap exists between "camera booted" and "ak_rtsp started the VENC
 * channel" (30-40s observed) showed up directly as a matching offset
 * between the two tracks' first timestamps, which broke VLC's initial A/V
 * sync badly (~34s stream-buffering stall, then large "buffer too late"
 * drops). A monotonically-incrementing sample count has no wall-clock
 * dependency at all and matches the actual data rate exactly, avoiding this
 * whole class of bug. Starts from a random 32-bit value per RFC 3550 §5.1
 * (receivers must not assume a stream's initial timestamp is 0). RFC 3551
 * L16 payload must be network (big-endian) byte order; our samples are
 * native (little-endian on ARM), so each sample is byte-swapped before
 * sending. */
static void send_audio_frame(Client *client)
{
    if (!client->audio_rtp_transport) return;

    static int16_t pcm[AUDIO_PERIOD_SAMPLES];
    size_t n_samples = 0;
    int got = audio_get_frame(pcm, AUDIO_PERIOD_SAMPLES, &n_samples);
    if (got <= 0) return; /* 0 = nothing new yet, -1 = audio not running */

    static uint16_t net_pcm[AUDIO_PERIOD_SAMPLES];
    for (size_t i = 0; i < n_samples; i++)
        net_pcm[i] = htons((uint16_t)pcm[i]);

    static uint32_t audio_rtp_ts = 0;
    static bool     audio_rtp_ts_init = false;
    if (!audio_rtp_ts_init) {
        audio_rtp_ts = (uint32_t)rand();
        audio_rtp_ts_init = true;
    }

    U8Slice99 payload = U8Slice99_new((uint8_t *)net_pcm, n_samples * sizeof(uint16_t));
    if (SmolRTSP_RtpTransport_send_packet(
            client->audio_rtp_transport, SmolRTSP_RtpTimestamp_Raw(audio_rtp_ts),
            true, U8Slice99_empty(), payload) == -1)
        perror("send RTP/audio");

    audio_rtp_ts += (uint32_t)n_samples;
}

/* -------------------------------------------------------------------------
 * RTCP Sender Reports.
 *
 * Added to fix a VLC(/live555)-specific bug: without ANY RTCP SR at all
 * (this server previously opened RTCP sockets but never sent anything on
 * them), VLC's audio track eventually hit a hard "Timestamp conversion
 * failed" error and dropped audio permanently for the rest of the session
 * — video was unaffected. ffplay played the identical stream with no
 * dropout, confirming the wire data itself was correct and the receiver
 * just had no RTP-timestamp-to-wallclock mapping to fall back on once its
 * own internal clock model drifted.
 * An SR gives a receiver exactly that mapping (an (NTP time, RTP
 * timestamp) pair) so it can correct for drift indefinitely instead of
 * eventually giving up. UDP-transport only for now — TCP-interleaved RTCP
 * would need to write framed bytes over the RTSP TCP connection instead of
 * a separate socket; not implemented since this project only exercises UDP
 * transport in practice. */
#define RTCP_SR_INTERVAL_SEC 5

static void send_one_rtcp_sr(SmolRTSP_RtpTransport *transport, int rtcp_fd) {
    if (!transport || rtcp_fd < 0) return;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    /* Unix epoch (1970) -> NTP epoch (1900): RFC 3550 §4. */
    uint32_t ntp_sec  = (uint32_t)ts.tv_sec + 2208988800u;
    uint32_t ntp_frac = (uint32_t)(((uint64_t)ts.tv_nsec << 32) / 1000000000ull);

    SmolRTSP_RtcpSr sr = {
        .padding     = false,
        .rc          = 0,
        .ssrc        = SmolRTSP_RtpTransport_ssrc(transport),
        .ntp_sec     = htonl(ntp_sec),
        .ntp_frac    = htonl(ntp_frac),
        .rtp_ts      = htonl(SmolRTSP_RtpTransport_last_rtp_ts(transport)),
        .pkt_count   = htonl(SmolRTSP_RtpTransport_pkt_count(transport)),
        .octet_count = htonl(SmolRTSP_RtpTransport_octet_count(transport)),
    };

    uint8_t buf[SMOLRTSP_RTCP_SR_SIZE_BASE];
    if (write(rtcp_fd, SmolRTSP_RtcpSr_serialize(sr, buf), sizeof(buf)) < 0)
        perror("send RTCP SR");
}

/* Sends an SR for every track that's been SETUP, at most once every
 * RTCP_SR_INTERVAL_SEC seconds. Called once per client-loop iteration —
 * cheap to call every time since it no-ops between intervals. */
static void send_rtcp_reports(Client *client) {
    time_t now = time(NULL);
    if (now < client->next_rtcp_sr_sec) return;
    client->next_rtcp_sr_sec = now + RTCP_SR_INTERVAL_SEC;

    send_one_rtcp_sr(client->rtp_transport, client->rtcp_udp_fd);
    send_one_rtcp_sr(client->audio_rtp_transport, client->audio_rtcp_udp_fd);
}

/* -------------------------------------------------------------------------
 * Client connection loop
 *
 * Sets client socket non-blocking and uses poll() with:
 *   timeout = 0   when is_playing (check for commands, then immediately send)
 *   timeout = 5000 ms when idle (block until a request arrives)
 * ------------------------------------------------------------------------- */
void handle_client(int client_fd, struct sockaddr_storage *client_addr,
                   socklen_t addr_len)
{
    int flags = fcntl(client_fd, F_GETFL, 0);
    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

    Client client;
    memset(&client, 0, sizeof(client));
    client.socket_fd = client_fd;
    memcpy(&client.addr, client_addr, addr_len);
    client.addr_len = addr_len;
    client.rtp_udp_fd = -1;
    client.rtcp_udp_fd = -1;
    client.audio_rtp_udp_fd = -1;
    client.audio_rtcp_udp_fd = -1;

    SmolRTSP_Controller controller = DYN(Client, SmolRTSP_Controller, &client);
    SmolRTSP_Writer writer = smolrtsp_fd_writer(&client_fd);

    char buf[4096];
    size_t buf_len = 0;

    printf("[ak_rtsp] Client connected\n");

    while (client_fd >= 0) {
        struct pollfd pfd;
        pfd.fd = client_fd;
        pfd.events = POLLIN;
        int poll_ret = poll(&pfd, 1, client.is_playing ? 0 : 5000);

        if (poll_ret < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }

        if (poll_ret > 0) {
            ssize_t n = recv(client_fd, buf + buf_len, sizeof(buf) - buf_len - 1, 0);
            if (n < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    perror("recv"); break;
                }
            } else if (n == 0) {
                printf("[ak_rtsp] Client disconnected\n");
                break;
            } else {
                buf_len += n;
                buf[buf_len] = '\0';

                SmolRTSP_Request req = SmolRTSP_Request_uninit();
                CharSlice99 input = CharSlice99_new(buf, buf_len);
                SmolRTSP_ParseResult res = SmolRTSP_Request_parse(&req, input);

                bool should_consume = false;
                size_t bytes_to_consume = 0;

                match(res) {
                    of(SmolRTSP_ParseResult_Success, status) {
                        match(*status) {
                            of(SmolRTSP_ParseStatus_Complete, offset) {
                                should_consume = true;
                                bytes_to_consume = *offset;
                            }
                            otherwise {}
                        }
                    }
                    of(SmolRTSP_ParseResult_Failure, err) {
                        (void)err;
                        printf("[ak_rtsp] Request parse failed (%zu bytes buffered)\n",
                               buf_len);
                        buf_len = 0;
                    }
                }

                if (should_consume) {
                    smolrtsp_dispatch(writer, controller, &req);
                    if (bytes_to_consume < buf_len) {
                        memmove(buf, buf + bytes_to_consume, buf_len - bytes_to_consume);
                        buf_len -= bytes_to_consume;
                    } else {
                        buf_len = 0;
                    }
                }
            }
        }

        if (client.is_playing) {
            if (send_video_frame(&client) < 0) {
                printf("[ak_rtsp] Error sending frame, closing\n");
                break;
            }
            send_audio_frame(&client); /* no-op until the audio track is SETUP */
            send_rtcp_reports(&client); /* no-op for tracks not yet SETUP, or off-interval */
        }
    }

    VCALL(DYN(Client, SmolRTSP_Droppable, &client), drop);
    close(client_fd);
    printf("[ak_rtsp] Client connection closed\n");
}
