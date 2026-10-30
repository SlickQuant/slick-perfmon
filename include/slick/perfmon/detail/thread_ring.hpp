// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Slick Quant
//
// One instrumenting thread's ring: single producer, single consumer, fixed
// 16-byte samples, overwrite on lap.
//
// The shared slick::queue costs a lock xadd on a counter every thread writes,
// and with two or more threads that line moves between cores on nearly every
// stamp. Here the producer owns everything it writes, so a publish is three
// relaxed stores and one release store, with no read-modify-write at all.

#pragma once

#include <slick/perfmon/stamp_types.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace slick::perfmon::detail {

/// Where one ring lives: its published head and its slots. A view, not an
/// owner. The ring table places both, on the heap or in a shared segment.
struct ring_view {
    /// Samples ever published into this ring. Only the owning thread stores
    /// it, and only ever upward, so it never resets when the ring changes owner.
    std::atomic<uint64_t>* head = nullptr;
    sample*                data = nullptr;
    uint64_t               mask = 0;  ///< capacity - 1; capacity is a power of two
};

/**
 * @brief The producer's half: the ring plus its own copy of the head.
 *
 * `next` mirrors `head` so a publish never loads the line the collector reads.
 * Lives in the thread's TLS, so it is constant-initialisable.
 */
struct ring_writer {
    sample*                data = nullptr;
    std::atomic<uint64_t>* head = nullptr;
    uint64_t               mask = 0;
    uint64_t               next = 0;

    /// Take over @p v, continuing from wherever its previous owner stopped.
    void attach(const ring_view& v) noexcept {
        data = v.data;
        head = v.head;
        mask = v.mask;
        next = v.head->load(std::memory_order_acquire);
    }

    /**
     * @brief Publish one sample.
     *
     * The release fence makes ring_read()'s after-copy check sound: a
     * reader that sees any byte of this write also sees the head store before
     * it, so it knows the slot may be mid-write. On x86 it is a compiler
     * barrier and emits nothing.
     */
    SLICK_PERFMON_FORCE_INLINE void publish(uint64_t payload, point_id p, event_id e) noexcept {
        std::atomic_thread_fence(std::memory_order_release);
        data[next & mask].store(payload, p, e);
        head->store(++next, std::memory_order_release);
    }
};

/// First index still guaranteed intact once the head reads @p h. The slot of
/// index h - capacity may be under the producer's pen right now, so a ring
/// keeps the newest capacity - 1 samples.
constexpr uint64_t first_intact(uint64_t h, uint64_t mask) noexcept {
    return h > mask ? h - mask : 0;
}

/// What ring_read() hands back: a run of consecutive samples starting at the
/// cursor it left.
struct ring_batch {
    const sample* data = nullptr;
    size_t        size = 0;
};

/**
 * @brief Copy up to @p max_n published samples from @p cursor into @p scratch.
 *
 * Copy first, validate after: the head is read again once the copy is done,
 * and anything the producer could have reached in between is discarded. On
 * return @p cursor names the first sample of the batch, having been moved past
 * whatever was overwritten, and that many samples are added to @p lost. The
 * caller advances @p cursor by however many of the batch it consumes, so
 * `consumed + lost` always equals what the producer published.
 */
inline ring_batch ring_read(const ring_view& r, uint64_t& cursor, sample* scratch, size_t max_n,
                            uint64_t& lost) noexcept {
    const uint64_t h = r.head->load(std::memory_order_acquire);
    if (h <= cursor || max_n == 0) {
        return {};
    }
    uint64_t start = cursor > first_intact(h, r.mask) ? cursor : first_intact(h, r.mask);
    const uint64_t end = h - start > max_n ? start + max_n : h;
    for (uint64_t i = start; i < end; ++i) {
        scratch[i - start] = r.data[i & r.mask].load();
    }

    std::atomic_thread_fence(std::memory_order_acquire);
    const uint64_t valid = first_intact(r.head->load(std::memory_order_relaxed), r.mask);
    const uint64_t first = valid > start ? (valid < end ? valid : end) : start;

    lost += first - cursor;
    cursor = first;
    return {scratch + (first - start), static_cast<size_t>(end - first)};
}

}  // namespace slick::perfmon::detail
