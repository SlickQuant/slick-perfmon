// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Slick Quant
//
// slick-perfmon backend.
//
// Nothing here runs on an instrumented thread. Pairing, statistics, unit
// conversion, formatting and file I/O all happen on the collector - in `local`
// mode a background thread, in the shared modes a separate process entirely.

#pragma once

#include <slick/perfmon.hpp>
#include <slick/perfmon/config.hpp>
#include <slick/perfmon/types.hpp>

#if SLICK_PERFMON_ENABLED

#include <slick/perfmon/control_block.hpp>
#include <slick/perfmon/detail/open_map.hpp>
#include <slick/perfmon/detail/tsc.hpp>
#include <slick/perfmon/pairing.hpp>
#include <slick/perfmon/queue.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <stdexcept>
#include <iomanip>
#include <limits>
#include <locale>
#include <memory>
#include <mutex>
#include <ostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace slick::perfmon {

/// One stage as published to readers: labels resolved, units applied.
struct StageReport {
    point_id    point = 0;
    uint8_t     from  = 0;
    uint8_t     to    = 0;
    std::string point_name;
    std::string stage_name;
    stats       interval;    ///< since the previous flush; what the CSV shows
    stats       cumulative;  ///< whole run; what the summary shows

    /// No sample landed in this row's interval, but its anomaly counters moved
    /// - the counts are the row's entire content. The CSV drops rows with no
    /// samples, and this is what exempts one from that, once, in the interval
    /// the anomaly actually happened.
    ///
    /// False on a row that merely carries a lifetime anomaly total already
    /// reported: such a row stays in reports() and in the summary, which are
    /// snapshots, but repeating it down a time series would be noise and a
    /// column nobody could sum.
    bool anomalies_only = false;

    /// The synthetic row carrying anomalies that overflowed the per-point
    /// table. `point` names nothing on it - the counts belong to however many
    /// points did not fit - so anything keying on the id has to test this
    /// first.
    ///
    /// A flag rather than a reserved id: point_id is the user's whole 32-bit
    /// space to partition, so no value in it can be borrowed as a marker
    /// without colliding with somebody's real stage, and a collision here is
    /// two different things answering to one (point, from, to) key.
    bool overflow = false;
};

namespace detail {

/// "p99.9" -> "p99_9", so the column name survives a CSV round trip.
///
/// The label is the schema: start() decides whether an existing file can be
/// appended to by comparing header text, so two configurations that differ in
/// a percentile have to differ in a column name. At the stream's default six
/// significant digits they need not - 99.9999999 and 99.99999991 both print as
/// "100" - and two sessions asking for different numbers would then agree on a
/// header and append incomparable rows under it, which is the exact failure
/// the header check exists to prevent.
///
/// So: the shortest form that reads back as the same double, found by raising
/// the precision until it does. Starting at the default and stopping as soon
/// as the value survives the trip is what keeps the ordinary labels spelled
/// the ordinary way - max_digits10 unconditionally would render 99.9 as
/// "99.900000000000006" and rename a column every existing file already has.
///
/// Both directions go through the classic locale. A global locale with a comma
/// for a decimal point would otherwise put "99,9" in a header - a column name
/// with a separator in it - and would break the round-trip test that decides
/// when to stop, making the schema depend on the locale of whoever started the
/// process.
inline std::string percentile_label(double p) {
    std::string s;
    for (int prec = 6; prec <= std::numeric_limits<double>::max_digits10; ++prec) {
        std::ostringstream os;
        os.imbue(std::locale::classic());
        os << std::setprecision(prec) << p;
        s = os.str();

        std::istringstream is(s);
        is.imbue(std::locale::classic());
        double back = 0.0;
        is >> back;
        if (!is.fail() && back == p) {
            break;
        }
    }
    for (char& c : s) {
        if (c == '.') {
            c = '_';
        }
    }
    return "p" + s;
}

/// Column suffix naming config::output_unit.
inline const char* unit_suffix(unit u) noexcept {
    switch (u) {
    case unit::cycles:       return "cycles";
    case unit::microseconds: return "us";
    case unit::nanoseconds:
    default:                 return "ns";
    }
}

/**
 * @brief The CSV header for a given configuration.
 *
 * Every anomaly counter stats exposes gets a column. `stalled` is
 * collector-wide and so repeats down the file, but a row that does not carry
 * it is a row you cannot judge on its own - and judging a row on its own is
 * the entire point of a machine-readable format.
 *
 * A function rather than a literal inside open_csv() because the schema has
 * two readers, not one: the collector writes it, and start() compares it with
 * the header already in an existing file. Everything that changes what a
 * number in a row *means* therefore has to be visible here, or two sessions
 * that disagree about it would share a header and append to each other:
 *
 *  - the percentile list, which decides which columns exist;
 *  - config::output_unit, as a suffix on every latency column, because
 *    cycles, nanoseconds and microseconds are three incomparable scales
 *    behind identical column names and nothing in a bare number says which;
 *  - config::subtract_overhead, as the presence of the `overhead` column,
 *    because a corrected figure and a raw one differ by a systematic offset
 *    that no reader could detect. The column carries the offset as well, so a
 *    reader can put it back.
 */
inline std::string csv_header(const std::vector<double>& percentiles, unit u,
                              bool subtract_overhead) {
    const std::string sfx = std::string("_") + unit_suffix(u);
    std::string       h   = "timestamp,point,stage,count,dropped,invalid,stalled,"
                            "abandoned,orphan,out_of_range";
    h += ",min" + sfx;
    h += ",mean" + sfx;
    h += ",stddev" + sfx;
    for (double p : percentiles) {
        h += ',';
        h += percentile_label(p);
        h += sfx;
    }
    h += ",max" + sfx;
    h += ",tsc_hz";
    if (subtract_overhead) {
        h += ",overhead" + sfx;
    }
    return h;
}

/**
 * @brief RFC 4180 quoting for one CSV field, straight to the stream.
 *
 * Point and stage labels come from the user's own constexpr table, so they can
 * contain anything - a comma in "decode, verify" alone is enough to shift every
 * later column by one and make the file parse into silent nonsense. Quoting is
 * applied here rather than in resolve_*_name() because the summary table wants
 * the bare name; only the CSV has a syntax to violate.
 *
 * Writes instead of returning a std::string: the common field needs no quoting
 * at all, and building one throwaway string per label per row per flush is an
 * allocation the collector can simply not make.
 */
inline void write_csv_field(std::ostream& os, std::string_view s) {
    if (s.find_first_of(",\"\r\n") == std::string_view::npos) {
        os.write(s.data(), static_cast<std::streamsize>(s.size()));
        return;
    }
    os.put('"');
    for (char c : s) {
        if (c == '"') {
            os.put('"');  // a quote is escaped by doubling it
        }
        os.put(c);
    }
    os.put('"');
}

inline std::string format_wall_clock(std::chrono::system_clock::time_point tp) {
    const auto     t  = std::chrono::system_clock::to_time_t(tp);
    const auto     ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            tp.time_since_epoch()).count() % 1000;
    std::tm tm{};
#if defined(_MSC_VER)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms));
    return buf;
}

/// Whether a row's counters say something even with no samples behind them.
/// `dropped` and `stalled` are deliberately not here: they are collector-wide
/// and already repeat on every row, so they can never be the only reason a
/// particular row has to be printed. An interval that writes no row at all
/// still has to report them, and Collector::write_csv() gives them their own.
constexpr bool has_anomaly(const stats& s) noexcept {
    return s.invalid != 0 || s.abandoned != 0 || s.orphan != 0 || s.out_of_range != 0;
}

/// Render a duration with a unit suffix, for the human-readable summary only.
/// The CSV stays unit-less numbers so it loads without parsing.
inline std::string format_duration(double ns) {
    char buf[32];
    if (ns < 1000.0) {
        std::snprintf(buf, sizeof(buf), "%.1fns", ns);
    } else if (ns < 1e6) {
        std::snprintf(buf, sizeof(buf), "%.2fus", ns / 1e3);
    } else if (ns < 1e9) {
        std::snprintf(buf, sizeof(buf), "%.2fms", ns / 1e6);
    } else {
        std::snprintf(buf, sizeof(buf), "%.2fs", ns / 1e9);
    }
    return buf;
}

inline const char* env_or_null(const char* name) {
#if defined(_MSC_VER)
    // getenv is deprecated under MSVC's secure-CRT warnings, and _dupenv_s
    // would need a free() on every call. The value is only read at startup.
    size_t      len = 0;
    static thread_local char buf[1024];
    if (getenv_s(&len, buf, sizeof(buf), name) != 0 || len == 0) {
        return nullptr;
    }
    return buf;
#else
    return std::getenv(name);
#endif
}

}  // namespace detail

/**
 * @brief Owns the ring, the control block and (outside shared_producer mode)
 *        the thread that turns events into statistics.
 *
 * One instance per process is the norm; instance() provides it. The indirection
 * behind instance() exists so a plugin loaded from a DLL can be pointed at the
 * host's collector rather than starting a second one.
 */
class Collector {
public:
    Collector() = default;

    ~Collector() { shutdown(); }

    Collector(const Collector&)            = delete;
    Collector& operator=(const Collector&) = delete;
    Collector(Collector&&)                 = delete;
    Collector& operator=(Collector&&)      = delete;

    static Collector& instance() noexcept { return *instance_ptr().load(std::memory_order_acquire); }

    /// Redirect this module's instance() at another module's collector.
    static void set_instance(Collector* c) noexcept {
        instance_ptr().store(c != nullptr ? c : &default_instance(), std::memory_order_release);
    }

    /**
     * @brief Open the transport and, where applicable, start the collector thread.
     *
     * Restartable: calling start() again shuts the previous session down first.
     *
     * Returns false when instrumentation stays off - the environment disabled
     * it, or this is a shared_producer and no collector is running. That is a
     * normal, cheap outcome, not an error: an instrumented binary in production
     * with no collector attached should cost nothing and say so, rather than
     * create a segment nobody drains.
     */
    bool start(config cfg = {}) {
        shutdown();

        cfg_ = std::move(cfg);
        if (!apply_environment()) {
            return false;
        }
        if (!validate_config()) {
            return false;
        }
        if (!open_transport()) {
            return false;
        }

        // Anchor the read cursor here, not on the collector thread. The thread
        // calibrates the TSC before its first drain, and anything stamped
        // during those milliseconds would be skipped if the cursor were taken
        // afterwards - initial_reading_index() means "start from now", and by
        // then "now" is already past the caller's first samples.
        cursor_.store(ring_->initial_reading_index(), std::memory_order_release);

        name_conflicts_ = control_.publish_names(cfg_.point_count, cfg_.name_of);

        started_ = true;

        if (cfg_.run_mode != mode::shared_producer) {
            running_.store(true, std::memory_order_release);
            thread_ = std::thread([this] { run(); });
        }

        // Publish the ring to this module's hot path last, so no stamp can land
        // before the collector is ready to pair it.
        enabled_.store(true, std::memory_order_relaxed);
        attach(ring_.get());
        return true;
    }

    void shutdown() {
        if (!started_) {
            return;
        }

        // Detach producers in this module first, so the final drain sees a
        // quiescent ring.
        attach(nullptr);
        enabled_.store(false, std::memory_order_relaxed);

        if (thread_.joinable()) {
            running_.store(false, std::memory_order_release);
            thread_.join();
        }

        // The ring is retired rather than freed. A thread that loaded the hot
        // pointer just before the detach above may still be inside stamp(), and
        // freeing under it would be a use-after-free. Holding it until this
        // Collector is destroyed costs one allocation per start/shutdown cycle
        // and removes the common case of that race. A stamp still in flight
        // when ~Collector runs remains the caller's responsibility to avoid.
        if (ring_) {
            retired_rings_.push_back(std::move(ring_));
        }
        control_.reset();

        // The stream has to be closed here, not merely left alone. open_csv()
        // only opens when the stream is closed, so a Collector restarted with a
        // different config::path would keep appending to the previous session's
        // file - the new path never opened, and the rows landing somewhere the
        // caller stopped looking. clear() drops any failbit a failed open left
        // behind, which would otherwise make every later session silent.
        csv_.close();
        csv_.clear();
        csv_buffer_.clear();
        csv_buffer_.shrink_to_fit();

        // reports_ deliberately survives - a caller may still read the final
        // snapshot - but the flush scratch behind it has no readers, and a
        // shut-down collector should not sit on it.
        build_rows_.clear();
        build_rows_.shrink_to_fit();
        point_names_.reset(0);
        stage_names_.reset(0);
        row_points_.reset(0);
        point_baseline_.reset(0);
        cached_name_generation_ = 0;

        started_ = false;
    }

    bool running() const noexcept { return started_; }

    /// Live on/off switch. Costs the producer nothing beyond the null check it
    /// already performs, because the switch *is* that pointer.
    void set_enabled(bool on) noexcept {
        if (!started_) {
            return;
        }
        enabled_.store(on, std::memory_order_relaxed);
        attach(on ? ring_.get() : nullptr);
    }

    bool enabled() const noexcept { return started_ && enabled_.load(std::memory_order_relaxed); }

    /// Drain everything published so far, fold it in, and write a CSV row set.
    /// Blocks until the collector has caught up, so a caller can read a
    /// snapshot immediately afterwards and know it includes their events.
    void flush() {
        if (!started_ || !thread_.joinable()) {
            return;
        }
        const uint64_t target = ring_->initial_reading_index();
        wait_until([&] { return cursor_.load(std::memory_order_acquire) >= target; });

        const uint64_t gen = flush_request_.fetch_add(1, std::memory_order_acq_rel) + 1;
        wait_until([&] { return flush_done_.load(std::memory_order_acquire) >= gen; });
    }

    /// Cumulative statistics for one transition. Flushes first, so the answer
    /// includes everything the caller has published.
    stats snapshot(point_id p, uint8_t from, uint8_t to) {
        flush();
        std::lock_guard<std::mutex> lock(report_mutex_);
        for (const StageReport& r : reports_) {
            // The overflow row carries no point of its own, so a request for a
            // real one must not be answered with it - whatever id it holds.
            if (!r.overflow && r.point == p && r.from == from && r.to == to) {
                return r.cumulative;
            }
        }
        return {};
    }

    /// Cumulative begin -> end statistics for a point.
    stats snapshot_total(point_id p) { return snapshot(p, kBeginStep, kBeginStep); }

    /// Every stage, as of the last flush.
    std::vector<StageReport> reports() {
        flush();
        std::lock_guard<std::mutex> lock(report_mutex_);
        return reports_;
    }

    void dump_summary(std::ostream& os) {
        flush();
        std::lock_guard<std::mutex> lock(report_mutex_);
        write_summary(os);
    }

    double tsc_hz() const noexcept { return clock_hz_.load(std::memory_order_acquire); }

    bool invariant_tsc() const noexcept { return invariant_.load(std::memory_order_acquire); }

    /// Measured cost of one stamp, in cycles. Subtracted from every transition
    /// when config::subtract_overhead is set.
    uint64_t overhead_cycles() const noexcept {
        return overhead_cycles_.load(std::memory_order_acquire);
    }

    /// Cycles the collector spent inside its most recent flush: the sweep, the
    /// report rows and the CSV write.
    ///
    /// Worth watching because a flush is time the collector is not draining,
    /// and the ring keeps filling meanwhile. It cannot be timed from outside -
    /// flush() is a handshake with a thread that may be asleep, so a caller
    /// measures the wake-up, which on Windows is a 15.6 ms timer tick however
    /// long the flush itself took.
    uint64_t last_flush_cycles() const noexcept {
        return last_flush_cycles_.load(std::memory_order_acquire);
    }

    /// Slots the (point, seq) table reached at its busiest. Worth watching when
    /// spans overlap heavily: approaching max_open_spans means evictions, and
    /// evictions are counted as abandoned.
    size_t peak_open_spans() const noexcept { return peak_open_.load(std::memory_order_acquire); }

    /// Names that disagreed with an already-published entry, plus names too
    /// long for a slot.
    uint32_t name_conflicts() const noexcept { return name_conflicts_; }

    sample_queue* ring() const noexcept { return ring_.get(); }

    const config& configuration() const noexcept { return cfg_; }

private:
    // ------------------------------------------------------------------
    // Singleton plumbing
    // ------------------------------------------------------------------

    static Collector& default_instance() {
        static Collector c;
        return c;
    }

    static std::atomic<Collector*>& instance_ptr() {
        static std::atomic<Collector*> p{&default_instance()};
        return p;
    }

    // ------------------------------------------------------------------
    // Startup
    // ------------------------------------------------------------------

    /// @return false when the environment turned instrumentation off.
    bool apply_environment() {
        if (const char* off = detail::env_or_null("SLICK_PERFMON_DISABLE")) {
            if (off[0] != '\0' && off[0] != '0') {
                return false;
            }
        }
        // Env vars fill in only what the caller left at its default, so an
        // explicit config always wins and a binary can still be redirected
        // without a rebuild.
        if (cfg_.shm_name.empty()) {
            if (const char* shm = detail::env_or_null("SLICK_PERFMON_SHM")) {
                cfg_.shm_name = shm;
                cfg_.run_mode = mode::shared_producer;
            }
        }
        if (const char* csv = detail::env_or_null("SLICK_PERFMON_CSV")) {
            if (cfg_.path == "perfmon.csv") {
                cfg_.path = csv;
            }
        }
        return true;
    }

    bool validate_config() {
        if (cfg_.queue_capacity < 2 || (cfg_.queue_capacity & (cfg_.queue_capacity - 1)) != 0) {
            throw std::invalid_argument(
                "slick-perfmon: queue_capacity must be a power of two and at least 2");
        }
        if (cfg_.run_mode != mode::local && cfg_.shm_name.empty()) {
            throw std::invalid_argument("slick-perfmon: the shared modes require a shm_name");
        }
        for (double p : cfg_.percentiles) {
            // A NaN passes every bounds check a percentile is given, every
            // comparison against it being false, so it would reach the rank
            // arithmetic in the histogram and be cast to an integer - which is
            // undefined behaviour, from one bad number in a configuration
            // vector. Histogram::percentile() is written so it cannot happen
            // there either, but the mistake belongs here, where it was made
            // and where it can still be named.
            if (!std::isfinite(p) || p < 0.0 || p > 100.0) {
                throw std::invalid_argument(
                    "slick-perfmon: every config::percentiles entry must be a finite "
                    "number in [0, 100]");
            }
        }
        validate_csv_schema();
        return true;
    }

    /**
     * @brief Refuse to append rows the file's existing header does not describe.
     *
     * A header is written only for a new or empty file, so a restart against
     * the same path continues one time series - which is right until the
     * columns change, or until the same columns start meaning something else.
     * The percentile list decides which columns exist; config::output_unit and
     * config::subtract_overhead decide what the numbers under them are. A
     * session that disagrees with the file about any of the three would append
     * rows the header on disk labels wrongly, and nothing in the file would say
     * so. No parser can detect that: it reads the wrong number under the right
     * name and is never given a reason to doubt it. Which is why all three are
     * part of the header - see detail::csv_header().
     *
     * A configuration error rather than grounds to truncate, and for the same
     * reason the layout checks in the control block throw: the rows already
     * there are somebody's data, and a collector that silently corrupts its own
     * output is worse than one that refuses to start.
     */
    void validate_csv_schema() const {
        // A shared producer runs no collector thread and so never flushes,
        // never opens the file, and must not be refused over a schema it was
        // never going to write - config::path keeps its default in a producer
        // that simply never set it.
        if (cfg_.path.empty() || cfg_.run_mode == mode::shared_producer) {
            return;
        }
        const std::string have = existing_csv_header();
        if (have.empty() || have == this_csv_header()) {
            return;  // nothing there yet, or the same schema this run writes
        }
        throw std::invalid_argument(
            "slick-perfmon: config::path '" + cfg_.path +
            "' already holds a CSV whose header does not match this configuration's "
            "columns - appending would change the schema mid-file. Use a different "
            "path, remove the file, or restore the percentile list, output_unit and "
            "subtract_overhead it was written with. On disk: '" + have +
            "'; this session: '" + this_csv_header() + "'.");
    }

    /**
     * @brief Open the ring and the control block for the configured mode.
     *
     * Two kinds of failure, deliberately handled differently. A segment that is
     * not there means no collector is running: normal, expected, and answered
     * with a quiet `false`. A segment that is there but disagrees about its
     * layout is a configuration error, and that propagates - silently running
     * unmeasured because two peers were built differently is exactly the kind
     * of thing that goes unnoticed for months.
     */
    bool open_transport() {
        const std::string meta = cfg_.shm_name + ".meta";

        switch (cfg_.run_mode) {
        case mode::local:
            ring_ = std::make_unique<sample_queue>(cfg_.queue_capacity);
            control_.create_local(cfg_.max_names);
            return true;

        case mode::shared_collector:
            ring_ = std::make_unique<sample_queue>(cfg_.queue_capacity, cfg_.shm_name.c_str());
            return control_.create_shared(meta, cfg_.max_names);

        case mode::shared_producer:
            if (cfg_.create_if_absent) {
                ring_ = std::make_unique<sample_queue>(cfg_.queue_capacity,
                                                        cfg_.shm_name.c_str());
                return control_.create_shared(meta, cfg_.max_names);
            }
            // Attach only. Nobody there is the safe default: the producer stays
            // silent and free rather than filling a ring nothing drains.
            try {
                ring_ = std::make_unique<sample_queue>(cfg_.shm_name.c_str());
            } catch (const std::exception&) {
                ring_.reset();
                return false;
            }
            // Outside the catch on purpose: a mismatch here is a misconfigured
            // peer, not an absent collector.
            return control_.attach_shared(meta, cfg_.max_names);
        }
        return false;
    }

    // ------------------------------------------------------------------
    // Collector thread
    // ------------------------------------------------------------------

    /// Everything that describes *this* run rather than the Collector object.
    ///
    /// A Collector is restartable, and these outlive a shutdown() on the
    /// instance. A high-water mark or an anomaly total carried in from the
    /// previous session would be read as something this session did - which is
    /// exactly the kind of number this library exists to get right.
    void reset_session_counters() noexcept {
        stalled_slots_     = 0;
        last_loss_         = 0;
        reported_stalled_  = 0;
        overflow_baseline_ = PointAnomalies{};
        stalled_since_     = {};
        // Anchored to the start of the run, not left at the clock's epoch: the
        // first interval would otherwise always be overdue and push, making
        // csv_flush_interval mean something different for the first flush than
        // for every one after it.
        last_csv_flush_ = std::chrono::steady_clock::now();
        peak_open_.store(0, std::memory_order_release);
        last_flush_cycles_.store(0, std::memory_order_release);
    }

    void run() {
        clock_.calibrate(cfg_.calibration_time);
        publish_clock();

        // Always measured, never conditionally: the floor is what tells you
        // whether a 60 ns figure is a measurement or an artefact of measuring.
        // Only the subtraction is optional.
        const uint64_t ov = measure_overhead();
        overhead_cycles_.store(ov, std::memory_order_release);

        // Order matters: reset() clears the overhead along with the tables, so
        // setting it first would leave every session reporting raw timings no
        // matter how config::subtract_overhead was set.
        pairer_.reset(cfg_);
        if (cfg_.subtract_overhead) {
            pairer_.set_overhead(ov);
        }

        reset_session_counters();

        // Sized like the stage table, which bounds all three: a run cannot
        // report more stages than it can accumulate, and it cannot touch more
        // distinct points than stages.
        point_names_.reset(cfg_.max_stages);
        stage_names_.reset(cfg_.max_stages);
        row_points_.reset(cfg_.max_stages);
        point_baseline_.reset(cfg_.max_stages);
        cached_name_generation_ = control_.name_generation();

        run_start_ = std::chrono::steady_clock::now();

        auto     next_flush = run_start_ + cfg_.flush_interval;
        uint32_t idle       = 0;

        // Yield for a short burst before sleeping. Unlike slick-logger there is
        // no park/wake handshake: nobody waits on a perf sample, so paying the
        // producer an atomic load per stamp to wake us promptly would be a
        // straight loss - and it could not work across processes anyway.
        constexpr uint32_t kIdleYieldLimit = 64;

        while (running_.load(std::memory_order_acquire)) {
            const bool progressed = drain_once();

            const auto now = std::chrono::steady_clock::now();
            if (now >= next_flush) {
                do_flush(now);
                next_flush = now + cfg_.flush_interval;
            }

            const uint64_t req = flush_request_.load(std::memory_order_acquire);
            if (req != flush_done_.load(std::memory_order_relaxed)) {
                do_flush(std::chrono::steady_clock::now());
                flush_done_.store(req, std::memory_order_release);
            }

            if (progressed) {
                idle = 0;
            } else if (idle < kIdleYieldLimit) {
                ++idle;
                std::this_thread::yield();
            } else {
                // On Windows this "1 ms" is really the ~15.6 ms timer
                // granularity, which is exactly what we want here: the ring is
                // sized to absorb that much production, and the collector costs
                // almost no CPU in return.
                std::this_thread::sleep_for(cfg_.poll_interval);
            }
        }

        while (drain_once()) {
        }
        do_flush(std::chrono::steady_clock::now());
        flush_done_.store(flush_request_.load(std::memory_order_acquire), std::memory_order_release);
        write_summary_file();
    }

    bool drain_once() {
        uint64_t c = cursor_.load(std::memory_order_relaxed);
        auto [ptr, n] = ring_->read(c);
        cursor_.store(c, std::memory_order_release);

        if (ptr == nullptr || n == 0) {
            check_stall();
            return false;
        }

        stalled_since_ = {};
        for (uint32_t i = 0; i < n; ++i) {
            pairer_.on_event(ptr[i]);
        }
        return true;
    }

    /**
     * @brief Step over a slot a producer reserved but never published.
     *
     * A producer killed between reserve() and publish() leaves a hole, and
     * read() will wait behind it forever. If the write cursor has moved past
     * our read cursor but read() still yields nothing, we are behind such a
     * hole; after the configured timeout we skip it and carry on. Everything
     * published behind the hole is still valid, so abandoning one slot is much
     * better than losing the rest of the run.
     */
    void check_stall() {
        const uint64_t head = ring_->initial_reading_index();
        const uint64_t cur  = cursor_.load(std::memory_order_relaxed);
        if (cur >= head) {
            stalled_since_ = {};
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (stalled_since_ == std::chrono::steady_clock::time_point{}) {
            stalled_since_ = now;
            return;
        }
        if (now - stalled_since_ < cfg_.stalled_sample_timeout) {
            return;
        }

        cursor_.store(cur + 1, std::memory_order_release);
        ++stalled_slots_;
        stalled_since_ = {};
    }

    /**
     * @brief Cost of one stamp, in cycles.
     *
     * Driven against a private ring so the live one is not polluted, and via
     * the same stamp_into() the hot path uses so the number cannot drift away
     * from the code it describes.
     *
     * The minimum, not the mean: any sample can be inflated by an interrupt or
     * a migration, but none can come in under the true cost.
     */
    uint64_t measure_overhead() {
        constexpr int      kIters    = 1000;
        constexpr uint32_t kCapacity = 4096;  // > 2 * kIters, so nothing wraps

        sample_queue cal(kCapacity);
        for (int i = 0; i < kIters; ++i) {
            detail::stamp_into<true>(cal, 0, make_event(kBeginStep));
            detail::stamp_into<false>(cal, 0, make_event(kEndStep));
        }

        std::vector<uint64_t> ts;
        ts.reserve(static_cast<size_t>(kIters) * 2);
        uint64_t cursor = 0;
        while (true) {
            auto [ptr, n] = cal.read(cursor);
            if (ptr == nullptr || n == 0) {
                break;
            }
            for (uint32_t i = 0; i < n; ++i) {
                ts.push_back(ptr[i].timestamp);
            }
        }

        uint64_t best = 0;
        bool     any  = false;
        for (size_t i = 0; i + 1 < ts.size(); i += 2) {
            if (ts[i + 1] < ts[i]) {
                continue;
            }
            const uint64_t d = ts[i + 1] - ts[i];
            if (!any || d < best) {
                best = d;
                any  = true;
            }
        }
        return any ? best : 0;
    }

    void publish_clock() {
        clock_hz_.store(clock_.hz(), std::memory_order_release);
        invariant_.store(clock_.invariant(), std::memory_order_release);
        control_.publish_calibration(clock_.hz(), clock_.invariant());
    }

    // ------------------------------------------------------------------
    // Flush
    // ------------------------------------------------------------------

    void do_flush(std::chrono::steady_clock::time_point now) {
        // Refine the TSC estimate from the whole run rather than this interval,
        // so it gets better the longer the process lives, for free.
        const uint64_t flush_start = detail::rdtsc_step();
        clock_.update(flush_start, now);
        publish_clock();

        if (const double hz = clock_.hz(); hz > 0.0) {
            const auto ms  = cfg_.stalled_sample_timeout.count();
            const auto age = static_cast<uint64_t>(hz * static_cast<double>(ms) / 1000.0);
            pairer_.sweep(detail::rdtsc_step(), age);
        }

        const uint64_t loss       = ring_ ? ring_->loss_count() : 0;
        const uint64_t new_losses = loss > last_loss_ ? loss - last_loss_ : 0;
        last_loss_ = loss;

        // The other collector-wide delta, and taken here rather than inside
        // build_reports() because both passes below need it: the rows carry it
        // as a column, and the writer needs it again to know whether an
        // interval that produced no row still has something to say.
        const uint64_t new_stalled = stalled_slots_ - reported_stalled_;
        reported_stalled_          = stalled_slots_;

        peak_open_.store(pairer_.peak_open_spans(), std::memory_order_release);

        build_reports(build_rows_, new_losses, new_stalled);
        write_csv(build_rows_, now, new_losses, new_stalled);

        {
            std::lock_guard<std::mutex> lock(report_mutex_);
            // Swapped rather than moved. What comes back is the rows published
            // two flushes ago, and every one of them still owns its label
            // strings and its percentile vector at the right size - so the next
            // build fills them in place and a steady-state flush allocates
            // nothing at all. Moving would have thrown that storage away and
            // reallocated the whole set every interval.
            reports_.swap(build_rows_);
        }

        pairer_.clear_intervals();

        last_flush_cycles_.store(detail::rdtsc_step() - flush_start,
                                 std::memory_order_release);
    }

    /// The row being filled, reusing the one left in `out` by an earlier flush
    /// where there is one. Stage counts only ever grow, so after the first
    /// couple of intervals this never reaches the emplace.
    static StageReport& next_row(std::vector<StageReport>& out, size_t& n) {
        if (n == out.size()) {
            out.emplace_back();
        }
        return out[n++];
    }

    /// Interval statistics come from the interval accumulator, cumulative from
    /// the lifetime one - so the merge has to happen between reading the two.
    ///
    /// `out` arrives holding an earlier flush's rows and is refilled in place;
    /// see do_flush() for why it is swapped rather than moved.
    void build_reports(std::vector<StageReport>& out, uint64_t new_losses,
                       uint64_t new_stalled) {
        refresh_name_cache();
        row_points_.clear();

        // Where a point's rows are, so the pass below can find the one that
        // stands for it without re-scanning what has been built. Answered from
        // a table rather than by that scan because the scan was quadratic in
        // points x stages, which is precisely the shape a high-cardinality
        // session has.
        const auto note_row = [&](point_id p, size_t index, const StageReport& r) {
            RowRef* ref = row_points_.find(p);
            if (ref == nullptr) {
                ref = row_points_.insert(p, RowRef{static_cast<uint32_t>(index), false});
                if (ref == nullptr) {
                    return;  // more distinct points than max_stages; see below
                }
            } else if (r.from == kBeginStep && r.to == kBeginStep) {
                // The total row is the point's own summary, so where there is
                // a choice it is the row an anomaly belongs on.
                ref->carrier = static_cast<uint32_t>(index);
            }
            ref->has_samples = ref->has_samples || r.interval.count != 0;
        };

        size_t n = 0;
        pairer_.for_each_stage([&](uint64_t, Stage& g) {
            StageReport& r   = next_row(out, n);
            r.point          = g.point;
            r.from           = g.from;
            r.to             = g.to;
            r.anomalies_only = false;
            r.overflow       = false;
            resolve_point_name(g.point, r.point_name);
            resolve_stage_name(g.point, g.from, g.to, r.stage_name);

            // The pairer's counters are lifetime totals, so the interval gets
            // what is new since the last flush and the cumulative row gets the
            // total. anomalies_of() rather than anomalies() because this path
            // must not insert: an entry for a point that never had an anomaly
            // takes a slot from one that did.
            const PointAnomalies an = pairer_.anomalies_of(g.point);
            to_stats(r.interval, g.interval, an - baseline_of(g.point), new_losses,
                     new_stalled);
            g.interval.merge_into(g.cumulative);
            to_stats(r.cumulative, g.cumulative, an, last_loss_, stalled_slots_);

            note_row(g.point, n - 1, r);
        });

        // An anomaly row is a total row with an empty accumulator: no samples
        // to report, only the counts. The per-point rows and the overflow row
        // after them differ in nothing but what names them, so they are built
        // the same way.
        const Accumulator& empty = empty_accumulator();
        auto anomaly_row = [&](point_id p, const PointAnomalies& delta,
                               const PointAnomalies& total) -> StageReport& {
            StageReport& r = next_row(out, n);
            r.point        = p;
            r.from         = kBeginStep;
            r.to           = kBeginStep;
            r.stage_name   = "total";
            // The row exists for as long as the lifetime counts do, so
            // reports() and the summary keep them; only an interval that
            // actually moved gets written to the time series.
            r.anomalies_only = any_anomaly(delta);
            r.overflow       = false;
            to_stats(r.interval, empty, delta, new_losses, new_stalled);
            to_stats(r.cumulative, empty, total, last_loss_, stalled_slots_);
            return r;
        };

        pairer_.for_each_point([&](point_id p, PointAnomalies& an) {
            if (!any_anomaly(an)) {
                return;  // nothing has ever gone wrong on this point
            }
            const PointAnomalies delta = an - baseline_of(p);

            // Every row above has reported what was new, so the baseline moves
            // up here - in the pass that is already walking this table rather
            // than a second one after it. The next interval then reports what
            // happens between now and then; without this an anomaly would
            // repeat on every row of every later interval for the rest of the
            // run, which is both noise and a column nobody could sum.
            if (any_anomaly(delta)) {
                if (PointAnomalies* b = point_baseline_.find(p)) {
                    *b = an;
                } else {
                    point_baseline_.insert(p, an);
                }
            }

            RowRef* ref = row_points_.find(p);
            if (ref == nullptr) {
                // A point can accumulate anomalies without ever completing a
                // span - a producer that only opens them, or one whose ends
                // were all dropped. It has no stage row at all, so its counts
                // would exist and be invisible. Give it an empty total row.
                StageReport& r = anomaly_row(p, delta, an);
                resolve_point_name(p, r.point_name);
                return;
            }
            if (ref->has_samples || !any_anomaly(delta)) {
                // Already carried on a row the CSV is going to write, or
                // nothing new to write about.
                return;
            }
            // The point has rows, but every one of them is empty this interval
            // and so would be dropped from the file, counters and all. Exempt
            // the one that stands for the point - one row, not one per stage,
            // because the counters are per point and a reader summing a column
            // would otherwise count the same orphan once per stage.
            out[ref->carrier].anomalies_only = true;
        });

        // The per-point table is sized like the stage table, so a run touching
        // more distinct points than max_stages folds the surplus into one
        // shared bucket. for_each_point() cannot reach it - it belongs to no
        // point - so it gets its own row here. Without this the run that most
        // needs its anomaly counts, the one already past its configured size,
        // is the one run that never sees them.
        if (const PointAnomalies ov = pairer_.overflow_anomalies(); any_anomaly(ov)) {
            StageReport& r = anomaly_row(0, ov - overflow_baseline_, ov);
            r.point_name   = "overflow";
            r.overflow     = true;
        }

        overflow_baseline_ = pairer_.overflow_anomalies();

        out.resize(n);

        std::sort(out.begin(), out.end(), [](const StageReport& a, const StageReport& b) {
            // The overflow row belongs to no point, so it sorts after all of
            // them rather than at whatever id it happens to carry.
            if (a.overflow != b.overflow) {
                return b.overflow;
            }
            if (a.point != b.point) {
                return a.point < b.point;
            }
            // `total` is (0, 0) and belongs last, under the stages it sums.
            const bool a_total = a.from == kBeginStep && a.to == kBeginStep;
            const bool b_total = b.from == kBeginStep && b.to == kBeginStep;
            if (a_total != b_total) {
                return b_total;
            }
            if (a.from != b.from) {
                return a.from < b.from;
            }
            return a.to < b.to;
        });
    }

    /// The accumulator every row with no samples behind it is built from.
    /// Shared and const rather than constructed where it is needed: it carries
    /// a histogram, and a row that exists to report counts has no use for
    /// ~7.6 KB of empty buckets - let alone a fresh set of them per flush.
    static const Accumulator& empty_accumulator() {
        static const Accumulator kEmpty;
        return kEmpty;
    }

    /// A point's anomaly counters as of the previous flush, or all zeroes for
    /// a point that had none. What the current totals are measured against to
    /// get the interval's share of them.
    PointAnomalies baseline_of(point_id p) {
        const PointAnomalies* b = point_baseline_.find(p);
        return b == nullptr ? PointAnomalies{} : *b;
    }

    /// Fills `s` rather than returning one: the row it belongs to has been
    /// alive since an earlier flush, and its percentiles vector is already the
    /// right size. Returning by value would have discarded that buffer and
    /// allocated a replacement twice per row per interval.
    void to_stats(stats& s, const Accumulator& acc, const PointAnomalies& an,
                  uint64_t dropped, uint64_t stalled) const {
        s.count        = acc.count();
        s.dropped      = dropped;
        s.invalid      = an.invalid;
        s.stalled      = stalled;
        s.out_of_range = an.out_of_range;
        s.abandoned    = an.abandoned;
        s.orphan       = an.orphan;
        s.invariant_tsc = clock_.invariant();

        s.min    = convert(static_cast<double>(acc.min()));
        s.max    = convert(static_cast<double>(acc.max()));
        s.mean   = convert(acc.mean());
        s.stddev = convert(acc.stddev());

        s.percentiles.clear();  // keeps the capacity a previous flush paid for
        s.percentiles.reserve(cfg_.percentiles.size());
        for (double p : cfg_.percentiles) {
            s.percentiles.push_back(convert(acc.percentile(p)));
        }
    }

    double convert(double cycles) const noexcept {
        switch (cfg_.output_unit) {
        case unit::cycles:       return cycles;
        case unit::microseconds: return clock_.to_ns(cycles) / 1000.0;
        case unit::nanoseconds:
        default:                 return clock_.to_ns(cycles);
        }
    }

    /**
     * @brief Drop the cached labels if the shared name table has grown.
     *
     * ControlBlockOwner::lookup() is a linear scan of the name table - 512
     * acquire loads at the default max_names - and build_reports() needs two
     * labels per row. At a hundred stages and a flush a second that is a
     * hundred thousand atomic loads a second spent re-deriving strings that
     * never change, so they are resolved once and kept.
     *
     * "Never change" holds per name, not per table: in the shared modes a
     * producer that attaches later publishes its own labels, and a point that
     * resolved to the "point_7" fallback before it arrived must pick up the
     * real name afterwards.
     *
     * The published-name generation is what notices, and it has to be that
     * rather than a count of claimed slots: a slot is claimed before its name
     * is written and is skipped by lookup() until it lands, so a count taken
     * inside that window already includes a name this cache cannot read - and
     * the fallback cached in its place would never be invalidated, because the
     * count never moves again. The generation moves only once a name is
     * readable - and costs one atomic load to read, where counting claimed
     * slots was a scan of the whole table, 512 acquire loads per flush at the
     * default max_names.
     */
    void refresh_name_cache() {
        const uint64_t generation = control_.name_generation();
        if (generation == cached_name_generation_) {
            return;
        }
        point_names_.clear();
        stage_names_.clear();
        cached_name_generation_ = generation;
    }

    void resolve_point_name(point_id p, std::string& out) {
        if (const std::string* hit = point_names_.find(p)) {
            out.assign(*hit);
            return;
        }
        const std::string_view nm = control_.lookup(p, kBeginStep);
        if (nm.empty()) {
            out.assign("point_").append(std::to_string(p));
        } else {
            out.assign(nm);
        }
        point_names_.insert(p, out);
    }

    /// A transition is labelled by where it ends: begin -> decode reads as
    /// "decode", which is how people describe the stage that just finished.
    void resolve_stage_name(point_id p, uint8_t from, uint8_t to, std::string& out) {
        if (from == kBeginStep && to == kBeginStep) {
            out.assign("total");
            return;
        }
        const uint64_t key = stage_key(p, from, to);
        if (const std::string* hit = stage_names_.find(key)) {
            out.assign(*hit);
            return;
        }
        const std::string_view nm = control_.lookup(p, to);
        if (nm.empty()) {
            out.assign("step_").append(std::to_string(from))
               .append("->").append(std::to_string(to));
        } else {
            out.assign(nm);
        }
        stage_names_.insert(key, out);
    }

    // ------------------------------------------------------------------
    // Output
    // ------------------------------------------------------------------

    /// One row, in the order detail::csv_header() names the columns.
    ///
    /// A function because there are two callers now, and a column written in
    /// one place and not the other is a file whose rows do not all mean the
    /// same thing - the one defect a machine-readable format cannot survive.
    void write_csv_row(const std::string& when, std::string_view point_name,
                       std::string_view stage_name, const stats& s,
                       double hz, double ov) {
        csv_ << when << ',';
        detail::write_csv_field(csv_, point_name);
        csv_ << ',';
        detail::write_csv_field(csv_, stage_name);
        csv_ << ',' << s.count << ',' << s.dropped << ',' << s.invalid << ','
             << s.stalled << ',' << s.abandoned << ',' << s.orphan << ','
             << s.out_of_range << ',' << s.min << ',' << s.mean << ',' << s.stddev;
        for (double p : s.percentiles) {
            csv_ << ',' << p;
        }
        csv_ << ',' << s.max << ',' << static_cast<uint64_t>(hz);
        if (cfg_.subtract_overhead) {
            csv_ << ',' << ov;
        }
        csv_ << '\n';
    }

    void write_csv(const std::vector<StageReport>& rows,
                   std::chrono::steady_clock::time_point now,
                   uint64_t new_losses, uint64_t new_stalled) {
        if (cfg_.path.empty()) {
            return;
        }
        if (!csv_.is_open()) {
            open_csv();
        }
        if (!csv_.is_open()) {
            return;
        }

        const std::string when = detail::format_wall_clock(std::chrono::system_clock::now());
        const double      hz   = clock_.hz();

        // Written per row like tsc_hz, and only when the correction is on - see
        // detail::csv_header().
        const double ov = convert(
            static_cast<double>(overhead_cycles_.load(std::memory_order_acquire)));

        bool wrote = false;
        for (const StageReport& r : rows) {
            // A stage with no samples this interval has nothing to say; writing
            // a zero row every second for every stage that ever fired would
            // bury the rows that matter.
            //
            // An anomaly row is the exception, and has to be: its counters
            // *are* its content. Dropped for want of a sample count, a
            // dropped-begin storm or a full stage table would show up in
            // reports() and in the summary and be missing from the file - the
            // one output anybody actually watches over time, and the one place
            // a reader can see *when* it started.
            if (r.interval.count == 0 && !r.anomalies_only) {
                continue;
            }
            write_csv_row(when, r.point_name, r.stage_name, r.interval, hz, ov);
            wrote = true;
        }

        // `dropped` and `stalled` are collector-wide, so every row above
        // already carries this interval's share of them and one written row is
        // enough to put them in the file. When none was written they would be
        // advanced and never appear anywhere - and the interval that writes no
        // row is exactly the one where the ring overflowed hard enough that
        // nothing completed, or where a producer died mid-stamp and the
        // collector stepped over the slot. A reader watching the file would
        // see silence, which reads as "nothing happened".
        //
        // So they get a row of their own, once, in the interval they happened.
        // Not by relaxing the rule above: a nonzero collector-wide count would
        // then exempt every idle stage in the table at once, and a reader
        // summing the column would get the same loss back once per stage.
        //
        // `collector` is a label, not a point - the counts belong to the
        // process, not to anything the user named - so it can collide with a
        // point that happens to carry the same name, exactly as `overflow`
        // can. A row whose stage is `total` and whose count is zero is not
        // ambiguous in any way that matters to what a reader does with it.
        if (!wrote && (new_losses != 0 || new_stalled != 0)) {
            to_stats(collector_row_, empty_accumulator(), PointAnomalies{},
                     new_losses, new_stalled);
            write_csv_row(when, "collector", "total", collector_row_, hz, ov);
        }

        // Pushing the stream is a write the collector thread blocks on, and it
        // is not draining the ring while it does. At the default
        // csv_flush_interval of zero it happens every interval, which is the
        // durable choice; a caller who would rather let the 64 KiB buffer fill
        // says so, and shutdown() closes the stream either way.
        if (cfg_.csv_flush_interval.count() == 0 ||
            now - last_csv_flush_ >= cfg_.csv_flush_interval) {
            csv_.flush();
            last_csv_flush_ = now;
        }
    }

    void open_csv() {
        // Header only for a new or empty file, so restarting a collector
        // appends to an existing time series instead of interrupting it.
        // start() has already refused any file whose header disagrees with
        // this configuration, so appending here is known to be safe.
        const bool needs_header = existing_csv_header().empty();

        csv_buffer_.resize(64 * 1024);
        csv_.rdbuf()->pubsetbuf(csv_buffer_.data(),
                                static_cast<std::streamsize>(csv_buffer_.size()));
        csv_.open(cfg_.path, std::ios::out | std::ios::app);
        if (!csv_.is_open()) {
            return;
        }
        csv_ << std::fixed << std::setprecision(1);

        if (needs_header) {
            csv_ << this_csv_header() << '\n';
        }
    }

    /// The header this configuration writes. One place, so the line the
    /// startup check compares against and the line open_csv() writes cannot
    /// come to disagree about what belongs in it.
    std::string this_csv_header() const {
        return detail::csv_header(cfg_.percentiles, cfg_.output_unit, cfg_.subtract_overhead);
    }

    /**
     * @brief The header line already in config::path, or empty if it has none.
     *
     * One reader for both the startup schema check and the decision to write a
     * header, so the two cannot disagree about what counts as an existing file.
     */
    std::string existing_csv_header() const {
        std::ifstream probe(cfg_.path, std::ios::binary);
        std::string   first;
        if (!probe.is_open() || !std::getline(probe, first)) {
            return {};
        }
        if (!first.empty() && first.back() == '\r') {
            first.pop_back();  // a file written on Windows and read back here
        }
        return first;
    }

    void write_summary_file() {
        if (cfg_.summary_path.empty()) {
            return;
        }
        std::ofstream os(cfg_.summary_path, std::ios::out | std::ios::trunc);
        if (!os.is_open()) {
            return;
        }
        std::lock_guard<std::mutex> lock(report_mutex_);
        write_summary(os);
    }

    /// Caller holds report_mutex_.
    ///
    /// Reads the published atomics rather than clock_ directly: this runs on
    /// whichever thread called dump_summary(), while the collector may be
    /// refining the calibration at the same moment.
    void write_summary(std::ostream& os) const {
        const double hz        = clock_hz_.load(std::memory_order_acquire);
        const bool   invariant = invariant_.load(std::memory_order_acquire);
        const auto   elapsed   = std::chrono::steady_clock::now() - run_start_;
        const auto   secs      = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();

        os << "slick-perfmon summary   tsc " << std::fixed << std::setprecision(4)
           << (hz / 1e9) << " GHz (" << (invariant ? "invariant" : "NOT INVARIANT")
           << ")   run " << (secs / 3600) << ':' << std::setw(2) << std::setfill('0')
           << ((secs / 60) % 60) << ':' << std::setw(2) << std::setfill('0') << (secs % 60)
           << std::setfill(' ') << "   overhead "
           << overhead_cycles_.load(std::memory_order_acquire) << " cyc/stamp"
           // Collector-wide rather than per-stage, so it belongs in the header
           // instead of as a column repeating one number down every row.
           << "   stalled " << stalled_slots_ << " slot(s)\n";

        if (!invariant) {
            os << "WARNING: this CPU has no invariant TSC. The counter changes rate with\n"
                  "         frequency and halts in deep C-states, so every figure below is\n"
                  "         unreliable.\n";
        }

        // Summed rather than hardcoded: a column added without widening the rule
        // under it is the sort of thing nobody notices until the output is ugly.
        constexpr int kRuleWidth = 20 + 16 + 12 + 10 + 11 + 10 + 14 + 12 + 12 + 12 + 12 + 12;

        os << std::left << std::setw(20) << "point" << std::setw(16) << "stage"
           << std::right << std::setw(12) << "count" << std::setw(10) << "dropped"
           << std::setw(11) << "abandoned" << std::setw(10) << "orphan"
           << std::setw(14) << "out_of_range"
           << std::setw(12) << "min" << std::setw(12) << "mean" << std::setw(12) << "p50"
           << std::setw(12) << "p99" << std::setw(12) << "max" << '\n';
        os << std::string(kRuleWidth, '-') << '\n';

        for (const StageReport& r : reports_) {
            // A stage that never completed a span has nothing to tabulate -
            // unless what it has is an anomaly count, which is exactly the
            // case a summary exists to put in front of somebody.
            if (r.cumulative.count == 0 && !detail::has_anomaly(r.cumulative)) {
                continue;
            }
            os << std::left << std::setw(20) << r.point_name << std::setw(16) << r.stage_name
               << std::right << std::setw(12) << r.cumulative.count
               << std::setw(10) << r.cumulative.dropped
               << std::setw(11) << r.cumulative.abandoned
               << std::setw(10) << r.cumulative.orphan
               << std::setw(14) << r.cumulative.out_of_range
               << std::setw(12) << unit_string(r.cumulative.min)
               << std::setw(12) << unit_string(r.cumulative.mean)
               << std::setw(12) << pick_percentile(r.cumulative, 50.0)
               << std::setw(12) << pick_percentile(r.cumulative, 99.0)
               << std::setw(12) << unit_string(r.cumulative.max) << '\n';
        }
    }

    std::string unit_string(double v) const {
        if (cfg_.output_unit == unit::cycles) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.0fcyc", v);
            return buf;
        }
        const double ns = cfg_.output_unit == unit::microseconds ? v * 1000.0 : v;
        return detail::format_duration(ns);
    }

    /// The summary shows p50 and p99 specifically; if the user configured a
    /// different set, show the nearest one rather than a blank column.
    std::string pick_percentile(const stats& s, double want) const {
        if (s.percentiles.empty()) {
            return "-";
        }
        size_t best  = 0;
        double bestd = 1e18;
        for (size_t i = 0; i < cfg_.percentiles.size() && i < s.percentiles.size(); ++i) {
            const double d = std::abs(cfg_.percentiles[i] - want);
            if (d < bestd) {
                bestd = d;
                best  = i;
            }
        }
        return unit_string(s.percentiles[best]);
    }

    // ------------------------------------------------------------------

    template <typename Pred>
    void wait_until(Pred&& done) {
        // A short yield burst covers the common case where the collector is
        // already awake; past that, sleeping beats burning a core while the
        // caller is blocked anyway.
        constexpr uint32_t kYieldLimit = 1024;
        for (uint32_t i = 0; i < kYieldLimit; ++i) {
            if (done()) {
                return;
            }
            std::this_thread::yield();
        }
        while (!done()) {
            if (!running_.load(std::memory_order_acquire)) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    config            cfg_;
    ControlBlockOwner control_;
    detail::tsc_clock clock_;
    Pairer            pairer_;

    std::unique_ptr<sample_queue>              ring_;
    std::vector<std::unique_ptr<sample_queue>> retired_rings_;

    std::thread           thread_;
    std::atomic<bool>     running_{false};
    std::atomic<bool>     enabled_{false};
    std::atomic<uint64_t> cursor_{0};
    std::atomic<uint64_t> flush_request_{0};
    std::atomic<uint64_t> flush_done_{0};
    std::atomic<double>   clock_hz_{0.0};
    std::atomic<bool>     invariant_{true};
    std::atomic<uint64_t> overhead_cycles_{0};
    std::atomic<size_t>   peak_open_{0};
    std::atomic<uint64_t> last_flush_cycles_{0};

    mutable std::mutex       report_mutex_;
    std::vector<StageReport> reports_;

    // Collector-thread scratch for the flush path. Kept as members rather than
    // built per flush so a steady-state interval allocates nothing: build_rows_
    // is swapped with reports_ and refilled in place, and the two name caches
    // stand in for a linear scan of the shared name table per label.
    /// Which of the interval's rows belong to one point: the row that stands
    /// for it, and whether any of them has a sample. Both are answers
    /// build_reports() needs per point and would otherwise re-derive by
    /// scanning every row it has built.
    struct RowRef {
        uint32_t carrier     = 0;  ///< index into the row vector
        bool     has_samples = false;
    };

    std::vector<StageReport>         build_rows_;
    detail::open_map<std::string>    point_names_;
    detail::open_map<std::string>    stage_names_;
    detail::open_map<RowRef>         row_points_;
    uint64_t                         cached_name_generation_ = 0;

    /// Per-point anomaly totals as of the last flush, so each interval reports
    /// its own share of counters the pairer only ever grows.
    detail::open_map<PointAnomalies>  point_baseline_;
    PointAnomalies                    overflow_baseline_;
    uint64_t                          reported_stalled_ = 0;

    /// Scratch for the collector-wide row write_csv() falls back to. A member
    /// for the same reason the rows are: it keeps the percentile vector it
    /// allocated the first time it was needed.
    stats                             collector_row_;

    std::ofstream     csv_;
    std::vector<char> csv_buffer_;

    std::chrono::steady_clock::time_point run_start_{};
    std::chrono::steady_clock::time_point stalled_since_{};
    std::chrono::steady_clock::time_point last_csv_flush_{};
    uint64_t                              last_loss_     = 0;
    uint64_t                              stalled_slots_ = 0;
    uint32_t                              name_conflicts_ = 0;
    bool                                  started_       = false;
};

using collector = Collector;

namespace detail {

/// Start from the environment when nothing called start() explicitly.
///
/// Eager rather than lazy, because with user-supplied point ids there is no
/// registration to order against - the only thing a stamp needs is the ring
/// pointer, and publishing it before main() is harmless. Does nothing unless
/// one of the environment variables is actually set.
struct auto_starter {
    auto_starter() {
        const bool want = env_or_null("SLICK_PERFMON_CSV") != nullptr ||
                          env_or_null("SLICK_PERFMON_SHM") != nullptr;
        if (!want) {
            return;
        }
        try {
            Collector::instance().start();
        } catch (...) {
            // A misconfigured environment variable must never stop a program
            // from starting. The absence of a CSV is the diagnostic.
        }
    }
};

inline auto_starter g_auto_starter;

}  // namespace detail

}  // namespace slick::perfmon

#else  // !SLICK_PERFMON_ENABLED

#include <ostream>
#include <vector>

namespace slick::perfmon {

struct StageReport {
    point_id    point = 0;
    uint8_t     from  = 0;
    uint8_t     to    = 0;
    std::string point_name;
    std::string stage_name;
    stats       interval;
    stats       cumulative;
    bool        anomalies_only = false;
    bool        overflow       = false;
};

/// Stub with the same surface, so instrumented code and its setup compile
/// unchanged in a build with the library switched off.
class Collector {
public:
    static Collector& instance() noexcept {
        static Collector c;
        return c;
    }
    static void set_instance(Collector*) noexcept {}

    bool start(config = {}) { return false; }
    void shutdown() {}
    bool running() const noexcept { return false; }
    void set_enabled(bool) noexcept {}
    bool enabled() const noexcept { return false; }
    void flush() {}

    stats snapshot(point_id, uint8_t, uint8_t) { return {}; }
    stats snapshot_total(point_id) { return {}; }
    std::vector<StageReport> reports() { return {}; }
    void   dump_summary(std::ostream&) {}
    double tsc_hz() const noexcept { return 0.0; }
    bool   invariant_tsc() const noexcept { return true; }
    uint64_t overhead_cycles() const noexcept { return 0; }
    uint64_t last_flush_cycles() const noexcept { return 0; }
    size_t   peak_open_spans() const noexcept { return 0; }
    uint32_t name_conflicts() const noexcept { return 0; }
};

using collector = Collector;

}  // namespace slick::perfmon

#endif  // SLICK_PERFMON_ENABLED
