// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Slick Quant
//
// The acceptance test for the low-latency claim. Reports min and median cycles
// over N repetitions, following the house benchmark style.
//
// Run-to-run spread on an unpinned desktop is roughly 10%, so treat any
// difference smaller than that as noise. The min is the number to trust: no
// sample can come in under the true cost, but any sample can be inflated by an
// interrupt or a migration.
//
// Exit codes: 0 success, 2 bad arguments, 3 the collector would not start.

#include <slick/perfmon.hpp>
#include <slick/perfmon/collector.hpp>
#include <slick/perfmon/detail/open_map.hpp>
#include <slick/perfmon/detail/tsc.hpp>
#include <slick/perfmon/pairing.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace slick::perfmon;

namespace {

constexpr point_id kPoint = 0;
constexpr uint8_t  kS1    = 1;
constexpr uint8_t  kS2    = 2;
constexpr uint8_t  kS3    = 3;
constexpr uint8_t  kS4    = 4;

int g_iters = 20000;
int g_reps  = 15;

struct Result {
    std::string label;
    double      min_cycles;
    double      median_cycles;
    int         stamps_per_op;
};

std::vector<Result> g_results;

/// Cost of one repetition of `op`, in cycles per operation.
template <typename Fn>
double time_op(Fn&& op, int iters) {
    const uint64_t t0 = detail::rdtsc_begin();
    for (int i = 0; i < iters; ++i) {
        op();
    }
    const uint64_t t1 = detail::rdtsc_step();
    return static_cast<double>(t1 - t0) / static_cast<double>(iters);
}

template <typename Fn>
void measure(const char* label, int stamps_per_op, Fn&& op) {
    // Warm the branch predictors and the ring before the first measurement, so
    // the first repetition is not systematically the slowest.
    for (int i = 0; i < 2000; ++i) {
        op();
    }

    std::vector<double> per_rep;
    per_rep.reserve(static_cast<size_t>(g_reps));
    for (int r = 0; r < g_reps; ++r) {
        per_rep.push_back(time_op(op, g_iters));
    }
    std::sort(per_rep.begin(), per_rep.end());

    g_results.push_back(Result{label, per_rep.front(),
                               per_rep[per_rep.size() / 2], stamps_per_op});
}

void print_results(double hz) {
    std::printf("\n%-42s %10s %10s %12s %10s\n", "case", "min cyc", "med cyc",
                "min cyc/stamp", "min ns");
    std::printf("%s\n", std::string(90, '-').c_str());
    for (const Result& r : g_results) {
        const double per_stamp =
            r.stamps_per_op > 0 ? r.min_cycles / r.stamps_per_op : r.min_cycles;
        const double ns = hz > 0.0 ? r.min_cycles * 1e9 / hz : 0.0;
        std::printf("%-42s %10.1f %10.1f %12.1f %10.1f\n", r.label.c_str(),
                    r.min_cycles, r.median_cycles, per_stamp, ns);
    }
}

void throughput(Collector& c, int threads) {
    constexpr int kSpans = 200000;

    const auto start = std::chrono::steady_clock::now();

    std::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(threads));
    for (int t = 0; t < threads; ++t) {
        pool.emplace_back([t] {
            // Every thread on ONE point, told apart by seq: the contended case,
            // and the one the open-span map has to survive.
            const span_seq seq = static_cast<span_seq>(t + 1);
            for (int i = 0; i < kSpans; ++i) {
                begin(kPoint, seq);
                end(kPoint, seq);
            }
        });
    }
    for (std::thread& th : pool) {
        th.join();
    }

    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    c.flush();
    const stats    s      = c.snapshot_total(kPoint);
    const uint64_t stamps = static_cast<uint64_t>(threads) * kSpans * 2;

    std::printf("%7d %14.2f %12llu %10llu %11llu %10llu %10zu\n", threads,
                static_cast<double>(stamps) / secs / 1e6,
                static_cast<unsigned long long>(s.count),
                static_cast<unsigned long long>(s.dropped),
                static_cast<unsigned long long>(s.abandoned),
                static_cast<unsigned long long>(s.orphan), c.peak_open_spans());
}


// --------------------------------------------------------------------------
// Collector backend.
//
// None of this runs on an instrumented thread, so it is outside the overhead
// budget - but the collector still has to keep up with the ring, and a flush
// that takes milliseconds is a flush during which nothing is being drained.
// These are the two backend paths that used to scale badly with size.
// --------------------------------------------------------------------------

/// Expiring many spans at once, which is what a crashed producer leaves
/// behind. erase_if() used to restart its scan after every removal, making
/// this quadratic in the number of expired spans.
void backend_sweep(int spans) {
    constexpr uint64_t kAge = 1000;

    config cfg         = {};
    cfg.max_open_spans = static_cast<uint32_t>(spans);
    cfg.max_stages     = 64;

    double best = 1e30;
    for (int r = 0; r < 5; ++r) {
        Pairer pairer;
        pairer.reset(cfg);
        // Opened and never closed, exactly as a producer killed mid-span
        // would leave them.
        for (int i = 0; i < spans; ++i) {
            const span_seq seq = static_cast<span_seq>(i + 1);
            pairer.on_event(sample{1, kPoint, make_event(kBeginStep, seq)});
        }

        const uint64_t t0 = detail::rdtsc_begin();
        const size_t   n  = pairer.sweep(1 + kAge * 2, kAge);
        const uint64_t t1 = detail::rdtsc_step();
        if (n != static_cast<size_t>(spans)) {
            std::fprintf(stderr, "sweep expired %zu of %d spans\n", n, spans);
        }
        best = std::min(best, static_cast<double>(t1 - t0));
    }
    std::printf("%-38s %8d %14.0f cyc %12.1f\n", "sweep expired spans", spans, best,
                best / spans);
}

/// Folding events back into spans and stage statistics - what the collector
/// does for every single sample it drains. Several producers stamping flat out
/// can outrun one consumer, so this is the number that decides whether samples
/// are dropped.
void backend_pair(int spans) {
    config cfg         = {};
    cfg.max_open_spans = 1024;
    cfg.max_stages     = 64;

    double best = 1e30;
    for (int r = 0; r < 5; ++r) {
        Pairer pairer;
        pairer.reset(cfg);
        // Warm the tables so the measured run is steady state, not first touch.
        for (int i = 0; i < 1000; ++i) {
            pairer.on_event(sample{static_cast<uint64_t>(i) * 2, kPoint, make_event(kBeginStep)});
            pairer.on_event(sample{static_cast<uint64_t>(i) * 2 + 1, kPoint, make_event(kEndStep)});
        }

        const uint64_t t0 = detail::rdtsc_begin();
        for (int i = 0; i < spans; ++i) {
            const uint64_t t = 2000 + static_cast<uint64_t>(i) * 2;
            pairer.on_event(sample{t, kPoint, make_event(kBeginStep)});
            pairer.on_event(sample{t + 1, kPoint, make_event(kEndStep)});
        }
        const uint64_t t1 = detail::rdtsc_step();
        best = std::min(best, static_cast<double>(t1 - t0));
    }
    std::printf("%-38s %8d %14.0f cyc %12.1f\n", "pair drained events", spans * 2, best,
                best / (spans * 2));
}

/// Opening spans against a table already at its cap. Once full it stays full -
/// every begin evicts one and inserts one - so this is not a one-off cost, it
/// is what every begin pays for as long as the overload lasts. Finding the
/// victim by scanning the table made that O(table) per event, which is a
/// feedback loop: the collector falls behind precisely when it is already
/// behind.
void backend_evict(int capacity) {
    constexpr int kEvictions = 20000;

    config cfg         = {};
    cfg.max_open_spans = static_cast<uint32_t>(capacity);
    cfg.max_stages     = 64;

    double best = 1e30;
    for (int r = 0; r < 5; ++r) {
        Pairer pairer;
        pairer.reset(cfg);
        // Fill it to the cap, oldest first, and never end any of them: a
        // producer killed mid-span leaves exactly this.
        for (int i = 0; i < capacity; ++i) {
            const span_seq seq = static_cast<span_seq>(i + 1);
            pairer.on_event(sample{static_cast<uint64_t>(i) + 1, kPoint,
                                   make_event(kBeginStep, seq)});
        }

        const uint64_t t0 = detail::rdtsc_begin();
        for (int i = 0; i < kEvictions; ++i) {
            const span_seq seq = static_cast<span_seq>(capacity + i + 1);
            pairer.on_event(sample{static_cast<uint64_t>(capacity + i) + 1, kPoint,
                                   make_event(kBeginStep, seq)});
        }
        const uint64_t t1 = detail::rdtsc_step();
        best = std::min(best, static_cast<double>(t1 - t0));
    }
    std::printf("%-38s %8d %14.0f cyc %12.1f\n", "evict at max_open_spans", capacity, best,
                best / kEvictions);
}

/// Rebuilding every report row, which happens once per flush. The cost used to
/// be dominated by a linear scan of the shared name table per label, plus a
/// fresh string and percentile vector per row.
void backend_flush(int points) {
    constexpr int kFlushes = 200;

    Collector c;
    config    cfg      = {};
    cfg.point_count    = 0;  // unnamed: the point_<p> fallback is the common case
    cfg.path           = "";
    cfg.summary_path   = "";
    cfg.queue_capacity = 1u << 16;
    cfg.max_stages     = static_cast<uint32_t>(points) * 2u;
    cfg.flush_interval = std::chrono::seconds(3600);
    // Spin instead of sleeping between polls. flush() is a handshake with the
    // collector thread, and at the default 1 ms poll - ~15.6 ms of real timer
    // granularity on Windows - the wake-up latency would be the whole number.
    cfg.poll_interval  = std::chrono::milliseconds(0);
    if (!c.start(cfg)) {
        std::fprintf(stderr, "backend collector would not start\n");
        return;
    }

    // One completed span per point, so every point contributes a row.
    for (int p = 0; p < points; ++p) {
        begin(static_cast<point_id>(p));
        end(static_cast<point_id>(p));
    }
    c.flush();  // drain and build the rows once, outside the measurement

    const size_t rows = c.reports().size();

    // The collector's own figure, not the round trip. flush() blocks on a
    // thread that may be asleep, and on Windows that wake-up is a 15.6 ms
    // timer tick whatever the flush actually cost - which at a thousand rows
    // would be the entire measurement.
    double best = 1e30;
    for (int i = 0; i < kFlushes; ++i) {
        c.flush();
        best = std::min(best, static_cast<double>(c.last_flush_cycles()));
    }
    c.shutdown();

    std::printf("%-38s %8zu %14.0f cyc %12.1f\n", "flush, rebuilding every row",
                rows, best, best / static_cast<double>(rows));
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--iters" && i + 1 < argc) {
            g_iters = std::atoi(argv[++i]);
        } else if (arg == "--reps" && i + 1 < argc) {
            g_reps = std::atoi(argv[++i]);
        } else {
            std::fprintf(stderr,
                         "usage: %s [--iters N] [--reps N]\n", argv[0]);
            return 2;
        }
    }
    if (g_iters < 1 || g_reps < 1) {
        std::fprintf(stderr, "iters and reps must be positive\n");
        return 2;
    }

#if SLICK_PERFMON_ENABLED
    std::printf("slick-perfmon benchmark  (SLICK_PERFMON_ENABLED=1, SERIALIZE=%d)\n",
                SLICK_PERFMON_SERIALIZE);
#else
    std::printf("slick-perfmon benchmark  (SLICK_PERFMON_ENABLED=0)\n");
#endif
    std::printf("iters=%d reps=%d\n", g_iters, g_reps);

    // The benchmark's own code is compiled /O2 or -O3 whatever the
    // configuration selects, but a debug *configuration* still links the debug
    // C runtime, and that alone costs roughly 3x per stamp. The binary builds
    // there - CI builds every target in both configurations - so it has to say
    // when its numbers are not the ones the README quotes, rather than printing
    // plausible figures that are wrong by a factor.
#if defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL != 0
    std::printf("WARNING: built against a debug C runtime (_ITERATOR_DEBUG_LEVEL=%d).\n"
                "         These numbers are not comparable with a release build -\n"
                "         use one before quoting any of them.\n",
                _ITERATOR_DEBUG_LEVEL);
#endif

    Collector c;
    config    cfg      = {};
    cfg.point_count    = 1;
    cfg.path           = "";
    cfg.summary_path   = "";
    cfg.queue_capacity = 1u << 20;  // large, so drops do not distort the timing
    cfg.max_open_spans = 8192;
    cfg.flush_interval = std::chrono::seconds(3600);

    const bool started = c.start(cfg);
#if SLICK_PERFMON_ENABLED
    if (!started) {
        std::fprintf(stderr, "collector would not start\n");
        return 3;
    }
#else
    (void)started;
#endif

    // 1. The primitive.
    measure("1. single stamp", 1, [] { stamp<false>(kPoint, make_event(kS1)); });

    // 2. A begin/end pair - should be about twice case 1.
    measure("2. begin/end pair", 2, [] {
        begin(kPoint);
        end(kPoint);
    });

    // 3. The same pair with a runtime seq. If this is slower than case 2, the
    //    IsBegin template is not doing its job and a branch survived.
    measure("3. begin/end pair, runtime seq", 2, [] {
        static volatile span_seq seq = 1;
        const span_seq           q   = seq;
        begin(kPoint, q);
        end(kPoint, q);
    });

    // 4. Five stages. Per-stamp cost has to stay flat - this is the case the
    //    event model exists for.
    measure("4. 5-stage pipeline (6 stamps)", 6, [] {
        begin(kPoint);
        step(kPoint, kS1);
        step(kPoint, kS2);
        step(kPoint, kS3);
        step(kPoint, kS4);
        end(kPoint);
    });

    // 5. The RAII guard, which must be within noise of case 2.
    measure("5. SLICK_PERFMON_SCOPE guard", 2, [] { SLICK_PERFMON_SCOPE(kPoint); });

    // 6. Runtime-disabled: one atomic load and a predicted branch.
    c.set_enabled(false);
    measure("6. begin/end pair, runtime-disabled", 2, [] {
        begin(kPoint);
        end(kPoint);
    });
    c.set_enabled(true);

    // 7. For contrast: what the standard library costs for the same job.
    measure("7. baseline: steady_clock::now() pair", 2, [] {
        volatile auto a = std::chrono::steady_clock::now();
        volatile auto b = std::chrono::steady_clock::now();
        (void)a;
        (void)b;
    });

    c.flush();
    print_results(c.tsc_hz());

#if SLICK_PERFMON_ENABLED
    std::printf("\ntsc %.4f GHz (%s), measured stamp overhead %llu cycles\n",
                c.tsc_hz() / 1e9, c.invariant_tsc() ? "invariant" : "NOT INVARIANT",
                static_cast<unsigned long long>(c.overhead_cycles()));
    std::printf("\nCompare case 8 against a build with SLICK_PERFMON_ENABLED=0\n"
                "(perfmon_bench_disabled), which must be indistinguishable from an\n"
                "empty loop.\n");

    std::printf("\nmulti-producer throughput, all threads on ONE point keyed by seq\n");
    std::printf("%7s %14s %12s %10s %11s %10s %10s\n", "threads", "M stamps/s",
                "spans", "dropped", "abandoned", "orphan", "peak open");
    std::printf("%s\n", std::string(80, '-').c_str());
    for (int threads : {1, 2, 4, 8}) {
        Collector tc;
        config    tcfg      = cfg;
        tcfg.queue_capacity = 1u << 20;
        if (!tc.start(tcfg)) {
            std::fprintf(stderr, "throughput collector would not start\n");
            return 3;
        }
        throughput(tc, threads);
        tc.shutdown();
    }

    std::printf("\ncollector backend - off the hot path, but on the critical path for\n"
                "keeping up with the ring\n");
    std::printf("%-38s %8s %17s %12s\n", "case", "n", "per call", "per n");
    std::printf("%s\n", std::string(80, '-').c_str());
    for (int spans : {200000}) {
        backend_pair(spans);
    }
    for (int spans : {1024, 4096, 16384}) {
        backend_sweep(spans);
    }
    for (int capacity : {1024, 4096, 16384}) {
        backend_evict(capacity);
    }
    for (int points : {32, 256, 1024}) {
        backend_flush(points);
    }
    std::printf("\nDrops are expected once several threads stamp flat out: pairing an\n"
                "event is cheaper than stamping one, but there is one collector and\n"
                "many producers, and it sweeps, aggregates and writes as well - so the\n"
                "ring buys burst absorption rather than unlimited sustained rate. What\n"
                "matters is that they are reported - a latency printed next to a\n"
                "non-zero anomaly count is not a latency you should trust.\n");
#else
    std::printf("\nEvery case above should be indistinguishable from an empty loop.\n");
#endif

    c.shutdown();
    return 0;
}
