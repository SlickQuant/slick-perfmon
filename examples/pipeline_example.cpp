// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Slick Quant
//
// A multi-step span: per-stage latencies and the end-to-end total come out of
// the same stamps, which is the thing the raw-event model buys you over
// measuring each stage as its own begin/end pair.

#include "perf_points.hpp"

#include <slick/perfmon/collector.hpp>

#include <chrono>
#include <cstdio>
#include <iostream>
#include <random>
#include <thread>

using namespace slick::perfmon;
using demo::perf;

namespace {

std::mt19937_64 g_rng(20260920);

void burn(int iterations) {
    volatile double sink = 0.0;
    for (int i = 0; i < iterations; ++i) {
        sink += static_cast<double>(i) * 1.000001;
    }
    (void)sink;
}

void handle_tick() {
    // Four stamps, three stages, plus the total: decode, match, send.
    begin(perf::tick_to_trade);

    burn(static_cast<int>(g_rng() % 200));
    step(perf::tick_to_trade, demo::kDecode);

    burn(static_cast<int>(g_rng() % 600));
    step(perf::tick_to_trade, demo::kMatch);

    burn(static_cast<int>(g_rng() % 300));
    step(perf::tick_to_trade, demo::kSend);

    burn(static_cast<int>(g_rng() % 100));
    end(perf::tick_to_trade);
}

}  // namespace

int main(int argc, char** argv) {
    const int ticks_wanted = argc > 1 ? std::atoi(argv[1]) : 100000;

    config cfg         = demo::base_config();
    cfg.path           = "pipeline_example.csv";
    cfg.summary_path   = "pipeline_example_summary.txt";
    cfg.flush_interval = std::chrono::milliseconds(200);
    // Five stamps per tick adds up fast. Size the ring for the burst you expect
    // rather than discovering the default was too small from the dropped
    // column - that is the main thing to get right when adopting this library.
    cfg.queue_capacity = 1u << 20;

    Collector& perfmon = Collector::instance();
    if (!perfmon.start(cfg)) {
        std::puts("slick-perfmon did not start");
        return 1;
    }

    std::printf("stamping %d ticks...\n", ticks_wanted);
    uint64_t ticks = 0;
    for (int i = 0; i < ticks_wanted; ++i) {
        handle_tick();
        ++ticks;
    }

    perfmon.flush();
    perfmon.dump_summary(std::cout);

    // The stages have to account for the whole span, with nothing unexplained.
    const point_id p       = to_point(perf::tick_to_trade);
    const stats    decode  = perfmon.snapshot(p, kBeginStep, demo::kDecode);
    const stats    match   = perfmon.snapshot(p, demo::kDecode, demo::kMatch);
    const stats    send    = perfmon.snapshot(p, demo::kMatch, demo::kSend);
    const stats    publish = perfmon.snapshot(p, demo::kSend, kEndStep);
    const stats    total   = perfmon.snapshot_total(p);

    std::printf("\n%llu ticks\n", static_cast<unsigned long long>(ticks));
    std::printf("stage means sum to %.1f ns; the total reads %.1f ns\n",
                decode.mean + match.mean + send.mean + publish.mean, total.mean);

    perfmon.shutdown();
    std::puts("wrote pipeline_example.csv and pipeline_example_summary.txt");
    return 0;
}
