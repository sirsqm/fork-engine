# ADR-0018: Multi-platform shipping — one runtime, many targets

- **Status** — Proposed
- **Supersedes** — none
- **Superseded by** — none

## Context

The engine must let a developer ship a game to every platform its players use,
not merely run the editor on several operating systems. Desktop is the initial
target; web, mobile, and eventually consoles follow.

Shipping is not only a graphics problem. A game that runs on a phone or in a
browser must deal with:

- A window or surface whose creation, input (touch, gamepad), and lifecycle
  (suspend, resume, resize, orientation) are OS-specific.
- A filesystem that may not exist in the usual sense (browsers) or that is
  sandboxed (mobile). A folder of loose asset files is not shippable there.
- A graphics API that differs per target: OpenGL 3.3 core on desktop, WebGL2 or
  WebGPU on the web, GLES/Vulkan/Metal on mobile.

The current platform layer already hides window, input, and context behind the
`platform` module, and the RHI already hides the backend. The gap is that
"cross-platform" has been a statement about the *editor's* host OS, not a plan
for the *games'* target platforms, and there is no packaging or export path.

## Decision

**Treat the platform layer as the complete OS contract, add a null backend,
package assets, and prove portability by exporting to the cheapest target (web)
before the expensive ones (mobile, consoles).**

### The platform contract

- `platform` owns *all* OS-specific behaviour: window/surface, input (keyboard,
  mouse, touch, gamepad), filesystem, threads, time, dynamic libraries, and
  application lifecycle events.
- A **null/headless backend** implements the contract with no OS: no window, a
  no-op input source, an in-memory filesystem view. It exists for CI, dedicated
  servers, and deterministic tests, and it is the proof that nothing leaks past
  the abstraction.
- Input is expressed as abstract actions where practical, so touch and gamepad
  map onto the same gameplay code as keyboard and mouse.

### Assets and export

- A **pack format**: assets are baked into one or a few files with a table of
  contents. The runtime loads from a pack through the same resource paths it
  uses on disk, so gameplay code does not know or care which is in use.
- **Export templates**: a prebuilt runtime per target, plus an export step that
  turns a project into a runnable build (the project's scenes, assets, and
  native code linked or loaded against the template).
- Web is proven first (Emscripten/WASM + WebGL2): no store review, no device
  matrix, and it exercises the sandboxed-filesystem path immediately.
- Mobile follows (Android/iOS): lifecycle, touch, GLES/Vulkan/Metal backends,
  and app packaging.

### Backends on demand

- A graphics backend is written when a target needs it, not in advance. The RHI
  already abstracts the device; adding a backend must not require touching code
  above the RHI.
- Consoles are an architecture goal, implemented only when SDK access permits.
  No console code lands without the SDKs.

## Consequences

- Portability is a property of the codebase that CI can *test*: the engine builds
  and its tests pass with the null backend, and the web target builds in CI.
- Asset handling gains a second storage backend (pack vs disk) behind one
  interface; the resource system must not assume a real filesystem.
- Export templates mean the engine has a *release artifact* per target, which is
  new build infrastructure (a separate concern from the library build).
- Choosing web first de-risks the abstraction cheaply, but defers the harder
  mobile lifecycle and input work; that work is not made easier by the delay,
  only better informed.
- The project takes on the maintenance cost of every backend it ships. Backends
  are added deliberately, not collected.

## Alternatives considered

- **Write all backends up front.** Rejected: it spends effort on targets before
  their requirements are known and before there is a game to ship.
- **Desktop only; call it "cross-platform".** Rejected: it does not meet the
  goal. The goal is that games reach players, not that the editor runs on three
  desktop OSes.
- **A third-party abstraction (SDL, sokol, bgfx).** Considered for the platform
  and graphics layers. GLFW already covers desktop windowing and input well and
  is adopted; extending `platform` with our own backends keeps the contract
  shaped to the engine's needs. bgfx was not adopted because the RHI is small
  and the engine wants its own pass and resource model, which a general-purpose
  abstraction would fight. Revisit per target if a backend proves costly.
- **Loose-file assets with a manifest.** Rejected for shipping: it is not viable
  on web or mobile and invites missing-file bugs in distributed builds.
