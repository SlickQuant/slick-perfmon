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

#include <slick/perfmon/detail/ring_table.hpp>
#include <slick/perfmon/detail/thread_ring.hpp>
#include <slick/perfmon/detail/tsc.hpp>
#include <slick/perfmon/queue.hpp>

#include <atomic>

namespace slick::perfmon {

namespace detail {

/**
 * @brief Everything one collector session's stamps go to.
 *
 * Owned by the Collector and never moved while published. `id` is unique per
 * session, so a thread's cached binding can tell a new session from an old
 * one even when the new one lands at a freed one's address.
 */
struct session {
    static constexpr size_t kMaxModules = 8;

    uint64_t      id      = 0;
    sample_queue* shared  = nullptr;  ///< threads past the ring cap, or every thread with rings off
    ring_table    rings;              ///< empty when config::max_thread_rings is 0
    uint64_t      owner   = 0;        ///< claim word this session writes: ring_owner(pid, tag, active)
    const void*   creator = nullptr;  ///< &g_hot of the module whose Collector made it

    /// Other modules that stamped into this session, by their &g_hot; see
    /// note_module(). One may go on stamping after the session ends, so its
    /// rings go to a later session only once each of them has moved on. Two
    /// Collectors in one module count as one writer: a pointer to the other's
    /// ended session, kept and stamped through, is not tracked.
    mutable std::atomic<const void*> modules[kMaxModules]{};
    mutable std::atomic<bool>        modules_full{false};  ///< more than kMaxModules: never reused

    bool has_module(const void* m) const noexcept {
        for (const auto& x : modules) {
            if (x.load(std::memory_order_relaxed) == m) {
                return true;
            }
        }
        return false;
    }
};

/**
 * @brief The state the hot path touches.
 *
 * `current` is both the session and the on/off switch: null means "not
 * started, or runtime-disabled", so one acquire load and one predicted branch
 * cover both. `live` is the session whether enabled or not; only binding and
 * thread exit read it.
 *
 * Cache-aligned because every instrumented thread reads it on every stamp and
 * it must not share a line with anything that gets written. Per module (EXE,
 * DLL); see attach().
 */
struct SLICK_PERFMON_CACHE_ALIGNED hot_state {
    std::atomic<const session*> current;
    std::atomic<const session*> live;
};

/// Namespace-scope and value-initialised, so constant-initialised: no guard on
/// the hottest load in the library.
inline hot_state g_hot{};

/// Point this module's hot path at @p s; null detaches it.
inline void bind_module(const session* s) noexcept {
    g_hot.live.store(s, std::memory_order_release);
    g_hot.current.store(s, std::memory_order_release);
}

/// Record that this module stamps into @p s, when another module made it.
/// Called from attach() and bind_thread(), so no way of stamping skips it.
inline void note_module(const session& s) noexcept {
    const void* self = &g_hot;
    if (self == s.creator || s.has_module(self)) {
        return;
    }
    for (auto& m : s.modules) {
        const void* expected = nullptr;
        if (m.compare_exchange_strong(expected, self, std::memory_order_relaxed) ||
            expected == self) {
            return;
        }
    }
    s.modules_full.store(true, std::memory_order_relaxed);
}

/// What this thread publishes through, cached per module. Trivially
/// destructible and constinit, so reading it costs no TLS init guard.
struct thread_state {
    uint64_t    session_id = 0;  ///< session `ring` belongs to; 0 = unbound
    ring_writer ring{};          ///< data == nullptr: publish into session::shared
};
inline constinit thread_local thread_state tls_state{};

/// Retires this thread's ring when the thread exits. Apart from tls_state
/// because it has a destructor, which makes touching it cost a guard; only
/// bind_thread() does.
struct thread_exit_hook {
    uint64_t session_id = 0;
    uint32_t ring       = 0;
    ~thread_exit_hook();
};
inline thread_local thread_exit_hook tls_exit_hook;

inline thread_exit_hook::~thread_exit_hook() {
    // Only through the module's live session: one that has ended may have
    // freed its table, and a table nobody drains needs no retiring.
    const session* s = g_hot.live.load(std::memory_order_acquire);
    if (session_id != 0 && s != nullptr && s->id == session_id) {
        s->rings.retire(ring, s->owner);
    }
}

/// Slow path: bind this thread to @p s, claiming a ring if one is free. With
/// none free the thread uses the shared ring for the rest of the session.
SLICK_PERFMON_NO_INLINE inline void bind_thread(thread_state& t, const session& s) noexcept {
    note_module(s);
    t.session_id = s.id;
    t.ring       = ring_writer{};
    const int i  = s.rings.claim(s.owner);
    if (i < 0) {
        return;
    }
    thread_exit_hook& h = tls_exit_hook;
    h.session_id        = s.id;
    h.ring              = static_cast<uint32_t>(i);
    t.ring.attach(s.rings.view(static_cast<uint32_t>(i)));
}

/// This thread's ring in @p s, or null for the shared one.
SLICK_PERFMON_FORCE_INLINE ring_writer* bound_writer(const session& s) noexcept {
    thread_state& t = tls_state;
    if (t.session_id != s.id) [[unlikely]] {
        bind_thread(t, s);
    }
    return t.ring.data != nullptr ? &t.ring : nullptr;
}

/// Hand one record to the shared ring: reserve a slot, fill it, release it.
SLICK_PERFMON_FORCE_INLINE void publish_into(sample_queue& q, uint64_t payload, point_id p,
                                             event_id e) noexcept {
    // reserve(1) cannot throw: it only rejects n == 0 or n > capacity, and the
    // collector validates capacity >= 2 before any ring reaches this function.
    const uint64_t slot = q.reserve();
    // Relaxed atomic stores, not plain ones: the ring overwrites, so this write
    // can overlap the collector reading the same slot. See sample::store().
    q[slot]->store(payload, p, e);
    q.publish(slot);
}

/// What a stamp and a value sample have in common: everything but where the
/// payload comes from.
SLICK_PERFMON_FORCE_INLINE void publish(ring_writer* w, const session& s, uint64_t payload,
                                        point_id p, event_id e) noexcept {
    if (w != nullptr) [[likely]] {
        w->publish(payload, p, e);
    } else {
        publish_into(*s.shared, payload, p, e);
    }
}

/// The stamp itself, against an explicit ring. Overhead calibration drives
/// these against a private ring, so it measures the real code rather than a
/// copy that would drift, and leaves the live rings alone.
template <bool IsBegin>
SLICK_PERFMON_FORCE_INLINE void stamp_into(sample_queue& q, point_id p, event_id e) noexcept {
    publish_into(q, read_tsc<IsBegin>(), p, e);
}

template <bool IsBegin>
SLICK_PERFMON_FORCE_INLINE void stamp_into(ring_writer& w, point_id p, event_id e) noexcept {
    w.publish(read_tsc<IsBegin>(), p, e);
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
 *
 * Finding this thread's ring goes on whichever side of the timestamp is
 * outside the interval: before it when opening a span, after it when closing.
 */
template <bool IsBegin>
SLICK_PERFMON_FORCE_INLINE void stamp(point_id p, event_id e) noexcept {
    const detail::session* s = detail::g_hot.current.load(std::memory_order_acquire);
    if (s == nullptr) [[unlikely]] {
        return;
    }
    if constexpr (IsBegin) {
        detail::ring_writer* w = detail::bound_writer(*s);
        detail::publish(w, *s, detail::read_tsc<true>(), p, e);
    } else {
        const uint64_t ts = detail::read_tsc<false>();
        detail::publish(detail::bound_writer(*s), *s, ts, p, e);
    }
}

/// Open a span. Pass `seq` when spans on this point can overlap.
template <point_like T>
SLICK_PERFMON_FORCE_INLINE void begin(T p, span_seq seq = kNoSeq) noexcept {
    stamp<true>(to_point(p), make_event(kBeginStep, seq));
}

/// Mark an intermediate boundary. Valid steps are 0x01..kMaxSpanStep (0xFD);
/// 0xFE is kValueStep and would be read back as a value sample.
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
 * @brief Record one value on a point: a queue depth, a batch size, a byte
 *        count - a number that is a measurement in itself.
 *
 * There is no begin and no end. Every call is one complete sample, and the
 * backend reports the distribution of the values a point was given: count,
 * min, mean, stddev, percentiles and max, exactly as it does for a latency but
 * in the caller's own unit, with no cycles-to-time conversion applied.
 *
 * Cheaper than a stamp, because the one expensive thing a stamp does is read a
 * serialized TSC and a value has no use for one: this is the null check and
 * the publish, with @p v stored where the timestamp would have gone.
 *
 * The value is an unsigned 64-bit integer. A negative number converts the way
 * the language says it does and is reported as a very large one.
 *
 * A point takes values or spans, not both - the two would share a name in the
 * output while meaning unrelated things.
 */
template <point_like T>
SLICK_PERFMON_FORCE_INLINE void value(T p, uint64_t v) noexcept {
    const detail::session* s = detail::g_hot.current.load(std::memory_order_acquire);
    if (s == nullptr) [[unlikely]] {
        return;
    }
    detail::publish(detail::bound_writer(*s), *s, v, to_point(p), make_event(kValueStep));
}

/**
 * @brief Point this module's hot path at a session owned by another module.
 *
 * Only needed in `local` mode when a DLL must report into the host's collector
 * (`attach(host.session())`): the hot state above is per-module, so a DLL that
 * never calls start() or attach() simply records nothing. The shared modes need
 * none of this: both modules attach the same segment and pair through it.
 *
 * Call it again after every host restart. Until then this module stamps into
 * the ended session, where nothing reads it, so it records nothing, and the
 * host holds that session's thread rings back from reuse. Once it has attached
 * to a later session, they go back into use.
 */
inline void attach(const detail::session* s) noexcept {
    if (s != nullptr) {
        detail::note_module(*s);
    }
    detail::bind_module(s);
}

/**
 * @brief Claim this thread's ring now and fault its pages in.
 *
 * Optional. A thread otherwise claims its ring on its first stamp, and its
 * first lap through the ring takes page faults, both inside whatever that stamp
 * measures. Call it at thread start to move that cost out. Collector::start()
 * does it for its own thread. A no-op when no session is attached or rings are
 * off.
 */
inline void prepare_thread() noexcept {
    const detail::session* s = detail::g_hot.live.load(std::memory_order_acquire);
    if (s == nullptr) {
        return;
    }
    detail::ring_writer* w = detail::bound_writer(*s);
    if (w == nullptr) {
        return;
    }
    // A write that changes nothing, so a collector still reading an earlier
    // owner's samples here sees no difference.
    constexpr uint64_t kPerPage = 4096 / sizeof(sample);
    for (uint64_t i = 0; i <= w->mask; i += kPerPage) {
        std::atomic_ref<uint64_t>{w->data[i].timestamp}.fetch_add(0, std::memory_order_relaxed);
    }
}

}  // namespace slick::perfmon

#else  // !SLICK_PERFMON_ENABLED

namespace slick::perfmon {

// Forward declaration only: a disabled build must not need slick-queue at all,
// so the session stays incomplete.
namespace detail {
struct session;
}

template <bool IsBegin>
SLICK_PERFMON_FORCE_INLINE void stamp(point_id, event_id) noexcept {}

template <point_like T>
SLICK_PERFMON_FORCE_INLINE void begin(T, span_seq = kNoSeq) noexcept {}

template <point_like T>
SLICK_PERFMON_FORCE_INLINE void step(T, uint8_t, span_seq = kNoSeq) noexcept {}

template <point_like T>
SLICK_PERFMON_FORCE_INLINE void end(T, span_seq = kNoSeq) noexcept {}

template <point_like T>
SLICK_PERFMON_FORCE_INLINE void value(T, uint64_t) noexcept {}

inline void attach(const detail::session*) noexcept {}

inline void prepare_thread() noexcept {}

}  // namespace slick::perfmon

#endif  // SLICK_PERFMON_ENABLED

#include <slick/perfmon/scoped.hpp>
