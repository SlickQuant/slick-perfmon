// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Slick Quant

#pragma once

#include <slick/perfmon/types.hpp>

// windows.h - which slick-shm pulls in - defines min and max as macros, and
// they then break any std::numeric_limits<T>::max() later in the translation
// unit. slick-queue takes the same precaution around the same include.
#if defined(_WIN32) && !defined(NOMINMAX)
    #define NOMINMAX
#endif

#include <slick/shm/shared_memory.hpp>

#if defined(_WIN32)
    #ifdef min
        #undef min
    #endif
    #ifdef max
        #undef max
    #endif
#endif

#include <atomic>
#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace slick::perfmon {

/**
 * @brief One published display name.
 *
 * `key_plus_one` rather than `key` because (point 0, step 0) is a perfectly
 * valid key, so zero has to mean "empty" on its own. Claiming a slot is a CAS
 * on that word, which tags it with control_block::kClaimBit; the name is
 * written after the claim and never mutated. So a producer that dies at any
 * point leaves an untouched slot, a complete one, or a slot claimed for a key
 * nobody will finish - never a half-written name, and nothing to clean up.
 */
struct name_entry {
    std::atomic<uint64_t> key_plus_one;
    char                  name[SLICK_PERFMON_NAME_CAPACITY];
};

/**
 * @brief Shared header: the name table plus the TSC calibration handoff.
 *
 * Small, because there is no registration here. Names are pushed in bulk at
 * start() from the user's own constexpr table, so this block is written once
 * per process and then only read.
 */
struct control_block {
    static constexpr uint32_t kMagic         = 0x314D5053u;  // 'SPM1'
    /// 3: the claim marker became a tag over the key rather than an opaque
    /// sentinel, so the word in a slot means something different mid-claim.
    /// Two peers built either side of that must not share a segment.
    static constexpr uint32_t kLayoutVersion = 3;

    static constexpr uint32_t kUninitialised = 0;
    static constexpr uint32_t kInitialising  = 1;
    static constexpr uint32_t kReady         = 2;

    /**
     * Set while a slot is claimed and its name not yet written. Readers skip a
     * tagged slot rather than reading bytes another process is still copying
     * in.
     *
     * A tag over the key rather than a sentinel in place of it, so a claim
     * says *which* key it is for from the moment it becomes visible. An opaque
     * sentinel said nothing: a publisher that met one had to wait to learn
     * whose slot it was, and a claimant that was preempted - or killed - never
     * answered. The waiter timed out, took the slot for somebody else's, and
     * could then publish the same key into a second slot, which both splits
     * one key across two rows and hides a genuine disagreement over the name,
     * the comparison that reports one being the comparison skipped. Tagged,
     * that case is decided from the first load and needs no wait at all.
     */
    static constexpr uint64_t kClaimBit = uint64_t{1} << 63;

    static constexpr bool is_claimed(uint64_t slot) noexcept {
        return (slot & kClaimBit) != 0;
    }

    /// The key a slot belongs to, whether claimed or published. Zero is free.
    static constexpr uint64_t key_of(uint64_t slot) noexcept { return slot & ~kClaimBit; }

    uint32_t magic;
    uint32_t layout_version;
    uint32_t max_names;
    uint32_t name_capacity;

    std::atomic<uint64_t> tsc_hz_milli;   ///< collector-published rate, in mHz
    std::atomic<uint32_t> invariant_tsc;  ///< 1 when the TSC is trustworthy
    std::atomic<uint32_t> init_state;

    /**
     * Bumped once per name that becomes *readable*, never on a claim.
     *
     * Readers cache resolved labels and need to know when one they resolved to
     * a fallback has since been published. A count of claimed slots cannot
     * answer that: a slot is claimed before its name is written and skipped by
     * lookup() until afterwards, so a reader can count the slot, cache the
     * fallback, and then never see the count change when the real name lands -
     * leaving "point_7" in every row for the life of the process.
     */
    std::atomic<uint64_t> name_generation;

    // name_entry[max_names] follows immediately.

    name_entry* names() noexcept {
        return reinterpret_cast<name_entry*>(reinterpret_cast<char*>(this) + sizeof(control_block));
    }

    static size_t bytes_for(uint32_t max_names) noexcept {
        return sizeof(control_block) + static_cast<size_t>(max_names) * sizeof(name_entry);
    }
};

static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "the control block is shared across processes; a lock-backed atomic would "
              "reference a mutex private to one of them");

/// Key under which a (point, step) name is published. `seq` is absent: it
/// identifies a span instance, never something worth naming.
constexpr uint64_t name_key(point_id p, uint8_t step) noexcept {
    return (static_cast<uint64_t>(p) << 8) | step;
}

// The tag has to sit above every key the table can hold, or a claim would read
// as a claim on some other key. A key is (point << 8) | step, plus one, so the
// whole space fits in 41 bits and the top bit is free by a wide margin.
static_assert(name_key(0xFFFFFFFFu, 0xFFu) + 1 < control_block::kClaimBit,
              "the claim tag overlaps the name key space");

/**
 * @brief Owns a control block, in shared memory or on the heap.
 *
 * Both are the same struct on purpose: `local` mode is not a separate code
 * path, it is the shared path with a heap allocation standing in for the
 * segment. Name publishing, lookup and the calibration handoff are written
 * once and never branch on which they are running against.
 */
class ControlBlockOwner {
public:
    ~ControlBlockOwner() { reset(); }

    ControlBlockOwner() = default;

    ControlBlockOwner(const ControlBlockOwner&)            = delete;
    ControlBlockOwner& operator=(const ControlBlockOwner&) = delete;

    /// Create (or adopt) the segment. Used by a collector, and by a producer
    /// with config::create_if_absent.
    bool create_shared(const std::string& shm_name, uint32_t max_names) {
        const size_t bytes = control_block::bytes_for(max_names);
        shm_ = std::make_unique<slick::shm::shared_memory>(
            shm_name.c_str(), bytes, slick::shm::open_or_create,
            slick::shm::access_mode::read_write, std::nothrow);
        if (!shm_->is_valid()) {
            shm_.reset();
            return false;
        }
        block_ = static_cast<control_block*>(shm_->data());

        // Recorded before anything below can throw: validate() rejects a
        // mismatched peer by throwing, and a segment we created still has to be
        // unlinked on the way out.
        shm_name_ = shm_name;
        owns_shm_ = shm_->is_creator();

        if (owns_shm_) {
            initialise(max_names);
        } else {
            if (!await_ready()) {
                reset();
                return false;
            }
            validate(max_names);
        }
        return true;
    }

    /// Attach to a segment someone else created. Returns false when it is not
    /// there, which is the normal "collector is not running" case and must stay
    /// cheap and silent rather than throwing.
    bool attach_shared(const std::string& shm_name, uint32_t max_names) {
        shm_ = std::make_unique<slick::shm::shared_memory>(
            shm_name.c_str(), slick::shm::open_existing,
            slick::shm::access_mode::read_write, std::nothrow);
        if (!shm_->is_valid()) {
            shm_.reset();
            return false;
        }
        block_    = static_cast<control_block*>(shm_->data());
        shm_name_ = shm_name;
        owns_shm_ = false;  // somebody else created it; never unlink it here
        if (!await_ready()) {
            reset();
            return false;
        }
        validate(max_names);
        return true;
    }

    void create_local(uint32_t max_names) {
        heap_.assign(control_block::bytes_for(max_names), std::byte{});
        block_ = reinterpret_cast<control_block*>(heap_.data());
        initialise(max_names);
    }

    control_block* block() const noexcept { return block_; }
    explicit operator bool() const noexcept { return block_ != nullptr; }

    /// Release the segment (or the heap block) and go back to the unstarted
    /// state, so a Collector can be restarted with a different config.
    ///
    /// A segment we created is also unlinked. On POSIX a shm object outlives
    /// every process that mapped it, so without this a name could only ever be
    /// used once per boot with the settings it was first created under: a rerun
    /// would adopt the old block and silently keep its stale labels and
    /// calibration, and a changed max_names would be rejected as a peer
    /// mismatch. On Windows the segment is refcounted by handle and remove() is
    /// a documented no-op, so this is one code path rather than two.
    ///
    /// The unlink matches what slick-queue already does for the sample ring it
    /// owns, so the two segments of a session appear and disappear together.
    /// A producer that created them under config::create_if_absent therefore
    /// takes both with it when it exits - which is the same trade the escape
    /// hatch already made for the ring.
    void reset() noexcept {
        const bool  own = owns_shm_;
        std::string name;
        name.swap(shm_name_);
        owns_shm_ = false;

        shm_.reset();  // unmap before unlinking, so the name goes last
        if (own && !name.empty()) {
            slick::shm::shared_memory::remove(name.c_str());
        }

        heap_.clear();
        heap_.shrink_to_fit();
        block_ = nullptr;
    }

    /**
     * @brief Push every non-empty name from the user's table into the block.
     *
     * Walks point_count x 256 and calls the user's constexpr function. That is
     * a lot of calls on paper and nothing in practice - it happens once, at
     * startup, and only names that actually exist consume a slot.
     *
     * @return the number of slots whose existing name disagreed with ours.
     *         Kept rather than thrown: one process disagreeing about a label
     *         should not stop the other from being measured.
     */
    uint32_t publish_names(point_id point_count, name_fn fn) {
        if (block_ == nullptr || fn == nullptr) {
            return 0;
        }
        uint32_t conflicts = 0;
        for (point_id p = 0; p < point_count; ++p) {
            for (uint32_t s = 0; s <= 0xFFu; ++s) {
                const std::string_view nm = fn(p, static_cast<uint8_t>(s));
                if (nm.empty()) {
                    continue;
                }
                if (nm.size() >= SLICK_PERFMON_NAME_CAPACITY) {
                    // Truncating would silently alias two points onto one row,
                    // which is worse than having no label at all.
                    ++conflicts;
                    continue;
                }
                if (!publish_one(name_key(p, static_cast<uint8_t>(s)), nm)) {
                    ++conflicts;
                }
            }
        }
        return conflicts;
    }

    /// Published name, or empty when this (point, step) was never named.
    std::string_view lookup(point_id p, uint8_t step) const noexcept {
        if (block_ == nullptr) {
            return {};
        }
        const uint64_t want = name_key(p, step) + 1;
        name_entry*    tbl  = const_cast<control_block*>(block_)->names();
        for (uint32_t i = 0; i < block_->max_names; ++i) {
            const uint64_t k = tbl[i].key_plus_one.load(std::memory_order_acquire);
            if (k == 0 || control_block::is_claimed(k)) {
                continue;  // free, or a name another process is still copying in
            }
            if (k == want) {
                return std::string_view(tbl[i].name);
            }
        }
        return {};
    }

    /**
     * @brief How many name slots have been claimed.
     *
     * publish_one() always claims the lowest free slot, so the claimed entries
     * are a dense prefix and the first untouched slot ends the count.
     *
     * Reports occupancy, not readability: a slot counted here may still be
     * mid-claim and unreadable by lookup(). Use name_generation() to decide
     * whether a cache of resolved labels is stale.
     */
    uint32_t claimed_names() const noexcept {
        if (block_ == nullptr) {
            return 0;
        }
        name_entry*    tbl = const_cast<control_block*>(block_)->names();
        const uint32_t max = block_->max_names;
        uint32_t       n   = 0;
        while (n < max && tbl[n].key_plus_one.load(std::memory_order_acquire) != 0) {
            ++n;
        }
        return n;
    }

    /**
     * @brief Generation of the *published* name set.
     *
     * Changes only when a name becomes readable, so a label cache keyed on it
     * is invalidated exactly when it has gone stale - which claimed_names()
     * cannot promise, counting as it does slots whose names lookup() still
     * refuses to read. See control_block::name_generation.
     */
    uint64_t name_generation() const noexcept {
        if (block_ == nullptr) {
            return 0;
        }
        return block_->name_generation.load(std::memory_order_acquire);
    }

    void publish_calibration(double hz, bool invariant) noexcept {
        if (block_ == nullptr) {
            return;
        }
        block_->tsc_hz_milli.store(static_cast<uint64_t>(hz * 1000.0),
                                   std::memory_order_release);
        block_->invariant_tsc.store(invariant ? 1u : 0u, std::memory_order_release);
    }

    double published_hz() const noexcept {
        if (block_ == nullptr) {
            return 0.0;
        }
        return static_cast<double>(block_->tsc_hz_milli.load(std::memory_order_acquire)) / 1000.0;
    }

private:
    void initialise(uint32_t max_names) {
        block_->init_state.store(control_block::kInitialising, std::memory_order_relaxed);
        block_->magic          = control_block::kMagic;
        block_->layout_version = control_block::kLayoutVersion;
        block_->max_names      = max_names;
        block_->name_capacity  = SLICK_PERFMON_NAME_CAPACITY;
        block_->tsc_hz_milli.store(0, std::memory_order_relaxed);
        block_->invariant_tsc.store(1, std::memory_order_relaxed);
        block_->name_generation.store(0, std::memory_order_relaxed);

        name_entry* tbl = block_->names();
        for (uint32_t i = 0; i < max_names; ++i) {
            tbl[i].key_plus_one.store(0, std::memory_order_relaxed);
            tbl[i].name[0] = '\0';
        }
        block_->init_state.store(control_block::kReady, std::memory_order_release);
    }

    /// A peer may map the segment between its creation and its initialisation,
    /// so an attacher waits for the ready flag instead of reading a
    /// half-written header. Bounded, because waiting forever on a creator that
    /// died would hang the producer's startup.
    bool await_ready() const noexcept {
        for (int i = 0; i < 10000; ++i) {
            if (block_->init_state.load(std::memory_order_acquire) == control_block::kReady) {
                return true;
            }
            std::this_thread::yield();
        }
        return false;
    }

    void validate(uint32_t max_names) const {
        if (block_->magic != control_block::kMagic) {
            throw std::runtime_error("slick-perfmon: control block magic mismatch");
        }
        if (block_->layout_version != control_block::kLayoutVersion) {
            throw std::runtime_error("slick-perfmon: control block layout version mismatch");
        }
        if (block_->name_capacity != SLICK_PERFMON_NAME_CAPACITY) {
            throw std::runtime_error(
                "slick-perfmon: peer built with a different SLICK_PERFMON_NAME_CAPACITY");
        }
        if (block_->max_names != max_names) {
            throw std::runtime_error("slick-perfmon: peer configured a different max_names");
        }
    }

    /// @return false if the slot already held a different name, or the table
    ///         is full.
    ///
    /// Claim-then-write-then-publish, rather than claiming with the final key
    /// and writing the name afterwards: with the latter a reader could observe
    /// the key and read name bytes still being copied in by another process.
    bool publish_one(uint64_t key, std::string_view nm) {
        name_entry*    tbl  = block_->names();
        const uint64_t want = key + 1;

        for (uint32_t i = 0; i < block_->max_names; ++i) {
            uint64_t cur = tbl[i].key_plus_one.load(std::memory_order_acquire);

            // A slot mid-claim is unreadable, but it is not necessarily
            // somebody else's: it may be this very key, being published from
            // another thread or process at the same moment. Skipping it
            // unexamined is how one key ends up occupying two slots - and
            // worse, how a genuine disagreement over a name goes uncounted,
            // because the comparison that reports it is the one being skipped.
            //
            // The claim carries its key, so only a claim on *our* key is worth
            // waiting for, and only because the name has to be readable before
            // it can be compared. A claim on any other key is recognised for
            // what it is and passed over at once, however long its claimant
            // takes.
            if (control_block::is_claimed(cur) && control_block::key_of(cur) == want) {
                cur = await_claim(tbl[i]);
            }
            if (control_block::key_of(cur) == want) {
                // Still tagged: the claimant of our own key was preempted, or
                // died holding it. Report a disagreement that cannot be ruled
                // out rather than publishing the same key into a second slot -
                // one leaked slot is much the cheaper failure, and it is the
                // only outcome that keeps a key to a single row.
                if (control_block::is_claimed(cur)) {
                    return false;
                }
                return std::string_view(tbl[i].name) == nm;  // agreement, or a conflict
            }
            if (cur != 0) {
                continue;  // owned or claimed by another key
            }

            uint64_t expected = 0;
            if (tbl[i].key_plus_one.compare_exchange_strong(expected,
                                                            want | control_block::kClaimBit,
                                                            std::memory_order_acq_rel,
                                                            std::memory_order_acquire)) {
                std::memcpy(tbl[i].name, nm.data(), nm.size());
                tbl[i].name[nm.size()] = '\0';
                tbl[i].key_plus_one.store(want, std::memory_order_release);
                // After the key, never before: this is what tells a reader that
                // its cached labels are stale, and one that acted on it while
                // the name was still unreadable would re-cache the very
                // fallback the publication was meant to replace.
                block_->name_generation.fetch_add(1, std::memory_order_release);
                return true;
            }

            // Lost the race for this slot. The winner's value names its key,
            // so resolve it the same way before moving on, for the same
            // reasons.
            if (control_block::is_claimed(expected) &&
                control_block::key_of(expected) == want) {
                expected = await_claim(tbl[i]);
            }
            if (control_block::key_of(expected) == want) {
                if (control_block::is_claimed(expected)) {
                    return false;
                }
                return std::string_view(tbl[i].name) == nm;
            }
        }
        return false;
    }

    /**
     * @brief Wait out a slot's claim window and return the value it settled on.
     *
     * Only ever called on a slot claimed for the caller's own key, where the
     * name has to become readable before it can be compared. The window is a
     * memcpy and a store by a thread that is already running, so this almost
     * always returns on its first load. Bounded all the same: a producer killed
     * between the claim and the store would otherwise stall every later
     * publisher forever. Returns a value still tagged when it never resolves,
     * and the caller reports a conflict rather than claiming a second slot for
     * a key that already has one.
     */
    static uint64_t await_claim(name_entry& e) noexcept {
        for (int i = 0; i < 1000; ++i) {
            const uint64_t k = e.key_plus_one.load(std::memory_order_acquire);
            if (!control_block::is_claimed(k)) {
                return k;
            }
            std::this_thread::yield();
        }
        return e.key_plus_one.load(std::memory_order_acquire);
    }

    std::unique_ptr<slick::shm::shared_memory> shm_;
    std::vector<std::byte>                     heap_;
    control_block*                             block_ = nullptr;
    std::string                                shm_name_;
    bool                                       owns_shm_ = false;
};

}  // namespace slick::perfmon
