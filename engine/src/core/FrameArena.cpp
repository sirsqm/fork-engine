// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
#include "fe/core/FrameArena.h"

#include <cstdint>

namespace fe {

namespace {

// Rounds `value` up to the next multiple of `alignment`. `alignment` is a power
// of two, so this is a mask, not a division.
std::uintptr_t align_up(std::uintptr_t value, usize alignment) noexcept {
    return (value + alignment - 1) & ~static_cast<std::uintptr_t>(alignment - 1);
}

} // namespace

FrameArena::FrameArena(usize capacity) : capacity_(capacity) {
    if (capacity_ > 0) {
        base_ = static_cast<u8*>(::operator new(capacity_));
    }
}

FrameArena::~FrameArena() {
    // The backing block was allocated with ::operator new; it is freed with
    // ::operator delete, with no destructor run (there is nothing to destroy in
    // a byte buffer).
    ::operator delete(base_);
}

void* FrameArena::allocate(usize size, usize alignment) {
    // alignment == 0 is degenerate; treat it as no alignment requirement. A
    // caller passing a non-power-of-two alignment is out of contract, but a
    // masked bump is well-defined for any value, so this stays noexcept.
    const usize effective_alignment = (alignment == 0) ? 1 : alignment;

    // Align by absolute address, not by offset: the backing block's own address
    // is only guaranteed to be aligned for std::max_align_t, so an offset that
    // is a multiple of 64 does not make `base_ + offset` 64-aligned. Computing
    // the aligned address directly honors the request for any power of two.
    const std::uintptr_t base_address = reinterpret_cast<std::uintptr_t>(base_);
    const std::uintptr_t aligned_address = align_up(base_address + used_, effective_alignment);
    const usize offset = static_cast<usize>(aligned_address - base_address);

    // Check the request fits after alignment. The padding counts against the
    // capacity, which is correct: it is genuinely consumed.
    if (offset > capacity_ || size > capacity_ - offset) {
        // Overflow: leave the bump pointer untouched so a failed request cannot
        // corrupt the live allocations, and let the caller reset() and retry.
        return nullptr;
    }

    u8* result = base_ + offset;
    used_ = offset + size;
    ++allocation_count_;
    if (used_ > peak_bytes_used_) {
        peak_bytes_used_ = used_;
    }
    return result;
}

void FrameArena::reset() {
    used_ = 0;
    allocation_count_ = 0;
    ++reset_count_;
}

void FrameArena::restore(ArenaMarker marker) noexcept {
    // A marker can only point at or before the current bump, so this never
    // rewinds past a live allocation. Clamp defensively so a stale or forged
    // marker cannot move the bump forward.
    if (marker.offset <= used_) {
        used_ = marker.offset;
    }
}

} // namespace fe
