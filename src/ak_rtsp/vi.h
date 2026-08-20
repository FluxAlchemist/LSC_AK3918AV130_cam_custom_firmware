#pragma once

/* Configure VI channel 0 attributes (done_mode, slice_num, block_num, crop,
 * resolution, buf_size). Must be called BEFORE vi_start_capture(). */
int vi_set_channel_attr(void);

/* Start capture: REQBUFS → QUERYBUF+mmap → QBUF → STREAMON.
 * Starts the ISP pipeline and triggers b2i_pp_chn_init. */
int vi_start_capture(void);

/* Grab one 640×360 NV12 frame from the sub channel and write it to path.
 * Blocks up to 200ms for a frame. Call only after vi_start_capture().
 * Trigger via: kill -USR1 $(pidof ak_rtsp)
 * View on PC: ffplay -f rawvideo -video_size 640x360 -pixel_format nv12 <file> */
int vi_grab_sub_frame(const char *path);
