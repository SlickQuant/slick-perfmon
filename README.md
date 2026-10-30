# slick-perfmon

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Header-only](https://img.shields.io/badge/header--only-yes-brightgreen.svg)](#installation)
[![Lock-free](https://img.shields.io/badge/hot%20path-lock--free-orange.svg)](#architecture)
[![CI](https://github.com/SlickQuant/slick-perfmon/actions/workflows/ci.yml/badge.svg)](https://github.com/SlickQuant/slick-perfmon/actions/workflows/ci.yml)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey.svg)](#platform-support)

A header-only C++20 latency measurement library for code that cannot afford to be
measured. The instrumented thread reads the TSC and publishes 16 bytes to a lock-free
ring of its own. Everything else — pairing, min/max/mean, Welford variance, histogram
percentiles, cycles-to-nanoseconds, formatting, file I/O — happens somewhere else, on a
collector thread or, in production, in a separate process. The same 16 bytes carry plain
values too — a queue depth, a batch size — with no timestamp read at all.

## How it works

```
  instrumented threads                    one SPSC ring per thread        collector
  ───────────────────────                ────────────────────────      ──────────────
  begin(tick, seq) ──┐                    ┌──────────────────┐
  step(tick, decode) ┼──► RDTSC(P) ──────►│ 16-byte events   │──┐      merge rings
  step(tick, match)  ┤    + 16B publish   │ ts | point | evt │  ├───►  by TSC, pair by
  end(tick, seq)  ───┘                    └──────────────────┘  │      (point, seq)
  other threads ────────────────────────► [ their own rings ] ──┘           │
                                                                            ▼
         one TSC read, four stores.                            min/max/mean, Welford,
         No read-modify-write, no                              histogram percentiles
         pairing, no locks.                                                │
                                                          ┌────────────────┴───────────┐
                                                          ▼                            ▼
                                                    perfmon.csv              perfmon_summary.txt
```

A span is `begin → [step …] → end`. Each call publishes one raw timestamp; the backend
reconstructs the span and reports **every stage transition plus the end-to-end total**
from the same events.

A value sample, `value(point, n)`, travels the same road without the timestamp: the
16-byte event carries the number itself, nothing is paired, and the collector reports
its distribution in the same files. See [Record a value](#record-a-value).

## Features

- **A fixed ~170 cycles per stamp, and nothing else, ever.** One serialized TSC read and
  four stores into the thread's own ring, with no read-modify-write, no delta, no lookup
  and no locks. Threads never contend, so the cost stays flat at 8 threads. Nothing is
  aggregated on the measured thread, however many points you add or however detailed the
  statistics get. See [Measured overhead](#measured-overhead).
- **Multi-step spans for free.** `begin → decode → match → send → end` yields per-stage
  latencies *and* the total, from four stamps rather than eight.
- **Start and end anywhere.** A span can open in one callback and close in another, on
  another thread, or in another DLL — the stamps carry everything needed to pair them.
- **Concurrent spans on one point**, told apart by a 24-bit correlation id you supply
  (an order id, a message sequence, a thread index).
- **Values as well as latencies.** `value(point, n)` records a number that is a
  measurement in itself — a queue depth, a batch size, a byte count — with no begin, no
  end and no timestamp, for ~16 cycles. It gets the same statistics, in the same CSV.
- **Out-of-process collection.** The measured process does zero analysis and no file
  I/O, and its numbers survive its own crash. The collector can be restarted under
  running producers without restarting them.
- **Compiles to nothing.** `SLICK_PERFMON_ENABLED=0` removes every stamp, the ring and
  the collector thread; instrumented source needs no `#ifdef` of its own.
- **Honest about its own failure modes.** Ring overflow, unmatched stamps, backwards
  timestamps and evicted spans are all counted and reported, because a latency figure
  you cannot trust is worse than none.
- **Bounded memory.** Fixed histograms and fixed tables, allocated at `start()` and
  never grown — a measurement tool that allocates under load distorts what it measures.

## Requirements

- C++20
- x86/x64 for the cycle counter. Other architectures fall back to `steady_clock`, which
  is correct but roughly an order of magnitude more expensive, and the compiler says so.
- An invariant TSC for trustworthy numbers. The library probes for one and shouts in the
  summary if it is missing.
- [slick-queue](https://github.com/SlickQuant/slick-queue) 2.3.0 and
  [slick-shm](https://github.com/SlickQuant/slick-shm) 0.1.6 — found with
  `find_package`, or fetched by the build if they are not installed

## Installation

### Using vcpkg

```bash
vcpkg install slick-perfmon
```

```cmake
find_package(slick-perfmon CONFIG REQUIRED)
target_link_libraries(main PRIVATE slick::perfmon)
```

### Using CMake FetchContent

```cmake
include(FetchContent)
FetchContent_Declare(
    slick-perfmon
    GIT_REPOSITORY https://github.com/SlickQuant/slick-perfmon.git
    GIT_TAG v0.3.0
)
FetchContent_MakeAvailable(slick-perfmon)
target_link_libraries(main PRIVATE slick::perfmon)
```

## Usage

### Headers

| Header | Contents | Who includes it |
|---|---|---|
| `<slick/perfmon.hpp>` | `begin` / `step` / `end`, `value`, `SLICK_PERFMON_SCOPE` | every instrumented file |
| `<slick/perfmon/types.hpp>` | `config`, `stats`, `name_fn` | whoever builds a config |
| `<slick/perfmon/collector.hpp>` | `Collector`, `StageReport` | `main()`, the collector, tests |

The hot-path header deliberately does not pull in `<vector>`, `<string>` or `<chrono>` -
it takes its types from `<slick/perfmon/stamp_types.hpp>`, which is scalars and one
16-byte POD. Configuring a session needs `config`, so the one file that calls `start()`
includes `types.hpp` as well, or gets it transitively from `collector.hpp`. In a build
with `SLICK_PERFMON_ENABLED=0`, where no ring is compiled in either, that keeps
`<slick/perfmon.hpp>` to about 4k preprocessed lines instead of 118k (MSVC 19.4).

### Declare your points

slick-perfmon never allocates an id. You declare them, and hand it a `constexpr`
function that turns a `(point, step)` pair into a label:

```cpp
// perf_points.hpp
#include <slick/perfmon.hpp>

#include <string_view>

enum class perf : slick::perfmon::point_id {
    tick_to_trade = 0,
    round_trip    = 1,
    book_update   = 2,
    count
};

enum : uint8_t { kDecode = 1, kMatch = 2, kSend = 3 };   // 0x01..0xFD are yours:
                                                         // 0x00 = begin, 0xFF = end,
                                                         // 0xFE = a value sample

constexpr std::string_view perf_name(slick::perfmon::point_id p, uint8_t step) noexcept {
    using namespace slick::perfmon;
    switch (static_cast<perf>(p)) {
    case perf::tick_to_trade:
        switch (step) {
        case kBeginStep: return "tick_to_trade";   // step 0 names the POINT
        case kDecode:    return "decode";          // the others name the stage
        case kMatch:     return "match";           // that ENDS there
        case kSend:      return "send";
        case kEndStep:   return "publish";
        default:         return {};
        }
    case perf::round_trip:
        return step == kBeginStep ? "round_trip" : std::string_view{};
    default: return {};
    }
}
```

Returning an empty view means "unnamed", and the output falls back to `point_<p>` /
`step_<a>-><b>`. `name_of` may also be `nullptr` for a quick one-off measurement. One
stage name is taken: `value` marks a [value sample](#record-a-value)'s row, so a step
named that is reported as unnamed.

### Start a session

```cpp
#include <slick/perfmon/collector.hpp>

int main() {
    slick::perfmon::config cfg;
    cfg.point_count = static_cast<slick::perfmon::point_id>(perf::count);
    cfg.name_of     = &perf_name;
    cfg.path        = "perfmon.csv";

    slick::perfmon::collector::instance().start(cfg);
    ...
}
```

Everything else has a default that gives a working in-process session:

| `config` field | Default | Effect |
|---|---|---|
| `run_mode` | `local` | `local`, `shared_producer` or `shared_collector` |
| `shm_name` | — | required by both shared modes |
| `point_count` | 0 | how many ids to scan when publishing names |
| `name_of` | `nullptr` | `(point, step)` → label; null falls back to generated names |
| `path` | `perfmon.csv` | CSV time series; empty writes none |
| `summary_path` | `perfmon_summary.txt` | written at shutdown; empty writes none |
| `flush_interval` | 1 s | reporting interval — one set of CSV rows per interval |
| `poll_interval` | 1 ms | collector sleep once the ring is empty |
| `csv_flush_interval` | 0 | 0 pushes the stream every interval; higher defers it |
| `calibration_time` | 10 ms | TSC calibration window inside `start()` |
| `stalled_sample_timeout` | 5 s | age at which an open span is abandoned and a ring hole stepped over; at shutdown holes get at most 100 ms in total |
| `queue_capacity` | 65536 | shared ring slots, for threads without a ring of their own; a power of two |
| `max_thread_rings` | 64 | per-thread rings; 0 sends every thread through the shared ring |
| `thread_ring_capacity` | 65536 | slots per thread ring; a power of two, 16 bytes each |
| `max_open_spans` | 4096 | concurrently open `(point, seq)` spans; a full table evicts |
| `max_stages` | 1024 | distinct stage accumulators, and the bound on distinct points |
| `max_names` | 512 | name slots in the shared control block |
| `percentiles` | 50, 90, 99, 99.9 | which percentile columns exist |
| `output_unit` | `nanoseconds` | `cycles`, `nanoseconds` or `microseconds` |
| `subtract_overhead` | `false` | subtract the measured per-stamp floor from every stage |
| `create_if_absent` | `false` | let a producer create the shared segments |

### Measure a span

`begin()` and `end()` do not have to share a scope, or a function, or a thread:

```cpp
void on_order_sent(const order& o) {
    slick::perfmon::begin(perf::round_trip, o.id);   // seq = the order id
    transport_.send(o);
}

void on_ack(uint32_t order_id) {                     // any thread, any time later
    slick::perfmon::end(perf::round_trip, order_id);
}
```

### Measure a pipeline

```cpp
void on_tick(const md& m) {
    slick::perfmon::begin(perf::tick_to_trade);
    auto order = decode(m);
    slick::perfmon::step(perf::tick_to_trade, kDecode);
    match(order);
    slick::perfmon::step(perf::tick_to_trade, kMatch);
    send(order);
    slick::perfmon::end(perf::tick_to_trade);
}
```

Four stamps produce three stage rows (`decode`, `match`, `publish`) plus a `total` row.
The stage means add up to the total mean, with nothing unexplained.

### Scoped measurement

```cpp
void on_book_update() {
    SLICK_PERFMON_SCOPE(perf::book_update);   // or (point, seq)
    do_work();
}   // end stamp published here, including on an early return or a throw
```

The guard is a wrapper over the two stamps — same instructions, one fewer way to get it
wrong. Two things worth knowing: it stamps during **exception unwinding**, so a throwing
region shows up as an outlier rather than as nothing (`cancel()` opts out), and nesting
measures true nesting as long as the nested guards use different points or different
`seq` values.

### Concurrent spans on one point

Two threads stamping the same point interleave in the ring as `begin, begin, end, end`,
and no backend can recover which end belongs to which begin. Pass a `seq` and they pair
correctly:

```cpp
const slick::perfmon::span_seq seq = order.id;   // anything that identifies the span
slick::perfmon::begin(perf::round_trip, seq);
...
slick::perfmon::end(perf::round_trip, seq);
```

`seq` only has to be unique among the spans **open on one point at one time**, not
globally — it wraps at 24 bits and that is harmless. Leave it out (`kNoSeq`, the
default) when a point is only ever used by one thread at a time; that is simply one
open span per point, and costs nothing extra.

If you get it wrong, you get `abandoned` counts rather than a plausible wrong number.

### Per-thread rings

Each instrumenting thread claims a ring of its own on its first stamp, so stamps never
contend: there is no shared counter and no cache line moving between cores. The
collector merges the rings by timestamp, so a span can still begin on one thread and end
on another.

- A session has `max_thread_rings` rings (64). Threads past that use the shared ring
  (`queue_capacity`) for the rest of the session. Nothing is lost; they just pay for the
  shared counter. `max_thread_rings = 0` sends every thread there.
- A thread that exits hands its ring back, and the next new thread reuses it.
- A ring keeps its newest `thread_ring_capacity - 1` samples. The collector pairs a stamp
  once it is 2 ms old, so a ring has to hold 2 ms of its thread's output plus one
  collector pass (up to ~15.6 ms on Windows when idle). The default 65536 covers ~3.5 M
  stamps/s per thread; beyond that, overwritten samples count as `dropped`.
- `prepare_thread()` claims the calling thread's ring and faults its pages in, so neither
  lands inside the first measurement. `start()` does it for its own thread.
- The rings take `max_thread_rings × thread_ring_capacity × 16` bytes of address space
  (64 MiB by default, at most 2 GiB), touched only as they are used. In the shared modes
  they live in the `.meta` segment, and a producer must use the collector's two
  settings. A local restart with the same two settings takes the previous session's
  rings back rather than allocating new ones, unless a DLL that attached to that session
  has not attached since (see [Important constraints](#important-constraints)).

### Record a value

Not everything worth watching is a duration. A value sample has no begin and no end —
each call is one complete measurement:

```cpp
void on_batch(const batch& b) {
    slick::perfmon::value(perf::batch_size, b.size());    // or SLICK_PERFMON_VALUE(...)
    slick::perfmon::value(perf::queue_depth, queue_.size());
    ...
}
```

The backend reports the distribution of the values a point was given — count, min,
mean, stddev, percentiles, max — exactly as it does for a latency, but **as recorded**:
no cycles-to-time conversion, no `output_unit`, no overhead subtraction. The unit is
whatever you passed in. A value is an unsigned 64-bit integer; a negative number
converts the way the language says and comes out as a very large one.

It is also the cheapest thing in the library. The expensive part of a stamp is the
serialized TSC read, and a value has no use for a timestamp, so `value()` is the null
check and the 16-byte publish and nothing else — ~16 cycles against ~170.

Value points come out of the same id space as span points and are named the same way
(`name_of(point, kBeginStep)`). Give a point values or spans, not both: the two would
share a name in the output while meaning unrelated things.

### Reading the results

```cpp
using namespace slick::perfmon;                      // to_point, kBeginStep
auto& perfmon = collector::instance();

perfmon.flush();                                     // drain and publish a snapshot
auto total = perfmon.snapshot_total(to_point(perf::tick_to_trade));
auto decode = perfmon.snapshot(to_point(perf::tick_to_trade), kBeginStep, kDecode);
auto batch = perfmon.snapshot_value(to_point(perf::batch_size));   // raw, not ns

perfmon.dump_summary(std::cout);
```

In `reports()`, a value point's row has `StageReport::value` set and the stage name
`value`.

### Out-of-process collection

The production shape. Start the collector first; it creates the segment and producers
attach to it.

```bash
slick_perfmon_collector --shm trading_perf --csv trading_perf.csv
```

```cpp
cfg.run_mode = slick::perfmon::mode::shared_producer;
cfg.shm_name = "trading_perf";
cfg.path     = "";               // a producer writes no files
slick::perfmon::collector::instance().start(cfg);
```

If the segments are not there, `start()` returns `false`, the process records nothing,
and every stamp costs nothing. That is the safe default for a binary shipped to
production with instrumentation compiled in — no segment is created, no data is
buffered, nothing is silently lost. Set `config::create_if_absent` if you need the
reverse order.

The collector binary knows nothing about your points and does not need to: producers
publish their names into the segment at startup and the collector reads them back.

**The collector can be restarted under running producers.** It never removes the
segments on its way out, so the one that replaces it opens the same ring and the same
name table, and a producer that kept running through the restart is collected again
under the names it published — nothing has to be restarted with it. A collector reads
from the moment it starts, so whatever was stamped while none was running is not
collected, and a span that straddles the restart shows up as an `orphan`.

Two consequences of the segments staying, both on POSIX (on Windows they vanish with
the last process attached):

- A producer started while **no collector is running** still attaches if an earlier
  session left its segments behind. `start()` returns `true` and it stamps at full cost
  into a ring nothing drains until a collector comes back. "Costs nothing without a
  collector" holds only for a name with no segments.
- A name keeps the `queue_capacity`, `max_names`, thread ring settings and labels it was
  created with. To change any of them — or simply to clean up — remove it first:

```cpp
slick::perfmon::collector::remove_shm("trading_perf");   // ring + control block
```

**The collector reclaims rings from a producer that dies.** At every flush it checks
whether each ring's process still exists. For one that is gone, it drains what was
published and frees the ring for the next thread.

### Environment overrides

Only fill in fields you left at their defaults, so an explicit config always wins:

| Variable | Effect |
|---|---|
| `SLICK_PERFMON_CSV=<path>` | local mode, auto-start, write there |
| `SLICK_PERFMON_SHM=<name>` | `shared_producer` mode, attach that segment |
| `SLICK_PERFMON_DISABLE=1` | force off regardless of the others |

If either of the first two is set and nothing ever calls `start()`, a session starts
automatically. Those sessions have no `name_of`, so stages render as
`point_<p>:step_<a>-><b>`.

### Turning it off

**At compile time** — `SLICK_PERFMON_ENABLED=0` (or `-DSLICK_PERFMON_ENABLED=OFF` in
CMake) makes every entry point an empty inline and the macros `((void)0)`. No RDTSC, no
ring, no collector thread. Instrumented source compiles unchanged.

Configured with `-DSLICK_PERFMON_ENABLED=OFF`, the build neither finds nor links
slick-queue and slick-shm, and the installed package records that choice: a downstream
`find_package(slick-perfmon)` then needs neither dependency either. The switch is a
property of the package as built, not of the consumer's cache - a consumer who wants a
disabled build from an enabled package defines `SLICK_PERFMON_ENABLED=0` on their own
target.

**At runtime** — `collector::set_enabled(false)`. The producer already checks one
pointer, and that pointer *is* the switch, so this costs nothing it was not paying.

## API overview

```cpp
namespace slick::perfmon {

using point_id = uint32_t;      // yours to partition
using span_seq = uint32_t;      // 24 usable bits; kNoSeq (0) = unkeyed

template <point_like T> void begin(T point, span_seq seq = kNoSeq) noexcept;
template <point_like T> void step (T point, uint8_t n, span_seq seq = kNoSeq) noexcept;
template <point_like T> void end  (T point, span_seq seq = kNoSeq) noexcept;
template <point_like T> void value(T point, uint64_t v) noexcept;   // no span, no timestamp

void prepare_thread() noexcept;                    // claim this thread's ring up front
void attach(const detail::session*) noexcept;      // a DLL stamping into the host's session

class ScopedSample { ... };     // begin on construction, end on destruction

class Collector {
    static Collector& instance() noexcept;
    static void set_instance(Collector*) noexcept;   // share one across DLLs

    bool start(config cfg = {});
    void shutdown();
    bool running() const noexcept;
    static void remove_shm(const std::string& shm_name) noexcept;   // never implicit
    void set_enabled(bool) noexcept;                 // live on/off switch
    bool enabled() const noexcept;
    void flush();

    stats snapshot(point_id, uint8_t from, uint8_t to);
    stats snapshot_total(point_id);
    stats snapshot_value(point_id);                  // a value point, as recorded
    std::vector<StageReport> reports();
    void dump_summary(std::ostream&);

    double   tsc_hz() const noexcept;
    bool     invariant_tsc() const noexcept;
    uint64_t overhead_cycles() const noexcept;
    uint64_t last_flush_cycles() const noexcept;
    size_t   peak_open_spans() const noexcept;
    uint32_t name_conflicts() const noexcept;        // names two peers disagreed on

    const detail::session* session() const noexcept; // what attach() takes
    sample_queue* ring() const noexcept;             // the shared ring, for threads past the cap
};

}
```

## Important constraints

- **A `seq` must be unique among the spans open on one point.** Reusing one while its
  span is still open counts as `abandoned`.
- **Step `0xFE` is reserved.** It marks a value sample (`kValueStep`), so the steps a
  span may use as intermediate boundaries are `0x01..0xFD` (`kMaxSpanStep`). A
  `step(point, 0xFE)` would be read back as a value sample holding a timestamp. Because
  that changes what an event in the ring means, the shared-memory layout version went up
  with it: a collector and a producer built either side of this change refuse to share a
  segment rather than misread each other, so rebuild them together.
- **The ring overwrites; it never blocks.** A producer that outruns the collector loses
  samples, and a lost stamp corrupts *pairing* rather than just perturbing one number.
  Check `dropped` first whenever `orphan` or `abandoned` climb, and raise
  `thread_ring_capacity` (or `queue_capacity` for threads past the ring cap). The
  overwrite itself is safe — producer and collector touch a contended slot through
  relaxed atomics, so the overlap is defined rather than a data race — but a sample it
  catches mid-write can mix two generations of the slot, which is one more reason
  `dropped` has to be read before the latencies beside it.
- **One collector has a throughput ceiling.** Producers don't limit each other: in the
  benchmark eight threads stamp flat out at ~120 M stamps/s in total. But there is one
  collector, pairing at ~90 cycles an event (~35 M events/s) while it also sweeps,
  aggregates and writes. The rings absorb bursts, and the collector sets the sustained
  rate. So the library can drop samples, but it never drops them silently: instrument
  the paths you care about rather than everything, and read the anomaly counters before
  trusting a figure.
- **Stamps are paired 2 ms late, and a thread stalled mid-stamp for longer can misorder
  a span.** Merging rings by timestamp means waiting until no ring can still hold an
  older stamp. `flush()`, `snapshot()` and `shutdown()` wait at least ~20 µs instead:
  they pair everything stamped before the call and leave anything stamped after it for
  the next pass, so a thread that keeps stamping cannot hold them up. If a thread is
  preempted for more than 2 ms between reading the TSC and publishing, its stamp arrives
  late. When the span's other stamps came from another thread they have already been
  paired, and the span shows up as `orphan` and `abandoned`. Threads past the ring cap
  are sorted by timestamp too, so a backwards TSC on one of them reads as `orphan`, not
  `invalid`.
- **The id space is shared in shm mode.** Give each process a disjoint range, or share
  ids deliberately so the numbers merge. Two processes publishing different names for
  one id is reported as a conflict, and the first name wins.
- **`shm_name` is short on macOS.** A session needs two segments, the ring under the
  name and the control block under `name + ".meta"`, and macOS caps a POSIX shm name at
  31 bytes including the leading `/` where Linux and Windows allow far more. That leaves
  **25 characters** for `config::shm_name` there against 249 elsewhere, so `start()`
  checks the length itself and throws `std::invalid_argument` naming the limit — an
  over-long name would otherwise open the ring, fail on the control block, and surface
  as a bare `false` or as "File name too long" from inside a dependency.
  `SLICK_PERFMON_SHM_NAME_MAX` carries the figure and can be overridden.
- **Overhead figures need a cycle counter.** Off x86 `read_tsc()` falls back to
  `steady_clock`, which on Apple Silicon resolves to ~41.67 ns — coarser than the stamp
  being measured. `overhead_cycles()` is a *minimum* over many pairs, so it legitimately
  reads 0 there, and `subtract_overhead` can measure an overhead larger than a whole
  span and clamp every stage to zero. Leave it off on those platforms; the per-stamp
  figures in this README are x86 numbers and say so.
- **`shutdown()` must not race with active instrumentation.** Retired sessions, rings
  included, are held until the `Collector` is destroyed, so a stamp in flight across
  `shutdown()` is safe; one in flight across `~Collector` is not. A shared ring is the
  exception: restarting on the same `shm_name` drops the retired mapping first, so the
  object is not left holding a second mapping of a segment it is about to open again.
  Only a stamp still in flight across that whole restart is exposed. Thread rings are
  reused by slot rather than freed - the next local session takes the table back, and a
  shared table outlives every session - so a stamp in flight across `shutdown()` can
  land in a ring another thread has claimed since. That costs one misattributed sample,
  never memory safety.
- **A DLL that stamped into the host's session must attach again after the host
  restarts**, whether it got there through `attach()` or by binding through
  `Collector::session()`. Each module has its own hot path, so until it does, its stamps
  go to the ended session and are not recorded. The host does not reuse that session's
  thread rings while the DLL may still write them, so one that never attaches again
  holds one table back for good.
- **Shared segments are never removed implicitly.** Neither `shutdown()` nor
  `~Collector` unlinks the ring (`<shm_name>`) or the control block (`<shm_name>.meta`),
  and since 2.3.0 slick-queue does not either: none of them can know whether a producer
  is still attached. That is what lets a collector restart under running producers — see
  [Out-of-process collection](#out-of-process-collection). The cost is on POSIX, where a
  segment outlives every process that mapped it, so after a session `/dev/shm/<shm_name>`
  and `/dev/shm/<shm_name>.meta` are still there and the name stays bound to what it was
  created with: a later session with a different `queue_capacity` throws "Shared memory
  size mismatch", one with a different `max_names` or thread ring settings throws a
  control-block mismatch, and a point keeps the label first published for it (a
  different one is counted in `name_conflicts()`). `Collector::remove_shm("<shm_name>")`
  removes both segments and gives the name a clean start; call it from whatever
  coordinates the session, once no peer will attach again. On Windows a segment is
  freed with its last handle, so it persists only while some process is still attached
  and there is nothing to remove.
- **Percentiles are approximate**, with a bounded relative error of ~3% at the default
  histogram precision. A percentile is the nearest rank - the smallest sample with at
  least p% of them at or below it, with no interpolation - so `p51` of two samples is the
  larger one. They are clamped to the observed `[min, max]`. Buckets are
  64-bit, so a single hot stage cannot wrap one and corrupt its percentiles however long
  the run lasts; the price is ~7.6 KB per histogram, two per stage, bounded by
  `config::max_stages` and paid entirely by the collector.
- **A flush is time the collector is not draining.** Rebuilding the rows costs roughly
  2,500 cycles per stage, so a thousand stages at the default one-second interval is
  under a millisecond - but at a 10 ms interval it is 10% of the collector, and the ring
  keeps filling throughout. `Collector::last_flush_cycles()` reports the real figure;
  timing `flush()` from outside does not, because it is a handshake with a thread that
  may be asleep. Widen `flush_interval` before narrowing it. Part of that cost is
  pushing the CSV stream to the OS, which happens once per interval by default;
  `config::csv_flush_interval` defers it so the 64 KB buffer behind the stream can fill,
  at the price of a wider window in which a hard kill loses rows. `shutdown()` writes
  everything out either way.
- **`shutdown()` drains everything reserved before it, holes included.** A slot a
  producer reserved but never published — killed between `reserve()` and `publish()` —
  does not end the final drain. Holes share one grace period (the shorter of
  `stalled_sample_timeout` and 100 ms) to be published, after which each is stepped over
  and counted in `stalled`, so the events behind them still reach the last report and the
  loss is visible in it. The drain stops at what was reserved when `shutdown()` began: a
  producer in another process is not detached by it, and one that keeps publishing does
  not hold shutdown open.
- **Running at `max_open_spans` is a sustained cost, not a one-off.** Once the table is
  full it stays full — every `begin` evicts one span and inserts one — so eviction is on
  the critical path for as long as the overload lasts. It is amortised to ~1,500 cycles
  at 1k open spans and ~1,800 at 16k — near enough flat in table size — but that is
  still ~17x what pairing a normal event costs. Treat a `peak_open_spans()` that sits at
  `max_open_spans` as a sizing error, not as a cost absorbed.
- **Without an invariant TSC every figure is unreliable.** The library detects this and
  says so in the summary rather than quietly reporting nonsense.

## Architecture

**Hot path.** One acquire load of the session pointer, which doubles as the enabled flag
so there is no second check. Then a compare against the thread's cached session id,
`LFENCE; RDTSC; LFENCE` for a begin or `RDTSCP; LFENCE` for anything else, three relaxed
stores into the thread's ring and a release store of its head. That is the whole thing:
no read-modify-write, no delta, no unit conversion, no bounds branch, no locks. The ring
lookup goes before the timestamp for a begin and after it for anything else, so it stays
outside the measured interval either way. Keeping the timestamp a raw 64-bit value is
what removes the ~1.4 s ceiling a 32-bit cycle delta would have imposed.

**Rings.** Each thread's ring has a single producer and the collector only reads it, so
nothing a publish writes is written by anyone else. The collector reads the head once
per pass, and the producer keeps its own copy, so a publish never has to load it. The
shared multi-producer ring stays for threads past `max_thread_rings`.
Claiming a ring is one compare-and-swap on a slot holding the claimant's pid, so a
producer that dies leaves enough behind for the collector to reclaim the ring.

**Merge.** Each ring is in timestamp order already, so the collector merges them with a
small heap, pairing a stamp only once it is 2 ms old. The hold-back lets a span cross
threads: a begin stamped just before a pass may not be visible yet, and pairing its end
first would orphan it. The ring keeps its newest `capacity - 1` samples, because
after copying a chunk the collector reads the head again and discards whatever the
producer may have reached meanwhile, counting it as `dropped`.

Intermediate stamps use `RDTSCP` because an intermediate stamp is simultaneously the
close of one stage and the open of the next: it has to wait for prior work to retire
*and* stop later work drifting up. `SLICK_PERFMON_SERIALIZE=0` drops the fences and buys
back 30–70 cycles per stamp depending on the shape of the span, at the cost of letting
the CPU move work across the timestamps.

**The record** is 16 bytes: a raw TSC, a 32-bit point and a 32-bit event holding
`(seq << 8) | step`. The three fields are written with *relaxed* atomic stores and read
back with relaxed atomic loads, which is a correctness requirement rather than a
precaution: the ring overwrites, so a producer that laps the collector writes a slot the
collector is reading. That overlap is deliberate and is counted as `dropped`, but with
plain stores it is also a data race — undefined behaviour, and a ThreadSanitizer
failure. Relaxed atomics make it a *defined* overlap whose only consequence is a sample
mixing two generations of the slot, which is exactly what `dropped` exists to warn about.
Relaxed is the whole point: no fence, no ordering, no lock, and on x86-64 and AArch64
each field lowers to the store instruction a plain assignment emitted. The only thing
given up is store merging across the two adjacent 32-bit fields, and the benchmark puts
that inside the run-to-run noise.

A value sample is the same record with the step set to `kValueStep` and the value in the
64 bits a timestamp would occupy. One record for both kinds, rather than a second ring,
keeps the hot path at a single pointer load and a shared-memory session at one ring
segment. The collector recognises the step before any pairing happens and sends the
value straight to its point's accumulator: it opens no span, so nothing about it can be
orphaned, abandoned or invalid.

The collector's side of the same contract: the drain snapshots each slot into a local
before pairing it, rather than handing the pairer a reference into live ring
memory. The pairer reads `event`, `point` and `timestamp` several times each, and a
producer lapping between two of those reads would otherwise let it pair a step from one
generation against a timestamp from another.

**Backend.** A per-span state machine keyed by `(point, seq)`. `kNoSeq` is not a special
case — it is simply the single bucket a point gets when its spans never overlap, so one
code path serves both. A `begin` on an already-open span counts `abandoned`; any other
stamp with nothing open counts `orphan`; a backwards timestamp counts `invalid` and
discards the span rather than emitting one bad stage and a bad total from the same
events.

**Idle behaviour.** The collector yields for a short burst, then sleeps. There is
deliberately no park/wake handshake — unlike a logger, nobody is waiting on a perf
sample, so paying the producer an atomic load per stamp to wake the collector promptly
would be a straight loss, and it could not work across processes anyway. On Windows the
1 ms poll is really the ~15.6 ms timer granularity, which is exactly what is wanted: the
ring is sized to absorb that much production and the collector costs almost no CPU.
When busy, it still waits at least 50 µs between passes. Every read of a ring costs its
producer a cache miss on its next stamp, and a collector polling flat out turns that
into nearly every stamp.

## Output

### CSV — one row per stage per interval

```
timestamp,point,stage,kind,count,dropped,invalid,stalled,abandoned,orphan,out_of_range,min_ns,mean_ns,stddev_ns,p50_ns,p90_ns,p99_ns,p99_9_ns,max_ns,tsc_hz
2026-09-20 18:51:00.000,tick_to_trade,decode,latency,120345,0,0,0,0,0,0,41.2,58.3,11.2,55.0,71.0,140.0,890.0,1204.0,2995200000
2026-09-20 18:51:00.000,tick_to_trade,match,latency,120345,0,0,0,0,0,0,88.0,131.5,22.4,126.0,168.0,310.0,1400.0,2980.0,2995200000
2026-09-20 18:51:00.000,tick_to_trade,total,latency,120345,0,0,0,0,0,0,201.0,284.9,41.7,275.0,340.0,690.0,3100.0,6200.0,2995200000
```

`point` and `stage` are separate columns so a `groupby` is trivial. `tsc_hz` is a column
rather than a comment line, so the file loads without a `comment=` argument and carries
its own calibration. Every latency column carries `config::output_unit` in its name
(`_ns`, `_us` or `_cycles`) — the values stay bare numbers that load without parsing,
but no reader can take one scale for another. With `config::subtract_overhead` on there
is one further column, `overhead_<unit>`, holding the per-stamp cost that was taken out
of every figure in the row, so a reader can put it back. Numbers are always written in
the classic `"C"` locale — a `.` decimal point and no digit grouping — whatever global
locale the process sets, so a German or French desktop cannot put a stray comma in a row.
The summary file follows the same rule; `dump_summary()` leaves a caller's stream as it is.

Value samples are rows of the same file, one per value point per interval. The `kind`
column says which a row is — `latency` or `value`:

```
2026-09-20 18:51:00.000,batch_size,value,value,120345,0,0,0,0,0,0,1.0,37.4,22.9,32.0,70.0,118.0,180.0,200.0,2995200000
```

The columns are the same for both kinds; what differs is how to read them. **On a
`kind=value` row the figures under `min_ns` … `max_ns` are the values as recorded, in
whatever unit you passed in, not nanoseconds.** Filter on `kind` before aggregating a
latency column. `count` and the anomaly counters mean what they mean on any row, `tsc_hz`
is carried as usual, and `overhead_<unit>`, where the file has it, is `0.0` because
nothing was subtracted. Rows that carry only counters — an anomaly-only `total`, the
`collector` and `overflow` rows below — are `latency`: the counters describe spans.

The stage label `value` is reserved as well, so the `stage` column and the summary table
agree with `kind`. A `name_of` that returns `"value"` for a step of a span is treated as
returning no name, and the stage gets the generated `step_<a>-><b>` label instead. A
*point* may still be called `value`. For code rather than a file, `StageReport::value`
says which kind a row is.

**A CSV written before the `kind` column existed is not appended to.** In such a file
any row could be a latency whatever its stage was called — `value` included, since the
label was not reserved then — so value rows added beneath it could never be told apart
from what was already there. Its header no longer matches, and `start()` throws
`std::invalid_argument` saying so. Point `config::path` somewhere new or move the old
file aside; nothing in it is modified.

Restarting a collector appends rather than starting over — but only onto a matching
schema. The percentile list decides which columns exist; the unit and the overhead
correction decide what the numbers under them are. All three are therefore part of the
header, and a session that disagrees with the file about any of them refuses to start
rather than append rows the header labels wrongly. No parser could have caught that on
its own: it reads the wrong number under the right name and is never given a reason to
doubt it. A percentile itself must be a finite number in `[0, 100]`; `start()` refuses
anything else, a NaN included, rather than let one reach the rank arithmetic. Its column
is named with as many digits as it takes to tell it from every other double — `p99_9`
stays `p99_9`, but `p99.9999999` and `p99.99999991` are two column names, not one, so
the schema check cannot be walked past by two values that merely print alike.

Every anomaly counter gets a column, so a row can be judged on its own without going
back to the summary. In the CSV they are **per interval**, like `count` and `dropped` —
an orphan appears in the row for the interval it happened in and is not repeated down
the rest of the file, so a column sums to what the run actually did. `reports()` and the
summary carry the lifetime totals beside them. Labels are RFC 4180 quoted, so a
`name_of` that returns a comma or a quote still produces a file any reader loads
correctly. `stalled` is collector-wide rather than per-stage and so repeats down the
file; in the summary it appears once, in the header line.

A stage with no samples in an interval writes no row — with one exception: a row whose
anomaly counters moved is written even though nothing completed, because the counters
are its content. A point that only ever produced orphans has no stage at all, and gets
one empty `total` row so its counts cannot exist invisibly.

An interval that writes no row at all is the remaining case. `dropped` and `stalled`
belong to the collector, not to a stage, so they reach the file as columns on whatever
rows it writes — and an interval where the ring overflowed hard enough that nothing
completed, or where a producer died mid-stamp and its slot had to be stepped over, is
exactly the interval with no row to carry them. They get one of their own instead, under
the point name `collector` with stage `total` and a `count` of zero, once, in the
interval they happened. Like `overflow` it is a label rather than a point.

A run that touches more distinct points than `config::max_stages` folds the surplus
anomaly counts into one bucket, reported under the point name `overflow`. It is the one
row that is not a real point — `StageReport::overflow` marks it, since no value in a
`point_id` space the caller owns could have been reserved to mean it — and its presence
means `max_stages` is too small for the point cardinality in use.

There is no `process` column: the 16-byte record carries no pid, and paying for one on
every stamp to get an occasionally-useful column is the wrong trade. In shm mode, events
from every process using a point merge into its rows — which is why the id space is
yours to partition.

### Summary — written at shutdown and on demand

```
slick-perfmon summary   tsc 2.9952 GHz (invariant)   run 0:05:12   overhead 165 cyc/stamp   stalled 0 slot(s)
point               stage                  count   dropped   invalid  abandoned    orphan  out_of_range         min        mean         p50         p99         max
-------------------------------------------------------------------------------------------------------------------------------------------------------------------
tick_to_trade       decode               1204531         0         0          0         0             0      41.2ns      58.3ns      55.0ns     140.0ns      1.20us
tick_to_trade       match                1204531         0         0          0         0             0      88.0ns     131.5ns     126.0ns     310.0ns      2.98us
tick_to_trade       total                1204531         0         0          0         0             0     201.0ns     284.9ns     275.0ns     690.0ns      6.20us
batch_size          value                1204531         0         0          0         0             0           1       37.42          32         118         200
```

The `p50` and `p99` columns are always the whole run's median and 99th percentile,
whatever `percentiles` is set to: taken from the configured list when it has them and
computed from the histogram when it does not. `StageReport::summary_p50` and
`summary_p99` carry the same two values.

A value point is a row of the same table, with stage `value` and its figures printed as
bare numbers: no unit is attached because none this library knows applies.

## Measured overhead

From `bench/perfmon_bench` on an AMD Ryzen 9 5900HX (3.29 GHz TSC) running Windows,
unpinned. Minimum of 15 repetitions of 20,000 iterations; run-to-run spread is around
10%, so treat smaller differences as noise.

| Case | cycles/stamp | ns/stamp |
|---|---|---|
| Single stamp, fenced (default) | 172 | ~52 |
| `begin`/`end` pair, fenced | 171 | ~52 |
| `begin`/`end` pair with a runtime `seq` | 168 | ~51 |
| 5-stage pipeline, 6 stamps | 194 | ~59 |
| `SLICK_PERFMON_SCOPE` guard | 169 | ~51 |
| Single stamp, `SLICK_PERFMON_SERIALIZE=0` | 128 | ~39 |
| `begin`/`end` pair, `SLICK_PERFMON_SERIALIZE=0` | 95 | ~29 |
| Value sample (`value()`, no timestamp) | **16** | ~5 |
| Runtime-disabled | **2.5** | ~0.8 |
| Compile-time disabled (`SLICK_PERFMON_ENABLED=0`) | **0.0** | **0.0** |

The next table has other threads stamping at the same time, in cycles, against v0.3.0's
single shared ring. "Publish" is the per-call cost of one `value()`, with the two fenced
TSC reads around it subtracted. "Span overhead" is what the stamps add to a measured
span (begin, a 1000-cycle spin, end): the collector's own figure less the spin.

The value sample above (16) and a one-thread publish below (51) are the same call. 16 is
its cost in a tight loop with the cache hot. 51 is one call on its own after a gap, timed
through a fenced bracket that rounds to the counter's 33-cycle tick, so cache misses and
that rounding are in it.

| Threads | publish mean, v0.3.0 → now | publish p50 | span overhead mean | span overhead p99 |
|---|---|---|---|---|
| 1 | 127 → 51 | 66 → 33 | 506 → 397 | 760 → 504 |
| 2 | 303 → 52 | 264 → 33 | 660 → 407 | 824 → 504 |
| 4 | 359 → 57 | 297 → 33 | 729 → 429 | 952 → 504 |
| 8 | 429 → 68 | 297 → 66 | 845 → 448 | 1112 → 504 |

Five things worth reading off those tables:

- **Per-stamp cost is flat.** A six-stamp pipeline costs about the same per stamp as a
  single one, which is what makes multi-step spans worth using.
- **It stays flat across threads.** In v0.3.0 each thread's stamps moved the shared
  ring's counter between cores, and that cost landed inside the spans being measured.
- **Most of a stamp is the timestamp.** A value sample is the same publish without the
  serialized TSC read in front of it, and costs about a tenth as much.
- **A runtime `seq` is free.** No worse than the unkeyed pair, confirming the
  RDTSC-versus-RDTSCP choice stays a compile-time decision even when the caller passes a
  runtime correlation id.
- **Compiled out really is nothing.** Every stamp case in the `SLICK_PERFMON_ENABLED=0`
  build measures 0.0 cycles — the optimizer removes the calls entirely. The one case
  that does not is the runtime-`seq` pair, at 1.7 cycles per iteration: that is the
  `volatile` the benchmark itself uses to keep the id alive, with no stamp left in the
  loop to attribute it to.

### An honest comparison with `std::chrono`

In the same benchmark, a bare `steady_clock::now()` pair costs **~300 cycles (~150 per
call)** on this machine. A slick-perfmon `begin`/`end` pair costs ~340. **slick-perfmon
is not cheaper than reading the clock**, and on Linux, where `steady_clock` is a vDSO
`clock_gettime` at roughly 70 cycles, it is clearly more expensive per timestamp.

What it buys instead:

- **The stamp is the entire cost, permanently.** Those ~170 cycles already include
  handing the sample off; `steady_clock::now()` gives you a number you still have to store,
  pair and aggregate. Add a min/max/histogram update to the measured thread and it is no
  longer close — and that cost grows with every statistic you want, while this one does
  not.
- **It is a fixed cost, not a variable one.** Adding measurement points, stages or
  percentiles changes nothing on the instrumented thread.
- Spans that cross functions, threads, DLLs and processes; out-of-process collection;
  survival of the measured process's own crash; and removal to literally zero at compile
  time.

If all you need is one timestamp pair in one function and you are happy aggregating
inline, `std::chrono` is the simpler tool and this library has nothing to offer you.

### Subtracting the floor

Part of a stamp's cost lands inside every measured stage: the tail of one stamp plus the
head of the next. `Collector::overhead_cycles()` reports it, measured at startup as the
minimum over a thousand back-to-back stamps (typically ~130 cycles here).

`config::subtract_overhead` will subtract it, but it is **off by default**. The
calibration is the minimum over back-to-back stamps against a private ring whose lines
never leave the core. On a live ring the collector's reads cost the producer the odd
cache miss, so a typical stamp runs above it. Subtracting a number that is
systematically low leaves a residue that is invisible, whereas reporting raw cycles
leaves the overhead visible and lets `overhead_cycles()` tell you where the floor is.

Turn it on when the regions you are measuring are short enough that the stamp dominates,
and read the result as a lower bound.

With it on, the CSV gains an `overhead_<unit>` column carrying the amount subtracted from
every figure in the row, and its presence in the header is what stops corrected rows from
being appended to a file of raw ones, or the reverse — the two differ by a systematic
offset that nothing in a bare number would reveal.

## Platform support

| Platform | Counter | Notes |
|---|---|---|
| Windows x64 | `__rdtsc` / `__rdtscp` | MSVC and clang-cl |
| Linux x64 | `__rdtsc` / `__rdtscp` | GCC and Clang |
| macOS x64 | `__rdtsc` / `__rdtscp` | |
| ARM / other | `steady_clock` | Correct, but the overhead figures do not apply |

## Building and testing

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_SLICK_PERFMON_BENCH=ON
cmake --build build --config Release -j 16
cd build && ctest -C Release --output-on-failure
```

| Option | Default | Effect |
|---|---|---|
| `SLICK_PERFMON_ENABLED` | ON | OFF compiles every stamp to nothing |
| `SLICK_PERFMON_SERIALIZE` | ON | OFF drops the fences: faster, reorderable |
| `BUILD_SLICK_PERFMON_TESTS` | top-level | |
| `BUILD_SLICK_PERFMON_EXAMPLES` | top-level | |
| `BUILD_SLICK_PERFMON_COLLECTOR` | top-level | the standalone collector binary |
| `BUILD_SLICK_PERFMON_BENCH` | OFF | a measuring tool, not part of a normal build |

Both feature switches are baked into the installed `Config.cmake` as the value they had
here, so a downstream `find_package` consumer cannot silently get a different build of
the headers — and a disabled package does not make them install slick-queue or
slick-shm.

`cmake --install` honours `CMAKE_INSTALL_INCLUDEDIR`, and the exported `slick::perfmon`
target points at wherever the headers went. The `slick-perfmon-package` test checks
that end to end: it installs a fresh configure of the tree under a non-default include
directory and builds a `find_package` consumer against it. Being a nested configure and
build, it is the slowest test in the suite.

Configuring with `-DSLICK_PERFMON_ENABLED=OFF` builds and tests the whole tree as well.
The tests that assert measurement *works* are not registered in that configuration —
they cannot pass when every stamp has been compiled away — but the encoding, pairing,
histogram and accumulator tests still run, along with the one that proves the disabled
build costs nothing. The name-table test is not among them: its subject is a shared
memory segment, and a disabled build deliberately has no slick-shm to build it against.

## Benchmarking

```bash
./build/bench/perfmon_bench
./build/bench/perfmon_bench_disabled     # must be indistinguishable from an empty loop
```

Reports min and median cycles over repeated runs. Run-to-run spread on an unpinned
desktop is roughly 10%, so treat anything smaller as noise; the min is the number to
trust, since no sample can come in under the true cost but any sample can be inflated by
an interrupt.

Two blocks measure contention: the per-call cost of one publish while 1 to 8 threads
publish at a realistic rate, and what the stamps add to a measured span at the same
thread counts. The throughput table beside them is a flat-out race, so it measures how
fast the rings fill rather than what a stamp costs.

The last block measures the collector rather than the hot path: pairing one drained
event, accumulating one drained value (~65 cycles against ~90 for a paired stamp, as it
skips the span table), expiring abandoned spans, and rebuilding the report rows. None of it is part of
the overhead budget, but all of it is time the ring is not being drained, so it is what
decides whether a busy session drops samples.

## Compile-time tunables

Pre-define any of these, or set them via `target_compile_definitions`:

| Macro | Default | Meaning |
|---|---|---|
| `SLICK_PERFMON_ENABLED` | 1 | master switch |
| `SLICK_PERFMON_SERIALIZE` | 1 | fence the timestamp reads |
| `SLICK_PERFMON_NAME_CAPACITY` | 64 | bytes per name slot, including the NUL |
| `SLICK_PERFMON_HIST_PRECISION` | 4 | mantissa bits per histogram bucket (~3% error) |

## Future work

- Auto-generated `seq` values. Today the caller supplies one.
- Hierarchical spans with parent/child attribution and flame-graph output.
- A raw per-event dump for offline analysis.
- Native ARM cycle counters (`CNTVCT_EL0`).

## License

MIT. See [LICENSE](LICENSE).
