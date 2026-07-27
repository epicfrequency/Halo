/*
 * net_io.h — tiny helpers for framed message I/O over a TCP socket.
 * Nothing clever on purpose: this is control-plane traffic (infrequent,
 * small) plus the AUDIO_DATA hot path, which callers stream in chunks
 * themselves rather than going through send_message.
 */
#ifndef HALO_NET_IO_H
#define HALO_NET_IO_H

#include <sys/socket.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include "protocol.h"

/* Reads exactly len bytes or returns -1 on error/EOF. */
static inline int halo_read_full(int fd, void *buf, size_t len) {
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, p + got, len - got, 0);
        if (n <= 0) return -1;
        got += (size_t)n;
    }
    return 0;
}

/* Writes exactly len bytes or returns -1 on error. */
static inline int halo_write_full(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

/* Sends a header + payload as one logical message. Takes a mutex because
 * both the network-read thread (FLUSH_ACK, CAPS) and the ALSA-writer /
 * position-reporting threads (POSITION, UNDERRUN) write to the same
 * socket concurrently. */
static inline int halo_send_message(int fd, pthread_mutex_t *send_mtx,
                                     uint16_t type, const void *payload,
                                     uint32_t length, uint64_t seq) {
    struct halo_header hdr;
    hdr.magic = HALO_MAGIC;
    hdr.type = type;
    hdr.flags = 0;
    hdr.length = length;
    hdr.seq = seq;

    int rc = 0;
    pthread_mutex_lock(send_mtx);
    if (halo_write_full(fd, &hdr, HALO_HEADER_SIZE) < 0) rc = -1;
    if (rc == 0 && length > 0 && payload) {
        if (halo_write_full(fd, payload, length) < 0) rc = -1;
    }
    pthread_mutex_unlock(send_mtx);
    return rc;
}

#endif /* HALO_NET_IO_H */
