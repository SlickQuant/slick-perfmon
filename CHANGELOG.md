# Changelog

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
