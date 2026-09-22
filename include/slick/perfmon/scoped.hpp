// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Slick Quant

#pragma once

#include <slick/perfmon/config.hpp>
#include <slick/perfmon/stamp_types.hpp>

namespace slick::perfmon {

/**
 * @brief RAII span guard: begin() on construction, end() on destruction.
 *
 * A wrapper over the two stamps, not a second mechanism - it emits the same
 * instructions, with one fewer way to get it wrong. The benchmark asserts it
 * stays within noise of a hand-written pair.
 *
 * Two behaviours worth knowing:
 *
 * - The end stamp is published during exception unwinding, because the
 *   destructor runs. That is usually what you want, but the measurement then
 *   includes the unwind path, so a throwing region shows up as an outlier
 *   rather than as nothing. cancel() is the opt-out.
 * - Nesting measures true nesting as long as the nested guards use different
 *   points or different seq values. Nesting the same (point, seq) re-opens the
 *   span, which the backend counts as `abandoned`.
 *
 * Non-copyable, movable. A moved-from guard is disarmed, so exactly one end
 * stamp is published no matter how the object travels.
 */
class ScopedSample {
public:
    template <point_like T>
    explicit ScopedSample(T p, span_seq seq = kNoSeq) noexcept
        : point_(to_point(p)), seq_(seq) {
        ::slick::perfmon::begin(point_, seq_);
    }

    ~ScopedSample() noexcept {
        if (armed_) {
            ::slick::perfmon::end(point_, seq_);
        }
    }

    ScopedSample(const ScopedSample&) = delete;
    ScopedSample& operator=(const ScopedSample&) = delete;

    ScopedSample(ScopedSample&& other) noexcept
        : point_(other.point_), seq_(other.seq_), armed_(other.armed_) {
        other.armed_ = false;
    }

    ScopedSample& operator=(ScopedSample&& other) noexcept {
        if (this != &other) {
            if (armed_) {
                ::slick::perfmon::end(point_, seq_);
            }
            point_       = other.point_;
            seq_         = other.seq_;
            armed_       = other.armed_;
            other.armed_ = false;
        }
        return *this;
    }

    /// Stamp an intermediate boundary inside this span.
    void step(uint8_t n) noexcept { ::slick::perfmon::step(point_, n, seq_); }

    /// Suppress the end stamp. The span is left open, and the backend closes it
    /// as `abandoned` when the point is next begun or the entry ages out.
    void cancel() noexcept { armed_ = false; }

    point_id point() const noexcept { return point_; }
    span_seq seq() const noexcept { return seq_; }

private:
    point_id point_;
    span_seq seq_;
    bool     armed_ = true;
};

using scoped_sample = ScopedSample;

}  // namespace slick::perfmon

// ============================================================================
// Macros
//
// The functions above already degrade to nothing when the library is disabled,
// so these exist for convenience rather than necessity - except SCOPE, which
// needs the __LINE__ trick to let two guards coexist in one function.
// ============================================================================

#if SLICK_PERFMON_ENABLED

#define SLICK_PERFMON_BEGIN(...) ::slick::perfmon::begin(__VA_ARGS__)
#define SLICK_PERFMON_STEP(...)  ::slick::perfmon::step(__VA_ARGS__)
#define SLICK_PERFMON_END(...)   ::slick::perfmon::end(__VA_ARGS__)

#define SLICK_PERFMON_SCOPE(...)                                  \
    ::slick::perfmon::ScopedSample SLICK_PERFMON_CAT(             \
        slick_perfmon_scope_, __LINE__){__VA_ARGS__}

#else

#define SLICK_PERFMON_BEGIN(...) ((void)0)
#define SLICK_PERFMON_STEP(...)  ((void)0)
#define SLICK_PERFMON_END(...)   ((void)0)
#define SLICK_PERFMON_SCOPE(...) ((void)0)

#endif
