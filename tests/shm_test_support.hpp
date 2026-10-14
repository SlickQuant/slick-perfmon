// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Slick Quant
//
// Helpers for the tests that create real shared-memory segments.
//
// Apart from test_support.hpp because that header is shared with tests that
// have no business including slick-shm - and, on Windows, <windows.h> with it.
// Only built where instrumentation is enabled: a disabled build has no
// slick-shm at all.

#pragma once

// Not slick-shm directly: this is the include that guards against the min and
// max macros <windows.h> would otherwise leave behind.
#include <slick/perfmon/control_block.hpp>

#include "test_support.hpp"

#include <string>

namespace slick::perfmon::test {

/**
 * @brief A shm name unique to one test, with the segments created under it
 *        removed when the test ends.
 *
 * The removal has to be explicit, because nothing in the library does it. On
 * POSIX a shm object outlives every process that mapped it until somebody
 * unlinks the name, and neither the ring (slick-queue, since 2.3.0) nor the
 * control block nor the Collector ever does: none of them can know whether a
 * peer is still attached, so removing a name is left to whoever coordinates
 * the session. In a test that is the test. Without this every shm test leaves
 * its segments behind in /dev/shm - 2 MB a ring at the default capacity, for
 * good, since the names are unique per run and nothing asks for them again.
 *
 * A guard rather than a call at the bottom of each test, so the segments go
 * even when an ASSERT returns early.
 *
 * Both names are removed whichever the test used: a Collector session keeps
 * its ring under the name and its control block under name + ".meta", while a
 * test driving ControlBlockOwner directly puts the block under the name
 * itself. Removing a name that is not there is harmless. On Windows a segment
 * is freed with its last handle and remove() is a documented no-op, so this is
 * one code path rather than two.
 */
class scoped_shm_name {
public:
    explicit scoped_shm_name(const char* tag) : name_(unique_shm_name(tag)) {}

    ~scoped_shm_name() {
        slick::shm::shared_memory::remove(name_.c_str());
        slick::shm::shared_memory::remove((name_ + SLICK_PERFMON_META_SUFFIX).c_str());
    }

    scoped_shm_name(const scoped_shm_name&)            = delete;
    scoped_shm_name& operator=(const scoped_shm_name&) = delete;

    const std::string& str() const noexcept { return name_; }

    /// So a test passes it wherever it used to pass the name itself.
    operator const std::string&() const noexcept { return name_; }

private:
    std::string name_;
};

}  // namespace slick::perfmon::test
