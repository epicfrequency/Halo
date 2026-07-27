/*
 * ring_buffer.h — single-producer/single-consumer lock-free byte ring
 * buffer. Producer = network reader thread. Consumer = ALSA writer thread.
 * This is the thing that decouples "network was jittery for 30ms" from
 * "ALSA needed samples right now" — see PROTOCOL.md / conversation notes
 * on why this exists.
 *
 * Capacity must be a power of two (cheap masking instead of modulo).
 */
#ifndef HALO_RING_BUFFER_H
#define HALO_RING_BUFFER_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    uint8_t *buf;
    size_t capacity;   /* power of two */
    size_t mask;
    _Atomic size_t head; /* written by producer, read by consumer */
    _Atomic size_t tail; /* written by consumer, read by producer */
} halo_ring_t;

static inline int halo_ring_init(halo_ring_t *r, size_t capacity_pow2) {
    if ((capacity_pow2 & (capacity_pow2 - 1)) != 0) return -1; /* not pow2 */
    r->buf = (uint8_t *)malloc(capacity_pow2);
    if (!r->buf) return -1;
    r->capacity = capacity_pow2;
    r->mask = capacity_pow2 - 1;
    atomic_store_explicit(&r->head, 0, memory_order_relaxed);
    atomic_store_explicit(&r->tail, 0, memory_order_relaxed);
    return 0;
}

static inline void halo_ring_free(halo_ring_t *r) {
    free(r->buf);
    r->buf = NULL;
}

static inline size_t halo_ring_used(const halo_ring_t *r) {
    size_t head = atomic_load_explicit((_Atomic size_t *)&r->head, memory_order_acquire);
    size_t tail = atomic_load_explicit((_Atomic size_t *)&r->tail, memory_order_acquire);
    return head - tail; /* wraps correctly with unsigned arithmetic */
}

static inline size_t halo_ring_free_space(const halo_ring_t *r) {
    return r->capacity - halo_ring_used(r);
}

/* Returns bytes actually written (may be less than len if buffer is full —
 * caller decides whether to block/retry or drop; for our use, the network
 * thread should back off and retry rather than drop audio). */
static inline size_t halo_ring_write(halo_ring_t *r, const uint8_t *data, size_t len) {
    size_t free_space = halo_ring_free_space(r);
    size_t to_write = len < free_space ? len : free_space;
    if (to_write == 0) return 0;

    size_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    size_t idx = head & r->mask;
    size_t first_chunk = r->capacity - idx;
    if (first_chunk > to_write) first_chunk = to_write;

    memcpy(r->buf + idx, data, first_chunk);
    if (to_write > first_chunk) {
        memcpy(r->buf, data + first_chunk, to_write - first_chunk);
    }

    atomic_store_explicit(&r->head, head + to_write, memory_order_release);
    return to_write;
}

/* Producer-side zero-copy write: hands back a pointer to the contiguous
 * writable span (shorter than free_space() when the write would wrap, in
 * which case the caller just loops) so data can be received straight into
 * the ring instead of via a bounce buffer. Commit with
 * halo_ring_commit_write once the bytes are actually there — until then the
 * consumer cannot see them, because `head` has not moved.
 *
 * Producer-thread only, like halo_ring_write. */
static inline size_t halo_ring_writable(const halo_ring_t *r, uint8_t **out) {
    size_t free_space = halo_ring_free_space(r);
    if (free_space == 0) { *out = NULL; return 0; }
    size_t head = atomic_load_explicit((_Atomic size_t *)&r->head, memory_order_relaxed);
    size_t idx = head & r->mask;
    size_t contiguous = r->capacity - idx;
    if (contiguous > free_space) contiguous = free_space;
    *out = r->buf + idx;
    return contiguous;
}

static inline void halo_ring_commit_write(halo_ring_t *r, size_t len) {
    size_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    atomic_store_explicit(&r->head, head + len, memory_order_release);
}

/* NOTE — there is deliberately no consumer-side mirror of the above.
 * Handing snd_pcm_writei a pointer into the ring would save the other copy,
 * but the pointer would have to stay valid across a blocking write of up to
 * a whole ALSA period, and halo_ring_clear() (FLUSH) runs on the *network*
 * thread. It would then be free to refill exactly the region ALSA is still
 * reading, producing a burst of wrong samples. Copying out first makes the
 * consumer immune to that by construction. Revisit only together with real
 * writer-quiescing coordination around FLUSH — not as a standalone
 * optimisation. */

/* Returns bytes actually read. */
static inline size_t halo_ring_read(halo_ring_t *r, uint8_t *out, size_t len) {
    size_t used = halo_ring_used(r);
    size_t to_read = len < used ? len : used;
    if (to_read == 0) return 0;

    size_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    size_t idx = tail & r->mask;
    size_t first_chunk = r->capacity - idx;
    if (first_chunk > to_read) first_chunk = to_read;

    memcpy(out, r->buf + idx, first_chunk);
    if (to_read > first_chunk) {
        memcpy(out + first_chunk, r->buf, to_read - first_chunk);
    }

    atomic_store_explicit(&r->tail, tail + to_read, memory_order_release);
    return to_read;
}

/* Discard everything currently buffered (used on FLUSH). Only safe to call
 * when producer and consumer are both quiesced for this instant — in our
 * daemon this is called from the main thread while the ALSA writer thread
 * is signaled to pause first. */
static inline void halo_ring_clear(halo_ring_t *r) {
    size_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    atomic_store_explicit(&r->tail, head, memory_order_release);
}

#endif /* HALO_RING_BUFFER_H */
