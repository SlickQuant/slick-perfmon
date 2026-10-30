// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Slick Quant
//
// The table of per-thread rings: one claim slot per ring, then the ring data.
// A view over memory someone else owns (a heap block in local mode, part of a
// shared segment otherwise), so both modes run the same claim protocol.
//
// Layout: [ring_table_header][ring_slot x count][sample x count x capacity]

#pragma once

#include <slick/perfmon/detail/thread_ring.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>

namespace slick::perfmon::detail {

/// Claim states, in the low byte of ring_slot::owner. Above it: a 24-bit tag
/// of the claiming session, then the claiming process's pid. The tag lets a
/// session hand back exactly its own rings when two sessions in one process
/// (an EXE and a DLL, say) share a table; the table hands them out, so two
/// sessions on one table never share one.
inline constexpr uint64_t kRingFree    = 0;
inline constexpr uint64_t kRingActive  = 1;
inline constexpr uint64_t kRingRetired = 2;

constexpr uint64_t ring_owner(uint32_t pid, uint32_t tag, uint64_t state) noexcept {
    return (static_cast<uint64_t>(pid) << 32) | (static_cast<uint64_t>(tag & 0xFFFFFFu) << 8) |
           state;
}
constexpr uint64_t ring_state(uint64_t owner) noexcept { return owner & 0xFFu; }
constexpr uint32_t ring_pid(uint64_t owner) noexcept { return static_cast<uint32_t>(owner >> 32); }

/**
 * @brief One ring's claim slot.
 *
 * Two lines: the head the owner stores on every publish, and the claim word,
 * written only on claim, retire and release. Kept apart so the collector
 * scanning claims never costs a producer a miss on its head.
 *
 * The claim is one word so a single CAS publishes state and pid together: a
 * producer that dies right after claiming still leaves a pid to check.
 */
struct ring_slot {
    alignas(64) std::atomic<uint64_t> head;
    alignas(64) std::atomic<uint64_t> owner;
};
static_assert(sizeof(ring_slot) == 128, "ring_slot is part of the shared layout");
static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "the ring table may be shared across processes");

struct alignas(64) ring_table_header {
    uint32_t              count;
    uint32_t              capacity;
    std::atomic<uint32_t> next_tag;  ///< next session tag; see ring_owner()
};
static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "the ring table may be shared across processes");

class ring_table {
public:
    static constexpr size_t bytes_for(uint32_t count, uint32_t capacity) noexcept {
        return sizeof(ring_table_header) + static_cast<size_t>(count) * sizeof(ring_slot) +
               static_cast<size_t>(count) * capacity * sizeof(sample);
    }

    /// Lay out a fresh table over @p mem: 64-byte aligned, bytes_for() long.
    /// Ring data is left untouched, so its pages fault in as rings are used.
    void create(void* mem, uint32_t count, uint32_t capacity) noexcept {
        auto* h     = ::new (mem) ring_table_header;
        h->count    = count;
        h->capacity = capacity;
        h->next_tag.store(1, std::memory_order_relaxed);
        adopt(h);
        for (uint32_t i = 0; i < count; ++i) {
            ring_slot* s = ::new (static_cast<void*>(slots() + i)) ring_slot;
            s->head.store(0, std::memory_order_relaxed);
            s->owner.store(kRingFree, std::memory_order_relaxed);
        }
    }

    /// Use a table someone else laid out.
    void adopt(void* mem) noexcept {
        base_ = static_cast<std::byte*>(mem);
        const auto* h = static_cast<const ring_table_header*>(mem);
        count_    = h->count;
        capacity_ = h->capacity;
    }

    /// Take over a table an ended session in this process laid out: every ring
    /// free again, every head kept. A stamp still in flight from that session
    /// then lands on its ring's count, costing at most a sample. A writer that
    /// keeps going would rewind the head under the new owner, which is why a
    /// table is not reused while another module may still stamp into it.
    /// next_tag carries on too, so no tag of the old session comes back.
    void reuse(void* mem) noexcept {
        adopt(mem);
        for (uint32_t i = 0; i < count_; ++i) {
            slot(i).owner.store(kRingFree, std::memory_order_relaxed);
        }
    }

    uint32_t count() const noexcept { return count_; }
    uint32_t capacity() const noexcept { return capacity_; }

    /// A tag no other session on this table holds (until 2^24 sessions later).
    uint32_t take_tag() const noexcept {
        return reinterpret_cast<ring_table_header*>(base_)->next_tag.fetch_add(
            1, std::memory_order_relaxed);
    }

    ring_slot& slot(uint32_t i) const noexcept { return slots()[i]; }

    ring_view view(uint32_t i) const noexcept {
        auto* data = reinterpret_cast<sample*>(reinterpret_cast<std::byte*>(slots() + count_)) +
                     static_cast<size_t>(i) * capacity_;
        return {&slot(i).head, data, static_cast<uint64_t>(capacity_) - 1};
    }

    // The three transitions of a claim word. Together they carry the head from
    // one owner to the next: the owner's retire releases its last head, the
    // collector acquires that before freeing with a release, and the next
    // claimant acquires the free. A CAS that fails changes nothing and
    // publishes nothing, so every failure order is relaxed.

    /// Claim a free ring for @p owner (an active word). -1 when all are taken.
    int claim(uint64_t owner) const noexcept {
        for (uint32_t i = 0; i < count_; ++i) {
            std::atomic<uint64_t>& w = slot(i).owner;
            uint64_t expected = kRingFree;
            if (w.load(std::memory_order_relaxed) == kRingFree &&
                w.compare_exchange_strong(expected, owner, std::memory_order_acquire,
                                          std::memory_order_relaxed)) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    /// Hand ring @p i back for the collector to drain and free, if @p owner
    /// still holds it.
    void retire(uint32_t i, uint64_t owner) const noexcept {
        uint64_t expected = owner;
        slot(i).owner.compare_exchange_strong(expected, (owner & ~uint64_t{0xFF}) | kRingRetired,
                                              std::memory_order_release, std::memory_order_relaxed);
    }

    /// Collector: free ring @p i, once drained, if it is still in state @p seen.
    /// The caller has already acquired @p seen.
    bool release(uint32_t i, uint64_t seen) const noexcept {
        return slot(i).owner.compare_exchange_strong(seen, kRingFree, std::memory_order_release,
                                                     std::memory_order_relaxed);
    }

private:
    ring_slot* slots() const noexcept {
        return reinterpret_cast<ring_slot*>(base_ + sizeof(ring_table_header));
    }

    std::byte* base_     = nullptr;
    uint32_t   count_    = 0;
    uint32_t   capacity_ = 0;
};

}  // namespace slick::perfmon::detail
