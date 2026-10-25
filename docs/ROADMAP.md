# Fork Engine Roadmap

The front line of the plan: why the work is sequenced this way, and what "next"
means. It is deliberately short. The two documents it points into hold the
detail:

- [SHIPPING_QUEUE.md](SHIPPING_QUEUE.md) — *what to do next*: the ordered backlog
  the daily automation works through, one item per run.
- [MILESTONES.md](MILESTONES.md) — *what each milestone contains*, and where it
  stands.

## Contents

- [How to read this plan](#how-to-read-this-plan)
- [The pillars](#the-pillars)
- [Near-term priorities](#near-term-priorities)
- [Where the work happens](#where-the-work-happens)
- [How a phase is finished](#how-a-phase-is-finished)
- [Milestones](#milestones)
- [Ongoing](#ongoing)

## How to read this plan

The engine is built incrementally. Each milestone is independently testable and
must build and pass its tests before the next one starts. This document is a
plan, not a promise; milestones may be reordered as the design becomes clearer,
and changes are recorded in the ADRs.

The plan was once three files that restated each other — `STRATEGY.md`,
`ROADMAP.md` and `SHIPPING_QUEUE.md`. It is three again, but each now has one
job: this file is *why and in what order*, `SHIPPING_QUEUE.md` is *the next
concrete step*, and `MILESTONES.md` is *what each milestone contains*.

The guiding rule: a few genuinely working systems beat a long list of
half-finished features.

## The pillars

Two capabilities shape how every subsystem is designed, not just what the
engine eventually features. They are the *direction*; the milestones are the
*sequence*.

### 1. Performance is a product feature

The engine is fast by construction and by measurement. "Feels smooth" is a
requirement, not a nice-to-have.

- *What it implies:* a frame profiler; a render-extraction step so the authoring
  model (the scene graph) and the processing model (a flat instance array) meet
  at one seam; a job system for CPU work, with the GPU submission boundary kept
  single-threaded; allocator discipline (a per-frame arena); and a benchmark
  harness in CI that fails on regression.
- *Risks:* premature optimization, mitigated by the measure-first rule and by
  the order of the work (nothing is parallelised or cached before P1 can show it
  matters); parallelism bugs, mitigated by keeping the device single-threaded,
  testing parallel paths against serial ones, and running the suite under a
  thread sanitizer once the job system lands.
- *Sequenced as* **M9**; trade-offs in [ADR-0017](adr/0017-performance-strategy.md).

### 2. Games ship anywhere

A developer can choose a target platform and get a runnable build of their game
without hand-editing the engine. This is about shipping *games*, not about the
editor running on several OSes.

- *What it implies:* a complete platform layer (window, input including touch
  and gamepad, filesystem, threads, time, dynamic libraries, application
  lifecycle), with everything OS-specific behind it; asset packaging (a
  self-describing pack the runtime mounts); export templates (a prebuilt runtime
  per target plus a build step that turns a project into a runnable game); and
  renderer backends added as targets demand them.
- *Risks:* scope explosion, mitigated by proving portability with the cheapest
  target (web) before mobile and by only adding a backend when a target needs
  one; platform-specific bugs hidden in shared code, mitigated by the platform
  audit, the null backend, and CI across the three desktop platforms.
- *Sequenced as* **M8**; trade-offs in
  [ADR-0018](adr/0018-multi-platform-shipping.md). The near-term target is
  desktop, and [ADR-0032](adr/0032-near-term-platform-scope.md) records why the
  wider claim is sequenced rather than asserted.

The pillars are deliberately ordered by risk. Performance is the lowest-risk and
improves everything else; shipping is the higher-risk of the two because it
touches the platform contract, so it follows.

## Near-term priorities

The order the next work is actually taken in, chosen so that a small number of
things get *finished* rather than many get started. This is the roadmap's front
line; the concrete steps are in [SHIPPING_QUEUE.md](SHIPPING_QUEUE.md) and the
milestone detail is in [MILESTONES.md](MILESTONES.md).

1. **A vertical slice** — one small game built end to end in the engine (M2.5).
   Everything else is secondary to having something real that exercises the whole
   stack together.
2. **Ship that slice to a browser, then a phone, then the desktop** — the
   shipping queue's four phases. This is the shortest path from "it runs" to
   "someone can play it".
3. **Honest scope** — the "ship anywhere" claim is scoped to desktop for now, in
   [ADR-0032](adr/0032-near-term-platform-scope.md), so the README does not
   promise more than the engine delivers.
4. **Scripting** — the language-agnostic seam plus C++ native modules as the
   primary language (M7). This is what turns the engine into something people can
   make games with.
5. **Guard the performance claims** — the benchmark as a CI regression gate
   (M9) and sanitizer builds (ASan/UBSan), while the codebase is still small
   enough to keep them cheaply green.
6. **Content pipeline depth** — glTF instead of only OBJ, and skeletal animation
   (M4, M6), once a game needs real 3D content.
7. **Editor polish and more platforms** — after the above.

The intent: finish the small things. The engines that last are the ones whose
authors ruthlessly complete them.

## Where the work happens

Three documents hold the plan, and each has one job. Knowing which to open is
the point of the split:

| Question | Document |
| --- | --- |
| Why is the work ordered this way? | This file |
| What is the next concrete step? | [SHIPPING_QUEUE.md](SHIPPING_QUEUE.md) |
| What does milestone N contain, and is it done? | [MILESTONES.md](MILESTONES.md) |
| Why was a design chosen? | [`docs/adr/`](adr/README.md) |
| How does a subsystem work today? | [`docs/ARCHITECTURE.md`](ARCHITECTURE.md) |

## How a phase is finished

The shipping queue is grouped into phases, each with a ship bar — a statement
that is either true or false, not a milestone label. A phase is finished when
its bar is met and every item under it is done; the queue then moves on. The
bars are in [SHIPPING_QUEUE.md](SHIPPING_QUEUE.md).

## Milestones

What each milestone contains, and where it stands, is in
[MILESTONES.md](MILESTONES.md). The milestones are the *sequence*; the pillars
above are the *direction*, and [SHIPPING_QUEUE.md](SHIPPING_QUEUE.md) is the
front line.

## Ongoing

These are not phases and never "finish"; they hold for every change.

- Keep the build reproducible and warning-clean.
- Keep the tests green; every fix gets a test that would have caught it.
- Run the suite under AddressSanitizer and UndefinedBehaviorSanitizer while the
  codebase is small enough to keep them cheaply green (`-DFE_ENABLE_ASAN=ON`);
  a dedicated CI job does this ([ADR-0035](adr/0035-ci-quality-gates.md)).
- Keep the benchmark a regression gate, not just a dev tool: CI records the
  numbers and fails on a large regression (see `benchmarks/` and
  [ADR-0033](adr/0033-performance-regression-gate.md)).
- Keep the architecture documentation and ADRs current.
- Review dependency licenses for compatibility with MIT.
