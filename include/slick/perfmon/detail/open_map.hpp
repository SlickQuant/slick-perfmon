// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Slick Quant

#pragma once

#include <slick/perfmon/config.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace slick::perfmon::detail {

/// splitmix64 finalizer. Our keys are packed bitfields - (point << 32) | seq,
/// or (point << 16) | step pair - so the low bits carry almost no entropy and
/// a bare mask would pile every point onto a handful of slots.
constexpr uint64_t mix64(uint64_t x) noexcept {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

constexpr uint64_t next_pow2(uint64_t v) noexcept {
    if (v < 2) {
        return 2;
    }
    --v;
    v |= v >> 1;  v |= v >> 2;  v |= v >> 4;
    v |= v >> 8;  v |= v >> 16; v |= v >> 32;
    return v + 1;
}

/**
 * @brief Fixed-capacity uint64 -> V map with linear probing.
 *
 * Lives entirely on the collector thread, so there is nothing atomic here. It
 * exists rather than std::unordered_map because both call sites are on the
 * drain loop: one lookup per event, and a node-based container would put a
 * cache miss and an allocation in that path.
 *
 * Erase uses Knuth's backward-shift deletion rather than tombstones. Open spans
 * are erased on every span end, so tombstones would accumulate as fast as
 * entries and force constant rehashing.
 *
 * Capacity is allocated once and never grows: this is a measurement tool, and
 * an allocation storm inside the collector would distort the thing it measures.
 * insert() returning nullptr when full is the honest failure, and every caller
 * counts it.
 */
template <typename V>
class open_map {
public:
    open_map() = default;

    /// @param max_entries the most live entries allowed; capacity is sized so
    ///        the table never exceeds a 0.5 load factor, where linear probing
    ///        still averages well under two probes.
    explicit open_map(uint32_t max_entries) { reset(max_entries); }

    void reset(uint32_t max_entries) {
        max_entries_ = max_entries == 0 ? 1u : max_entries;
        const uint64_t cap = next_pow2(static_cast<uint64_t>(max_entries_) * 2ull);
        keys_.assign(static_cast<size_t>(cap), 0);
        used_.assign(static_cast<size_t>(cap), 0);
        vals_.assign(static_cast<size_t>(cap), V{});
        mask_ = static_cast<size_t>(cap - 1);
        size_ = 0;
    }

    size_t size() const noexcept { return size_; }
    size_t capacity() const noexcept { return max_entries_; }
    bool   full() const noexcept { return size_ >= max_entries_; }

    V* find(uint64_t key) noexcept {
        if (used_.empty()) {
            return nullptr;
        }
        size_t i = home(key);
        while (used_[i]) {
            if (keys_[i] == key) {
                return &vals_[i];
            }
            i = (i + 1) & mask_;
        }
        return nullptr;
    }

    /// Insert, or return the existing value if the key is already present.
    /// Returns nullptr when the table is at capacity and the key is new.
    V* insert(uint64_t key, V value) {
        if (used_.empty()) {
            return nullptr;
        }
        size_t i = home(key);
        while (used_[i]) {
            if (keys_[i] == key) {
                return &vals_[i];
            }
            i = (i + 1) & mask_;
        }
        if (full()) {
            return nullptr;
        }
        keys_[i] = key;
        vals_[i] = std::move(value);
        used_[i] = 1;
        ++size_;
        return &vals_[i];
    }

    bool erase(uint64_t key) noexcept {
        if (used_.empty()) {
            return false;
        }
        size_t i = home(key);
        while (used_[i]) {
            if (keys_[i] == key) {
                erase_at(i);
                --size_;
                return true;
            }
            i = (i + 1) & mask_;
        }
        return false;
    }

    /// Visit every live entry. `fn(uint64_t key, V&)`.
    template <typename Fn>
    void for_each(Fn&& fn) {
        for (size_t i = 0; i <= mask_ && !used_.empty(); ++i) {
            if (used_[i]) {
                fn(keys_[i], vals_[i]);
            }
        }
    }

    /// Erase every entry for which `pred(key, V&)` is true, in one pass.
    ///
    /// Backward-shift deletion can move a live entry into the slot just
    /// vacated, so the scan re-tests that slot instead of stepping over it -
    /// but it never restarts. Restarting made expiry quadratic in the number
    /// of removals, which is exactly the case this exists for: a producer that
    /// died holding thousands of open spans turns one sweep into millions of
    /// probes, inside the collector, while it is meant to be draining.
    ///
    /// Nothing is skipped, because erase_at() only ever moves an entry
    /// *backwards* along the cyclic probe order starting at the slot being
    /// cleared. An entry the scan has not reached can therefore only land on
    /// the current slot, which is re-tested; an entry that has already been
    /// tested can land ahead of the scan and be tested again. `pred` must
    /// accordingly be side-effect-free when it returns false, as sweeps are -
    /// they count only the entries they evict.
    template <typename Pred>
    size_t erase_if(Pred&& pred) {
        if (used_.empty()) {
            return 0;
        }
        size_t removed = 0;
        for (size_t i = 0; i <= mask_;) {
            if (used_[i] && pred(keys_[i], vals_[i])) {
                erase_at(i);
                --size_;
                ++removed;
                continue;  // slot i may now hold a shifted entry
            }
            ++i;
        }
        return removed;
    }

    /// Drop every entry, keeping the allocation and the capacity.
    ///
    /// reset() would do the job, but it reassigns three vectors; this is the
    /// one the collector wants when it invalidates a cache every flush.
    void clear() noexcept {
        std::fill(used_.begin(), used_.end(), static_cast<uint8_t>(0));
        size_ = 0;
    }

private:
    size_t home(uint64_t key) const noexcept {
        return static_cast<size_t>(mix64(key)) & mask_;
    }

    /// Cyclic distance from a to b, in slots.
    size_t dist(size_t a, size_t b) const noexcept { return (b - a) & mask_; }

    /// Knuth Algorithm R. After clearing slot i, walk forward looking for an
    /// entry whose home position makes it legal to move back into i; moving it
    /// opens a new hole, so repeat from there.
    void erase_at(size_t i) noexcept {
        size_t j = i;
        while (true) {
            used_[i] = 0;
            while (true) {
                j = (j + 1) & mask_;
                if (!used_[j]) {
                    return;
                }
                const size_t k = home(keys_[j]);
                // The entry at j may fill i only if i lies within its probe
                // run, i.e. i is cyclically inside [k, j].
                if (dist(k, i) <= dist(k, j)) {
                    break;
                }
            }
            keys_[i] = keys_[j];
            vals_[i] = std::move(vals_[j]);
            used_[i] = 1;
            i = j;
        }
    }

    std::vector<uint64_t> keys_;
    std::vector<uint8_t>  used_;
    std::vector<V>        vals_;
    size_t                mask_        = 0;
    size_t                size_        = 0;
    uint32_t              max_entries_ = 0;
};

}  // namespace slick::perfmon::detail
