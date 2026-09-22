// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Slick Quant
//
// The types the hot path needs, and nothing else.
//
// Split out of types.hpp so <slick/perfmon.hpp> can stay cheap to include:
// the collector's config and its reporting structs need <chrono>, <string> and
// <vector>, and instrumented code has no business paying for those. Everything
// here is a scalar, a constant or a 16-byte POD, so this header costs four
// freestanding standard headers and no allocation-aware machinery at all.
// <atomic> is the fourth and is not a new cost: perfmon.hpp includes it for the
// hot state, and queue.hpp gets it from <slick/queue.hpp>.

#pragma once

#include <slick/perfmon/config.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace slick::perfmon {

/// A measurement point. The full 32-bit range is the user's to partition;
/// slick-perfmon never allocates one.
using point_id = uint32_t;

/// (seq << 8) | step. Packed rather than two fields because the step and the
/// correlation id are always written and read together.
using event_id = uint32_t;

/// Correlation id distinguishing concurrently open spans on one point. Only the
/// low 24 bits survive the pack.
using span_seq = uint32_t;

inline constexpr uint8_t  kBeginStep = 0x00;
inline constexpr uint8_t  kEndStep   = 0xFF;
inline constexpr span_seq kNoSeq     = 0;
inline constexpr span_seq kMaxSeq    = 0x00FFFFFFu;

/// Synthetic step pair identifying the begin -> end total of a span. 0x0000
/// cannot collide with a real transition because a real one always ends on a
/// step that is not kBeginStep.
inline constexpr uint16_t kTotalStagePair = 0x0000;

constexpr event_id make_event(uint8_t step, span_seq seq = kNoSeq) noexcept {
    return (static_cast<event_id>(seq & kMaxSeq) << 8) | step;
}

constexpr uint8_t step_of(event_id e) noexcept { return static_cast<uint8_t>(e & 0xFFu); }

constexpr span_seq seq_of(event_id e) noexcept { return e >> 8; }

/**
 * @brief One timestamped event. Exactly 16 bytes, trivially copyable, and
 *        layout-stable across processes so it can live in shared memory.
 *
 * The timestamp stays a raw 64-bit TSC read: no delta, no unit conversion and
 * no saturation happen on the instrumented thread. That is what removes the
 * ~1.4 s ceiling a 32-bit cycle delta would have imposed, and it is why the
 * hot path has no arithmetic in it at all.
 *
 * The fields are plain scalars, but a sample that lives in the ring is only
 * ever touched through store() and load() below. See those for why.
 */
struct alignas(16) sample {
    uint64_t timestamp;   ///< raw TSC
    point_id point;       ///< the user's measurement point
    event_id event;       ///< (seq << 8) | step

    /**
     * @brief Write this sample into ring memory.
     *
     * The stores are relaxed atomics rather than plain assignments, and that is
     * a correctness requirement rather than a style choice. The ring overwrites
     * rather than blocking, so a producer that laps the collector writes this
     * slot while the collector is reading it - by design, and counted as
     * `dropped`. With plain stores that overlap is a data race, which is
     * undefined behaviour and which ThreadSanitizer rightly fails the build
     * over. Relaxed atomics make it a *defined* overlap whose only consequence
     * is a sample mixing two generations of the slot - exactly the corruption
     * `dropped` already exists to warn about.
     *
     * Relaxed is the whole point: no fence, no ordering, no lock. On x86-64 and
     * AArch64 each of these lowers to the same store instruction the plain
     * assignment emitted. The one thing given up is store merging - the
     * compiler may no longer fuse `point` and `event` into a single 8-byte
     * write - and that does not show up in the benchmark: five interleaved
     * before/after runs of bench/perfmon_bench put a single stamp at 223.5 vs
     * 216.6 min cycles and a begin/end pair at 222.4 vs 217.0 per stamp, which
     * is inside the ~10% run-to-run spread the benchmark itself warns about.
     *
     * Ordering between the fields and the slot's readiness is not this
     * function's job: queue::publish() releases the slot afterwards, and the
     * collector acquires it, so a reader that is *not* lapping us sees all
     * three fields.
     */
    SLICK_PERFMON_FORCE_INLINE void store(uint64_t ts, point_id p, event_id e) noexcept {
        std::atomic_ref<uint64_t>{timestamp}.store(ts, std::memory_order_relaxed);
        std::atomic_ref<point_id>{point}.store(p, std::memory_order_relaxed);
        std::atomic_ref<event_id>{event}.store(e, std::memory_order_relaxed);
    }

    /**
     * @brief Take one snapshot of a sample sitting in ring memory.
     *
     * The counterpart to store(), and it must be used for the same reason: the
     * loads race with a lapping producer and have to be atomic to be defined.
     *
     * It returns by value instead of handing out a reference because a lapping
     * producer can change the slot *between* two reads of it. The pairer reads
     * `event`, `point` and `timestamp` several times each; against live ring
     * memory that could hand it a step from one generation and a timestamp from
     * another, so a span would be closed against a begin it never had. One
     * snapshot up front costs 16 bytes of stack and makes every later read
     * agree with itself.
     *
     * Non-const because std::atomic_ref does not bind to a const object. The
     * slot really is mutable - that is the whole problem being solved here.
     */
    SLICK_PERFMON_FORCE_INLINE sample load() noexcept {
        sample out;  // every field is assigned below
        out.timestamp = std::atomic_ref<uint64_t>{timestamp}.load(std::memory_order_relaxed);
        out.point     = std::atomic_ref<point_id>{point}.load(std::memory_order_relaxed);
        out.event     = std::atomic_ref<event_id>{event}.load(std::memory_order_relaxed);
        return out;
    }
};

static_assert(sizeof(sample) == 16, "sample must stay one quarter of a cache line");
static_assert(std::is_trivially_copyable_v<sample>, "sample crosses a shared-memory boundary");
static_assert(std::is_aggregate_v<sample>, "sample is brace-initialised across the tests");
static_assert(offsetof(sample, point) + sizeof(point_id) == offsetof(sample, event),
              "point and event must stay adjacent so a snapshot is one cache line");

// store() and load() are only lock-free - only single instructions - if every
// field is naturally aligned for its own atomic_ref. alignas(16) on the struct
// plus the declaration order gives that, but nothing in the language guarantees
// it survives a field being reordered or widened, and the failure mode would be
// a silent collapse to a lock table on the hottest path in the library.
static_assert(alignof(sample) >= std::atomic_ref<uint64_t>::required_alignment &&
                  offsetof(sample, timestamp) % std::atomic_ref<uint64_t>::required_alignment == 0,
              "sample::timestamp must be aligned for a lock-free atomic_ref");
static_assert(offsetof(sample, point) % std::atomic_ref<point_id>::required_alignment == 0,
              "sample::point must be aligned for a lock-free atomic_ref");
static_assert(offsetof(sample, event) % std::atomic_ref<event_id>::required_alignment == 0,
              "sample::event must be aligned for a lock-free atomic_ref");
static_assert(std::atomic_ref<uint64_t>::is_always_lock_free &&
                  std::atomic_ref<point_id>::is_always_lock_free,
              "a stamp must not take a lock");

/// Any enum or integral convertible to point_id, so call sites need no cast.
template <typename T>
concept point_like = std::is_enum_v<T> || std::is_integral_v<T>;

template <point_like T>
constexpr point_id to_point(T p) noexcept {
    if constexpr (std::is_enum_v<T>) {
        return static_cast<point_id>(static_cast<std::underlying_type_t<T>>(p));
    } else {
        return static_cast<point_id>(p);
    }
}

}  // namespace slick::perfmon
