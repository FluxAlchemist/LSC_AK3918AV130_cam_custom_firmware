/* venc_dump.h — shared VENC_IOCTL_CREATE_ENC struct dumper.
 *
 * Used by BOTH:
 *   - ak_rtsp/isp_hook.c (LD_PRELOAD capture of stock anyka_ipc's real ioctl
 *     payload — no libc, raw ARM syscalls)
 *   - ak_rtsp/venc.c, gated behind DEBUG_IOCTL_DUMP (dumps our OWN payload
 *     right before/after we send it — links a real libc, uses write(2))
 *
 * One field list, one format, defined once here, so the two logs are always
 * byte-for-byte comparable and can never drift out of sync the way two
 * independently hand-copied field lists eventually would. Field offsets
 * match ak_rtsp/ak_ioctls.h's venc_create_enc_req exactly (verified against
 * ExtractedData/isp_hook_n.log, 2026-07-04/05 — this is what caught the
 * "rc_mode was wrong all along" bug).
 *
 * Include this header AFTER defining AKDUMP_WRITE(fd, ptr, len) as the raw
 * "write these bytes to fd" primitive for your context:
 *   isp_hook.c:  #define AKDUMP_WRITE(fd,ptr,len) _sc3(SYS_WRITE,fd,(long)(ptr),(long)(len))
 *   venc.c:      #define AKDUMP_WRITE(fd,ptr,len) write((fd),(ptr),(len))
 * Both are ultimately just write(2) semantics, so one shared implementation
 * works for either caller despite the very different runtime environments. */
#pragma once
#include <stdint.h>

#ifndef AKDUMP_WRITE
#error "define AKDUMP_WRITE(fd, ptr, len) before including venc_dump.h"
#endif

static void akdump_str(int fd, const char *s)
{
    const char *e = s; while (*e) e++;
    AKDUMP_WRITE(fd, s, e - s);
}

static void akdump_u32(int fd, uint32_t v)
{
    char nb[16];
    int i = 15; nb[i] = '\0';
    if (!v) { nb[--i] = '0'; }
    else { while (v) { nb[--i] = '0' + (v % 10); v /= 10; } }
    akdump_str(fd, nb + i);
}

static void akdump_hex(int fd, uint32_t v)   /* 0x prefix + 8 hex digits */
{
    char h[11]; h[0]='0'; h[1]='x'; h[10]='\0';
    for (int i = 9; i >= 2; i--) {
        int d = v & 0xf;
        h[i] = d < 10 ? '0'+d : 'a'+(d-10);
        v >>= 4;
    }
    akdump_str(fd, h);
}

/* Dump a venc_create_enc_req's fields, given as a raw byte pointer so it
 * works identically whether the caller has a live struct instance
 * (ak_rtsp's own `req`) or a void* ioctl argument (isp_hook.c's `arg`) — the
 * wire layout is the same either way. `label` distinguishes the source in a
 * merged/diffed log (e.g. "STOCK" vs "AK_RTSP"). */
static void akdump_create_enc(int fd, const char *label, const void *buf, uint32_t ret)
{
    const uint8_t *b = (const uint8_t *)buf;
#define RD32(off) (*(const uint32_t*)(b+(off)))
#define RD16(off) (*(const uint16_t*)(b+(off)))
#define RD8(off)  (*(const uint8_t *)(b+(off)))
    akdump_str(fd, label);
    akdump_str(fd," CREATE_ENC chn_id=");    akdump_u32(fd, RD32(0x00));
    akdump_str(fd," codec=");                akdump_hex(fd, RD32(0x04));
    akdump_str(fd," chroma=");               akdump_u32(fd, RD32(0x08));
    akdump_str(fd," w=");                    akdump_u32(fd, RD16(0x0c));
    akdump_str(fd," h=");                    akdump_u32(fd, RD16(0x0e));
    akdump_str(fd," rc_mode=");              akdump_u32(fd, RD32(0x18));
    akdump_str(fd," fps=");                  akdump_u32(fd, RD16(0x1c));
    akdump_str(fd," goplen=");               akdump_u32(fd, RD16(0x1e));
    akdump_str(fd," max_fps=");              akdump_u32(fd, RD32(0x20));
    akdump_str(fd," enc_level=");            akdump_u32(fd, RD32(0x24));
    akdump_str(fd," qp_or_kbps=");           akdump_u32(fd, RD16(0x28));
    akdump_str(fd," qp2=");                  akdump_u32(fd, RD16(0x2a));
    akdump_str(fd,"\n  smart_mode=");        akdump_u32(fd, RD16(0x34));
    akdump_str(fd," smart_goplen=");         akdump_u32(fd, RD16(0x36));
    akdump_str(fd," smart_quality=");        akdump_u32(fd, RD16(0x38));
    akdump_str(fd," rc_flag=");              akdump_u32(fd, RD32(0x3c));
    akdump_str(fd,"\n  rc.cub_size=");       akdump_u32(fd, RD32(0x40));
    akdump_str(fd," rc.minqp=");             akdump_u32(fd, RD16(0x44));
    akdump_str(fd," rc.maxqp=");             akdump_u32(fd, RD16(0x46));
    akdump_str(fd," rc.delta=");             akdump_u32(fd, RD32(0x48));
    akdump_str(fd," rc.I_pic_size=");        akdump_u32(fd, RD32(0x4c));
    akdump_str(fd," rc.P_pic_size=");        akdump_u32(fd, RD32(0x50));
    akdump_str(fd," rc.B_pic_size=");        akdump_u32(fd, RD32(0x54));
    akdump_str(fd," rc.flag=");              akdump_u32(fd, RD32(0x58));
    akdump_str(fd," rc.flag2=");             akdump_u32(fd, RD8(0x5c));
    akdump_str(fd," rc.srd_threshold=");     akdump_u32(fd, RD8(0x5d));
    akdump_str(fd,"\n  raw[0..23]:");
    for (int i = 0; i < 24; i++) { akdump_str(fd," "); akdump_hex(fd, RD32(i*4)); }
    akdump_str(fd," ret="); akdump_u32(fd, ret);
    akdump_str(fd,"\n");
#undef RD32
#undef RD16
#undef RD8
}
