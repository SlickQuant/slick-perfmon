// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Slick Quant
//
// Eight threads measuring the *same* point. That only works because each span
// carries a correlation id: without one, the stamps interleave in the ring as
// begin, begin, end, end and no backend can say which end belongs to which
// begin. With one, they all aggregate into a single row.

#include "perf_points.hpp"

#include <slick/perfmon/collector.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <thread>
#include <vector>

using namespace slick::perfmon;
using demo::perf;

namespace {

constexpr int kThreads      = 8;
constexpr int kSpansPerTask = 20000;

void burn(int iterations) {
    volatile double sink = 0.0;
    for (int i = 0; i < iterations; ++i) {
        sink += static_cast<double>(i);
    }
    (void)sink;
}

void worker(int index) {
    // Spans never overlap within one thread, so the thread index is a
    // sufficient discriminator. In real code a natural id - an order id, a
    // message sequence - is usually already to hand and is a better choice,
    // because it also survives the span moving between threads.
    const span_seq seq = static_cast<span_seq>(index + 1);

    for (int i = 0; i < kSpansPerTask; ++i) {
        begin(perf::round_trip, seq);
        burn(i % 64);
        end(perf::round_trip, seq);
    }
}

}  // namespace

int main() {
    config cfg = demo::base_config();
    cfg.path   = "multithread_example.csv";
    cfg.summary_path = "";
    // Eight producers stamping flat out will outrun one collector thread, so
    // the ring has to be big enough to hold the whole burst: 8 x 20000 spans is
    // 320k stamps against 1M slots. Sizing this is the main thing to get right
    // when adopting the library - the alternative is finding out from the
    // dropped column.
    cfg.queue_capacity = 1u << 20;
    cfg.max_open_spans = 4096;
    cfg.flush_interval = std::chrono::milliseconds(250);

    Collector& perfmon = Collector::instance();
    if (!perfmon.start(cfg)) {
        std::puts("slick-perfmon did not start");
        return 1;
    }

    const auto wall_start = std::chrono::steady_clock::now();

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back(worker, t);
    }
    for (std::thread& th : threads) {
        th.join();
    }

    const auto wall = std::chrono::duration<double>(
                          std::chrono::steady_clock::now() - wall_start).count();

    perfmon.flush();
    perfmon.dump_summary(std::cout);

    const stats    s      = perfmon.snapshot_total(to_point(perf::round_trip));
    const uint64_t stamps = static_cast<uint64_t>(kThreads) * kSpansPerTask * 2;

    std::printf("\n%d threads on ONE point, told apart by seq\n", kThreads);
    std::printf("spans recorded : %llu of %llu\n",
                static_cast<unsigned long long>(s.count),
                static_cast<unsigned long long>(kThreads) * kSpansPerTask);
    std::printf("stamps         : %llu in %.2f s (%.2f M stamps/s)\n",
                static_cast<unsigned long long>(stamps), wall,
                static_cast<double>(stamps) / wall / 1e6);
    std::printf("peak open spans: %zu\n", perfmon.peak_open_spans());
    std::printf("dropped=%llu abandoned=%llu orphan=%llu\n",
                static_cast<unsigned long long>(s.dropped),
                static_cast<unsigned long long>(s.abandoned),
                static_cast<unsigned long long>(s.orphan));
    if (s.dropped || s.abandoned || s.orphan) {
        std::puts("^ non-zero anomalies mean the numbers above are not trustworthy");
    }

    perfmon.shutdown();
    return 0;
}
