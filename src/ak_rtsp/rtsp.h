#pragma once
#include <sys/socket.h>

/* Accept a client connection and run the full RTSP session loop until the
 * client disconnects or teardown is received. Blocks until session ends. */
void handle_client(int client_fd, struct sockaddr_storage *client_addr,
                   socklen_t addr_len);

/* Drain one pending frame from the VENC ring buffer without sending it.
 * Call this in a tight loop while waiting in accept() so the ring doesn't
 * wrap and the next client starts near an I-frame. */
void drain_venc_ring(void);
