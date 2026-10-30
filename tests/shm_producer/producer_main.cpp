// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Slick Quant
//
// A real second process for the shared-memory tests. Attaching the same
// segment from another address space is the thing being tested, and nothing
// inside one process can stand in for it.
//
// usage: producer <shm_name> <point> <spans> [--leave-open] [--end-only]
//                 [--steps] [--seq-base <n>] [--threads <n>] [--hang]

#include <slick/perfmon.hpp>
#include <slick/perfmon/collector.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr uint8_t kMid = 2;
}

int main(int argc, char** argv) {
    using namespace slick::perfmon;

    if (argc < 4) {
        std::fprintf(stderr,
                     "usage: %s <shm_name> <point> <spans> [--leave-open] [--end-only] "
                     "[--steps] [--seq-base <n>] [--threads <n>] [--hang]\n",
                     argv[0]);
        return 2;
    }

    const std::string shm_name = argv[1];
    const auto        point    = static_cast<point_id>(std::strtoul(argv[2], nullptr, 10));
    const int         spans    = std::atoi(argv[3]);

    bool     leave_open = false;
    bool     end_only   = false;
    bool     with_steps = false;
    bool     hang       = false;
    int      threads    = 1;
    span_seq seq_base   = 0;
    for (int i = 4; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--leave-open") {
            leave_open = true;
        } else if (arg == "--end-only") {
            // Closes spans another process opened: a span across processes.
            end_only = true;
        } else if (arg == "--steps") {
            with_steps = true;
        } else if (arg == "--hang") {
            // Stamp, then wait to be killed: a producer that dies holding rings.
            hang = true;
        } else if (arg == "--threads" && i + 1 < argc) {
            threads = std::atoi(argv[++i]);
        } else if (arg == "--seq-base" && i + 1 < argc) {
            // Lets two producers share a point without their seqs colliding,
            // which is how the documented "merge into one row" usage works.
            seq_base = static_cast<span_seq>(std::strtoul(argv[++i], nullptr, 10));
        }
    }

    config cfg;
    cfg.run_mode = mode::shared_producer;
    cfg.shm_name = shm_name;
    // A producer writes no files - the collector owns all output.
    cfg.path         = "";
    cfg.summary_path = "";

    Collector& c = Collector::instance();
    if (!c.start(cfg)) {
        // The documented behaviour when no collector is running: report it and
        // exit cleanly rather than failing the program.
        std::fprintf(stderr, "producer: no collector on '%s'\n", shm_name.c_str());
        return 3;
    }

    // Thread t takes seqs base + t * spans + 1 .. base + (t + 1) * spans.
    auto stamp_spans = [&](int t) {
        for (int i = 0; i < spans; ++i) {
            const auto seq = static_cast<span_seq>(seq_base + t * spans + i + 1);
            if (!end_only) {
                begin(point, seq);
            }
            if (with_steps) {
                step(point, kMid, seq);
            }
            if (!leave_open) {
                end(point, seq);
            }
        }
    };
    // With --hang every thread stays alive holding its ring until the process
    // is killed, so none of them hands it back.
    std::atomic<int>         done{0};
    std::vector<std::thread> pool;
    for (int t = 1; t < threads; ++t) {
        pool.emplace_back([&, t] {
            stamp_spans(t);
            done.fetch_add(1);
            while (hang) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        });
    }
    stamp_spans(0);
    if (hang) {
        while (done.load() < threads - 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::printf("ready\n");
        std::fflush(stdout);
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    for (std::thread& th : pool) {
        th.join();
    }

    // shutdown() only detaches this process; the segment belongs to the
    // collector and outlives us.
    c.shutdown();
    return 0;
}
