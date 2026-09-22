// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Slick Quant

#pragma once

#include <slick/perfmon/stamp_types.hpp>

#include <slick/queue.hpp>

namespace slick::perfmon {

/**
 * @brief Ring configuration.
 *
 * Pinning the traits explicitly is required, not stylistic. slick::default_queue_traits
 * resolves differently under NDEBUG, the queue type appears in our headers, and in
 * shared-memory mode every peer must agree on enable_read_last or the attach throws.
 * Naming the traits here makes a Debug producer and a Release collector a link error
 * rather than a runtime surprise.
 */
struct queue_traits : slick::queue_traits {
    /// The collector reads a cursor, never the latest item. Leaving this on
    /// would cost a CAS on every publish for something nothing calls.
    static constexpr bool enable_read_last = false;

    /// Consumer-side only, so the producer pays nothing. In an event model a
    /// dropped sample corrupts pairing rather than perturbing one number, so
    /// knowing the ring overflowed is not optional.
    static constexpr bool enable_loss_detection = true;
};

using sample_queue = slick::queue<sample, queue_traits>;

}  // namespace slick::perfmon
