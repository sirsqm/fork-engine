// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#pragma once

#include "fe/core/Types.h"

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace fe {

// A per-frame linear (bump) allocator over one pre-allocated buffer.
//
// Design notes (see docs/adr/0051-frame-arena.md):
//   * The backing block is allocated once, in the constructor. The allocate
//     path only bumps a pointer and aligns it; it never calls the heap. That is
//     what removes per-frame allocation churn: an extraction array or a command
//     list can be built and thrown away each frame with no malloc/free pair.
//   * reset() reclaims everything at once by moving the bump pointer back to
//     the start. It is the per-frame operation. Nothing is freed individually
//     because nothing is allocated individually -- the arena is a stack, not a
//     heap.
//   * The arena does not run destructors. That is a compile-time rule, not a
//     comment: create<T>() static_asserts that T is trivially destructible.
//   * Nothing here is thread-safe. An arena belongs to the thread that owns the
//     frame; a worker that needs scratch gets its own arena.
//
// Lifetime rule: data allocated from the arena must not outlive the reset (or
// the arena). A pointer returned by allocate() is invalidated by reset(),
// restore(), and the destructor. This is a documented rule, not an enforced
// one; review is the guard (ADR-0017).
//
// This is a tested core primitive. Wiring it into the frame loop and the
// render-extraction seam -- its first real consumer -- is the follow-on; the
// frame loop does not use it yet.
class FrameArena {
public:
    // A saved bump position. Restoring a marker reclaims every allocation made
    // after it and nothing before it, which is what a nested transient scope
    // (one sub-step of extraction) needs.
    struct ArenaMarker {
        usize offset = 0;
    };

    // Allocates the backing block once. `capacity` of 0 is allowed and yields an
    // arena that can only satisfy zero-size allocations.
    explicit FrameArena(usize capacity);
    ~FrameArena();

    FrameArena(const FrameArena&) = delete;
    FrameArena& operator=(const FrameArena&) = delete;
    FrameArena(FrameArena&&) = delete;
    FrameArena& operator=(FrameArena&&) = delete;

    // Bumps an aligned pointer and returns it. The returned block is `size`
    // bytes, aligned to `alignment`, and never overlaps a live allocation.
    //
    // Returns nullptr on overflow (the request does not fit in the remaining
    // capacity). Errors are values, not exceptions (ADR-0003): a full arena is
    // an ordinary, expected condition, so the caller checks for nullptr rather
    // than catching. A failed allocation does not corrupt the arena -- the bump
    // pointer is left where it was, so the caller can reset() and carry on.
    //
    // `alignment` must be a power of two; it is honored exactly. The default is
    // the alignment suitable for any object type.
    [[nodiscard]] void* allocate(usize size, usize alignment = alignof(std::max_align_t));

    // Allocates sizeof(T) at alignof(T) and placement-news a T from `args`.
    // Returns nullptr on overflow. T must be trivially destructible: the arena
    // never runs destructors, so a non-trivial T would leak.
    template <typename T, typename... Args>
    T* create(Args&&... args) {
        static_assert(std::is_trivially_destructible_v<T>,
                      "FrameArena::create requires a trivially destructible T: the arena never runs "
                      "destructors, so a non-trivial T would leak.");
        void* memory = allocate(sizeof(T), alignof(T));
        if (memory == nullptr) {
            return nullptr;
        }
        return ::new (memory) T(std::forward<Args>(args)...);
    }

    // Reclaims every allocation at once. The capacity becomes available again
    // and the next allocation reuses the same backing block.
    void reset();

    // Saves the current bump position for a nested scope.
    [[nodiscard]] ArenaMarker mark() const noexcept { return ArenaMarker{used_}; }

    // Reclaims every allocation made after `marker` and keeps the rest.
    void restore(ArenaMarker marker) noexcept;

    // Bytes currently handed out since the last reset.
    [[nodiscard]] usize bytes_used() const noexcept { return used_; }

    // Total bytes the backing block can hold.
    [[nodiscard]] usize bytes_capacity() const noexcept { return capacity_; }

    // Successful allocate() calls since the last reset. This is the churn
    // counter: a per-frame loop should hold it roughly constant, and a test can
    // assert that the loop itself performs no heap allocation.
    [[nodiscard]] usize allocation_count() const noexcept { return allocation_count_; }

    // Number of reset() calls since construction.
    [[nodiscard]] usize reset_count() const noexcept { return reset_count_; }

    // High-water mark of bytes_used() since construction. It does not fall on
    // reset, so it reports the worst frame the arena has seen.
    [[nodiscard]] usize peak_bytes_used() const noexcept { return peak_bytes_used_; }

private:
    u8* base_ = nullptr; // the backing block; null only when capacity is 0
    usize capacity_ = 0; // bytes in the backing block
    usize used_ = 0;     // bump offset from base_
    usize allocation_count_ = 0;
    usize reset_count_ = 0;
    usize peak_bytes_used_ = 0;
};

} // namespace fe
