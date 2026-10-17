# ADR-0061: C++ gameplay modules and hot reload

- **Status** — Accepted
- **Date** — 2026-10-17
- **Supersedes** — none
- **Superseded by** — none

## Context

M7 landed the scripting seam and C++ as the language behind it
([ADR-0044](0044-scripting-seam.md)): `Script`, `ScriptHost`, `ScriptRegistry`,
and `ScriptSystem`, all header-only and headless. What it deliberately did not
land is the rest of M7's C++ path, and the roadmap names the gap precisely:

> C++ native gameplay modules: compile against the engine, load, and hot-reload
> without restarting the editor.

That row is the difference between a seam that *could* load game code and one
that *does*. Today a game's behaviours must be compiled into the executable that
links the engine — the breakout slice does exactly that. A designer cannot change
"this door opens when the player has the key" without a rebuild and a restart,
which is the whole reason the seam exists.

Two things block the row, and both are recorded as deferred work rather than
discovered here:

1. **There is no dynamic-library support.** The platform contract
   ([ADR-0041](0041-platform-contract-and-null-backend.md)) lists "Dynamic
   libraries" as a row, and the row is *Deferred*. Nothing in the engine opens a
   shared object or resolves a symbol; there is no seam for it and no code that
   names `dlopen` or `LoadLibrary`.
2. **There is no module contract.** Even with an image open, nothing states what
   a gameplay module exports or how the engine finds a behaviour in it. The
   `ScriptRegistry` maps a name to a factory, but nothing populates it from a
   file.

The forces that shape the answer:

- **The seam must not change.** ADR-0044 promised that loading a C++ module is
  "a second `ScriptRegistry` population and a second way to make `Script`
  instances — not a change to the engine." If this cut needs to change `Script`,
  the seam was wrong.
- **Crossing a binary boundary needs a contract.** A module is compiled
  separately and may be built against a different engine revision. The engine
  must be able to tell a compatible module from an incompatible one *before*
  calling into it, or a version skew becomes memory corruption rather than an
  error.
- **Hot reload is a lifetime problem, not a file-watching problem.** Detecting a
  rebuilt file is trivial. The hard part is that a live `Script` is an object
  whose vtable and methods live inside the image; unloading the image while an
  instance exists leaves a dangling vtable. Any reload design is really a design
  for destroying those instances first.
- **Platforms differ.** Linux and macOS will let a loaded image's file be
  replaced and re-opened; Windows will not, so a reload there must load a *new*
  file and retire the old one. A design that assumes "reload the same path" is a
  design that works on two platforms out of three.

## Decision

**A gameplay module is a shared library that registers behaviours through a
versioned entry point; the platform layer gains a `DynamicLibrary` seam to open
it; and `ScriptLibrary` loads it, owns the behaviours it provides, and hot-reloads
it by destroying the live scripts first and rebinding them to the new code.**

The scripting seam is unchanged. `Script`, `ScriptHost`, `ScriptSystem`, and the
shape of `ScriptRegistry` are exactly what ADR-0044 landed; this ADR adds a
loader in front of them and one row to the platform contract.

### 1. The platform gains its dynamic-library row

`platform::DynamicLibrary` (`fe/platform/DynamicLibrary.h`) is the deferred row
of the platform contract, implemented:

- `load(path)` opens an image, returning an `Error` with the OS message on
  failure; `symbol(name)` resolves an export and returns `nullptr` for an absent
  one (a normal lookup result, not an error, because a loader tries several names
  in turn); `unload()` closes it.
- `decorate(name)` appends the platform's shared-library suffix (`.so`, `.dylib`,
  `.dll`) so a caller can turn a logical module name into a filename without
  naming the OS.
- It is move-only, and the only file in the engine that names `dlopen`/`dlsym`
  or `LoadLibrary`/`GetProcAddress`. On POSIX it opens with `RTLD_NOW | RTLD_LOCAL`:
  `NOW` so a module with a missing dependency fails at load with a clear message
  rather than at first call, `LOCAL` so two modules cannot collide by exporting
  the same name.

This is the same pattern the window already uses: one concrete OS entry point
behind a small interface, selected by the caller, with no `Platform` object
threaded through the runtime.

### 2. The module contract is an ABI, and it is versioned

`fe/script/ScriptModuleApi.h` is the whole of the agreement between a module and
the engine:

- `kScriptModuleAbiVersion` — a version number. It changes only when the entry
  point's signature or the meaning of its arguments changes.
- `ScriptModuleEntry` — `u32 (*)(ScriptRegistry&)`, the entry point that registers
  the module's behaviours into a registry and reports how many it added.
- `FE_SCRIPT_MODULE(entry_function)` — the macro that emits the two exported
  symbols a module must have: `fe_script_module_abi_version()` and
  `fe_script_module_register(...)`. It uses `extern "C"` (so the name is not
  mangled and can be found by a fixed string) and default visibility (so it
  survives a `-fvisibility=hidden` build).

The engine **checks the version before calling anything**. A module built against
a different contract is refused with an error naming both versions, not entered.
Registering a behaviour or adding a component to a registration does not change
the ABI; only the entry point's signature does.

### 3. `ScriptLibrary` loads, owns, and reloads modules

`fe/script/ScriptLibrary.h` is the loader. It owns the `ScriptRegistry` and the
loaded images, and it is where hot reload lives:

- `load_module(path)` opens the image, checks the ABI, calls the entry point, and
  records which behaviours that module added (by diffing the registry's names
  around the call). The resolved path is the module's *id*.
- `attach(host, node, behaviour)` builds a script from the registry and attaches
  it to the node through the ordinary `ScriptHost`, and records the binding so a
  reload can rebind it.
- `reload_module(host, module_id, new_image_path)` is the hot reload, in four
  ordered steps:
  1. **Destroy the live scripts first**, while the old code is still loaded. Each
     is detached through the host, which runs its `detach()` inside the image.
     The node pointers are captured before detaching, because detaching removes
     the host's binding.
  2. **Unload the old image** and forget the behaviours it registered.
  3. **Load the new image** and register its behaviours.
  4. **Rebind** a fresh script to every node that had one, built from the new
     code.

The order is the whole safety argument: no `Script` instance outlives the image
its code lives in. Step 3's `new_image_path` is why the design is portable — a
reload may load a *different* file, which is what Windows requires and what lets
a test load genuinely different code.

If step 3 fails, the module is dropped: its behaviours are unregistered and the
nodes it scripted are left unscripted. Attaching a fresh script to a behaviour
that no longer exists would just fail, so rebinding is deliberately skipped.

### 4. The seam did not change, and the reload is a host concern

`Script`, `ScriptHost`, and `ScriptSystem` are untouched in shape. The loader
uses only what ADR-0044 already exposed, plus two small additions that are
observability, not new model:

- `ScriptRegistry::remove_script(name)`, so a dropped module's behaviours stop
  resolving.
- `ScriptHost::node(node_id)`, so a reload can rebind a script to the node it was
  on.

Both are consistent with the seam's existing contract, and neither is a second
execution model: the loader attaches through `ScriptHost::attach_script` exactly
as a game would, and the scripts it binds run through the same `ScriptSystem`.

## Consequences

**Easier:**

- A game's behaviours live in a module the engine loads, not in the executable
  that links the engine. Changing a behaviour is a rebuild of the module and a
  `reload_module` call, not a restart.
- The seam's promise held: loading C++ code is a registry population and a way to
  make `Script` instances, and nothing in `core`, `scene`, `systems`, or `rhi`
  changed.
- The platform contract's "Dynamic libraries" row is implemented and tested, so
  the same `DynamicLibrary` is available to anything else that later needs to load
  an image.
- A version-skewed module is a clear error at load, not corruption at first call.
- The reload is headless and deterministic: a test loads two real module images
  and proves the second one's code runs after the swap, with no editor and no
  window.

**Harder:**

- **The reload is only as safe as its ordering.** The four steps must stay in
  order, and the destructor has the same shape: a registered behaviour is a
  `std::function` whose target lives inside a module, so the registry must be
  destroyed before the images. Both are documented at the point of the code
  because getting either wrong is a call into unmapped memory, not a leak.
- **A module is not sandboxed.** It is native code with the engine's privileges;
  a bad module is a crash, not a contained error. The failure containment
  ADR-0044 built covers a script that returns an error or throws from `update()`,
  not a module that corrupts memory.
- **The engine is a static library, so a module carries its own copy of the
  header-only engine code, and process-global engine state is not shared with
  it.** `fork_engine` is `STATIC`, so the module's own link of it is not the
  executable's: a function-local singleton such as `reflect::Registry::instance()`
  resolves to a *different object* inside the module than in the game. This was
  measured, not assumed — a module that calls `Registry::instance()` sees an
  address distinct from the executable's. The consequence for this cut is
  contained: `ScriptLibrary` passes the `ScriptRegistry` in explicitly (it is a
  value type, not a singleton), so behaviour registration crosses the boundary
  correctly, and the C++ seam is unaffected. But a *later* cut that wants a
  module to register a reflected type the inspector can see must not rely on the
  singleton: it has to pass the registry in through the entry point, the way
  `ScriptRegistry` already is. A module also must not depend on a function-local
  static shared with the engine. Making the engine a shared library would remove
  this whole class of surprise at the cost of shipping and versioning a runtime
  `.so`/`.dll`; that trade is not made here, and is the first thing to revisit if
  reflection-backed module bindings are built.
- **The registry is keyed by name, so two modules that register the same
  behaviour name collide** (the later registration wins). Per-module namespacing
  is a later cut if a game needs it; the honest behaviour today is that a name
  identifies one behaviour process-wide.
- **Rebinding rebuilds the script; it does not preserve its state.** A reloaded
  script starts fresh. Carrying live state across a reload means serializing it,
  which is the reflection-backed-bindings cut below, not this one.

**Deliberately not in this cut (the remaining M7 rows, named so they are
planned, not forgotten):**

- **File watching and automatic reload.** The mechanism is here
  (`reload_module`); deciding *when* to call it — watching a module's file for a
  change, debouncing a build — is editor work and lands with editor integration.
- **Reflection-backed bindings.** Exposing a script's properties to the inspector
  and the serializer is the next cut; it reuses
  [ADR-0006](0006-component-reflection.md) and does not change this seam.
- **Editor integration and debugging.** An attach/inspect UI, a hot-reload
  button, and a debugger.
- **A module manifest.** The module contract is the two exported symbols; richer
  metadata (a declared engine version, a list of behaviours, dependencies) can
  join the ABI if it earns its place.

## Alternatives considered

- **Compile gameplay into the executable and never load a module.** Rejected: it
  is the status quo this ADR exists to leave. It makes "a designer changes a
  behaviour" a rebuild-and-restart, which is the exact cost the seam was built to
  remove.
- **A C++ interface (an abstract `IModule`) with a `create()` export.** Rejected
  as the contract. It requires the module and the engine to agree on a C++ ABI
  for a virtual class — vtable layout, name mangling, `std::` types in the
  signature — which is precisely the fragile surface a versioned `extern "C"`
  entry point avoids. The module still uses C++ freely; only the *boundary* is C.
- **Load the same file in place, without a `new_image_path`.** Rejected. It works
  on Linux and macOS and fails on Windows, where a loaded image's file cannot be
  replaced. Taking the replacement path as a parameter makes the portable case the
  default and the swap expressible, at the cost of one argument.
- **Reload by leaking the old image and never unloading it.** Rejected: it makes
  the reload safe by making it accumulate mappings, so an editor that reloads
  through a session leaks one image per reload. The lifetime problem is real and
  is better solved by ordering than by leaking.
- **Rebind by re-running the module's entry point over the existing registry
  entry.** Rejected as the reload path. It replaces the factory without touching
  live instances, so an existing script keeps running old code — the reload would
  appear to succeed and change nothing. Destroying the instances first is what
  makes the new code actually live.
- **A `Platform` interface that owns dynamic loading.** Rejected for the same
  reason ADR-0041 rejected it for the window: it is a second indirection over a
  single entry point. `DynamicLibrary` is the seam; a `Platform` object is
  speculative abstraction over it.
