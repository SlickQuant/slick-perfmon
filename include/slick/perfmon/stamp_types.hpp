// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Slick Quant
//
// The types the hot path needs, and nothing else.
//
// Split out of types.hpp so <slick/perfmon.hpp> can stay cheap to include:
// the collector's config and its reporting structs need <chrono>, <string> and
// <vector>, and instrumented code has no business paying for those. Everything
// here is a scalar, a constant or a 16-byte POD, so this header costs three
// freestanding standard headers and no allocation-aware machinery at all.

#pragma once

#include <slick/perfmon/config.hpp>

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace slick::perfmon {

/// A measurement point. The full 32-bit range is the user's to partition;
/// slick-perfmon never allocates one.
using point_id = uint32_t;

/// (seq << 8) | step. Packed rather than two fields because the step and the
/// correlation id are always written and read together.
using event_id = uint32_t;

/// Correlation id distinguishing concurrently open spans on one point. Only the
/// low 24 bits survive the pack.
using span_seq = uint32_t;

inline constexpr uint8_t  kBeginStep = 0x00;
inline constexpr uint8_t  kEndStep   = 0xFF;
inline constexpr span_seq kNoSeq     = 0;
inline constexpr span_seq kMaxSeq    = 0x00FFFFFFu;

/// Synthetic step pair identifying the begin -> end total of a span. 0x0000
/// cannot collide with a real transition because a real one always ends on a
/// step that is not kBeginStep.
inline constexpr uint16_t kTotalStagePair = 0x0000;

constexpr event_id make_event(uint8_t step, span_seq seq = kNoSeq) noexcept {
    return (static_cast<event_id>(seq & kMaxSeq) << 8) | step;
}

constexpr uint8_t step_of(event_id e) noexcept { return static_cast<uint8_t>(e & 0xFFu); }

constexpr span_seq seq_of(event_id e) noexcept { return e >> 8; }

/**
 * @brief One timestamped event. Exactly 16 bytes, trivially copyable, and
 *        layout-stable across processes so it can live in shared memory.
 *
 * The timestamp stays a raw 64-bit TSC read: no delta, no unit conversion and
 * no saturation happen on the instrumented thread. That is what removes the
 * ~1.4 s ceiling a 32-bit cycle delta would have imposed, and it is why the
 * hot path has no arithmetic in it at all.
 */
struct alignas(16) sample {
    uint64_t timestamp;   ///< raw TSC
    point_id point;       ///< the user's measurement point
    event_id event;       ///< (seq << 8) | step
};

static_assert(sizeof(sample) == 16, "sample must stay one quarter of a cache line");
static_assert(std::is_trivially_copyable_v<sample>, "sample crosses a shared-memory boundary");
static_assert(offsetof(sample, point) + sizeof(point_id) == offsetof(sample, event),
              "point and event must be adjacent so the compiler can merge the two stores");

/// Any enum or integral convertible to point_id, so call sites need no cast.
template <typename T>
concept point_like = std::is_enum_v<T> || std::is_integral_v<T>;

template <point_like T>
constexpr point_id to_point(T p) noexcept {
    if constexpr (std::is_enum_v<T>) {
        return static_cast<point_id>(static_cast<std::underlying_type_t<T>>(p));
    } else {
        return static_cast<point_id>(p);
    }
}

}  // namespace slick::perfmon
