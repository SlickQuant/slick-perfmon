// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Slick Quant
//
// The production shape: this process only stamps. Pairing, statistics and file
// I/O all happen in slick_perfmon_collector, in another process, so nothing
// here competes with the measured threads for cache or CPU - and if this
// program crashes, everything published up to that moment is already in the
// segment and still gets written out.
//
//   1) slick_perfmon_collector --shm demo_perf --csv demo_perf.csv
//   2) shm_producer_example demo_perf

#include "perf_points.hpp"

#include <slick/perfmon/collector.hpp>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using namespace slick::perfmon;
using demo::perf;

namespace {

void burn(int iterations) {
    volatile double sink = 0.0;
    for (int i = 0; i < iterations; ++i) {
        sink += static_cast<double>(i) * 1.5;
    }
    (void)sink;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string shm     = argc > 1 ? argv[1] : "demo_perf";
    const int         seconds = argc > 2 ? std::atoi(argv[2]) : 10;

    config cfg   = demo::base_config();
    cfg.run_mode = mode::shared_producer;
    cfg.shm_name = shm;
    // A producer writes no files. The collector owns every output.
    cfg.path         = "";
    cfg.summary_path = "";

    Collector& perfmon = Collector::instance();
    if (!perfmon.start(cfg)) {
        // The safe default: with no collector attached, an instrumented binary
        // costs nothing and creates nothing. Note this is not an error - the
        // program carries on, just unmeasured.
        std::printf("no collector on '%s' - running unmeasured.\n", shm.c_str());
        std::printf("start one with: slick_perfmon_collector --shm %s\n", shm.c_str());
    } else {
        std::printf("attached to '%s', stamping for %d seconds\n", shm.c_str(), seconds);
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    uint64_t   spans    = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        for (int i = 0; i < 500; ++i) {
            const span_seq seq = static_cast<span_seq>(spans + 1);
            begin(perf::tick_to_trade, seq);
            burn(i % 128);
            step(perf::tick_to_trade, demo::kDecode, seq);
            burn(i % 64);
            end(perf::tick_to_trade, seq);
            ++spans;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::printf("stamped %llu spans. Statistics are in the collector.\n",
                static_cast<unsigned long long>(spans));

    // Detaches this process only; the segment belongs to the collector.
    perfmon.shutdown();
    return 0;
}
