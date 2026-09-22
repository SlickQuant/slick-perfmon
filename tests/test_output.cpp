// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Slick Quant
//
// The CSV is the product. It has to load without a parser dialect, carry its
// own units, and account for every span that was stamped.

#include <slick/perfmon.hpp>
#include <slick/perfmon/collector.hpp>

#include "test_support.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace slick::perfmon;
using namespace slick::perfmon::test;

namespace {

enum class pt : point_id { pipeline = 0, count };
constexpr uint8_t kMid = 2;

constexpr std::string_view names(point_id p, uint8_t step) noexcept {
    if (p != 0) {
        return {};
    }
    switch (step) {
    case kBeginStep: return "pipeline";
    case kMid:       return "middle";
    case kEndStep:   return "finish";
    default:         return {};
    }
}

std::vector<std::string> split(const std::string& line, char sep) {
    std::vector<std::string> out;
    std::string              field;
    std::istringstream       is(line);
    while (std::getline(is, field, sep)) {
        out.push_back(field);
    }
    return out;
}

/// Index of a named column, or header.size() when it is not there - so a
/// caller can ASSERT on it before indexing a row with it.
size_t column_of(const std::vector<std::string>& header, const std::string& name) {
    const auto it = std::find(header.begin(), header.end(), name);
    return it == header.end() ? header.size()
                              : static_cast<size_t>(std::distance(header.begin(), it));
}

std::vector<std::string> read_lines(const std::string& path) {
    std::vector<std::string> lines;
    std::ifstream            in(path);
    std::string              line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    return lines;
}

/// Each test gets its own files, and removes them first so a previous run
/// cannot be mistaken for this one.
struct TempFiles {
    std::string csv;
    std::string summary;

    explicit TempFiles(const char* tag)
        : csv(std::string("perfmon_test_") + tag + ".csv"),
          summary(std::string("perfmon_test_") + tag + "_summary.txt") {
        std::remove(csv.c_str());
        std::remove(summary.c_str());
    }

    ~TempFiles() {
        std::remove(csv.c_str());
        std::remove(summary.c_str());
    }
};

}  // namespace

TEST(Output, WritesAHeaderAndOneRowPerStage) {
    TempFiles files("basic");

    Collector c;
    config    cfg    = quiet_config();
    cfg.point_count  = static_cast<point_id>(pt::count);
    cfg.name_of      = &names;
    cfg.path         = files.csv;
    cfg.summary_path = files.summary;
    ASSERT_TRUE(c.start(cfg));

    constexpr int kSpans = 500;
    for (int i = 0; i < kSpans; ++i) {
        begin(pt::pipeline);
        step(pt::pipeline, kMid);
        end(pt::pipeline);
    }
    c.shutdown();

    const std::vector<std::string> lines = read_lines(files.csv);
    ASSERT_GE(lines.size(), 2u) << "a header plus at least one data row";

    const std::vector<std::string> header = split(lines[0], ',');
    ASSERT_GE(header.size(), 16u);
    EXPECT_EQ(header[0], "timestamp");
    EXPECT_EQ(header[1], "point");
    EXPECT_EQ(header[2], "stage");
    EXPECT_EQ(header[3], "count");
    EXPECT_EQ(header.back(), "tsc_hz");

    // "p99.9" has to become "p99_9", or the column name breaks a CSV reader
    // that splits on the decimal point. The unit rides along on every latency
    // column, so a file cannot be read in the wrong one.
    EXPECT_NE(std::find(header.begin(), header.end(), "p99_9_ns"), header.end());
    EXPECT_EQ(std::find(header.begin(), header.end(), "p99.9_ns"), header.end());
    EXPECT_NE(std::find(header.begin(), header.end(), "min_ns"), header.end());
    EXPECT_NE(std::find(header.begin(), header.end(), "max_ns"), header.end());

    // The correction is off in this session, so the column that reports it
    // must not be there - its presence is what stops a corrected run from
    // appending to a raw one.
    EXPECT_EQ(std::find(header.begin(), header.end(), "overhead_ns"), header.end());

    for (size_t i = 1; i < lines.size(); ++i) {
        EXPECT_EQ(split(lines[i], ',').size(), header.size())
            << "row " << i << " does not match the header width";
    }
}

TEST(Output, CountsAccountForEverySpanStamped) {
    TempFiles files("counts");

    Collector c;
    config    cfg    = quiet_config();
    cfg.point_count  = static_cast<point_id>(pt::count);
    cfg.name_of      = &names;
    cfg.path         = files.csv;
    cfg.summary_path = "";
    // One flush at the end, so the arithmetic is unambiguous.
    cfg.flush_interval = std::chrono::seconds(60);
    ASSERT_TRUE(c.start(cfg));

    constexpr int kSpans = 1234;
    for (int i = 0; i < kSpans; ++i) {
        begin(pt::pipeline);
        step(pt::pipeline, kMid);
        end(pt::pipeline);
    }
    c.shutdown();

    const std::vector<std::string> lines  = read_lines(files.csv);
    const std::vector<std::string> header = split(lines[0], ',');

    size_t   total_rows  = 0;
    uint64_t total_count = 0;
    for (size_t i = 1; i < lines.size(); ++i) {
        const std::vector<std::string> row = split(lines[i], ',');
        ASSERT_EQ(row.size(), header.size());
        if (row[2] == "total") {
            ++total_rows;
            total_count += std::stoull(row[3]);
        }
    }
    EXPECT_EQ(total_rows, 1u) << "one flush should produce one total row";
    EXPECT_EQ(total_count, kSpans);
}

TEST(Output, LabelsStagesByTheirDestinationStep) {
    TempFiles files("labels");

    Collector c;
    config    cfg    = quiet_config();
    cfg.point_count  = static_cast<point_id>(pt::count);
    cfg.name_of      = &names;
    cfg.path         = files.csv;
    cfg.summary_path = "";
    ASSERT_TRUE(c.start(cfg));

    for (int i = 0; i < 100; ++i) {
        begin(pt::pipeline);
        step(pt::pipeline, kMid);
        end(pt::pipeline);
    }
    c.shutdown();

    std::vector<std::string> stages;
    const auto               lines = read_lines(files.csv);
    for (size_t i = 1; i < lines.size(); ++i) {
        const auto row = split(lines[i], ',');
        EXPECT_EQ(row[1], "pipeline") << "the point column takes the step-0 name";
        stages.push_back(row[2]);
    }

    // A transition is named after where it ends, which is how people describe
    // the stage that just finished.
    EXPECT_NE(std::find(stages.begin(), stages.end(), "middle"), stages.end());
    EXPECT_NE(std::find(stages.begin(), stages.end(), "finish"), stages.end());
    EXPECT_NE(std::find(stages.begin(), stages.end(), "total"), stages.end());
}

TEST(Output, FallsBackToGeneratedLabelsWithoutANameFunction) {
    TempFiles files("unnamed");

    Collector c;
    config    cfg    = quiet_config();
    cfg.point_count  = static_cast<point_id>(pt::count);
    cfg.name_of      = nullptr;
    cfg.path         = files.csv;
    cfg.summary_path = "";
    ASSERT_TRUE(c.start(cfg));

    for (int i = 0; i < 50; ++i) {
        begin(pt::pipeline);
        step(pt::pipeline, kMid);
        end(pt::pipeline);
    }
    c.shutdown();

    const auto lines = read_lines(files.csv);
    ASSERT_GE(lines.size(), 2u);
    bool saw_generated = false;
    for (size_t i = 1; i < lines.size(); ++i) {
        const auto row = split(lines[i], ',');
        EXPECT_EQ(row[1], "point_0");
        if (row[2].rfind("step_", 0) == 0) {
            saw_generated = true;
        }
    }
    EXPECT_TRUE(saw_generated) << "unnamed stages need a readable fallback, not a blank";
}

TEST(Output, PercentilesAreOrderedWithinEveryRow) {
    TempFiles files("ordering");

    Collector c;
    config    cfg    = quiet_config();
    cfg.point_count  = static_cast<point_id>(pt::count);
    cfg.name_of      = &names;
    cfg.path         = files.csv;
    cfg.summary_path = "";
    ASSERT_TRUE(c.start(cfg));

    for (int i = 0; i < 5000; ++i) {
        begin(pt::pipeline);
        end(pt::pipeline);
    }
    c.shutdown();

    const auto lines  = read_lines(files.csv);
    const auto header = split(lines[0], ',');

    auto column = [&](const std::string& name) -> size_t {
        const auto it = std::find(header.begin(), header.end(), name);
        EXPECT_NE(it, header.end()) << "missing column " << name;
        return static_cast<size_t>(std::distance(header.begin(), it));
    };

    const size_t c_min  = column("min_ns");
    const size_t c_p50  = column("p50_ns");
    const size_t c_p90  = column("p90_ns");
    const size_t c_p99  = column("p99_ns");
    const size_t c_p999 = column("p99_9_ns");
    const size_t c_max  = column("max_ns");

    ASSERT_GE(lines.size(), 2u);
    for (size_t i = 1; i < lines.size(); ++i) {
        const auto   row  = split(lines[i], ',');
        const double mn   = std::stod(row[c_min]);
        const double p50  = std::stod(row[c_p50]);
        const double p90  = std::stod(row[c_p90]);
        const double p99  = std::stod(row[c_p99]);
        const double p999 = std::stod(row[c_p999]);
        const double mx   = std::stod(row[c_max]);

        EXPECT_LE(mn, p50) << "row " << i;
        EXPECT_LE(p50, p90) << "row " << i;
        EXPECT_LE(p90, p99) << "row " << i;
        EXPECT_LE(p99, p999) << "row " << i;
        // A bucket midpoint must never be allowed to exceed the observed max.
        EXPECT_LE(p999, mx) << "row " << i;
    }
}

TEST(Output, TscHzIsCarriedInEveryRow) {
    TempFiles files("hz");

    Collector c;
    config    cfg    = quiet_config();
    cfg.point_count  = static_cast<point_id>(pt::count);
    cfg.name_of      = &names;
    cfg.path         = files.csv;
    cfg.summary_path = "";
    ASSERT_TRUE(c.start(cfg));
    for (int i = 0; i < 100; ++i) {
        begin(pt::pipeline);
        end(pt::pipeline);
    }
    c.shutdown();

    const auto lines = read_lines(files.csv);
    ASSERT_GE(lines.size(), 2u);
    const auto header = split(lines[0], ',');
    for (size_t i = 1; i < lines.size(); ++i) {
        const auto     row = split(lines[i], ',');
        const uint64_t hz  = std::stoull(row[header.size() - 1]);
        // The file has to be self-describing: no separate metadata, no comment
        // line a naive reader would choke on.
        EXPECT_GT(hz, 500'000'000ull);
        EXPECT_LT(hz, 10'000'000'000ull);
    }
}

TEST(Output, AppendingToAnExistingFileDoesNotRepeatTheHeader) {
    TempFiles files("append");

    for (int run = 0; run < 2; ++run) {
        Collector c;
        config    cfg    = quiet_config();
        cfg.point_count  = static_cast<point_id>(pt::count);
        cfg.name_of      = &names;
        cfg.path         = files.csv;
        cfg.summary_path = "";
        ASSERT_TRUE(c.start(cfg));
        for (int i = 0; i < 100; ++i) {
            begin(pt::pipeline);
            end(pt::pipeline);
        }
        c.shutdown();
    }

    // Restarting a collector should continue the time series, not interrupt it.
    const auto lines  = read_lines(files.csv);
    size_t     headers = 0;
    for (const std::string& line : lines) {
        if (line.rfind("timestamp,", 0) == 0) {
            ++headers;
        }
    }
    EXPECT_EQ(headers, 1u);
    EXPECT_GE(lines.size(), 3u);
}

TEST(Output, WritesASummaryFileAtShutdown) {
    TempFiles files("summary");

    Collector c;
    config    cfg    = quiet_config();
    cfg.point_count  = static_cast<point_id>(pt::count);
    cfg.name_of      = &names;
    cfg.path         = files.csv;
    cfg.summary_path = files.summary;
    ASSERT_TRUE(c.start(cfg));
    for (int i = 0; i < 200; ++i) {
        begin(pt::pipeline);
        step(pt::pipeline, kMid);
        end(pt::pipeline);
    }
    c.shutdown();

    const auto lines = read_lines(files.summary);
    ASSERT_FALSE(lines.empty());
    EXPECT_NE(lines[0].find("slick-perfmon summary"), std::string::npos);
    EXPECT_NE(lines[0].find("GHz"), std::string::npos);
    EXPECT_NE(lines[0].find("cyc/stamp"), std::string::npos);

    std::string body;
    for (const std::string& line : lines) {
        body += line;
        body += '\n';
    }
    EXPECT_NE(body.find("pipeline"), std::string::npos);
    EXPECT_NE(body.find("total"), std::string::npos);
    EXPECT_NE(body.find("middle"), std::string::npos);
}

TEST(Output, DumpSummaryWorksOnDemandWhileRunning) {
    Collector c;
    config    cfg   = quiet_config();
    cfg.point_count = static_cast<point_id>(pt::count);
    cfg.name_of     = &names;
    ASSERT_TRUE(c.start(cfg));
    for (int i = 0; i < 100; ++i) {
        begin(pt::pipeline);
        end(pt::pipeline);
    }

    std::ostringstream os;
    c.dump_summary(os);
    const std::string body = os.str();
    EXPECT_NE(body.find("slick-perfmon summary"), std::string::npos);
    EXPECT_NE(body.find("pipeline"), std::string::npos);
    c.shutdown();
}

TEST(Output, AnEmptyPathDisablesFileWritingEntirely) {
    Collector c;
    config    cfg    = quiet_config();
    cfg.point_count  = static_cast<point_id>(pt::count);
    cfg.path         = "";
    cfg.summary_path = "";
    ASSERT_TRUE(c.start(cfg));
    for (int i = 0; i < 100; ++i) {
        begin(pt::pipeline);
        end(pt::pipeline);
    }
    // Statistics still work; only the files are suppressed.
    EXPECT_EQ(c.snapshot_total(to_point(pt::pipeline)).count, 100u);
    c.shutdown();
}

TEST(Output, RestartingWithANewPathStopsWritingToTheOldFile) {
    // Regression: shutdown() left the ofstream open, and open_csv() only opens
    // when the stream is closed - so a second session kept appending to the
    // first session's file and the new path was never created at all.
    TempFiles first("restart_old");
    TempFiles second("restart_new");

    config cfg       = quiet_config();
    cfg.point_count  = static_cast<point_id>(pt::count);
    cfg.name_of      = &names;
    cfg.summary_path = "";

    Collector c;

    cfg.path = first.csv;
    ASSERT_TRUE(c.start(cfg));
    for (int i = 0; i < 100; ++i) {
        begin(pt::pipeline);
        end(pt::pipeline);
    }
    c.shutdown();

    const size_t first_lines = read_lines(first.csv).size();
    ASSERT_GE(first_lines, 2u) << "the first session has to have written something";

    cfg.path = second.csv;
    ASSERT_TRUE(c.start(cfg));
    for (int i = 0; i < 100; ++i) {
        begin(pt::pipeline);
        end(pt::pipeline);
    }
    c.shutdown();

    const auto second_lines = read_lines(second.csv);
    ASSERT_GE(second_lines.size(), 2u) << "the new path was never opened";
    EXPECT_EQ(second_lines[0].rfind("timestamp,", 0), 0u) << "a new file needs its header";

    EXPECT_EQ(read_lines(first.csv).size(), first_lines)
        << "the previous session's file kept receiving rows after shutdown";
}

namespace {

constexpr uint8_t kAwkward = 3;

/// Labels are the user's to choose, and nothing stops them containing the
/// characters CSV reserves.
constexpr std::string_view awkward_names(point_id p, uint8_t step) noexcept {
    if (p != 0) {
        return {};
    }
    switch (step) {
    case kBeginStep: return "say \"hello\"";
    case kAwkward:   return "decode, verify";
    default:         return {};
    }
}

/// RFC 4180 field splitter: commas inside a quoted field do not separate, and
/// a doubled quote inside one is a literal quote.
std::vector<std::string> split_csv(const std::string& line) {
    std::vector<std::string> out;
    std::string              field;
    bool                     quoted = false;

    for (size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (quoted) {
            if (ch == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    field.push_back('"');
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                field.push_back(ch);
            }
        } else if (ch == '"') {
            quoted = true;
        } else if (ch == ',') {
            out.push_back(field);
            field.clear();
        } else {
            field.push_back(ch);
        }
    }
    out.push_back(field);
    return out;
}

}  // namespace

TEST(Output, LabelsContainingCommasAndQuotesStayValidCsv) {
    // Regression: names went into the file verbatim, so a single comma in a
    // label shifted every later column by one and the file parsed into silent
    // nonsense - the worst failure mode a measurement tool can have.
    TempFiles files("escaping");

    Collector c;
    config    cfg    = quiet_config();
    cfg.point_count  = static_cast<point_id>(pt::count);
    cfg.name_of      = &awkward_names;
    cfg.path         = files.csv;
    cfg.summary_path = "";
    ASSERT_TRUE(c.start(cfg));

    for (int i = 0; i < 100; ++i) {
        begin(pt::pipeline);
        step(pt::pipeline, kAwkward);
        end(pt::pipeline);
    }
    c.shutdown();

    const auto lines = read_lines(files.csv);
    ASSERT_GE(lines.size(), 2u);
    const auto header = split_csv(lines[0]);

    bool saw_comma_label = false;
    bool saw_quote_label = false;
    for (size_t i = 1; i < lines.size(); ++i) {
        const auto row = split_csv(lines[i]);
        ASSERT_EQ(row.size(), header.size())
            << "row " << i << " does not match the header width: " << lines[i];
        EXPECT_EQ(row[1], "say \"hello\"") << "the quote has to round-trip";
        saw_quote_label = true;
        if (row[2] == "decode, verify") {
            saw_comma_label = true;
        }
    }
    EXPECT_TRUE(saw_quote_label);
    EXPECT_TRUE(saw_comma_label) << "the comma-bearing stage label has to round-trip whole";

    // A naive split must see *more* fields than the quote-aware one - that is
    // the proof the quoting is doing the work and not the parser being lenient.
    bool any_quoted = false;
    for (size_t i = 1; i < lines.size(); ++i) {
        if (split(lines[i], ',').size() > header.size()) {
            any_quoted = true;
        }
    }
    EXPECT_TRUE(any_quoted);
}

TEST(Output, EveryAnomalyCounterHasAColumn) {
    // stats exposes seven counters. A row that reports only five of them leaves
    // two failure modes reachable in the API and invisible in the product.
    TempFiles files("anomaly_cols");

    Collector c;
    config    cfg    = quiet_config();
    cfg.point_count  = static_cast<point_id>(pt::count);
    cfg.name_of      = &names;
    cfg.path         = files.csv;
    cfg.summary_path = "";
    ASSERT_TRUE(c.start(cfg));
    for (int i = 0; i < 100; ++i) {
        begin(pt::pipeline);
        end(pt::pipeline);
    }
    c.shutdown();

    const auto lines  = read_lines(files.csv);
    ASSERT_GE(lines.size(), 2u);
    const auto header = split(lines[0], ',');

    for (const char* col : {"dropped", "invalid", "stalled", "abandoned", "orphan",
                            "out_of_range"}) {
        EXPECT_NE(std::find(header.begin(), header.end(), std::string(col)), header.end())
            << "missing column " << col;
    }
    for (size_t i = 1; i < lines.size(); ++i) {
        EXPECT_EQ(split(lines[i], ',').size(), header.size()) << "row " << i;
    }
}

TEST(Output, AppendingWithDifferentColumnsIsRefused) {
    // The header is written only for a new or empty file, so a restart against
    // the same path continues one time series. That is right until the columns
    // change: the percentile columns come from the configuration, so a session
    // configured differently would append rows the header on disk labels
    // wrongly - and no parser can detect that. It reads the wrong number under
    // the right name and is never given a reason to doubt it.
    TempFiles files("schema");

    auto session = [&](std::vector<double> percentiles) {
        config cfg       = quiet_config();
        cfg.point_count  = static_cast<point_id>(pt::count);
        cfg.name_of      = &names;
        cfg.path         = files.csv;
        cfg.summary_path = "";
        cfg.percentiles  = std::move(percentiles);
        return cfg;
    };

    const std::vector<double> original = {50.0, 90.0, 99.0, 99.9};
    {
        Collector c;
        ASSERT_TRUE(c.start(session(original)));
        for (int i = 0; i < 100; ++i) {
            begin(pt::pipeline);
            end(pt::pipeline);
        }
        c.shutdown();
    }
    ASSERT_GE(read_lines(files.csv).size(), 2u) << "the first session must have written";

    {
        Collector c;
        EXPECT_THROW((void)c.start(session({50.0, 99.0})), std::invalid_argument);
    }

    // The same column set appends, and appends without a second header.
    {
        Collector c;
        ASSERT_TRUE(c.start(session(original)));
        for (int i = 0; i < 100; ++i) {
            begin(pt::pipeline);
            end(pt::pipeline);
        }
        c.shutdown();
    }

    const std::vector<std::string> lines = read_lines(files.csv);
    size_t headers = 0;
    for (const std::string& line : lines) {
        if (line.rfind("timestamp,", 0) == 0) {
            ++headers;
        }
    }
    EXPECT_EQ(headers, 1u) << "a restart continues the series, it does not re-head it";

    const size_t width = split(lines[0], ',').size();
    for (size_t i = 1; i < lines.size(); ++i) {
        EXPECT_EQ(split(lines[i], ',').size(), width)
            << "row " << i << " does not match the header width";
    }
}

TEST(Output, CsvFlushIntervalDefersThePushToTheOs) {
    // Pushing the stream is a write the collector thread blocks on, and at a
    // short flush_interval it is a hundred of them a second to emit a handful
    // of rows. config::csv_flush_interval trades that per-interval durability
    // for the 64 KiB buffer behind the stream; zero, the default, keeps it.
    auto session = [](const std::string& path, std::chrono::milliseconds csv_flush) {
        config cfg           = quiet_config();
        cfg.point_count      = static_cast<point_id>(pt::count);
        cfg.name_of          = &names;
        cfg.path             = path;
        cfg.summary_path     = "";
        cfg.csv_flush_interval = csv_flush;
        return cfg;
    };

    auto stamp_and_flush = [](Collector& c) {
        for (int i = 0; i < 200; ++i) {
            begin(pt::pipeline);
            end(pt::pipeline);
        }
        (void)c.reports();  // forces a collector flush, so rows have been written
    };

    {
        TempFiles files("csvflush_default");
        Collector c;
        ASSERT_TRUE(c.start(session(files.csv, std::chrono::milliseconds(0))));
        stamp_and_flush(c);
        EXPECT_GE(read_lines(files.csv).size(), 2u)
            << "the default must have the rows on disk before shutdown";
        c.shutdown();
    }

    {
        TempFiles files("csvflush_deferred");
        Collector c;
        ASSERT_TRUE(c.start(session(files.csv, std::chrono::hours(1))));
        stamp_and_flush(c);
        EXPECT_TRUE(read_lines(files.csv).empty())
            << "a deferred push must leave the rows in the stream buffer";

        // Deferred, not dropped: closing the stream writes everything out.
        c.shutdown();
        const auto lines = read_lines(files.csv);
        ASSERT_GE(lines.size(), 2u) << "shutdown must still write the rows";
        EXPECT_EQ(lines[0].rfind("timestamp,", 0), 0u);
    }
}

TEST(Output, AnomalyCountsWithNoSamplesBehindThemStillReachTheFile) {
    // A point that only ever produced anomalies completes no span, so its row
    // carries no samples - and the CSV drops rows with no samples, because a
    // zero row every interval for every stage that ever fired would bury the
    // rows that matter. The counters are the exception: dropped for want of a
    // count, a dropped-begin storm showed up in reports() and in the summary
    // and was missing from the one output that says *when* it started.
    TempFiles files("anomaly_only");

    Collector c;
    config    cfg      = quiet_config();
    cfg.point_count    = static_cast<point_id>(pt::count);
    cfg.name_of        = &names;
    cfg.path           = files.csv;
    cfg.summary_path   = files.summary;
    cfg.flush_interval = std::chrono::seconds(60);  // one flush, at shutdown
    ASSERT_TRUE(c.start(cfg));

    constexpr int kOrphans = 5;
    for (int i = 0; i < kOrphans; ++i) {
        end(pt::pipeline);  // an end with no begin: an orphan, and no stage row
    }
    c.shutdown();

    const auto lines = read_lines(files.csv);
    ASSERT_GE(lines.size(), 2u) << "the orphans have to have produced a row";
    const auto header = split(lines[0], ',');

    const size_t c_count  = column_of(header, "count");
    const size_t c_orphan = column_of(header, "orphan");
    ASSERT_LT(c_count, header.size());
    ASSERT_LT(c_orphan, header.size());

    bool found = false;
    for (size_t i = 1; i < lines.size(); ++i) {
        const auto row = split(lines[i], ',');
        ASSERT_EQ(row.size(), header.size()) << "row " << i;
        if (std::stoull(row[c_orphan]) == kOrphans) {
            found = true;
            EXPECT_EQ(std::stoull(row[c_count]), 0u) << "no span completed on that point";
        }
    }
    EXPECT_TRUE(found) << "the orphan count belongs in the file, not only in reports()";

    // The summary drops empty rows for the same reason and needs the same
    // exception, or the file written at shutdown is silent about it too.
    const auto summary = read_lines(files.summary);
    bool in_summary = false;
    for (const std::string& line : summary) {
        if (line.rfind("pipeline", 0) == 0) {
            in_summary = true;
        }
    }
    EXPECT_TRUE(in_summary) << "the summary must show a point that only had anomalies";
}

TEST(Output, AnAnomalyIsWrittenOnceAndNotOnEveryLaterInterval) {
    // The pairer's counters are lifetime totals. Written straight into every
    // interval's row they would repeat for the rest of the run - noise, and a
    // column nobody could sum, since the same orphan would appear in every
    // later interval. A row reports what is new since the previous flush.
    TempFiles files("anomaly_once");

    Collector c;
    config    cfg      = quiet_config();
    cfg.point_count    = static_cast<point_id>(pt::count);
    cfg.name_of        = &names;
    cfg.path           = files.csv;
    cfg.summary_path   = "";
    cfg.flush_interval = std::chrono::hours(1);  // only the flushes asked for
    ASSERT_TRUE(c.start(cfg));

    end(pt::pipeline);   // one orphan, in the first interval
    (void)c.reports();   // flush 1: writes it
    (void)c.reports();   // flush 2: nothing new to say
    (void)c.reports();   // flush 3: still nothing
    c.shutdown();        // and the flush at shutdown

    const auto lines  = read_lines(files.csv);
    ASSERT_GE(lines.size(), 2u);
    const auto   header   = split(lines[0], ',');
    const size_t c_orphan = column_of(header, "orphan");
    ASSERT_LT(c_orphan, header.size());

    size_t rows_with_orphans = 0;
    for (size_t i = 1; i < lines.size(); ++i) {
        if (std::stoull(split(lines[i], ',')[c_orphan]) != 0) {
            ++rows_with_orphans;
        }
    }
    EXPECT_EQ(rows_with_orphans, 1u) << "one orphan, one row - in the interval it happened";
}

TEST(Output, AppendingInADifferentUnitOrCorrectionIsRefused) {
    // The percentile list decides which columns exist; output_unit and
    // subtract_overhead decide what the numbers under them are. Cycles,
    // nanoseconds and microseconds are three incomparable scales behind
    // identical column names, and a corrected figure differs from a raw one by
    // a systematic offset - so a header the file already has must describe all
    // three, or a session that disagrees appends numbers no reader can tell
    // apart from the ones above them.
    TempFiles files("schema_unit");

    auto session = [&](unit u, bool correct) {
        config cfg            = quiet_config();
        cfg.point_count       = static_cast<point_id>(pt::count);
        cfg.name_of           = &names;
        cfg.path              = files.csv;
        cfg.summary_path      = "";
        cfg.output_unit       = u;
        cfg.subtract_overhead = correct;
        return cfg;
    };

    {
        Collector c;
        ASSERT_TRUE(c.start(session(unit::nanoseconds, false)));
        for (int i = 0; i < 100; ++i) {
            begin(pt::pipeline);
            end(pt::pipeline);
        }
        c.shutdown();
    }
    ASSERT_GE(read_lines(files.csv).size(), 2u) << "the first session must have written";

    for (unit u : {unit::microseconds, unit::cycles}) {
        Collector c;
        EXPECT_THROW((void)c.start(session(u, false)), std::invalid_argument)
            << "a different unit under the same column names";
    }
    {
        Collector c;
        EXPECT_THROW((void)c.start(session(unit::nanoseconds, true)), std::invalid_argument)
            << "overhead-corrected rows are not the same measurement";
    }

    // The same configuration still appends, and without a second header.
    {
        Collector c;
        ASSERT_TRUE(c.start(session(unit::nanoseconds, false)));
        for (int i = 0; i < 100; ++i) {
            begin(pt::pipeline);
            end(pt::pipeline);
        }
        c.shutdown();
    }
    size_t headers = 0;
    for (const std::string& line : read_lines(files.csv)) {
        if (line.rfind("timestamp,", 0) == 0) {
            ++headers;
        }
    }
    EXPECT_EQ(headers, 1u) << "a matching restart continues the series";
}

TEST(Output, TheCorrectionItAppliedIsInTheFile) {
    // With the correction on, every latency in the file has had the measured
    // per-stamp cost taken out of it. The column says so, and carries the
    // amount, so a reader can put it back - and so the header of a corrected
    // file cannot be mistaken for the header of a raw one.
    TempFiles files("overhead_col");

    Collector c;
    config    cfg         = quiet_config();
    cfg.point_count       = static_cast<point_id>(pt::count);
    cfg.name_of           = &names;
    cfg.path              = files.csv;
    cfg.summary_path      = "";
    cfg.subtract_overhead = true;
    ASSERT_TRUE(c.start(cfg));
    for (int i = 0; i < 200; ++i) {
        begin(pt::pipeline);
        end(pt::pipeline);
    }
    c.shutdown();

    const auto lines = read_lines(files.csv);
    ASSERT_GE(lines.size(), 2u);
    const auto   header      = split(lines[0], ',');
    const size_t c_overhead = column_of(header, "overhead_ns");
    ASSERT_LT(c_overhead, header.size()) << "a corrected run has to say so in its schema";
    EXPECT_EQ(c_overhead, header.size() - 1) << "a trailing per-row constant, like tsc_hz";

    double first = -1.0;
    for (size_t i = 1; i < lines.size(); ++i) {
        const auto   row = split(lines[i], ',');
        ASSERT_EQ(row.size(), header.size()) << "row " << i;
        const double ov = std::stod(row[c_overhead]);

#if SLICK_PERFMON_X86
        EXPECT_GT(ov, 0.0) << "row " << i;
#else
        // Off x86 the fallback clock is coarser than a stamp - ~41.67 ns on
        // Apple Silicon - and measure_overhead() takes a *minimum*, so the
        // calibration legitimately comes back as nothing to subtract. A zero
        // correction is still a correction the file has to state.
        EXPECT_GE(ov, 0.0) << "row " << i;
#endif

        // A per-row constant like tsc_hz, and the reason the column exists at
        // all: a reader puts the correction back by adding this to every
        // latency beside it, which only works if every row agrees on it.
        if (first < 0.0) {
            first = ov;
        } else {
            EXPECT_DOUBLE_EQ(ov, first) << "row " << i;
        }
    }
}

TEST(Output, ACollectorWideStallReachesTheFileWithNoRowsBehindIt) {
    // Regression: `dropped` and `stalled` are collector-wide, so they ride
    // along on every stage row - and an interval that writes no stage row at
    // all used to advance them and write them nowhere. That interval is
    // precisely the interesting one: the ring overflowed hard enough that
    // nothing completed, or a producer died mid-stamp and the collector had to
    // step over its slot. The file, the one output anybody watches over time,
    // said nothing happened.
    TempFiles files("collector_row");

    Collector c;
    config    cfg              = quiet_config();
    cfg.point_count            = static_cast<point_id>(pt::count);
    cfg.name_of                = &names;
    cfg.path                   = files.csv;
    cfg.summary_path           = "";
    cfg.flush_interval         = std::chrono::hours(1);  // only the flushes asked for
    cfg.stalled_sample_timeout = std::chrono::milliseconds(20);
    ASSERT_TRUE(c.start(cfg));

    // A slot claimed and never published: what a producer killed between
    // reserve() and publish() leaves behind. No span is stamped in this test,
    // so when the sweep steps over the hole the interval has a stalled count
    // and nothing else whatsoever.
    ASSERT_NE(c.ring(), nullptr);
    (void)c.ring()->reserve(1);

    // Long enough for the hole to outlive stalled_sample_timeout and for the
    // collector's poll to come back round to it.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    c.flush();
    c.shutdown();

    const auto lines = read_lines(files.csv);
    ASSERT_GE(lines.size(), 2u) << "the stalled slot has to have produced a row";
    const auto header = split(lines[0], ',');

    const size_t c_point   = column_of(header, "point");
    const size_t c_count   = column_of(header, "count");
    const size_t c_stalled = column_of(header, "stalled");
    ASSERT_LT(c_point, header.size());
    ASSERT_LT(c_count, header.size());
    ASSERT_LT(c_stalled, header.size());

    size_t rows_with_stalls = 0;
    for (size_t i = 1; i < lines.size(); ++i) {
        const auto row = split(lines[i], ',');
        ASSERT_EQ(row.size(), header.size()) << "row " << i;
        if (std::stoull(row[c_stalled]) == 0) {
            continue;
        }
        ++rows_with_stalls;
        EXPECT_EQ(row[c_point], "collector") << "the count belongs to no point";
        EXPECT_EQ(std::stoull(row[c_count]), 0u) << "nothing completed this interval";
    }

    // Once, in the interval it happened: the row stands in for rows that were
    // not written, so a second one would be the same slot counted twice.
    EXPECT_EQ(rows_with_stalls, 1u);
}

TEST(Output, PercentilesTooCloseToPrintAreStillDifferentColumns) {
    // Regression: the label went through the stream's default six significant
    // digits, so 99.9999999 and 99.99999991 both came out "p100". Two sessions
    // asking for different percentiles then agreed on a header, and the second
    // appended its numbers under the first's column names - the corruption the
    // header check exists to refuse, waved through by the check itself.
    EXPECT_NE(detail::percentile_label(99.9999999), detail::percentile_label(99.99999991));

    // And the ordinary labels keep their ordinary spelling: rendering every
    // one at max_digits10 would have made p99_9 "p99_900000000000006" and
    // orphaned every file already written.
    EXPECT_EQ(detail::percentile_label(50.0), "p50");
    EXPECT_EQ(detail::percentile_label(99.9), "p99_9");
    EXPECT_EQ(detail::percentile_label(99.99), "p99_99");

    TempFiles files("percentile_precision");

    auto session = [&](double p) {
        config cfg       = quiet_config();
        cfg.point_count  = static_cast<point_id>(pt::count);
        cfg.name_of      = &names;
        cfg.path         = files.csv;
        cfg.summary_path = "";
        cfg.percentiles  = {p};
        return cfg;
    };

    {
        Collector c;
        ASSERT_TRUE(c.start(session(99.9999999)));
        for (int i = 0; i < 100; ++i) {
            begin(pt::pipeline);
            end(pt::pipeline);
        }
        c.shutdown();
    }

    const auto lines = read_lines(files.csv);
    ASSERT_GE(lines.size(), 2u);
    const auto header = split(lines[0], ',');
    EXPECT_LT(column_of(header, "p99_9999999_ns"), header.size())
        << "the header has to name the percentile it was asked for, in full";

    {
        Collector c;
        EXPECT_THROW((void)c.start(session(99.99999991)), std::invalid_argument);
    }

    // The same value still appends, so the fix did not make the schema depend
    // on anything but the number.
    {
        Collector c;
        EXPECT_TRUE(c.start(session(99.9999999)));
        c.shutdown();
    }
}
