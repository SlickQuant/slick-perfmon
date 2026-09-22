// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Slick Quant

#pragma once

#include <slick/perfmon/version.hpp>

// ============================================================================
// Tunables
//
// Every knob follows the #ifndef/#define pattern so a user can pre-define it
// before including any slick-perfmon header, or via target_compile_definitions.
// ============================================================================

/// Master switch. 0 compiles every stamp to nothing: no RDTSC, no queue, no
/// collector thread, no binary size. The instrumented source still compiles.
#ifndef SLICK_PERFMON_ENABLED
#define SLICK_PERFMON_ENABLED 1
#endif

/// Fence the timestamp reads. Off is roughly 20 cycles cheaper per stamp, but
/// lets the CPU hoist or sink work across the timestamp, which silently moves
/// work into or out of the measured region. On unless you know why you want it
/// off - a measurement that is cheap and wrong is worse than one that costs
/// twenty cycles.
#ifndef SLICK_PERFMON_SERIALIZE
#define SLICK_PERFMON_SERIALIZE 1
#endif

/// Bytes per name slot in the shared control block, including the NUL. Fixed
/// size because the table lives in shared memory and must have a stable layout
/// across processes. A longer name is rejected, never truncated: truncation
/// would silently alias two different points onto one row.
#ifndef SLICK_PERFMON_NAME_CAPACITY
#define SLICK_PERFMON_NAME_CAPACITY 64
#endif

/// Mantissa bits per histogram bucket. 4 gives 16 sub-buckets per power of two,
/// so the worst-case relative error on a reported percentile is 1/32 (~3%), and
/// a stage costs ~2.5 KB. Raising it halves the error and doubles the memory.
#ifndef SLICK_PERFMON_HIST_PRECISION
#define SLICK_PERFMON_HIST_PRECISION 4
#endif

// ============================================================================
// Portable compiler macros
//
// Rebased from slick-orderbook's config.hpp so the two stay recognisably the
// same header; only the prefix and the subset actually used here differ.
// ============================================================================

#if defined(__has_feature)
    #define SLICK_PERFMON_HAS_FEATURE(x) __has_feature(x)
#else
    #define SLICK_PERFMON_HAS_FEATURE(x) 0
#endif

#if defined(__SANITIZE_THREAD__) || defined(__TSAN__) || SLICK_PERFMON_HAS_FEATURE(thread_sanitizer)
    #define SLICK_PERFMON_TSAN_ENABLED 1
#else
    #define SLICK_PERFMON_TSAN_ENABLED 0
#endif

#if defined(__SANITIZE_ADDRESS__) || defined(__ASAN__) || SLICK_PERFMON_HAS_FEATURE(address_sanitizer)
    #define SLICK_PERFMON_ASAN_ENABLED 1
#else
    #define SLICK_PERFMON_ASAN_ENABLED 0
#endif

#if defined(_MSC_VER)
    #define SLICK_PERFMON_FORCE_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
    #define SLICK_PERFMON_FORCE_INLINE inline __attribute__((always_inline))
#else
    #define SLICK_PERFMON_FORCE_INLINE inline
#endif

#if defined(_MSC_VER)
    #define SLICK_PERFMON_NO_INLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    #define SLICK_PERFMON_NO_INLINE __attribute__((noinline))
#else
    #define SLICK_PERFMON_NO_INLINE
#endif

#define SLICK_PERFMON_CACHE_LINE_SIZE 64
#define SLICK_PERFMON_CACHE_ALIGNED alignas(SLICK_PERFMON_CACHE_LINE_SIZE)

// x86 is the only architecture where this library delivers on its overhead
// budget. Everything else falls back to steady_clock, which is correct but
// costs an order of magnitude more - so it warns rather than pretending.
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    #define SLICK_PERFMON_X86 1
#else
    #define SLICK_PERFMON_X86 0
#endif

/// Two-level concatenation, so __LINE__ expands before it is pasted.
#define SLICK_PERFMON_CAT_(a, b) a##b
#define SLICK_PERFMON_CAT(a, b) SLICK_PERFMON_CAT_(a, b)
