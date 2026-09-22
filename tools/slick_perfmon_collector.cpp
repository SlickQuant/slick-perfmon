// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Slick Quant
//
// Standalone collector. Attaches a shared-memory ring, pairs the events it
// finds there and writes the CSV and summary, so that none of that work
// happens inside the process being measured.
//
// Labels: this binary knows nothing about a user's points, and does not need
// to - producers publish their names into the segment at startup, and the
// collector reads them back. Points nobody named render as point_<p>.

#include <slick/perfmon/collector.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

namespace {

std::atomic<bool> g_stop{false};

extern "C" void on_signal(int) { g_stop.store(true, std::memory_order_release); }

void usage(const char* argv0) {
    std::printf(
        "usage: %s --shm <name> [options]\n"
        "\n"
        "  --shm <name>        shared-memory segment to collect from (required)\n"
        "  --csv <path>        CSV time series           (default perfmon.csv)\n"
        "  --summary <path>    summary table at exit     (default perfmon_summary.txt)\n"
        "  --interval <ms>     flush interval            (default 1000)\n"
        "  --capacity <n>      ring slots, power of two  (default 65536)\n"
        "  --max-open <n>      concurrent open spans     (default 4096)\n"
        "  --max-stages <n>    distinct stage rows       (default 1024)\n"
        "  --unit <u>          ns | us | cycles          (default ns)\n"
        "  --subtract-overhead subtract the measured per-stamp cost (off by default;\n"
        "                      it is calibrated uncontended, so it reads low)\n"
        "  --duration <s>      stop after N seconds      (default: until Ctrl-C)\n"
        "\n"
        "The collector creates the segment; producers attach to it. Start this\n"
        "first - a producer that finds no segment records nothing and costs\n"
        "nothing, which is the safe default for a production binary.\n",
        argv0);
}

bool parse_unit(const char* text, slick::perfmon::unit& out) {
    using slick::perfmon::unit;
    if (std::strcmp(text, "ns") == 0) {
        out = unit::nanoseconds;
    } else if (std::strcmp(text, "us") == 0) {
        out = unit::microseconds;
    } else if (std::strcmp(text, "cycles") == 0) {
        out = unit::cycles;
    } else {
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace slick::perfmon;

    config cfg;
    cfg.run_mode = mode::shared_collector;
    int duration_seconds = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string arg  = argv[i];
        const bool        more = (i + 1) < argc;

        if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            return 0;
        } else if (arg == "--shm" && more) {
            cfg.shm_name = argv[++i];
        } else if (arg == "--csv" && more) {
            cfg.path = argv[++i];
        } else if (arg == "--summary" && more) {
            cfg.summary_path = argv[++i];
        } else if (arg == "--interval" && more) {
            cfg.flush_interval = std::chrono::milliseconds(std::atoi(argv[++i]));
        } else if (arg == "--capacity" && more) {
            cfg.queue_capacity = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (arg == "--max-open" && more) {
            cfg.max_open_spans = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (arg == "--max-stages" && more) {
            cfg.max_stages = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (arg == "--unit" && more) {
            if (!parse_unit(argv[++i], cfg.output_unit)) {
                std::fprintf(stderr, "unknown unit '%s'\n", argv[i]);
                return 2;
            }
        } else if (arg == "--subtract-overhead") {
            cfg.subtract_overhead = true;
        } else if (arg == "--duration" && more) {
            duration_seconds = std::atoi(argv[++i]);
        } else {
            std::fprintf(stderr, "unknown argument '%s'\n\n", arg.c_str());
            usage(argv[0]);
            return 2;
        }
    }

    if (cfg.shm_name.empty()) {
        std::fprintf(stderr, "--shm is required\n\n");
        usage(argv[0]);
        return 2;
    }

    Collector collector;
    try {
        if (!collector.start(cfg)) {
            std::fprintf(stderr, "failed to open segment '%s'\n", cfg.shm_name.c_str());
            return 1;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "failed to start: %s\n", e.what());
        return 1;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    std::printf("collecting from '%s' -> %s\n", cfg.shm_name.c_str(),
                cfg.path.empty() ? "(no csv)" : cfg.path.c_str());
    std::printf("Ctrl-C to stop.\n");

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(duration_seconds > 0 ? duration_seconds : 0);

    // The collector runs on its own thread; this one only waits for a reason
    // to stop. Polling a flag beats a condition variable here because a signal
    // handler can do almost nothing safely.
    while (!g_stop.load(std::memory_order_acquire)) {
        if (duration_seconds > 0 && std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::printf("\n");
    collector.dump_summary(std::cout);
    if (collector.name_conflicts() > 0) {
        std::printf("\n%u name conflicts: two producers disagreed about a label, or a "
                    "name was too long. The first name published wins.\n",
                    collector.name_conflicts());
    }

    collector.shutdown();
    if (!cfg.summary_path.empty()) {
        std::printf("wrote %s\n", cfg.summary_path.c_str());
    }
    return 0;
}
