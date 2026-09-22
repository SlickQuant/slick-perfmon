// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Slick Quant
//
// What a user's own points file looks like. slick-perfmon never allocates an
// id - you declare them, and you hand it a constexpr function that turns a
// (point, step) pair into a label.

#pragma once

#include <slick/perfmon.hpp>
#include <slick/perfmon/types.hpp>

#include <string_view>

namespace demo {

/// Measurement points. The id space is yours: partition it across processes,
/// or share ids deliberately so their numbers merge.
enum class perf : slick::perfmon::point_id {
    tick_to_trade = 0,
    book_update   = 1,
    round_trip    = 2,
    count
};

/// Intermediate step boundaries within tick_to_trade. 0x00 is begin and 0xFF
/// is end, so these live in between.
enum : uint8_t {
    kDecode = 1,
    kMatch  = 2,
    kSend   = 3,
};

/// Step 0 names the point itself; the other steps name the stage that ends
/// there. Anything returning an empty view falls back to point_<p> /
/// step_<a>-><b> in the output.
constexpr std::string_view perf_name(slick::perfmon::point_id p, uint8_t step) noexcept {
    using namespace slick::perfmon;
    switch (static_cast<perf>(p)) {
    case perf::tick_to_trade:
        switch (step) {
        case kBeginStep: return "tick_to_trade";
        case kDecode:    return "decode";
        case kMatch:     return "match";
        case kSend:      return "send";
        case kEndStep:   return "publish";
        default:         return {};
        }
    case perf::book_update:
        return step == kBeginStep ? "book_update" : std::string_view{};
    case perf::round_trip:
        return step == kBeginStep ? "round_trip" : std::string_view{};
    default:
        return {};
    }
}

/// Fill in the two fields every session needs, so the examples do not repeat
/// themselves.
inline slick::perfmon::config base_config() {
    slick::perfmon::config cfg;
    cfg.point_count = static_cast<slick::perfmon::point_id>(perf::count);
    cfg.name_of     = &perf_name;
    return cfg;
}

}  // namespace demo
