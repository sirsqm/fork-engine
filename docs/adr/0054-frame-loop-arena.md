# ADR-0054: The frame arena in the frame loop

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0017](0017-performance-strategy.md),
  [ADR-0051](0051-frame-arena.md),
  [ADR-0052](0052-render-extraction-seam.md),
  [ADR-0003](0003-error-handling.md),
  [ADR-0041](0041-platform-contract-and-null-backend.md)

## Context

[ADR-0051](0051-frame-arena.md) added `fe::FrameArena` to `core`: a linear bump
allocator over one pre-allocated buffer, with `allocate`/`create<T>`/`reset`, a
nested-scope `mark`/`restore`, and allocation counters. It landed as a tested
primitive and said so plainly — *"Nothing in the engine constructs one yet: the
frame loop does not own an arena, does not reset it, and does not allocate from
it."* That record claimed the primitive, not an integration.

[ADR-0052](0052-render-extraction-seam.md) landed the render extraction seam
between the authoring model (the scene graph) and the processing model (a flat
instance array). The extraction step is the first real per-frame consumer the
arena was designed for: it builds an array of instances every frame and throws it
away at the end. It currently builds that array in a `std::vector`, which is a
malloc/free pair per frame.

[ADR-0017](0017-performance-strategy.md) names the memory work explicitly:
*"A per-frame arena for transient data (extraction arrays, command lists), reset
each frame, to remove per-frame heap churn. Allocation counters make churn
observable."* Two of the three pieces now exist — the arena (0051) and a
transient producer (0052). The missing piece is the thing that makes them meet:
**the frame loop owns one arena, resets it once per frame, and hands it to every
system.**

The forces are the ones ADR-0051 already set out. Per-frame data has a lifetime
of exactly one frame; it is a stack, not a heap; it is never freed individually.
The frame loop is the one place that knows when a frame begins and ends, so it is
the only place that can own the arena and reset it at the right moment. The
question this record settles is not *whether* the arena belongs in the loop, but
*where in the loop it is reset* and *how a system reaches it* without the loop
growing a dependency on any particular consumer.

## Decision

**`Application` owns one `FrameArena`, created in `run()`, reset once per frame
before the systems run, and exposed to every system through a new
`FrameContext::arena` member.**

### The loop owns the arena, not a system

The arena is a member of `Application`, created once in `run()` (not per frame),
sized from `Application::set_frame_arena_bytes()` (default 1 MiB), and destroyed
with the application after the loop. Owning it in the loop rather than in a
consumer keeps the allocation policy in one place: the arena is reset on the
frame's cadence, and no system has to reason about another system's allocation.
It also matches the ADR-0051 lifetime rule — the arena must not outlive the frame
loop, and tying it to `run()` makes that structural rather than a convention.

### Reset before the systems run, not after

The reset happens at the top of the update phase, immediately before
`schedule_.update(frame, scene_)`:

```
frame_arena_->reset();   // everything from last frame is dead
FrameContext frame;
...
frame.arena = frame_arena_.get();
schedule_.update(frame, scene_);
```

Reset-before rather than reset-after for two reasons. First, it means a system
can allocate from the arena during its own update and the data is valid for the
whole rest of that frame — resetting after would reclaim an allocation the
renderer had not read yet. Second, the frame's lifetime is exactly the loop body:
reset at the top, allocate during the body, reclaim at the top of the next
iteration. Nothing is freed individually, which is the property ADR-0051 chose
the bump allocator for.

### `FrameContext::arena`, nullable, like `resources` and `input`

A system reaches the arena the same way it reaches the resource manager and the
input snapshot: a pointer on `FrameContext`. It is **nullable**. A tool or a test
that runs a schedule with no frame loop passes no arena, and a system must
null-check it rather than assume one exists — the identical contract
`FrameContext::resources` and `FrameContext::input` already carry. This is the
"nullable dependency" pattern the runtime uses throughout, and it is what keeps a
system testable headlessly: a test can construct a `FrameContext` with an arena
and exercise the allocation path with no window and no loop.

### Disabling it is an explicit, tested path

`set_frame_arena_bytes(0)` leaves the arena null, so every system sees a null
arena and the run behaves exactly as it did before the arena existed. This is not
a debug switch; it is the honest "opt out" that ADR-0051's integration note
implied, and a test pins it. A run that wants zero arena activity — a host that
manages its own memory, or a measurement of the pre-arena baseline — sets it and
gets the old behaviour.

### Overflow is handled, not assumed away

The default 1 MiB is sized to hold a frame's transient data with room to spare,
but it is a fixed capacity: an arena sized too small returns `nullptr` from
`allocate` under load rather than growing (ADR-0051). A system that allocates a
large, data-dependent amount must treat that null as an expected, recoverable
condition — the same value-not-exception contract ADR-0003 sets for every other
failure. The loop does not hide this: it hands over a real arena with a real
capacity and lets the consumer check.

## Consequences

**Easier:**

- The arena has its first real consumer path. A system can build transient data
  with no malloc/free pair by allocating from `context.arena`; the extraction
  seam (ADR-0052) can be moved onto it next with no further plumbing.
- Churn is observable end to end. The loop's per-frame `reset()` and the arena's
  counters (`reset_count`, `peak_bytes_used`, `allocation_count`) let a test
  assert that a frame's arena usage is flat across frames — a stronger and more
  stable assertion than a timing.
- The allocation policy lives in one place. The loop resets on the frame
  boundary; no system coordinates with another over a shared buffer.
- The integration is testable headlessly. The null window backend (ADR-0041) runs
  the loop with no display, so the arena tests are pure logic with no GL context.

**Harder:**

- `FrameContext` gains a member. It is an aggregate with defaulted members, so
  existing `FrameContext` constructions (all of which set fields individually)
  are unaffected — but a new field is a new thing every system author must know
  is nullable.
- Data must not outlive the frame, and must not outlive a `reset`/`restore` it
  sits under. This is the ADR-0051 documented rule, still not enforced by the
  type system: a dangling pointer into the arena is a use-after-reset review must
  catch.
- The default capacity is a guess. 1 MiB is generous for the transient data the
  engine builds today, but a consumer with a large, data-dependent frame will
  overflow and get `nullptr`. Sizing remains a real decision, and the overflow is
  the consumer's to handle.
- A disabled arena is a second code path. Every consumer that reads
  `context.arena` must handle null; the alternative (a null-object arena) was
  rejected because it would make "no arena" indistinguishable from "a zero-capacity
  arena" and hide the overflow the caller is supposed to see.

## Alternatives considered

- **A per-frame `std::vector` reset with `clear()` in each consumer.** Rejected.
  It removes reallocation only if the capacity happens to be enough, keeps one
  vector per consumer with its own growth policy, and gives no single place to
  observe or bound the frame's transient memory. This is the option ADR-0051
  already rejected at the primitive level; the loop is where the shared buffer
  belongs.
- **Reset after the systems run instead of before.** Rejected. Resetting after
  would reclaim allocations a later phase of the same frame still needs — the
  renderer reads the extraction output after the systems run. Reset-before makes
  the frame's arena lifetime exactly the loop body.
- **A non-null arena always present (a null-object that allocates nothing).**
  Rejected. It would make "the arena is disabled" indistinguishable from "the
  arena is full", and a system could not tell whether its `nullptr` meant "no
  arena" or "overflow". A nullable pointer keeps both conditions visible, and
  matches `resources`/`input`.
- **Own the arena in `Engine` rather than `Application`.** Rejected. `Engine`
  does not run a frame loop (its own header says so); the arena's reset cadence
  is the frame, and only the thing that runs frames knows when a frame begins.
  Putting it in `Engine` would tie a per-frame lifetime to a per-process object.
- **A thread-local arena per worker.** Rejected for now. The frame is driven by
  one thread (ADR-0017 keeps the device single-threaded), and the systems run on
  that thread. A worker that needs scratch can hold its own arena; adding a
  worker-local arena before a parallel system exists is speculative, and
  ADR-0051 explicitly left the arena non-thread-safe.
- **Move the extraction seam onto the arena in this change.** Rejected as scope
  creep. The seam's `std::vector` path is correct and tested; switching it to the
  arena is a separate, measurable change with its own before/after. This record
  lands the wiring that makes that change a one-line follow-on, and proves the
  wiring with its own tests.
