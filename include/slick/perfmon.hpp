// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Slick Quant
//
// slick-perfmon hot path.
//
// This header is what instrumented code includes, so it stays deliberately
// small: no <fstream>, no <ostream>, no <vector>, no <string>, no <chrono>, no
// histogram, no pairing. Everything that turns stamps into statistics lives in
// <slick/perfmon/collector.hpp>, which only main(), the collector executable
// and the tests need to see.
//
// That is why the types arrive as <slick/perfmon/stamp_types.hpp> rather than
// as types.hpp: config and stats need <chrono>, <string> and <vector>, and a
// translation unit that only stamps should not pay for them. Code that builds
// a config - typically the one file that calls start() - includes
// <slick/perfmon/types.hpp>, or gets it transitively from collector.hpp.

#pragma once

#include <slick/perfmon/config.hpp>
#include <slick/perfmon/stamp_types.hpp>

#if SLICK_PERFMON_ENABLED

#include <slick/perfmon/detail/tsc.hpp>
#include <slick/perfmon/queue.hpp>

#include <atomic>

namespace slick::perfmon {

namespace detail {

/**
 * @brief The one piece of state the hot path touches.
 *
 * A single pointer serves as both the ring and the on/off switch: null means
 * "not started, or runtime-disabled", so one acquire load and one
 * predicted-not-taken branch cover both cases. A separate enabled flag would
 * have cost a second load for nothing.
 *
 * Cache-aligned because every instrumented thread reads it on every stamp and
 * it must not share a line with anything that gets written.
 *
 * It is a function-local static rather than an inline variable so that each
 * loaded module (EXE, DLL) gets a well-defined instance; see attach().
 */
struct SLICK_PERFMON_CACHE_ALIGNED hot_state {
    std::atomic<sample_queue*> ring;
};

/// Namespace-scope and value-initialised, so it is constant-initialised before
/// the program starts. A function-local static would be equivalent in meaning
/// but not in cost: it carries a thread-safe-initialisation guard that is
/// checked on every call, and this is the single hottest load in the library.
inline hot_state g_hot{};

SLICK_PERFMON_FORCE_INLINE std::atomic<sample_queue*>& hot_ring() noexcept {
    return g_hot.ring;
}

/// The publish itself, against an explicit ring.
///
/// Factored out so overhead calibration can drive the real code path against a
/// private ring instead of re-implementing it - a calibration that measured a
/// copy of this function would drift away from it at the first change, and it
/// must not pollute the live ring with its own samples.
template <bool IsBegin>
SLICK_PERFMON_FORCE_INLINE void stamp_into(sample_queue& q, point_id p, event_id e) noexcept {
    const uint64_t t = read_tsc<IsBegin>();

    // reserve(1) cannot throw: it only rejects n == 0 or n > capacity, and the
    // collector validates capacity >= 2 before any ring reaches this function.
    const uint64_t slot = q.reserve();
    sample* s = q[slot];
    s->timestamp = t;
    s->point     = p;
    s->event     = e;
    q.publish(slot);
}

}  // namespace detail

/**
 * @brief Publish one timestamped event.
 *
 * @tparam IsBegin selects RDTSC (opening a span) over RDTSCP (closing a stage).
 *         A template parameter rather than a test on the step value: when the
 *         caller passes a runtime `seq` the event_id is not a constant, so a
 *         `step_of(e) == kBeginStep` branch would survive into the hot path.
 *
 * The enabled check precedes the timestamp deliberately. Putting it after would
 * keep those one or two cycles out of the measured interval, but would burn a
 * full RDTSC on every stamp while disabled, which defeats the point of having a
 * runtime switch. Overhead calibration measures back-to-back stamps and so
 * already accounts for the check.
 */
template <bool IsBegin>
SLICK_PERFMON_FORCE_INLINE void stamp(point_id p, event_id e) noexcept {
    sample_queue* q = detail::hot_ring().load(std::memory_order_acquire);
    if (q == nullptr) [[unlikely]] {
        return;
    }
    detail::stamp_into<IsBegin>(*q, p, e);
}

/// Open a span. Pass `seq` when spans on this point can overlap.
template <point_like T>
SLICK_PERFMON_FORCE_INLINE void begin(T p, span_seq seq = kNoSeq) noexcept {
    stamp<true>(to_point(p), make_event(kBeginStep, seq));
}

/// Mark an intermediate boundary. Valid steps are 0x01..0xFE.
template <point_like T>
SLICK_PERFMON_FORCE_INLINE void step(T p, uint8_t n, span_seq seq = kNoSeq) noexcept {
    stamp<false>(to_point(p), make_event(n, seq));
}

/// Close a span.
template <point_like T>
SLICK_PERFMON_FORCE_INLINE void end(T p, span_seq seq = kNoSeq) noexcept {
    stamp<false>(to_point(p), make_event(kEndStep, seq));
}

/**
 * @brief Point this module's hot path at a ring owned by another module.
 *
 * Only needed in `local` mode when a DLL must report into the host's collector:
 * the hot state above is per-module, so a DLL that never calls start() or
 * attach() simply records nothing. The shared modes need none of this - both
 * modules attach the same segment and pair through it naturally.
 */
inline void attach(sample_queue* ring) noexcept {
    detail::hot_ring().store(ring, std::memory_order_release);
}

}  // namespace slick::perfmon

#else  // !SLICK_PERFMON_ENABLED

namespace slick::perfmon {

// Forward declaration only: a disabled build must not need slick-queue at all,
// so queue.hpp is not included above and sample_queue stays incomplete.
class sample_queue;

template <bool IsBegin>
SLICK_PERFMON_FORCE_INLINE void stamp(point_id, event_id) noexcept {}

template <point_like T>
SLICK_PERFMON_FORCE_INLINE void begin(T, span_seq = kNoSeq) noexcept {}

template <point_like T>
SLICK_PERFMON_FORCE_INLINE void step(T, uint8_t, span_seq = kNoSeq) noexcept {}

template <point_like T>
SLICK_PERFMON_FORCE_INLINE void end(T, span_seq = kNoSeq) noexcept {}

inline void attach(sample_queue*) noexcept {}

}  // namespace slick::perfmon

#endif  // SLICK_PERFMON_ENABLED

#include <slick/perfmon/scoped.hpp>
