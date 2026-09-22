// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Slick Quant
//
// Helpers shared by the test executables.

#pragma once

#include <slick/perfmon/pairing.hpp>
#include <slick/perfmon/types.hpp>

#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace slick::perfmon::test {

/// Build one event the way the hot path would, but with a timestamp we choose.
/// Driving the Pairer directly keeps the pairing tests free of threads, files
/// and wall-clock timing - every anomaly path can be provoked exactly.
inline sample ev(point_id p, uint8_t step, uint64_t ts, span_seq seq = kNoSeq) {
    sample s{};
    s.timestamp = ts;
    s.point     = p;
    s.event     = make_event(step, seq);
    return s;
}

inline Stage* find_stage(Pairer& pairer, point_id p, uint8_t from, uint8_t to) {
    Stage* found = nullptr;
    pairer.for_each_stage([&](uint64_t, Stage& g) {
        if (g.point == p && g.from == from && g.to == to) {
            found = &g;
        }
    });
    return found;
}

inline Stage* find_total(Pairer& pairer, point_id p) {
    return find_stage(pairer, p, kBeginStep, kBeginStep);
}

inline size_t stage_count(Pairer& pairer) {
    size_t n = 0;
    pairer.for_each_stage([&](uint64_t, Stage&) { ++n; });
    return n;
}

/// A config with everything that touches the filesystem or the clock turned
/// down, for tests that only care about pairing and statistics.
inline config quiet_config() {
    config cfg;
    cfg.path              = "";
    cfg.summary_path      = "";
    cfg.calibration_time  = std::chrono::milliseconds(2);
    cfg.flush_interval    = std::chrono::milliseconds(10);
    cfg.poll_interval     = std::chrono::milliseconds(1);
    cfg.subtract_overhead = false;  // tests assert on exact deltas
    return cfg;
}

/// Unique-per-run shared-memory name, so a crashed earlier run cannot leave a
/// stale segment that makes the next one fail mysteriously.
/// A shm name unique to this run that fits the platform's limit.
///
/// Built here rather than spelled out at each call site because the budget is
/// far tighter than it looks: macOS caps a POSIX shm name at 31 bytes including
/// the leading '/', and a collector session needs both <name> and <name>.meta,
/// which leaves 25 against 249 on Linux and Windows. Names that fit everywhere
/// else fail only on macOS, and only sometimes - a decimal random suffix varies
/// in width, so the same test passes or fails on the draw. Sizing against the
/// same constant start() validates, with a fixed-width suffix, removes both.
inline std::string unique_shm_name(const char* tag) {
    std::random_device                      rd;
    std::uniform_int_distribution<uint32_t> dist;

    constexpr size_t kUniqueChars = 8;  // one uint32 in hex, always this wide

    // The truncation below subtracts from the budget, so a budget smaller than
    // the parts would wrap rather than shorten anything.
    static_assert(SLICK_PERFMON_SHM_NAME_MAX >= 16,
                  "shm name budget too small to hold a tag and a unique suffix");

    std::string name = "spm_";
    name += tag;
    if (name.size() + 1 + kUniqueChars > SLICK_PERFMON_SHM_NAME_MAX) {
        name.resize(SLICK_PERFMON_SHM_NAME_MAX - 1 - kUniqueChars);
    }

    static constexpr char kHex[] = "0123456789abcdef";
    const uint32_t        r      = dist(rd);
    name += '_';
    for (int shift = 28; shift >= 0; shift -= 4) {
        name += kHex[(r >> shift) & 0xFu];
    }
    return name;
}

}  // namespace slick::perfmon::test
