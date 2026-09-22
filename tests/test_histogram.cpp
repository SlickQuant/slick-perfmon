// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Slick Quant

#include <slick/perfmon/histogram.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

using namespace slick::perfmon;

namespace {

/// Exact percentile of a sorted sample, using the same "at least p% are <=
/// this" rule the histogram implements, so a disagreement means the histogram
/// is wrong rather than the two using different definitions.
double exact_percentile(std::vector<uint64_t> v, double p) {
    std::sort(v.begin(), v.end());
    if (v.empty()) {
        return 0.0;
    }
    auto rank = static_cast<size_t>((p / 100.0) * static_cast<double>(v.size()) + 0.5);
    if (rank == 0) {
        rank = 1;
    }
    if (rank > v.size()) {
        rank = v.size();
    }
    return static_cast<double>(v[rank - 1]);
}

void expect_within_error(double got, double expected) {
    // A bucket midpoint is at most half a bucket from any value in it, which is
    // exactly kRelativeError. Allow a hair more for the exact/approximate rank
    // landing in adjacent buckets.
    const double tolerance = expected * Histogram::kRelativeError * 2.0 + 1.0;
    EXPECT_NEAR(got, expected, tolerance)
        << "expected " << expected << ", got " << got << ", tolerance " << tolerance;
}

}  // namespace

TEST(Histogram, CountsSmallValuesExactly) {
    // Below 2^precision every value gets its own bucket, so these are not
    // approximations at all.
    Histogram h;
    for (uint64_t v = 0; v < Histogram::kSubBuckets; ++v) {
        EXPECT_EQ(Histogram::index_of(v), v);
        EXPECT_EQ(Histogram::bucket_lower(static_cast<uint32_t>(v)), v);
        EXPECT_EQ(Histogram::bucket_width(static_cast<uint32_t>(v)), 1u);
    }
    h.add(0);
    h.add(1);
    h.add(2);
    EXPECT_EQ(h.total(), 3u);
}

TEST(Histogram, BucketIndicesAreMonotonicAndContiguous) {
    uint32_t previous = 0;
    for (uint64_t v = 0; v < (1ull << 22); v = v + 1 + v / 64) {
        const uint32_t idx = Histogram::index_of(v);
        EXPECT_GE(idx, previous) << "value " << v << " went backwards";
        EXPECT_LT(idx, Histogram::kBucketCount);
        // The value must fall inside the bucket it was mapped to.
        const uint64_t lo = Histogram::bucket_lower(idx);
        EXPECT_LE(lo, v);
        EXPECT_LT(v, lo + Histogram::bucket_width(idx));
        previous = idx;
    }
}

TEST(Histogram, IndexZeroBoundaryIsContinuous) {
    // The linear region has to hand over to the log region without a gap: the
    // first log bucket must start exactly where the linear one stops.
    const uint32_t last_linear = Histogram::index_of(Histogram::kSubBuckets - 1);
    const uint32_t first_log   = Histogram::index_of(Histogram::kSubBuckets);
    EXPECT_EQ(first_log, last_linear + 1);
    EXPECT_EQ(Histogram::bucket_lower(first_log), Histogram::kSubBuckets);
}

TEST(Histogram, HandlesExtremeValues) {
    Histogram h;
    h.add(0);
    h.add(1);
    h.add(std::numeric_limits<uint32_t>::max());
    h.add(std::numeric_limits<uint64_t>::max());
    EXPECT_EQ(h.total(), 4u);
    EXPECT_LT(Histogram::index_of(std::numeric_limits<uint64_t>::max()),
              Histogram::kBucketCount);
    // p100 must land in the top occupied bucket, not wrap or saturate to zero.
    EXPECT_GT(h.percentile(100.0), 1e18);
}

TEST(Histogram, EmptyHistogramReportsZeroRatherThanGarbage) {
    Histogram h;
    EXPECT_EQ(h.total(), 0u);
    EXPECT_DOUBLE_EQ(h.percentile(50.0), 0.0);
    EXPECT_DOUBLE_EQ(h.percentile(0.0), 0.0);
    EXPECT_DOUBLE_EQ(h.percentile(100.0), 0.0);
}

TEST(Histogram, RecoversPercentilesOfAUniformDistribution) {
    std::mt19937_64                              rng(12345);
    std::uniform_int_distribution<uint64_t>      dist(100, 100000);
    std::vector<uint64_t>                        values;
    Histogram                                    h;

    for (int i = 0; i < 200000; ++i) {
        const uint64_t v = dist(rng);
        values.push_back(v);
        h.add(v);
    }

    for (double p : {50.0, 90.0, 99.0, 99.9}) {
        expect_within_error(h.percentile(p), exact_percentile(values, p));
    }
}

TEST(Histogram, RecoversPercentilesOfALatencyShapedDistribution) {
    // A tight body with a long tail is what real latency looks like, and it is
    // where a linear histogram would either lose the body or miss the tail.
    std::mt19937_64                     rng(999);
    std::lognormal_distribution<double> dist(6.0, 1.2);
    std::vector<uint64_t>               values;
    Histogram                           h;

    for (int i = 0; i < 200000; ++i) {
        const auto v = static_cast<uint64_t>(dist(rng));
        values.push_back(v);
        h.add(v);
    }

    for (double p : {50.0, 90.0, 99.0, 99.9}) {
        expect_within_error(h.percentile(p), exact_percentile(values, p));
    }
}

TEST(Histogram, ANonFinitePercentileIsNotUndefinedBehaviour) {
    // Every comparison against a NaN is false, so a bounds check written as
    // `p <= 0.0` lets one straight through to the rank arithmetic, where
    // static_cast<uint64_t>(NaN) is undefined - an unpredictable rank, or a
    // trap. percentile() has to be total over every double it can be handed,
    // because it is public and the cast is not something a caller can be
    // expected to know about.
    Histogram h;
    for (uint64_t v = 1; v <= 100; ++v) {
        h.add(v);
    }

    const double lo = h.percentile(0.0);
    const double hi = h.percentile(100.0);

    EXPECT_DOUBLE_EQ(h.percentile(std::numeric_limits<double>::quiet_NaN()), lo);
    EXPECT_DOUBLE_EQ(h.percentile(-std::numeric_limits<double>::infinity()), lo);
    EXPECT_DOUBLE_EQ(h.percentile(std::numeric_limits<double>::infinity()), hi);
}

TEST(Histogram, PercentilesAreOrdered) {
    std::mt19937_64                     rng(7);
    std::lognormal_distribution<double> dist(5.0, 1.0);
    Histogram                           h;
    for (int i = 0; i < 50000; ++i) {
        h.add(static_cast<uint64_t>(dist(rng)));
    }
    EXPECT_LE(h.percentile(50.0), h.percentile(90.0));
    EXPECT_LE(h.percentile(90.0), h.percentile(99.0));
    EXPECT_LE(h.percentile(99.0), h.percentile(99.9));
    EXPECT_LE(h.percentile(99.9), h.percentile(100.0));
}

TEST(Histogram, MergePreservesTheCombinedDistribution) {
    std::mt19937_64                         rng(4242);
    std::uniform_int_distribution<uint64_t> dist(1, 50000);
    Histogram                               a, b, both;
    std::vector<uint64_t>                   values;

    for (int i = 0; i < 50000; ++i) {
        const uint64_t v = dist(rng);
        values.push_back(v);
        (i % 2 == 0 ? a : b).add(v);
        both.add(v);
    }

    a.merge(b);
    EXPECT_EQ(a.total(), both.total());
    for (double p : {50.0, 90.0, 99.0}) {
        EXPECT_DOUBLE_EQ(a.percentile(p), both.percentile(p));
    }
}

TEST(Histogram, ClearResetsEverything) {
    Histogram h;
    for (uint64_t v = 1; v < 1000; ++v) {
        h.add(v);
    }
    h.clear();
    EXPECT_EQ(h.total(), 0u);
    EXPECT_DOUBLE_EQ(h.percentile(50.0), 0.0);
}

TEST(Histogram, StatedErrorBoundActuallyHolds) {
    // The documented guarantee is a half-bucket, and it is the number the
    // README quotes - so check it against every bucket rather than trusting
    // the arithmetic in the comment.
    //
    // The error is measured from the value actually reported, which is the
    // bucket midpoint. That matters at the bottom of the range: the linear
    // region has width 1, so its midpoint IS the value and the error is zero,
    // even though half a bucket over a small lower bound would look enormous.
    for (uint32_t i = 0; i < Histogram::kBucketCount; ++i) {
        const uint64_t lo = Histogram::bucket_lower(i);
        if (lo == 0) {
            continue;
        }
        const uint64_t width    = Histogram::bucket_width(i);
        const uint64_t midpoint = Histogram::bucket_midpoint(i);
        const uint64_t hi       = lo + width - 1;

        ASSERT_GE(midpoint, lo) << "bucket " << i;
        ASSERT_LE(midpoint, hi) << "bucket " << i;

        // Worst case over the bucket is at its lower edge, where the reported
        // midpoint is furthest from the truth relative to the truth.
        const double worst =
            static_cast<double>(midpoint - lo) / static_cast<double>(lo);
        EXPECT_LE(worst, Histogram::kRelativeError + 1e-12)
            << "bucket " << i << " lower=" << lo << " width=" << width
            << " midpoint=" << midpoint;
    }
}

TEST(Histogram, TheLinearRegionIsExact) {
    // Values below 2^precision get a bucket each, so there is no approximation
    // at all down there - which is what keeps sub-100-cycle stages readable.
    for (uint64_t v = 0; v < Histogram::kSubBuckets; ++v) {
        const uint32_t idx = Histogram::index_of(v);
        EXPECT_EQ(Histogram::bucket_midpoint(idx), v) << "value " << v;
    }
}

TEST(Histogram, ABucketHoldsMoreThanFourBillionSamples) {
    // Regression: the buckets were uint32_t while total() was uint64_t, so a
    // hot stage silently wrapped one bucket to zero after 2^32 samples into it
    // - a few hours of a single busy point - and every percentile derived from
    // that bucket became wrong with nothing to show for it.
    //
    // Doubling by merge rather than adding 2^34 samples one at a time: the
    // wrap is a property of the counter width, not of how it was reached, and
    // this runs in microseconds instead of minutes.
    Histogram h;
    h.add(7);  // linear region, so the reported value is exact

    constexpr int kDoublings = 34;  // 2^34, comfortably past a 32-bit bucket
    for (int i = 0; i < kDoublings; ++i) {
        const Histogram copy = h;
        h.merge(copy);
    }

    EXPECT_EQ(h.total(), uint64_t{1} << kDoublings);
    // With a 32-bit bucket the count wraps to zero here while total() does not,
    // the percentile walk finds nothing, and this reads 0 instead of 7.
    EXPECT_DOUBLE_EQ(h.percentile(50.0), 7.0);
    EXPECT_DOUBLE_EQ(h.percentile(99.9), 7.0);
    EXPECT_DOUBLE_EQ(h.percentile(100.0), 7.0);
}

// --------------------------------------------------------------------------
// The occupied band.
//
// clear(), merge() and percentile() only touch the range of buckets that has
// actually been written, which is what keeps a flush from walking 7.6 KB per
// stage. Everything below guards the invariant that makes that legal: every
// bucket outside the band is zero.
// --------------------------------------------------------------------------

TEST(Histogram, ReuseAcrossClearCyclesMatchesAFreshHistogram) {
    // The band is bookkeeping carried across clear(), so a histogram reused for
    // interval after interval - which is exactly how the collector uses it -
    // must report what a brand new one would.
    Histogram reused;
    std::mt19937_64                        rng(4242);
    std::uniform_int_distribution<uint64_t> dist(1, 5'000'000);

    for (int round = 0; round < 8; ++round) {
        std::vector<uint64_t> values;
        for (int i = 0; i < 5000; ++i) {
            values.push_back(dist(rng));
        }

        reused.clear();
        Histogram fresh;
        for (uint64_t v : values) {
            reused.add(v);
            fresh.add(v);
        }

        ASSERT_EQ(reused.total(), fresh.total()) << "round " << round;
        for (double p : {0.0, 1.0, 25.0, 50.0, 90.0, 99.0, 99.9, 100.0}) {
            EXPECT_DOUBLE_EQ(reused.percentile(p), fresh.percentile(p))
                << "round " << round << ", p" << p;
        }
    }
}

TEST(Histogram, ClearLeavesNoCountsBehindForTheNextInterval) {
    // If clear() zeroed only part of what had been written, the leftovers would
    // silently join the next interval's distribution.
    Histogram h;
    for (uint64_t v = 1; v <= 100000; v *= 10) {
        h.add(v);  // deliberately spread across many powers of two
    }
    h.clear();
    EXPECT_EQ(h.total(), 0u);
    EXPECT_DOUBLE_EQ(h.percentile(50.0), 0.0);

    h.add(7);
    EXPECT_EQ(h.total(), 1u);
    EXPECT_DOUBLE_EQ(h.percentile(0.0), 7.0) << "a stale count would drag this down";
    EXPECT_DOUBLE_EQ(h.percentile(100.0), 7.0) << "a stale count would drag this up";
    EXPECT_DOUBLE_EQ(h.percentile(50.0), 7.0);
}

TEST(Histogram, MergingDisjointRangesKeepsBothEnds) {
    // The merged band has to be the union, not either operand's.
    Histogram low;
    for (int i = 0; i < 100; ++i) {
        low.add(5);
    }
    Histogram high;
    for (int i = 0; i < 100; ++i) {
        high.add(50'000'000);
    }

    Histogram merged;
    merged.merge(high);  // high first, so the band has to extend downwards too
    merged.merge(low);

    EXPECT_EQ(merged.total(), 200u);
    EXPECT_DOUBLE_EQ(merged.percentile(0.0), 5.0);
    EXPECT_NEAR(merged.percentile(100.0), 50'000'000.0,
                50'000'000.0 * Histogram::kRelativeError);
    EXPECT_NEAR(merged.percentile(50.0), 5.0, 1.0);
}

TEST(Histogram, MergingAnEmptyHistogramChangesNothing) {
    Histogram h;
    for (uint64_t v = 100; v < 200; ++v) {
        h.add(v);
    }
    const double before_lo = h.percentile(0.0);
    const double before_hi = h.percentile(100.0);

    const Histogram empty;
    h.merge(empty);

    EXPECT_EQ(h.total(), 100u);
    EXPECT_DOUBLE_EQ(h.percentile(0.0), before_lo);
    EXPECT_DOUBLE_EQ(h.percentile(100.0), before_hi);

    // And an empty one merging a populated one takes its band wholesale.
    Histogram fresh;
    fresh.merge(h);
    EXPECT_EQ(fresh.total(), 100u);
    EXPECT_DOUBLE_EQ(fresh.percentile(0.0), before_lo);
    EXPECT_DOUBLE_EQ(fresh.percentile(100.0), before_hi);
}

TEST(Histogram, MergeAfterClearStartsFromNothing) {
    Histogram dst;
    for (int i = 0; i < 50; ++i) {
        dst.add(1'000'000);
    }
    dst.clear();

    Histogram src;
    for (int i = 0; i < 3; ++i) {
        src.add(11);
    }
    dst.merge(src);

    EXPECT_EQ(dst.total(), 3u);
    EXPECT_DOUBLE_EQ(dst.percentile(100.0), 11.0)
        << "the cleared band must not resurrect the previous interval";
}
