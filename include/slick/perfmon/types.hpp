// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Slick Quant

#pragma once

#include <slick/perfmon/config.hpp>
#include <slick/perfmon/stamp_types.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace slick::perfmon {

/// Maps a (point, step) pair to a display name. A constexpr function, a
/// captureless lambda or a table lookup all satisfy it. Returning an empty
/// view means "unnamed", and the backend falls back to point_<p> / step_<s>.
/// `seq` is deliberately not passed: it identifies a span instance, not a
/// thing worth labelling.
using name_fn = std::string_view (*)(point_id, uint8_t) noexcept;

/// Where the ring and the collector live. See "Out-of-process collection" in
/// the README.
enum class mode {
    local,            ///< heap ring, collector thread in this process
    shared_producer,  ///< shm ring, no collector here
    shared_collector  ///< shm ring, this process runs the collector loop
};

enum class unit { cycles, nanoseconds, microseconds };

/**
 * @brief Everything the collector needs. Defaults are chosen so `start({})`
 *        gives a working in-process session writing perfmon.csv once a second.
 */
struct config {
    mode        run_mode = mode::local;
    std::string shm_name;            ///< required for the shared modes

    point_id point_count = 0;        ///< how far to scan when publishing names
    name_fn  name_of     = nullptr;  ///< null -> point_<p>:step_<a>-><b>

    uint32_t max_open_spans = 4096;  ///< concurrently open (point, seq) spans
    uint32_t max_stages     = 1024;  ///< distinct stage accumulators
    uint32_t max_names      = 512;   ///< control-block name slots

    std::string path         = "perfmon.csv";
    std::string summary_path = "perfmon_summary.txt";  ///< empty = no summary file

    std::chrono::milliseconds flush_interval{1000};
    std::chrono::milliseconds poll_interval{1};

    /**
     * How often the CSV stream is pushed out to the OS.
     *
     * Zero - the default - pushes at the end of every reporting interval, so a
     * crash loses at most the interval in progress. That is one write per
     * interval whatever the rows cost, and it defeats the 64 KiB buffer behind
     * the stream: at a 10 ms flush_interval it is a hundred writes a second to
     * emit a handful of rows, and the write happens on the collector thread,
     * which is not draining the ring while it blocks.
     *
     * Set it to spend that per-interval durability on throughput. Rows still
     * land in order and shutdown() still writes everything out; only the
     * window in which a hard kill loses rows gets wider. Values below
     * flush_interval change nothing, there being nothing to push between two
     * sets of rows.
     */
    std::chrono::milliseconds csv_flush_interval{0};
    std::chrono::milliseconds calibration_time{10};
    std::chrono::milliseconds stalled_sample_timeout{5000};

    uint32_t queue_capacity = 1u << 16;   ///< slots; must be a power of two

    std::vector<double> percentiles = {50.0, 90.0, 99.0, 99.9};
    unit output_unit = unit::nanoseconds;

    /**
     * Subtract the measured per-stamp cost from every transition.
     *
     * Off by default, and deliberately so. The calibration runs back-to-back
     * stamps against a private, uncontended ring, which is measurably cheaper
     * than the real thing: on the live ring the producer contends with the
     * collector for the reservation counter and the slot's cache line, and in
     * practice costs around twice the calibrated figure. Subtracting a number
     * that is systematically low leaves a residue that is invisible, whereas
     * reporting raw cycles leaves the overhead visible and lets
     * Collector::overhead_cycles() say how big the floor is.
     *
     * Turn it on when the regions being measured are short enough that the
     * stamp cost dominates, and read the result as a lower bound.
     */
    bool subtract_overhead = false;

    bool create_if_absent = false;  ///< let a producer create the segments
};

/**
 * @brief A snapshot of one stage, in config::output_unit.
 *
 * The anomaly counters are not an afterthought: in an event model a dropped or
 * unmatched stamp corrupts pairing rather than just perturbing one number, so
 * they are the primary signal that a latency figure should not be trusted.
 */
struct stats {
    uint64_t count        = 0;
    uint64_t dropped      = 0;  ///< ring overwrote unread events
    uint64_t invalid      = 0;  ///< timestamp went backwards within a span
    uint64_t stalled      = 0;  ///< ring holes abandoned by the watchdog
    uint64_t out_of_range = 0;  ///< stage table full
    uint64_t abandoned    = 0;  ///< span re-opened or evicted before its end
    uint64_t orphan       = 0;  ///< non-begin stamp with no open span

    double min    = 0.0;
    double max    = 0.0;
    double mean   = 0.0;
    double stddev = 0.0;

    std::vector<double> percentiles;

    bool invariant_tsc = true;
};

}  // namespace slick::perfmon
