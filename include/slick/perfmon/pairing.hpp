// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Slick Quant

#pragma once

#include <slick/perfmon/accumulator.hpp>
#include <slick/perfmon/detail/open_map.hpp>
#include <slick/perfmon/types.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace slick::perfmon {

/// Stage accumulators are keyed by the transition, not by the span instance:
/// `seq` identifies which span an event belongs to, and is deliberately absent
/// here so every concurrent span on a point aggregates into one row.
constexpr uint64_t stage_key(point_id p, uint8_t from, uint8_t to) noexcept {
    return (static_cast<uint64_t>(p) << 16) | (static_cast<uint32_t>(from) << 8) | to;
}

/// The synthetic begin -> end stage. (0, 0) cannot collide with a real
/// transition: a recorded transition always ends on a step that is not
/// kBeginStep, so `to` is never 0.
constexpr uint64_t total_key(point_id p) noexcept {
    return (static_cast<uint64_t>(p) << 16) | kTotalStagePair;
}

constexpr uint64_t span_key(point_id p, span_seq seq) noexcept {
    return (static_cast<uint64_t>(p) << 32) | (seq & kMaxSeq);
}

constexpr point_id span_point(uint64_t key) noexcept {
    return static_cast<point_id>(key >> 32);
}

/// Per-point anomaly counters. Every one of these means "a number you are
/// about to read may be wrong", which is why they travel with the statistics
/// rather than being logged and forgotten.
struct PointAnomalies {
    uint64_t invalid      = 0;  ///< timestamp went backwards inside a span
    uint64_t abandoned    = 0;  ///< span re-opened, evicted, or aged out
    uint64_t orphan       = 0;  ///< non-begin stamp with no open span
    uint64_t out_of_range = 0;  ///< stage table full, transition dropped
};

/// Whether a counter set has anything to report. A point with none of these
/// needs no row of its own; one with any of them needs one even if it never
/// completed a span.
constexpr bool any_anomaly(const PointAnomalies& a) noexcept {
    return a.invalid != 0 || a.abandoned != 0 || a.orphan != 0 || a.out_of_range != 0;
}

/// Counts accrued since a baseline. The counters only ever grow within a
/// session, so this is a plain subtraction - written once here because the
/// reporting path needs it for every point and for the overflow bucket, and
/// four hand-written subtractions per call site is four chances to forget one
/// when a counter is added.
constexpr PointAnomalies operator-(const PointAnomalies& a,
                                   const PointAnomalies& b) noexcept {
    return PointAnomalies{a.invalid - b.invalid, a.abandoned - b.abandoned,
                          a.orphan - b.orphan, a.out_of_range - b.out_of_range};
}

/// One measured transition. `from`/`to` are kept so a row can be labelled
/// without reversing the key.
struct Stage {
    point_id    point = 0;
    uint8_t     from  = 0;
    uint8_t     to    = 0;
    Accumulator interval;    ///< reset at every flush; drives the CSV rows
    Accumulator cumulative;  ///< lifetime; drives the shutdown summary
};

/**
 * @brief Turns the raw event stream back into spans and stage latencies.
 *
 * Events reach this in publish order on a single consumer, so no ordering has
 * to be reconstructed - only association. A span is identified by
 * (point, seq); `seq == kNoSeq` is not a special case, it is simply the single
 * bucket a point gets when its spans never overlap, so one code path serves
 * both the simple and the concurrent usage.
 */
class Pairer {
public:
    /// Clears the whole pairer, session counters included. The tables are the
    /// obvious part; `overflow_` and `peak_open_` are the easy ones to forget,
    /// and a peak or an anomaly count surviving into the next session would be
    /// reported as that session's.
    void reset(const config& cfg) {
        spans_.reset(cfg.max_open_spans);
        stages_.reset(cfg.max_stages);
        points_.reset(cfg.max_stages);  // at most one entry per distinct point
        overhead_cycles_ = 0;
        overflow_        = PointAnomalies{};
        peak_open_       = 0;

        // One scan feeds this many evictions; see evict_oldest().
        //
        // A fraction of the table rather than a constant: the scan is
        // proportional to the table, so the batch has to be too or the linear
        // growth comes straight back. An eighth measures flat at ~1.5k cycles
        // per eviction from 1k spans to 16k, against 13k and 659k for the scan
        // it replaces; a smaller divisor buys a little more, but the remaining
        // cost is the probe-and-erase floor, not the scan. Two bytes of queue
        // per span held - 32 KiB at a 16k-span table, beside the 1 MiB the
        // table itself takes - and reserved here so no eviction ever allocates.
        const size_t batch = cfg.max_open_spans / 8u;
        evict_batch_       = batch == 0 ? 1u : batch;
        evict_queue_.clear();
        evict_queue_.reserve(evict_batch_);
        evict_next_ = 0;
    }

    /// Per-stage cost of a stamp, subtracted from every transition. Measured
    /// once at startup; see Collector::calibrate_overhead().
    void set_overhead(uint64_t cycles) noexcept { overhead_cycles_ = cycles; }

    void on_event(const sample& s) {
        const uint8_t  st  = step_of(s.event);
        const span_seq sq  = seq_of(s.event);
        const uint64_t key = span_key(s.point, sq);

        if (st == kBeginStep) {
            open_begin(key, s);
            return;
        }

        OpenSpan* sp = spans_.find(key);
        if (sp == nullptr) {
            // A step or end with nothing open. Almost always a dropped begin,
            // which is why `dropped` is the first thing to check when this
            // climbs.
            ++anomalies(s.point).orphan;
            return;
        }

        if (s.timestamp < sp->last_tsc) {
            // The TSC moved backwards mid-span: the thread migrated across an
            // unsynchronised counter domain. Discard the whole span rather
            // than emit one bad stage plus a bad total from the same events.
            ++anomalies(s.point).invalid;
            spans_.erase(key);
            return;
        }

        if (st == kEndStep) {
            // A span with no intermediate steps has exactly one transition, and
            // that transition *is* the total. Recording both would put two
            // identical rows in every report for the simplest and most common
            // shape of measurement.
            if (sp->last_step != kBeginStep) {
                record(s.point, sp->last_step, st, s.timestamp - sp->last_tsc);
            }
            // The total spans every transition, so it carries every transition's
            // worth of stamp overhead. Subtracting a single stamp here would
            // leave the stages and the total disagreeing by (n-1) stamps - the
            // one thing a multi-stage report has to get right is that its parts
            // add up to its whole.
            record(s.point, kBeginStep, kBeginStep, s.timestamp - sp->start_tsc,
                   sp->transitions + 1u);
            spans_.erase(key);
        } else {
            record(s.point, sp->last_step, st, s.timestamp - sp->last_tsc);
            sp->last_tsc  = s.timestamp;
            sp->seen_tsc  = s.timestamp;
            sp->last_step = st;
            ++sp->transitions;
        }
    }

    /// Evict spans untouched for longer than `max_age_cycles`. Without this a
    /// `seq` whose end never arrives - a crashed producer, an order that was
    /// never acked - would hold its slot for the life of the process.
    size_t sweep(uint64_t now_tsc, uint64_t max_age_cycles) {
        if (max_age_cycles == 0) {
            return 0;
        }
        return spans_.erase_if([&](uint64_t key, OpenSpan& sp) {
            if (now_tsc > sp.seen_tsc && (now_tsc - sp.seen_tsc) > max_age_cycles) {
                ++anomalies(span_point(key)).abandoned;
                return true;
            }
            return false;
        });
    }

    /// Start a new interval. Called once per flush, after the rows have been
    /// built.
    ///
    /// Clearing only: the fold into `cumulative` happens while the rows are
    /// being built, because a row has to report the interval *and* the
    /// cumulative figure that already includes it. Merging here as well would
    /// count every sample twice.
    void clear_intervals() {
        stages_.for_each([](uint64_t, Stage& g) { g.interval.clear(); });
    }

    template <typename Fn>
    void for_each_stage(Fn&& fn) {
        stages_.for_each([&](uint64_t key, Stage& g) { fn(key, g); });
    }

    /// Visit the anomaly counters of every point that has any.
    ///
    /// Needed because anomalies are reported alongside stages, and a point can
    /// accumulate anomalies without ever completing a span - a producer that
    /// only ever opens spans, say. Without this the counts would exist and be
    /// invisible, which is the one thing this library must not do.
    template <typename Fn>
    void for_each_point(Fn&& fn) {
        points_.for_each([&](uint64_t key, PointAnomalies& a) {
            fn(static_cast<point_id>(key), a);
        });
    }

    /// The counters a point has accrued, or all zeroes when it has none.
    ///
    /// Separate from anomalies() because the reporting path must not insert.
    /// An entry created for a point that never had an anomaly consumes a slot
    /// in a table sized for the points that do, and once that table is full
    /// anomalies() answers with the shared overflow bucket - which on the
    /// report path would print one point's counts under another's name.
    PointAnomalies anomalies_of(point_id p) {
        const PointAnomalies* a = points_.find(p);
        return a == nullptr ? PointAnomalies{} : *a;
    }

    PointAnomalies& anomalies(point_id p) {
        if (PointAnomalies* a = points_.find(p)) {
            return *a;
        }
        if (PointAnomalies* a = points_.insert(p, PointAnomalies{})) {
            return *a;
        }
        // The point table is sized like the stage table, so this is only
        // reachable if a run uses more distinct points than max_stages. Fold
        // the counts into a shared bucket rather than losing them.
        return overflow_;
    }

    const PointAnomalies& overflow_anomalies() const noexcept { return overflow_; }

    size_t open_span_count() const noexcept { return spans_.size(); }
    size_t peak_open_spans() const noexcept { return peak_open_; }
    size_t stage_count() const noexcept { return stages_.size(); }

private:
    struct OpenSpan {
        uint64_t start_tsc = 0;  ///< the kBeginStep timestamp
        uint64_t last_tsc  = 0;  ///< the most recent event's timestamp
        uint64_t seen_tsc  = 0;  ///< same, kept separately for age sweeps
        /// Intermediate transitions recorded so far. The end adds one more, and
        /// the sum is how much stamp overhead the total has accumulated.
        uint32_t transitions = 0;
        uint8_t  last_step   = kBeginStep;
    };

    void open_begin(uint64_t key, const sample& s) {
        const OpenSpan fresh{s.timestamp, s.timestamp, s.timestamp, 0, kBeginStep};

        if (OpenSpan* sp = spans_.find(key)) {
            // Still open. Either its end was lost, its producer died, its seq
            // was reused, or two threads are sharing kNoSeq on one point.
            ++anomalies(s.point).abandoned;
            *sp = fresh;
            return;
        }

        if (spans_.insert(key, fresh) == nullptr) {
            evict_oldest();
            spans_.insert(key, fresh);
        }
        if (spans_.size() > peak_open_) {
            peak_open_ = spans_.size();
        }
    }

    /**
     * @brief Make room by dropping the least recently touched span.
     *
     * Once the table is at its cap it stays there - every begin evicts one and
     * inserts one - so this does not run once, it runs on every begin for as
     * long as the overload lasts. Finding the minimum by scanning was O(table)
     * per event, which is a feedback loop: the collector falls behind exactly
     * when it is already behind, and the extra drops open more spans.
     *
     * So one scan collects the oldest `evict_batch_` keys and later evictions
     * pop from that queue. The order is unchanged - a candidate is still the
     * oldest live span when its turn comes, because everything inserted since
     * is newer by construction, and a candidate that was touched or erased in
     * the meantime is no longer the oldest and is skipped on exactly that
     * test. Approximating the policy was not necessary to make it cheap.
     */
    void evict_oldest() {
        // Two passes at most: a refill either produces a candidate that is live
        // and untouched by construction, or the table is empty.
        for (int pass = 0; pass < 2; ++pass) {
            while (evict_next_ < evict_queue_.size()) {
                const EvictCandidate c = evict_queue_[evict_next_++];

                const OpenSpan* sp = spans_.find(c.key);
                if (sp == nullptr || sp->seen_tsc != c.seen_tsc) {
                    continue;  // erased, or touched since the scan: not oldest
                }
                ++anomalies(span_point(c.key)).abandoned;
                spans_.erase(c.key);
                return;
            }
            refill_evict_queue();
            if (evict_queue_.empty()) {
                return;  // nothing open to evict
            }
        }
    }

    /// The oldest `evict_batch_` spans, oldest first.
    ///
    /// A bounded max-heap rather than sorting the table: the batch is a small
    /// fraction of the table, so this stays one pass over the entries and
    /// O(log batch) per candidate, and it touches no memory beyond the queue
    /// reserved in reset().
    void refill_evict_queue() {
        evict_queue_.clear();
        evict_next_ = 0;

        // Max-heap on seen_tsc, so the *newest* of the candidates so far sits
        // at the front and is the one a genuinely older entry displaces.
        const auto newest_first = [](const EvictCandidate& a, const EvictCandidate& b) {
            return a.seen_tsc < b.seen_tsc;
        };

        spans_.for_each([&](uint64_t key, OpenSpan& sp) {
            const EvictCandidate c{key, sp.seen_tsc};
            if (evict_queue_.size() < evict_batch_) {
                evict_queue_.push_back(c);
                std::push_heap(evict_queue_.begin(), evict_queue_.end(), newest_first);
            } else if (c.seen_tsc < evict_queue_.front().seen_tsc) {
                std::pop_heap(evict_queue_.begin(), evict_queue_.end(), newest_first);
                evict_queue_.back() = c;
                std::push_heap(evict_queue_.begin(), evict_queue_.end(), newest_first);
            }
        });

        // sort_heap leaves the queue ascending under the same comparator, so
        // the oldest span is at the front and evictions walk forward.
        std::sort_heap(evict_queue_.begin(), evict_queue_.end(), newest_first);
    }

    /// @param transitions how many stamp intervals `cycles` spans. One for a
    ///        stage; for the synthetic total, however many transitions the span
    ///        actually made.
    void record(point_id p, uint8_t from, uint8_t to, uint64_t cycles,
                uint32_t transitions = 1) {
        // Subtracting the per-stamp overhead can only ever take a measurement
        // to zero, never below it.
        const uint64_t ov = overhead_cycles_ * transitions;
        cycles            = cycles > ov ? cycles - ov : 0;

        const uint64_t key = stage_key(p, from, to);
        Stage*         g   = stages_.find(key);
        if (g == nullptr) {
            Stage fresh;
            fresh.point = p;
            fresh.from  = from;
            fresh.to    = to;
            g = stages_.insert(key, std::move(fresh));
            if (g == nullptr) {
                ++anomalies(p).out_of_range;
                return;
            }
        }
        g->interval.add(cycles);
    }

    /// A span nominated for eviction by the last scan. `seen_tsc` is carried
    /// so a span touched since then can be recognised and passed over: it is
    /// no longer the oldest, and evicting it would report a live span as
    /// abandoned.
    struct EvictCandidate {
        uint64_t key      = 0;
        uint64_t seen_tsc = 0;
    };

    detail::open_map<OpenSpan>       spans_;
    detail::open_map<Stage>          stages_;
    detail::open_map<PointAnomalies> points_;
    PointAnomalies                   overflow_;
    uint64_t                         overhead_cycles_ = 0;
    size_t                           peak_open_       = 0;

    std::vector<EvictCandidate>      evict_queue_;
    size_t                           evict_next_  = 0;
    size_t                           evict_batch_ = 1;
};

}  // namespace slick::perfmon
