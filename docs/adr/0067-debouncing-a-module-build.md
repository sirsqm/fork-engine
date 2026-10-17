# ADR-0067: Debouncing a gameplay-module build

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0044](0044-scripting-seam.md),
  [ADR-0046](0046-resource-change-detection-and-reload.md),
  [ADR-0061](0061-cpp-gameplay-modules-hot-reload.md),
  [ADR-0066](0066-editor-driven-module-hot-reload.md)

## Context

[ADR-0066](0066-editor-driven-module-hot-reload.md) gave the editor a per-frame
poll that hot-reloads a gameplay module whose image was rebuilt on disk. It
recorded each module's write time at load and reported a module as soon as the
time differed. That ADR named the gap it left:

> **Debouncing a build.** A build that writes an image in several steps could be
> observed mid-write. The poll reports the change once per observed write time,
> and a partial image fails to load and is dropped; a real debounce (wait for the
> file to settle) is a refinement, not a correctness gap, and is not here.

The roadmap's M7 row agrees: "Still to do: debouncing a build". This ADR is that
refinement.

The forces at play:

- **A build does not write the image atomically.** GNU `ld`, the linker a Linux
  build uses, creates or truncates the output file and writes the object contents
  into it. For a short window the file exists, has a new write time, and is not a
  valid shared object. A poll that fires inside that window sees "changed" and
  hands the path to `reload_module()`.
- **A failed reload is not a no-op.** ADR-0061's policy is that a module which
  will not load is dropped, and ADR-0066 carries it into the editor: a reload that
  fails logs and drops the module. So a mid-write read is not merely a wasted
  attempt - it *destroys the working module* and leaves its nodes unscripted until
  the developer rebuilds again. The "refinement, not a correctness gap" framing of
  ADR-0066 was too kind: the failure is visible and recoverable, but it loses the
  running code for a change that was never real.
- **The trigger is already a poll, and that is an advantage here.** A poll can
  look at a candidate more than once; an OS watch fires once per event and would
  need its own timer to debounce. Waiting for a value to *hold* is the natural
  shape for a poll and costs nothing but a remembered stamp and a counter.
- **The cost is latency, and it is bounded by the frame.** Debouncing means a real
  rebuild is adopted one poll later, not never. At a frame a poll, one extra poll
  is a frame of latency - invisible next to the time it takes to link a module.

## Decision

**A change is reported only once the module's write time has held across a
requested number of consecutive polls. `poll_changed` takes that number
(`settle_polls`), defaults to 1 - report as soon as the time stops moving - and
the editor's frame loop passes 2.**

### 1. The settle count lives in the library, not the editor

`ScriptLibrary::poll_changed(u32 settle_polls = 1)` keeps, per module, the
candidate write time last seen (`pending_stamp`) and how many consecutive polls
have seen it (`settled_polls`). A poll that sees a time different from the loaded
one:

- if it differs from `pending_stamp`, records it and sets `settled_polls` to 1 -
  the candidate is new, so the count starts over;
- otherwise increments `settled_polls`;
- reports the module only when `settled_polls` reaches `settle_polls`, at which
  point the time is recorded as loaded and the candidate state is cleared.

`settle_polls` of 0 is treated as 1, so "off" is not a way to lose a change
entirely; a caller that wants the old behaviour passes nothing.

This is the change-detection step, so it belongs with the detection (ADR-0066 put
the poll in the library for the same reason). The editor supplies only the number,
because it is the editor that knows its poll is once per frame.

### 2. The editor debounces by one frame

`ScriptCatalog::poll_changed_modules(settle_polls = 1)` and
`EditorContext::poll_script_modules(settle_polls = 1)` pass the count through, and
`EditorHost::step()` calls the poll with 2. One frame of latency skips the common
"the linker has the file open" window without delaying a real rebuild
perceptibly. The default stays 1, so every other caller and every existing test
keeps the immediate behaviour.

## Consequences

**Easier:**

- A rebuild that takes more than one poll to write is adopted once, when it is
  finished, instead of being read mid-write. The failure mode that motivated this
  - a poll firing during a link and dropping a working module - is gone for any
  build that pauses between writes.
- The debounce is headless and deterministic: it is a pure function of a sequence
  of observed write times, so a test drives it with `touch_forward()` and no
  clock. There is no timer to wait on and nothing to sleep for.
- The knob is explicit and per-call. A caller with a slow poll can ask for a
  larger settle count; a caller that wants the old "report immediately" passes 1
  or nothing.

**Harder:**

- **Debouncing cannot make a non-atomic write atomic.** If a build writes the
  image and then waits longer than the settle window before finishing, the poll
  still sees a settled file and reloads a partial one. The guard covers the
  ordinary case - a linker that writes in a burst - not every writer. The
  remaining failure is ADR-0061's, unchanged: the module is dropped and a rebuild
  brings it back.
- **A very fast double rebuild can be coalesced.** Two writes close together can
  land in one settle window, so the poll reports the module once and the reload
  reads whatever is on disk then. For a build that has finished, that is the final
  image; for one still writing, the settle rule above applies. Coalescing two
  *finished* rebuilds is harmless - the newer image is what loads.
- **One more piece of per-module state, and a per-call parameter.** The poll now
  carries a candidate time and a count alongside the loaded time. It is a stat, two
  comparisons, and an increment per module per poll - the same order as before,
  and still trivial for the handful of modules a game has.

**Deliberately not in this cut (named so they are planned, not forgotten):**

- **Debugging support.** A debugger attached to a reloaded module is the remaining
  M7 row.
- **State preservation across a reload.** ADR-0061's limitation, carried forward:
  a reloaded script starts fresh. Serializing live script state is a later cut.
- **An OS watch instead of a poll.** Still deferred (ADR-0066's alternative). If
  it lands, the settle rule moves behind the same `poll_changed` signature and no
  caller changes.

## Alternatives considered

- **Wait a fixed wall-clock time after a change before reloading.** Rejected: it
  needs a clock and a "when did I first see this" per module, sleeps a thread or
  schedules a wakeup, and its correctness depends on the sleep being longer than
  the build - a guess. Counting polls needs no clock, is deterministic in a test,
  and ties the wait to the caller's own cadence, which is what "settled" means
  here.
- **Only reload if the image is currently loadable.** Rejected: the check is a
  `dlopen` of the candidate, which either succeeds (so the file was complete and
  the poll was not early after all) or fails with the loader's error. It would
  make detection do the load, so the "detect, then adopt" split of ADR-0046 and
  ADR-0066 would collapse, and a legitimate load failure would be
  indistinguishable from a mid-write one.
- **Reject a reload when the file size is not stable.** Rejected as a second,
  weaker signal for the same thing: the write time already moves on every write,
  and a size check misses a rewrite that keeps the length (a patch to an existing
  image) while adding another field to carry. One signal, held across polls, is
  enough.
- **Debounce in the editor only, leaving `ScriptLibrary::poll_changed` immediate.**
  Rejected: the settle state (the candidate time and the count) has to live
  somewhere across polls, and the library is what holds the per-module state. The
  editor would have to keep a parallel map keyed by module id - the same
  bookkeeping, in the wrong place, and invisible to the headless tests.
- **Default `settle_polls` to 2.** Rejected: it would silently add a frame of
  latency to every existing caller and test, including ones that poll a single
  time and expect the change. The immediate behaviour is the honest default for a
  poll whose caller chose its own cadence; the editor, which knows it polls every
  frame, opts in.
