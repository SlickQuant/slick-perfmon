// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Slick Quant

#pragma once

#include <slick/perfmon/config.hpp>

#include <array>
#include <cstdint>

#if defined(_MSC_VER)
    #include <intrin.h>
#endif

namespace slick::perfmon {

namespace detail {

/// floor(log2(v)) for v >= 1. Undefined at 0, which callers filter first.
inline uint32_t floor_log2(uint64_t v) noexcept {
#if defined(_MSC_VER)
    unsigned long idx = 0;
    _BitScanReverse64(&idx, v);
    return static_cast<uint32_t>(idx);
#else
    return 63u - static_cast<uint32_t>(__builtin_clzll(v));
#endif
}

}  // namespace detail

/**
 * @brief Log-linear latency histogram with bounded relative error.
 *
 * Values below 2^P are counted exactly. Above that, each power of two is split
 * into 2^P equal sub-buckets, so the worst-case relative error on a reported
 * value is one half-bucket: 1/2^(P+1), which is ~3% at the default P = 4.
 *
 * Fixed size, allocated with the stage and never resized - an exact record of
 * every sample would grow without bound, and a measurement tool that allocates
 * under load distorts what it is measuring. The price is that percentiles are
 * approximate, with an error that is stated rather than hoped for.
 */
class Histogram {
public:
    static constexpr uint32_t kPrecision  = SLICK_PERFMON_HIST_PRECISION;
    static constexpr uint32_t kSubBuckets = 1u << kPrecision;

    /// Exponents 0..63 each contribute kSubBuckets slots above the linear
    /// region, which itself occupies the first kSubBuckets.
    static constexpr uint32_t kBucketCount = (64u - kPrecision + 1u) * kSubBuckets;

    /// Worst-case relative error of any value this histogram reports.
    static constexpr double kRelativeError = 1.0 / static_cast<double>(2u * kSubBuckets);

    void add(uint64_t value) noexcept {
        const uint32_t i = index_of(value);
        ++counts_[i];
        ++total_;
        // Two predictable compares per sample, which buy back far more than
        // they cost at every flush: a stage's latencies occupy a narrow band of
        // the 976 buckets, and merging, clearing and reading percentiles then
        // touch only that band instead of the whole array. See used_hi_.
        if (i < used_lo_) {
            used_lo_ = i;
        }
        if (i > used_hi_) {
            used_hi_ = i;
        }
    }

    void clear() noexcept {
        // Only the occupied band can be non-zero, so only it needs zeroing.
        for (uint32_t i = used_lo_; i <= used_hi_; ++i) {
            counts_[i] = 0;
        }
        total_   = 0;
        used_lo_ = kBucketCount;
        used_hi_ = 0;
    }

    uint64_t total() const noexcept { return total_; }

    /// Merge another histogram into this one. Used to fold an interval's
    /// counts into the cumulative record without re-walking the samples.
    void merge(const Histogram& other) noexcept {
        if (other.total_ == 0) {
            return;
        }
        for (uint32_t i = other.used_lo_; i <= other.used_hi_; ++i) {
            counts_[i] += other.counts_[i];
        }
        total_ += other.total_;
        if (other.used_lo_ < used_lo_) {
            used_lo_ = other.used_lo_;
        }
        if (other.used_hi_ > used_hi_) {
            used_hi_ = other.used_hi_;
        }
    }

    /**
     * @brief Value at the given percentile, in the same unit as add().
     *
     * @param p percentile in [0, 100]. Returns 0 for an empty histogram - a
     *          caller reporting stats always checks count first. Outside the
     *          range, and for a value that is not a number at all, the nearer
     *          end of the observed range: this has to be total over every
     *          double a caller can hand it, for the reason below.
     */
    double percentile(double p) const noexcept {
        if (total_ == 0) {
            return 0.0;
        }
        // `!(p > 0.0)` rather than `p <= 0.0`, so a NaN takes this branch.
        // Every comparison against a NaN is false, so written the other way it
        // would fall through both bounds checks and reach the rank arithmetic
        // below, where `static_cast<uint64_t>(NaN)` is undefined behaviour -
        // an unpredictable rank, or a trap, from a single bad number in a
        // configuration vector. Collector::validate_config() rejects one at
        // the boundary; this makes the class safe on its own as well, because
        // it is public and the cast is not the kind of thing a caller can be
        // expected to know about.
        if (!(p > 0.0)) {
            return static_cast<double>(bucket_midpoint(first_used()));
        }
        if (p >= 100.0) {
            return static_cast<double>(bucket_midpoint(last_used()));
        }

        // Rank of the sample we want, 1-based. ceil() so p = 50 on two samples
        // picks the second, matching the usual "at least p% are <= this" rule.
        const uint64_t rank = static_cast<uint64_t>(
            (p / 100.0) * static_cast<double>(total_) + 0.5);
        const uint64_t want = rank == 0 ? 1 : rank;

        uint64_t seen = 0;
        for (uint32_t i = used_lo_; i <= used_hi_; ++i) {
            seen += counts_[i];
            if (seen >= want) {
                return static_cast<double>(bucket_midpoint(i));
            }
        }
        return static_cast<double>(bucket_midpoint(last_used()));
    }

    /// Bucket index for a value. Exposed for tests; the mapping is the whole
    /// contract of this class.
    static uint32_t index_of(uint64_t value) noexcept {
        if (value < kSubBuckets) {
            return static_cast<uint32_t>(value);
        }
        const uint32_t exp   = detail::floor_log2(value);
        const uint32_t shift = exp - kPrecision;
        const uint32_t sub   = static_cast<uint32_t>(value >> shift) - kSubBuckets;
        return ((exp - kPrecision + 1u) << kPrecision) + sub;
    }

    /// Smallest value that lands in this bucket.
    static uint64_t bucket_lower(uint32_t index) noexcept {
        if (index < kSubBuckets) {
            return index;
        }
        const uint32_t block = index >> kPrecision;
        const uint32_t sub   = index & (kSubBuckets - 1u);
        const uint32_t shift = block - 1u;
        return static_cast<uint64_t>(kSubBuckets + sub) << shift;
    }

    /// Width of this bucket in value units.
    static uint64_t bucket_width(uint32_t index) noexcept {
        if (index < kSubBuckets) {
            return 1;
        }
        return uint64_t{1} << ((index >> kPrecision) - 1u);
    }

    /// Value reported for anything that lands in this bucket.
    ///
    /// The midpoint rather than the lower bound, which halves the worst-case
    /// error and is what makes kRelativeError a half-bucket and not a whole one.
    /// In the linear region the width is 1, so this is the exact value.
    static uint64_t bucket_midpoint(uint32_t index) noexcept {
        return bucket_lower(index) + bucket_width(index) / 2;
    }

private:

    /// Both are maintained by add() and merge() rather than searched for.
    /// Every caller is on the flush path, where the search was 976 loads.
    uint32_t first_used() const noexcept { return total_ == 0 ? 0 : used_lo_; }

    uint32_t last_used() const noexcept { return total_ == 0 ? 0 : used_hi_; }

    /// 64-bit, matching total_ and stats::count. A hot stage puts most of its
    /// samples into a handful of bins, so a 32-bit bin wraps after 4.29e9
    /// samples into that one bin - a few hours of a single busy point at the
    /// rates this library is built for. The wrap would be silent and would
    /// corrupt every percentile derived from the bucket, which is exactly the
    /// kind of quiet wrongness a measurement tool must not have. The price is
    /// ~7.6 KB per histogram instead of ~3.8 KB; it is paid by the collector,
    /// never by an instrumented thread, and config::max_stages bounds it.
    std::array<uint64_t, kBucketCount> counts_{};
    uint64_t                           total_ = 0;

    /// The occupied band, inclusive. Every bucket outside it is zero, which is
    /// what lets clear(), merge() and percentile() skip the rest of the array -
    /// a stage's samples cluster into a few dozen of the 976 buckets, so this
    /// is the difference between touching 7.6 KB per operation and touching a
    /// few cache lines.
    ///
    /// Empty is encoded as lo > hi, so every `for (i = lo; i <= hi; ++i)` loop
    /// is naturally a no-op on an untouched histogram and needs no guard.
    uint32_t used_lo_ = kBucketCount;
    uint32_t used_hi_ = 0;
};

}  // namespace slick::perfmon
