// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Slick Quant

#pragma once

#include <slick/perfmon/histogram.hpp>

#include <cmath>
#include <cstdint>
#include <limits>

namespace slick::perfmon {

/**
 * @brief Running statistics for one stage. Collector-thread-local, so nothing
 *        here is atomic.
 *
 * Variance uses Welford's method rather than accumulating sum-of-squares. With
 * cycle counts in the millions and millions of samples, the naive form loses
 * most of its significant digits to cancellation - the stddev of a tight
 * latency distribution is a small difference between two very large numbers.
 * Welford costs the same O(1) per sample and does not.
 */
class Accumulator {
public:
    void add(uint64_t value) noexcept {
        ++count_;
        if (value < min_) {
            min_ = value;
        }
        if (value > max_) {
            max_ = value;
        }
        sum_ += static_cast<double>(value);

        const double x     = static_cast<double>(value);
        const double delta = x - mean_;
        mean_ += delta / static_cast<double>(count_);
        m2_ += delta * (x - mean_);

        hist_.add(value);
    }

    void clear() noexcept {
        count_ = 0;
        min_   = (std::numeric_limits<uint64_t>::max)();
        max_   = 0;
        sum_   = 0.0;
        mean_  = 0.0;
        m2_    = 0.0;
        hist_.clear();
    }

    /// Fold this accumulator into another, so the cumulative record can be
    /// maintained without keeping a second copy of every sample.
    void merge_into(Accumulator& dst) const noexcept {
        if (count_ == 0) {
            return;
        }
        if (dst.count_ == 0) {
            dst.min_ = min_;
            dst.max_ = max_;
        } else {
            if (min_ < dst.min_) {
                dst.min_ = min_;
            }
            if (max_ > dst.max_) {
                dst.max_ = max_;
            }
        }

        // Chan's parallel form: combining two Welford states exactly, rather
        // than replaying samples we no longer have.
        const double n_a     = static_cast<double>(dst.count_);
        const double n_b     = static_cast<double>(count_);
        const double n       = n_a + n_b;
        const double delta   = mean_ - dst.mean_;
        dst.m2_   += m2_ + delta * delta * n_a * n_b / n;
        dst.mean_ += delta * n_b / n;
        dst.count_ += count_;
        dst.sum_   += sum_;
        dst.hist_.merge(hist_);
    }

    uint64_t count() const noexcept { return count_; }
    uint64_t min() const noexcept { return count_ == 0 ? 0 : min_; }
    uint64_t max() const noexcept { return max_; }
    double   mean() const noexcept { return count_ == 0 ? 0.0 : mean_; }
    double   sum() const noexcept { return sum_; }

    /// Sample standard deviation. Zero for fewer than two samples, where it is
    /// undefined rather than zero - but a row reading 0 next to count=1 is
    /// clearer than a NaN in a CSV.
    double stddev() const noexcept {
        if (count_ < 2) {
            return 0.0;
        }
        return std::sqrt(m2_ / static_cast<double>(count_ - 1));
    }

    /// Percentile, clamped to the range actually observed.
    ///
    /// The histogram reports a bucket midpoint, which for the top bucket can
    /// sit above every sample that landed in it. Without the clamp a report can
    /// show p99.9 exceeding max, which reads as a bug in the tool even though
    /// the estimate is within its stated error.
    double percentile(double p) const noexcept {
        if (count_ == 0) {
            return 0.0;
        }
        const double v = hist_.percentile(p);
        const double lo = static_cast<double>(min_);
        const double hi = static_cast<double>(max_);
        return v < lo ? lo : (v > hi ? hi : v);
    }

    const Histogram& histogram() const noexcept { return hist_; }

private:
    uint64_t  count_ = 0;
    uint64_t  min_   = (std::numeric_limits<uint64_t>::max)();
    uint64_t  max_   = 0;
    double    sum_   = 0.0;
    double    mean_  = 0.0;
    double    m2_    = 0.0;
    Histogram hist_;
};

}  // namespace slick::perfmon
