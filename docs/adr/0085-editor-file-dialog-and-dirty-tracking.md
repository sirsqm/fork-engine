# 0085. The editor File menu, file dialog, and unsaved-changes tracking

Date: 2026-10-25

Status: Accepted

## Context

The editor could open a project and a scene from the command line and edit them,
but it could not save, open, or create a document from the UI. There was no File
menu, no way to pick a file, and no notion of a document being modified. Three
concrete consequences:

* A user could edit a scene and had no way to write it back to disk from the
  editor. Saving was a missing feature, not a rough edge.
* There was no "unsaved changes" state, so a New or a Quit would silently discard
  work. A destructive action with no prompt is a data-loss bug waiting for a
  user to hit it.
* Any file choice had to be a hardcoded path or a command-line argument, because
  there was no browser for the filesystem.

The editor is also tested headlessly (`fork_editor_tests`, no window), so whatever
decides what a dialog shows and what a confirm prompt does had to be testable
without ImGui, exactly as the hierarchy and profiler view models already are.

## Decision

Split the feature into a headless, testable core and a thin ImGui layer, and make
the destructive path go through a prompt.

1. **`FileDialogViewModel` is the dialog, headlessly.** It holds the current
   directory, the rows, the file name, and the filter, and it answers
   `enter()`, `open()`, `selected_path()`, and so on. It touches the filesystem
   only through `fe::fs`, so it is unit-tested with no window and no ImGui. This
   is the same seam as `HierarchyViewModel` and `AssetBrowserViewModel`.

2. **`fe::fs::list_directory` and `fe::fs::is_directory` are the primitives.**
   `list_directory` returns the immediate children of a directory as
   `std::vector<DirectoryEntry>`, directories first and then by name, so the
   dialog renders a stable, deterministic list; a missing directory yields an
   empty list rather than an error, matching `list_files_recursive`.
   `is_directory` is the one place directory-ness is decided, so `enter()` and
   the dialog's fallback do not each invent their own test.

3. **The document carries a dirty flag.** `EditorContext` gains `is_dirty()`,
   set by an edit and cleared on New, Open, and Save. `EditorUi` shows a trailing
   `*` after the document name when it is set. The flag is the single source of
   truth for "would a New lose work?".

4. **A destructive action asks first.** `EditorContext` carries a one-shot
   `FileRequest` channel (mirroring `PlayRequest`) and a `confirming_` state. When
   a request that would discard work arrives on a dirty document — New, Open, Quit
   — the host holds it pending and shows a `ConfirmPanel`; the action runs only if
   the user confirms. `ConfirmDiscard` and `CancelDiscard` are requests on the
   same channel, so the answer is a value the host consumes, not a callback the
   panel owns. That is what makes the flow testable headlessly: a test sends the
   request and then the answer, exactly as the buttons do.

5. **The host owns the prompt's lifetime.** `EditorHost::handle_file_request()`
   clears the prompt when it consumes an answer, rather than relying on the panel
   to clear it on a click. A programmatic answer (a test, a future script) goes
   through the same path and leaves no stuck prompt behind.

6. **Saving uses the path when there is one, and the dialog when there is not.**
   "Save Scene" writes straight to `scene_path_` when the document has one;
   otherwise it opens the dialog, because there is nowhere to write. "Save As…"
   always opens the dialog. This is what makes the common case one keystroke and
   the first save a deliberate choice.

## Consequences

* The File menu (New, Open, Save, Save As…) and the Ctrl+N / Ctrl+O shortcuts are
  live, with shortcuts suppressed while a text field has focus so typing a scene
  name does not trigger them.
* The dirty flag, the dialog, and the confirm flow are all covered by headless
  tests: `test_file_dialog.cpp` for the view model, `test_editor_context.cpp` for
  the flag and the request handshake, and `test_editor_host.cpp` for the whole
  path through a real host (save, dirty, reopen, confirm, cancel).
* A destructive action can no longer silently drop unsaved work.
* The dialog is a panel, not a native OS dialog. That is deliberate: a native
  dialog is per-platform code the editor does not need yet, and a panel keeps the
  editor's own look and its headless testability. A native picker is a later
  option behind the same view model.

## See also

* ADR-0007 — the editor UI toolkit.
* ADR-0064 — export runtime and player (the shipping path the tools serve).
* `docs/ROADMAP.md` — the editor work sits on the authoring side of the shipping
  path.
