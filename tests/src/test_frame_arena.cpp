// Copyright (c) 2026 sqm
// SPDX-License-Identifier: MIT
//
// The `core` frame arena: a linear (bump) allocator over one pre-allocated
// buffer. These tests pin the correctness the ADR promises -- alignment,
// non-overlap, reset reuse, overflow as nullptr, mark/restore, create<T> -- and
// the counter assertion STRATEGY P5 asks for: the steady-state allocate/reset
// loop performs no heap allocation. No mocks; every path runs the real arena.
//
// The heap-allocation counter is a global operator new/delete replacement in
// this translation unit (below). It counts every heap call the test makes, so
// the "no churn" test can snapshot the count around a tight loop and assert the
// delta is zero, while the arena's single constructor allocation happens before
// the snapshot.

#include <doctest/doctest.h>

#include "fe/core/FrameArena.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

// ---------------------------------------------------------------------------
// Counting heap replacement.
//
// These replace the global allocation functions for the whole test binary. They
// forward to std::malloc/std::free and bump atomic counters. The counters must
// be atomic: this binary also runs the job-system tests, whose worker threads
// allocate through this replacement concurrently, so a plain counter would be a
// data race.
//
// Every replaceable form is replaced -- scalar and array, throwing and nothrow,
// and the sized deletes -- so that a pointer is always freed through the same
// path that allocated it. Replacing only the throwing scalar form is a trap: the
// standard library allocates through the nothrow form in places
// (std::get_temporary_buffer, used by stable_sort), and if that allocation is
// intercepted by the sanitizer's own operator new while the matching free goes
// through this file, AddressSanitizer reports an alloc-dealloc mismatch. Keeping
// both sides here makes every pair malloc/free-consistent.
//
// Note: this file is the *only* translation unit that must define these, or the
// linker reports a duplicate symbol. That is intentional -- it keeps the
// counter scoped to this test.
// ---------------------------------------------------------------------------
namespace {

std::atomic<std::size_t> g_heap_allocations{0};
std::atomic<std::size_t> g_heap_frees{0};

} // namespace

void* operator new(std::size_t size) {
    ++g_heap_allocations;
    if (void* memory = std::malloc(size == 0 ? 1 : size)) {
        return memory;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
    return ::operator new(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    ++g_heap_allocations;
    return std::malloc(size == 0 ? 1 : size);
}

void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept {
    return ::operator new(size, tag);
}

void operator delete(void* memory) noexcept {
    if (memory != nullptr) {
        ++g_heap_frees;
        std::free(memory);
    }
}

void operator delete[](void* memory) noexcept {
    ::operator delete(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    ::operator delete(memory);
}

void operator delete[](void* memory, std::size_t) noexcept {
    ::operator delete(memory);
}

void operator delete(void* memory, const std::nothrow_t&) noexcept {
    ::operator delete(memory);
}

void operator delete[](void* memory, const std::nothrow_t&) noexcept {
    ::operator delete(memory);
}

using fe::FrameArena;
using fe::u8;
using fe::usize;

namespace {

// A trivially destructible payload for create<T>().
struct Pod {
    int a = 0;
    int b = 0;
    double c = 0.0;
};

std::uintptr_t address_of(const void* pointer) {
    return reinterpret_cast<std::uintptr_t>(pointer);
}

} // namespace

TEST_CASE("allocations are non-null, distinct, and non-overlapping") {
    FrameArena arena(4096);

    const usize sizes[] = {1, 7, 16, 64, 3, 200, 1000};
    void* previous = nullptr;
    usize previous_size = 0;
    usize count = 0;

    for (const usize size : sizes) {
        void* block = arena.allocate(size);
        REQUIRE(block != nullptr);
        CHECK((block != previous));
        if (previous != nullptr) {
            // The new block starts at or after the end of the previous one.
            CHECK(address_of(block) >= address_of(previous) + previous_size);
        }
        previous = block;
        previous_size = size;
        ++count;
    }

    CHECK(arena.allocation_count() == count);
}

TEST_CASE("a byte written to one block does not corrupt another") {
    FrameArena arena(4096);

    u8* first = static_cast<u8*>(arena.allocate(32));
    u8* second = static_cast<u8*>(arena.allocate(32));
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);

    for (usize i = 0; i < 32; ++i) {
        first[i] = 0xAA;
        second[i] = 0x55;
    }

    for (usize i = 0; i < 32; ++i) {
        CHECK(first[i] == 0xAA);
        CHECK(second[i] == 0x55);
    }
}

TEST_CASE("alignment is honored exactly") {
    FrameArena arena(1 << 16);

    const usize alignments[] = {1, 2, 4, 8, 16, 32, 64};
    for (const usize alignment : alignments) {
        // Vary the size so a mis-aligned bump shows up on a later request.
        for (usize size = 1; size <= 9; ++size) {
            void* block = arena.allocate(size, alignment);
            REQUIRE(block != nullptr);
            CHECK(address_of(block) % alignment == 0);
        }
    }

    // A default-aligned allocation is suitable for any type.
    void* block = arena.allocate(24);
    REQUIRE(block != nullptr);
    CHECK(address_of(block) % alignof(std::max_align_t) == 0);
}

TEST_CASE("reset reclaims everything and reuses the same backing block") {
    FrameArena arena(1024);

    void* first = arena.allocate(100);
    REQUIRE(first != nullptr);
    CHECK(arena.bytes_used() >= 100);
    CHECK(arena.reset_count() == 0);

    arena.reset();

    CHECK(arena.bytes_used() == 0);
    CHECK(arena.allocation_count() == 0);
    CHECK(arena.reset_count() == 1);

    // The next allocation lands at the same address: the buffer is reused, not
    // reallocated.
    void* again = arena.allocate(100);
    CHECK((again == first));
}

TEST_CASE("an oversized allocation returns nullptr and does not corrupt the arena") {
    FrameArena arena(64);

    void* live = arena.allocate(16);
    REQUIRE(live != nullptr);
    const usize used_before = arena.bytes_used();

    // Larger than the whole capacity.
    CHECK(arena.allocate(1000) == nullptr);
    // Larger than the remaining capacity, but smaller than the whole.
    CHECK(arena.allocate(60) == nullptr);

    // The failed requests left the bump pointer untouched.
    CHECK(arena.bytes_used() == used_before);
    CHECK(arena.allocation_count() == 1);

    // A request that fits still works, and a reset recovers the capacity.
    CHECK(arena.allocate(16) != nullptr);
    arena.reset();
    CHECK(arena.allocate(64) != nullptr);
}

TEST_CASE("mark and restore reclaim only the nested scope") {
    FrameArena arena(1024);

    void* outer = arena.allocate(32);
    REQUIRE(outer != nullptr);
    for (usize i = 0; i < 32; ++i) {
        static_cast<u8*>(outer)[i] = 0x7E;
    }

    const FrameArena::ArenaMarker marker = arena.mark();
    const usize used_at_mark = arena.bytes_used();

    void* inner = arena.allocate(128);
    REQUIRE(inner != nullptr);
    CHECK(arena.bytes_used() > used_at_mark);

    arena.restore(marker);

    CHECK(arena.bytes_used() == used_at_mark);
    // The nested allocation was reclaimed; the next one reuses its address.
    void* reused = arena.allocate(128);
    CHECK((reused == inner));

    // The outer block survived with its contents intact.
    for (usize i = 0; i < 32; ++i) {
        CHECK(static_cast<u8*>(outer)[i] == 0x7E);
    }
}

TEST_CASE("create<T> constructs a value with its arguments") {
    FrameArena arena(1024);

    Pod* pod = arena.create<Pod>(7, 11, 2.5);
    REQUIRE(pod != nullptr);
    CHECK(pod->a == 7);
    CHECK(pod->b == 11);
    CHECK(pod->c == doctest::Approx(2.5));
    CHECK(address_of(pod) % alignof(Pod) == 0);

    // A create<T> that cannot fit returns nullptr, exactly like allocate().
    struct Big {
        u8 payload[2048];
    };
    CHECK(arena.create<Big>() == nullptr);
}

TEST_CASE("peak_bytes_used is a high-water mark that survives reset") {
    FrameArena arena(512);

    (void)arena.allocate(100);
    const usize peak = arena.peak_bytes_used();
    CHECK(peak >= 100);

    arena.reset();
    // The peak does not fall on reset.
    CHECK(arena.peak_bytes_used() == peak);

    // A smaller frame does not lower it.
    (void)arena.allocate(16);
    CHECK(arena.peak_bytes_used() == peak);
}

TEST_CASE("the heap counter observes a real allocation") {
    // Guards the no-heap test below: if the global operator new replacement were
    // not linked, that test would pass vacuously. A real heap allocation must
    // move the counter.
    const std::size_t before = g_heap_allocations;
    int* value = new int(42);
    CHECK(g_heap_allocations > before);
    delete value;
}

TEST_CASE("the steady-state allocate/reset loop performs no heap allocation") {
    // The one buffer allocation happens here, before the snapshot.
    FrameArena arena(4096);

    // Warm up once so any first-iteration effect is outside the measured region.
    for (int i = 0; i < 8; ++i) {
        (void)arena.allocate(64, 16);
    }
    arena.reset();

    const std::size_t allocations_before = g_heap_allocations;

    // A tight per-frame loop: allocate a few transient blocks, then reset. This
    // is the shape P5 says must stop churning the heap. Nothing here may
    // allocate: no logging, no containers, no CHECK.
    for (int frame = 0; frame < 1000; ++frame) {
        void* a = arena.allocate(128, 16);
        void* b = arena.allocate(256, 32);
        void* c = arena.create<Pod>(frame, frame, static_cast<double>(frame));
        // Use the results so the compiler cannot elide the calls.
        volatile std::uintptr_t sink = address_of(a) ^ address_of(b) ^ address_of(c);
        (void)sink;
        arena.reset();
    }

    const std::size_t allocations_after = g_heap_allocations;

    // The loop allocated nothing from the heap. This is the counter assertion
    // STRATEGY P5 asks for: churn is observable, and here it is zero.
    CHECK(allocations_after == allocations_before);
}
