# Changelog

## v0.3.0 - 10-13-2026

Several changes here are breaking: the entries in bold under Changed.

### Added

- Value samples. `value(point, v)`, or `SLICK_PERFMON_VALUE`, records an unsigned 64-bit
  number that is a measurement in itself — a queue depth, a batch size — with no begin,
  no end and no timestamp. It reuses the 16-byte ring record and skips the TSC read, at
  ~29 cycles against ~196 for a stamp. The backend reports the same statistics as for a
  latency but as recorded: no unit conversion and no overhead subtraction.
- `Collector::snapshot_value()` for a value point's statistics, and `StageReport::value`
  to tell its row from a latency row in `reports()`.
- A `kind` column in the CSV, `latency` or `value`, saying what a row's figure columns
  hold. Value points are rows of the same CSV and the same summary table, under the
  stage name `value`; in the summary their figures print as bare numbers.
- `Collector::remove_shm(name)`, which removes both shared segments of a session. It is
  now the only thing that does — see below.
- `kValueStep`, `kMaxSpanStep` and `is_value_event()` beside the existing step constants.
- Benchmark cases for a value sample and for accumulating a drained value.

### Changed

- **Step `0xFE` is reserved** as the value marker. Span steps are `0x01..0xFD`; a
  `step(point, 0xFE)` is read back as a value sample.
- **The shared-memory layout version is 4**, because an event with step `0xFE` now means
  something else. A collector and a producer built either side of this release refuse to
  share a segment; rebuild them together.
- **The CSV header has changed** — `kind` follows `stage` — and anything reading the file
  by column position sees the columns from `count` onward move one place right. A CSV
  written by an earlier version is not appended to: `start()` throws
  `std::invalid_argument`, since rows in such a file cannot be told from value samples.
  Use a new `config::path` or move the old file aside.
- **The stage label `value` is reserved.** A `name_of` that returns it for a step of a
  span is treated as returning no name, and the stage gets the generated
  `step_<a>-><b>` label.
- **Shared segments are never removed implicitly.** Neither `shutdown()` nor
  `~Collector` unlinks the ring or the control block, so a collector restarted under
  running producers reopens the same segments and reports them under the names they
  published. On POSIX, where a segment outlives every process that mapped it, this means:
  - `/dev/shm/<shm_name>` and `<shm_name>.meta` remain after a session until
    `Collector::remove_shm()` is called;
  - a later session on the same name with a different `queue_capacity` or `max_names`
    throws a mismatch, and a renamed point keeps its first label, until the name is
    removed — including the same-`Collector` restart with a new `queue_capacity` that
    v0.2.0 made work;
  - a producer started with no collector running attaches to segments an earlier session
    left behind, returns `true` from `start()` and stamps at full cost into a ring
    nothing drains. "Costs nothing without a collector" holds only for a name with no
    segments.

  On Windows a segment is freed with its last handle, so it persists only while some
  process is attached.
- slick-queue 2.3.0 is now the minimum, for the build and for the installed package.

## v0.2.0 - 10-04-2026

### Changed

- The summary table has an `invalid` column, after `dropped`, so every point anomaly
  appears there. Anything reading the summary by column position sees the columns from
  `invalid` onward move one place right; the CSV already had it and is unchanged.
- The summary's `p50` and `p99` columns are always the run's real median and 99th
  percentile, taken from `config::percentiles` when it has them and computed from the
  histogram when it does not. `StageReport::summary_p50` and `summary_p99` carry them.
- slick-queue 2.1.0 is now the minimum, for the build and for the installed package.

### Fixed

- Percentiles take the documented nearest rank, `ceil(p / 100 * n)`, instead of
  rounding it: `p51` of `{1, 10000}` reported 1, though only half the samples are at or
  below it. The rank is computed in integers, exactly, over every count a `uint64_t` can
  hold — a floating-point `ceil()` would put `p99.9` of 1000 samples one rank too high.
- The summary printed the nearest configured percentile under its fixed `p50` and `p99`
  headings: with `percentiles = {90}` both columns showed the p90.
- A span discarded for a backwards timestamp showed in the summary as a row of zeros,
  because `invalid` was the one anomaly counter it did not print.
- `shutdown()` stopped its final drain at the first unpublished slot, silently dropping
  every event published behind it. It now drains everything reserved before it; holes
  share one grace period, the shorter of `stalled_sample_timeout` and 100 ms, and are
  then stepped over and counted in `stalled`.
- A shared-memory producer that kept publishing held `shutdown()` open for as long as it
  ran. The final drain now stops at what was reserved when shutdown began.
- `dump_summary()` read the stall count while the collector thread was incrementing it,
  a data race on every live call during a stall. The header now shows the count
  published with the rows beneath it.
- `dump_summary()` left the caller's stream in the summary's format — fixed notation,
  four decimals, its own fill and alignment. The stream's format is now restored.
- CSV and summary numbers follow the process's global locale no longer: a decimal comma
  or digit grouping put extra separators in a CSV row. Both are written in the classic
  locale; a stream passed to `dump_summary()` keeps its own.
- Restarting a shared session on the same `shm_name` reopened the previous session's
  segment, which the retired ring was still holding, and threw on a changed
  `queue_capacity`. The retired ring is now freed first.
- `cmake --install` with a custom `CMAKE_INSTALL_INCLUDEDIR` produced a package whose
  exported target still pointed at `include`, so every consumer failed at generation.
  The exported path now comes from the same variable the headers are installed with.

## v0.1.0 - 09-22-2026

Initial release.

### Measurement model

- Raw-event measurement: `begin` / `step` / `end` each publish one 16-byte timestamped
  event to a lock-free MPMC ring, and the backend reconstructs spans from them. No
  delta, no lookup and no thread-local state on the instrumented thread.
- Multi-step spans, reporting every stage transition plus the end-to-end total from the
  same stamps.
- Optional 24-bit correlation id (`seq`), so concurrent spans on a single point pair
  correctly.
- Spans may open and close in different functions, threads, DLLs or processes.
- `ScopedSample` RAII guard and `SLICK_PERFMON_SCOPE`, stamping through exception
  unwinding, with `cancel()` as the opt-out.
- User-defined point ids plus a `constexpr` `(point, step)` to name function; no
  registration table.

### Backend

- Statistics: count, min, max, mean, Welford standard deviation, and histogram
  percentiles with a bounded ~3% relative error.
- Anomaly reporting: `dropped`, `invalid`, `abandoned`, `orphan`, `stalled`,
  `out_of_range` — per interval in the CSV, as lifetime totals in `reports()` and the
  summary.
- Bounded, preallocated tables: `max_open_spans` open spans with amortised eviction,
  `max_stages` stage accumulators, `max_names` shared name slots. Nothing grows under
  load, and the surplus is counted rather than dropped.
- Continuous TSC calibration against `steady_clock`, refined over the whole run, and an
  invariant-TSC probe that says so in the summary when there is none.
- Startup measurement of the per-stamp floor, reported by `overhead_cycles()` and
  optionally subtracted from every stage (`config::subtract_overhead`, off by default).
- Introspection for sizing a session: `peak_open_spans()`, `last_flush_cycles()`,
  `name_conflicts()`.

### Deployment

- Three modes: in-process `local`, and `shared_producer` / `shared_collector` over
  shared memory, with a standalone `slick_perfmon_collector` executable.
- Producers publish their point and stage names into the segment; the collector binary
  needs no knowledge of them, and two peers disagreeing about one name is counted as a
  conflict rather than silently resolved.
- A producer whose collector is not running records nothing, creates no segment and
  costs nothing; `config::create_if_absent` reverses the startup order.
- `Collector::set_instance()` shares one collector across DLL boundaries.
- `SLICK_PERFMON_CSV`, `SLICK_PERFMON_SHM` and `SLICK_PERFMON_DISABLE` configure or
  auto-start a session without touching the instrumented binary; they only fill in
  fields left at their defaults.

### Output

- CSV time series, one row per stage per interval, carrying its own unit
  (`config::output_unit`: cycles, nanoseconds or microseconds), its own `tsc_hz`, and
  RFC 4180 quoting for user-supplied labels.
- Restarting against an existing file appends only onto a matching header; a session
  whose percentile list, unit or overhead correction disagrees with the file refuses to
  start rather than write rows the header labels wrongly.
- Rows with no samples are dropped, except where the counters are the content: an
  interval's anomalies, the `overflow` bucket for runs past `max_stages`, and a
  `collector` row for a dropped or stalled interval that produced nothing else.
- Human-readable summary table, written at shutdown and on demand via `dump_summary()`.
- `config::csv_flush_interval` trades per-interval durability for fewer writes on the
  collector thread.

### Build

- Header-only, C++20, MSVC / GCC / Clang on Windows, Linux and macOS. x86-64 uses
  RDTSC/RDTSCP; other architectures fall back to `steady_clock` and say so.
- `SLICK_PERFMON_ENABLED=0` compiles every stamp out - measured at 0.0 cycles in the
  benchmark - and drops the slick-queue and slick-shm dependencies with it. Both feature
  switches are baked into the installed `Config.cmake`, so a `find_package` consumer
  gets the headers as they were built.
- Runtime enable/disable costs nothing beyond the null check already on the path
  (~2.5 cycles per stamp).
- Compile-time tunables: `SLICK_PERFMON_SERIALIZE`, `SLICK_PERFMON_NAME_CAPACITY`,
  `SLICK_PERFMON_HIST_PRECISION`.

### Measured cost

On an AMD Ryzen 9 5900HX (3.29 GHz TSC, Windows, unpinned): ~205 cycles for a single
fenced stamp and ~195 per stamp in a span, ~130-172 with `SLICK_PERFMON_SERIALIZE=0`.
Per-stamp cost is flat across span length, and a runtime `seq` is free. See the README
for the full table.
