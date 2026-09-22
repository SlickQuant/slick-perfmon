// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Slick Quant
//
// The smallest useful session: local mode, one point, begin and end in
// different functions.

#include "perf_points.hpp"

#include <slick/perfmon/collector.hpp>

#include <chrono>
#include <cstdio>
#include <iostream>
#include <thread>

using namespace slick::perfmon;
using demo::perf;

namespace {

void start_work() {
    // begin() and end() need not share a scope - the backend pairs them by
    // (point, seq), not by any state the caller has to carry.
    begin(perf::book_update);
}

void finish_work() {
    end(perf::book_update);
}

void do_something(int n) {
    volatile int sink = 0;
    for (int i = 0; i < n; ++i) {
        sink += i * i;
    }
    (void)sink;
}

}  // namespace

int main() {
    config cfg       = demo::base_config();
    cfg.path         = "basic_example.csv";
    cfg.summary_path = "";  // printed to stdout below instead
    cfg.flush_interval = std::chrono::milliseconds(200);

    Collector& perfmon = Collector::instance();
    if (!perfmon.start(cfg)) {
        std::puts("slick-perfmon did not start (check SLICK_PERFMON_DISABLE)");
        return 1;
    }

    for (int i = 0; i < 20000; ++i) {
        start_work();
        do_something(i % 200);
        finish_work();
    }

    // A scoped guard for the cases that do fit in one scope.
    for (int i = 0; i < 5000; ++i) {
        SLICK_PERFMON_SCOPE(perf::tick_to_trade);
        do_something(50);
    }

    perfmon.dump_summary(std::cout);

    const stats s = perfmon.snapshot_total(to_point(perf::book_update));
    std::printf("\nbook_update: %llu spans, mean %.1f ns, p99 %.1f ns\n",
                static_cast<unsigned long long>(s.count), s.mean,
                s.percentiles.size() > 2 ? s.percentiles[2] : 0.0);
    std::printf("measurement floor: %llu cycles per stamp\n",
                static_cast<unsigned long long>(perfmon.overhead_cycles()));

    perfmon.shutdown();
    std::puts("\nwrote basic_example.csv");
    return 0;
}
