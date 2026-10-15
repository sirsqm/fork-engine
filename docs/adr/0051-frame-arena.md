# ADR-0051: A per-frame arena in `core`

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0017](0017-performance-strategy.md),
  [ADR-0003](0003-error-handling.md),
  [ADR-0022](0022-profiler-and-frame-timing.md)

## Context

[ADR-0017](0017-performance-strategy.md) makes measurement the first move and
names the memory work plainly: *"A per-frame arena for transient data
(extraction arrays, command lists), reset each frame, to remove per-frame heap
churn. Allocation counters make churn observable."* `docs/STRATEGY.md` pillar P5
is the same commitment, with its verify clause: an arena correctness test and
counter assertions.

The forces are specific. The engine's frame does real per-frame work that
currently allocates: extraction builds a list of instances, a renderer builds a
command list, a system builds a scratch array — and each of those is destroyed
at the end of the frame. A general-purpose allocator is the wrong tool for data
with a lifetime of one frame. The allocation pattern is a stack: everything
allocated during the frame dies together when the frame ends, in the reverse of
no particular order, because nothing is freed individually. A bump allocator
matches that pattern exactly — allocate by moving a pointer, reclaim everything
by moving it back — and removes the malloc/free pair, the size-class bookkeeping,
and the fragmentation that a per-frame `std::vector` churn causes.

Two things shape the decision. First, **where it lives**. The arena depends on
nothing — no scene, no resource, no RHI — so it belongs in `core`, the module
with no dependencies, beside `Error`, logging, and the profiler. Second, **what
it is not**. ADR-0017 warns that "the frame arena changes how transient
allocations are written; code that outlives a frame must not allocate from it.
That is a documented rule, not an enforced one." A bump allocator cannot free
one allocation, cannot grow a block in place, and cannot run a destructor. Every
one of those is a real limitation that must be stated rather than papered over.

## Decision

**Add `fe::FrameArena` to `core`: a linear (bump) allocator over one
pre-allocated buffer, with `allocate`/`create<T>`/`reset`, a nested-scope
`mark`/`restore`, and allocation counters.**

### One buffer, one bump pointer

The constructor allocates the backing block **once** with `::operator new`. The
allocate path does nothing but align a pointer and advance it: no heap call, no
branch to a slow path, no growth. `reset()` moves the bump pointer back to the
start, which is the per-frame operation. The block is freed in the destructor
with `::operator delete`.

The arena is non-copyable and non-movable. It owns a buffer, and a copy or move
would either duplicate or dangle that ownership; deleting both makes the rule
explicit.

### Overflow is a value, not an exception

`allocate(size, alignment)` returns `nullptr` when the request does not fit. This
is [ADR-0003](0003-error-handling.md) applied to an allocator: a full arena is an
ordinary, expected condition, so it is a return value, not a thrown
`std::bad_alloc`. A failed allocation leaves the bump pointer untouched, so it
cannot corrupt the live blocks, and the caller recovers with `reset()` and
retries. A caller that wants the value-or-error shape can wrap the pointer.

`create<T>(args...)` allocates `sizeof(T)` at `alignof(T)`, placement-news a `T`,
and returns it (or `nullptr`). It `static_assert`s that `T` is trivially
destructible. The arena never runs destructors, so a non-trivial `T` would leak;
the assert turns that from a comment into a compile error.

Alignment is honored exactly, computed from the **absolute address** of the
block (`base_ + used_` aligned up), not from the offset. The backing block is
only guaranteed to be aligned for `std::max_align_t`, so an offset that is a
multiple of 64 does not make `base_ + offset` 64-aligned. Aligning the address
is what makes over-aligned requests (SIMD vectors, cache-line-padded structs)
correct.

### Nested scopes and counters

`mark()` saves the bump offset and `restore(marker)` moves the bump back to it,
reclaiming everything allocated after the mark and nothing before. This is what a
sub-step of extraction needs: scratch that must not outlive the step, without a
second arena and without reasoning about the whole frame.

The introspection is the "allocation counters" ADR-0017 asks for: `bytes_used()`,
`bytes_capacity()`, `allocation_count()` (successful allocations since the last
reset), `reset_count()`, and `peak_bytes_used()` (a high-water mark that does not
fall on reset, so it reports the worst frame seen). These make churn observable —
a per-frame loop's `allocation_count` should hold steady, and a test can assert
that the loop performs no heap allocation at all.

### What this does *not* do yet

The arena is a **tested `core` primitive**. Nothing in the engine constructs one
yet: the frame loop does not own an arena, does not reset it, and does not
allocate from it, and the render-extraction seam that will be its first real
consumer is not built. That wiring is the follow-on. This record claims the
primitive and its tests, not an integration.

## Consequences

**Easier:**

- A frame's transient data — extraction arrays, command lists, scratch — can be
  built with no malloc/free pair. The cost of a frame's allocations drops to
  moving a pointer.
- The allocation counters make churn measurable. A test can prove a loop performs
  no heap allocation, which is a stronger and more stable assertion than a
  timing.
- `mark`/`restore` gives nested transient scopes without a second allocator or a
  lifetime protocol.
- The primitive is in `core`, so every module can use it without a new edge in
  the module graph.

**Harder:**

- Data must not outlive the arena, and must not outlive a `reset`/`restore` it
  sits under. This is a documented rule, not an enforced one (ADR-0017): a
  dangling pointer into the arena is a use-after-reset the type system does not
  catch. Review is the guard.
- The arena cannot free one allocation, cannot grow a block in place, and cannot
  run destructors. A type that needs any of those does not belong in it. The
  `create<T>` `static_assert` enforces the destructor half; the rest is on the
  caller.
- Capacity is fixed at construction. An arena sized too small returns `nullptr`
  under load rather than growing, which surfaces as a null check the caller must
  handle — or, if ignored, a crash. Sizing is a real decision, not a default.
- Nothing uses it yet, so it buys no measured win today. Its value is realized
  when the extraction seam lands and can allocate from it; until then it is
  tested infrastructure.

## Alternatives considered

- **`std::pmr::monotonic_buffer_resource`.** Rejected as the *primary* choice,
  though it is the standard's version of the same idea. It brings
  `std::pmr` allocator plumbing and `std::pmr::vector` types through the code
  that uses it, and it does not expose the counters (allocations, peak, resets)
  that ADR-0017 requires for observability. A small, counted arena is the
  smaller surface for the same job; the counters are the reason it exists.
- **A general-purpose allocator with a free list.** Rejected. Per-frame data is
  a stack, not a heap: it is never freed individually. A free list adds size
  classes and fragmentation to serve an operation the frame never performs.
- **Reuse a `std::vector` per frame and `clear()` it.** Rejected as the general
  mechanism. It removes the reallocation only if the capacity happens to be
  enough, and it keeps one vector per consumer with its own growth policy. The
  arena centralizes the buffer and makes the per-frame cost constant.
- **A thread-safe arena.** Rejected for now. A frame is driven by one thread
  (ADR-0017 keeps the device single-threaded), and the transient data is built
  on that thread. A worker that needs scratch can hold its own arena. Locking on
  the hot allocate path would defeat the point; adding it before a consumer needs
  it is speculative.
- **An arena that runs destructors.** Rejected. It would require tracking every
  constructed object and running them in reverse on reset, which turns a pointer
  bump into a registration list. Trivially-destructible data is the intended
  content, and the `static_assert` makes that the contract.
- **Wiring the arena into the frame loop and the extraction seam in this
  change.** Rejected as scope creep and as premature. The extraction seam is a
  separate architectural change (M9, ADR-0017); the arena is its prerequisite,
  and landing the primitive with tests is the honest, reviewable first step.
