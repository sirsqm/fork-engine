# ADR-0007: Editor UI toolkit - immediate-mode Dear ImGui with a docking host

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0001](0001-engine-architecture-overview.md)

## Context

Milestone 2 turns Fork Engine into an editor. The editor needs a tooling UI:
docked panels (a scene hierarchy, a play/stats panel, later a property inspector,
asset browser, and console), resizable splits, and a central area that can host
a live view of the scene. This is the first graphical UI the project has needed,
so the choice of toolkit has long-term consequences for build time, dependency
surface, look and feel, and how much UI code we own.

The constraints are concrete:

- The engine is MIT licensed. The toolkit must be licence-compatible with an MIT
  project and must not impose copyleft obligations on the editor binary.
- The build is pinned, offline-capable after fetch, and warning-clean with
  warnings-as-errors on GCC, Clang, and MSVC. A toolkit that ships a large build
  system or its own idea of compiler flags fights this.
- The UI must integrate with what already exists: the `platform` window (GLFW)
  and the OpenGL 3.3 context that `rhi` drives. There is no second windowing
  system and no interest in adding one.
- The editor is a *client* of the runtime (ADR-0001). The UI toolkit must not
  leak into the engine library or redefine engine concepts.
- The component set is tiny and the reflection-based inspector is deferred
  (ADR-0005), so the toolkit cannot assume generated property UI. Panels are
  written by hand for now.

Two broad approaches exist. A **retained-mode** toolkit (Qt, GTK, wxWidgets)
gives a widget tree, native theming, and mature docking, but each is a large
dependency with its own build system, licensing model, and event loop - and it
would pull a second object model alongside the engine's. An **immediate-mode**
toolkit (Dear ImGui) rebuilds the UI each frame from ordinary code: no widget
objects, no callbacks, no lifecycle, and it is designed for exactly this kind of
game tooling.

## Decision

Use **Dear ImGui**, on its docking branch, pinned to the tag
`v1.92.9-docking`, vendored through `FetchContent` like the other dependencies.

Specifics:

- **Licence:** Dear ImGui is MIT, the same licence as the engine. No copyleft
  obligation, no attribution requirement beyond keeping the notice in the
  vendored sources; the `third_party`-style notices in `third_party/README.md`
  policy apply.
- **Scope of the dependency:** Dear ImGui is used only by the `editor` targets.
  Nothing in the `fork_engine` library includes an ImGui header, and no engine
  module gains an edge. The dependency lives beside the engine, not inside it.
- **Backends:** `imgui_impl_glfw` (platform, reusing the existing GLFW
  dependency) and `imgui_impl_opengl3` (renderer). The editor passes the window's
  opaque handle (`Window::native_handle()`) to the GLFW backend; the engine
  remains free of any ImGui or GLFW type in its public headers.
- **Docking:** the docking branch is used for `DockSpace`, panels, and user
  rearrangeable splits. Multi-viewport (`ImGuiConfigFlags_ViewportsEnable`) is
  deliberately **not** enabled: it creates additional OS windows and GL contexts,
  which complicates context ownership for no benefit at this stage.
- **GL loader:** `imgui_impl_opengl3` uses its own embedded GL3 loader. This is
  independent of the `fe::rhi` backend's glad loader; both resolve the same
  driver entry points and coexist. No attempt is made to share one loader,
  because that would force ImGui to include an engine GL header.
- **Warning policy:** the ImGui sources are compiled as third-party code. They
  are **not** given the `fe_warnings` interface, and their include directory is
  marked `SYSTEM`, exactly like GLFW and nlohmann/json. Our editor translation
  units include `<imgui.h>` as an external header.

## Consequences

**Easier:**

- No widget object model, no signal/slot graph, no second event loop. A panel is
  a function that reads engine state and issues UI calls.
- One small, permissively licensed dependency with no build system of its own,
  consistent with the project's dependency policy.
- Panels compose directly over engine data. The hierarchy panel walks `Scene`
  through its public API and never needs a translation layer.
- Build cost is a handful of `.cpp` files; the editor stays easy to build on all
  three CI platforms.

**Harder:**

- Immediate mode re-emits the UI every frame. That is negligible at editor
  scale, but it means no retained widget to inspect, animate, or drive from
  outside a frame.
- The editor owns more UI code than a retained toolkit would require; docking
  layout defaults, selection, and tree state are ours to manage.
- Accessibility and native platform theming are out of reach. Accepted: this is
  an internal tool for a game engine, not a shipping consumer application.
- Dear ImGui is not a stable ABI/API contract across versions. The pin is exact,
  and upgrades are a deliberate change (as with every pinned dependency).

## Alternatives considered

- **Qt (Widgets or Quick).** Mature docking and a rich widget set, but it is a
  very large dependency (and its own build system), and open-source use is under
  LGPLv3 - a poor fit for a permissively licensed engine, and a heavier
  integration than the editor justifies. Rejected.
- **GTK.** LGPL, C-first, and again a full application framework with its own
  main loop and theming stack. Rejected for the same reasons as Qt.
- **wxWidgets.** Permissive licence and native widgets, but a large framework
  with a long build and native-backend complexity on every platform. Rejected as
  disproportionate to a game-engine tool.
- **A bespoke retained-mode UI on top of `rhi`.** Full control, no dependency,
  and it would reuse the renderer - but implementing docking, panels, text,
  input focus, and scrolling is weeks of work that is not the engine's
  differentiating feature. Rejected as speculative; it can be revisited if the
  UI ever needs to ship inside the product rather than beside it.
- **SDL + a third-party UI library.** Adds a second windowing library beside
  GLFW, duplicating the `platform` layer for no gain. Rejected.
