# 0109. An in-engine debugging surface: a bounded log buffer and recorded script failures

Date: 2026-10-31

Status: Accepted

## Context

M7's only open roadmap box is **debugging support**: "a script that misbehaves
cannot be inspected: no breakpoints, no call stack, no watch. For C++ modules
this means either a debugger integration story or an in-engine logging/assertion
surface a game author can use without a debugger." The near-term priorities name
it directly (item 3): "what remains is debugging support, which is what a game
author needs the first time a script misbehaves."

The engine already *contains* a failing script correctly. `ScriptHost` wraps each
`update()` in a `try`/`catch`, turns a returned `Error` or a thrown exception into
a log line, disables the offending script, and keeps the frame going (ADR-0044,
ADR-0003). What it does not do is let anyone *see* that after the fact:

- **The log goes to stderr and is not kept.** `LogSink` is write-only; the
  default sink formats a line and drops it. A game author running an exported
  build, with no debugger and no terminal, has nothing to look at. Even in the
  editor, the log is only on the console the editor was launched from.
- **A contained failure is reported exactly once.** The script is disabled after
  it fails, so it never runs again and never logs again. The one line that
  describes the most useful event in the session scrolls out of view, and a tool
  that polls the log periodically (an editor panel redrawing each frame) will
  usually miss it.
- **The failure's structure is lost.** The log line is a formatted string. A tool
  that wants to show "which node, which behaviour, what went wrong" would have to
  parse English out of it.

The forces:

- **A game author's first need is to see what happened.** The cheapest useful
  debugging surface is not a breakpoint server; it is a console that shows the
  engine log and the script failures, and that survives a failure being reported
  once. That is a read-out, not a debugger, and it is what the roadmap box names
  as the acceptable alternative.
- **A read-out needs a reader, and the engine has no UI.** The engine is headless
  by design (ADR-0041, ADR-0042): the log lives in `core`, the script seam lives
  in `script`, and neither may depend on the editor or on ImGui (ADR-0007). So
  the *record* belongs in the engine, where both an editor panel and a test can
  read it, and only the *widgets* belong in the editor.
- **It must be bounded.** An unbounded in-memory log is a memory leak with a
  friendly name, and a script that fails every frame would grow a failure list
  without limit. A console only ever shows the tail.
- **A periodic reader must be able to tell "nothing new" from "I missed some".**
  A panel polls every frame; copying the whole buffer every frame is wasteful, and
  a buffer that wrapped past the reader has a real gap that must not be hidden.
- **The editor already owns a document with a log-shaped lifetime.** The editor's
  `EditorContext` is constructed when the editor starts and destroyed when it
  stops, which is exactly the lifetime a console's log buffer wants.

## Decision

**Record what the engine logs and what the script host contains, in the engine,
in two bounded structures a tool can read; and show them in an editor console
panel.**

### `core::LogBuffer` — a bounded in-memory log sink

A `LogSink` that keeps the most recent N records (default 512) in memory. Each
record carries its `LogLevel`, the formatted message, and a monotonically
increasing `sequence` assigned at write time and never reused. `snapshot()`
returns a copy; `newest_sequence()` is the marker a periodic reader keeps;
`dropped_count()` reports how many records were discarded to stay bounded, so a
reader that looked away can say so rather than silently showing a gap.

It is a plain sink, so it is installed with the existing `log::add_sink`. The
editor installs one per `EditorContext`; a test can install one directly. The
engine does not install one globally: a game that wants its log kept decides to
keep it, and the default (stderr) is unchanged.

### `script::ScriptDiagnostics` — the contained failures, structured

A small bounded record (default 64) of the failures `ScriptHost` contains: the
node id, the node's name at the time, the behaviour, the message, the frame, and
a `sequence`. `ScriptHost::report_failure` writes both the log line it already
wrote and one of these, so the structured record and the human line are produced
together and cannot drift. `ScriptHost::diagnostics()` exposes it.

Recording the failure in both places is deliberate: the log line is what a human
reading a terminal wants, and the structured record is what a tool wants. They
are not redundant because they serve different readers, and generating them in
one function is what keeps them consistent.

### `editor::ConsolePanelViewModel` and `editor::ConsolePanel`

The view model is the headless half: it polls both sources incrementally (copying
only records newer than the last it saw, so a quiet frame copies nothing), merges
them into one bounded ring, filters by level, and surfaces the "the log wrapped
past me" flag. The panel is a thin projection that emits widgets, the same
arrangement the hierarchy, asset-browser, and profiler panels use. The panel
registers in the editor host's panel set.

The editor installs its `LogBuffer` as a sink from `EditorContext`'s constructor
and removes it in the destructor, so the console shows the engine's own log
rather than a second, parallel one, and a headless test that constructs an
`EditorContext` can read what the editor logged.

## Consequences

**Easier:**

- A game author sees, in the editor, what the engine logged and which script
  failed on which node and why — the read-out the roadmap box asks for, without a
  debugger integration.
- A failure is not lost when it is reported once: it is recorded, so a panel that
  polls every frame still shows it.
- Both records are engine types with no UI dependency, so a test drives them
  directly and a future crash-report or export-time log dump can read the same
  structures.
- The incremental polling keeps the per-frame cost of a quiet console to a
  comparison, and the bounded rings keep a long session's memory flat.

**Harder / accepted costs:**

- A `LogBuffer` is installed per `EditorContext`, so an editor that never opens
  the console pays for a bounded ring. That is a few hundred small strings, and
  the alternative — installing the sink only when the panel first draws — would
  miss everything logged before the panel opened, which is usually the
  interesting part.
- The record types carry `std::string`, so a snapshot copies. That is the right
  trade for a debugging surface read a few times a second; a zero-copy view would
  have to hold the buffer's lock across the read and would be far more fragile.
- This is a read-out, not a debugger. There are still no breakpoints, no call
  stack, and no watch. Those remain future work; the ADR records that the
  in-engine surface is the chosen first step, not the whole story.

## Alternatives considered

- **A debugger integration (GDB/LLDB attach to a C++ module).** The real answer
  for a developer with a debugger and a local build, but it does not help an
  exported build, and it is a much larger commitment than a console. Deferred.
- **Install the log buffer only when the console panel first draws.** Rejected:
  it would miss everything logged before the panel opened, and in the editor the
  interesting startup failures happen exactly then.
- **A global log buffer the engine installs at startup.** Rejected: the engine
  keeps no process-global mutable state it can avoid, and whether to keep the log
  is a decision the host (editor, test, or game) should make, not the library.
- **Keep only the structured script failures, not the log.** Rejected: the log
  carries everything the engine and the gameplay code report, not only script
  failures, and a console that cannot show an ordinary error is not a console.
- **An unbounded buffer.** Rejected: a long session would grow it without limit,
  and a console shows only the tail regardless.
