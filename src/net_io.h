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
#include <errno.h>
#include <stdio.h>
#include <pthread.h>
#include "protocol.h"

/* Both helpers below rely on SO_RCVTIMEO/SO_SNDTIMEO being set on the
 * socket, which turns an indefinite block into a poll they can bound.
 * Everything here runs on threads that other threads depend on — the reader
 * also delivers PAUSE/RESUME/FLUSH, and the sender lock is shared with the
 * reader's own replies — so a stall in either direction can wedge the whole
 * daemon rather than just slowing one path down. TCP keepalive covers a peer
 * that has *died*; these cover a peer that is alive but has stopped making
 * progress, which keepalive answers happily and never reports. */
#define HALO_SOCKET_TIMEOUT_SECONDS 15
/* How long a peer may leave a message half-sent, or refuse to read, before
 * the connection is treated as dead. Generous: a busy sender pausing mid
 * message for a few seconds is normal, one doing it for a minute is not. */
#define HALO_STALL_LIMIT_SECONDS 60

/* Reads exactly len bytes or returns -1 on error/EOF.
 *
 * An idle wait is unbounded on purpose — sitting between messages with
 * nothing to do is the normal state and must not be mistaken for a fault.
 * A wait *part-way through* a message is different: the framing is already
 * committed, so there is no way to resynchronise, and waiting forever means
 * the thread that would deliver the next control message never returns. */
static inline int halo_read_full(int fd, void *buf, size_t len) {
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;
    int stalled_seconds = 0;
    while (got < len) {
        ssize_t n = recv(fd, p + got, len - got, 0);
        if (n > 0) {
            got += (size_t)n;
            stalled_seconds = 0;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (got == 0) continue; /* idle between messages: keep waiting */
            stalled_seconds += HALO_SOCKET_TIMEOUT_SECONDS;
            if (stalled_seconds >= HALO_STALL_LIMIT_SECONDS) {
                fprintf(stderr, "halo: peer stopped mid-message for %ds "
                                "(%zu of %zu bytes), dropping connection\n",
                        stalled_seconds, got, len);
                return -1;
            }
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return -1;
    }
    return 0;
}

/* Writes exactly len bytes or returns -1 on error.
 *
 * A peer that stops reading blocks this once its receive window closes, and
 * the send lock is shared with the reader thread's own replies — so one
 * unresponsive peer can stop the daemon from processing anything at all.
 * That is precisely the state a hung client leaves behind, so it has to be
 * bounded even though giving up means closing the connection: a partial
 * message cannot be resynchronised. */
static inline int halo_write_full(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;
    int stalled_seconds = 0;
    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += (size_t)n;
            stalled_seconds = 0;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            stalled_seconds += HALO_SOCKET_TIMEOUT_SECONDS;
            if (stalled_seconds >= HALO_STALL_LIMIT_SECONDS) {
                fprintf(stderr, "halo: peer stopped reading for %ds, "
                                "dropping connection\n", stalled_seconds);
                return -1;
            }
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return -1;
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
