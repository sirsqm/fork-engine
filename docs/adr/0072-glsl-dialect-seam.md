# ADR-0072: A GLSL dialect translation seam in the RHI

- **Status:** Accepted
- **Date:** 2026-10
- **Related:** [ADR-0010](0010-3d-rendering-foundation.md), [ADR-0042](0042-null-rhi-device.md),
  [ADR-0071](0071-web-first-2d-shipping-path.md)

## Context

Every shader in the engine is a hardcoded `#version 330 core` source string,
authored in the desktop GLSL dialect and handed to the driver unchanged. A
WebGL2 context cannot compile those sources: GLSL ES 3.00 requires
`#version 300 es`, and a fragment shader must additionally declare a default
float precision. This is the first blocker on the web-first shipping path
([ADR-0071](0071-web-first-2d-shipping-path.md)).

The two obvious ways to fix it are both wrong. Forking every shader string into a
desktop and an ES variant doubles the shader surface and guarantees the two drift
apart. Making each renderer pick a version string leaks the graphics API into
code that is supposed to know only the RHI.

## Decision

**Translate at the RHI seam, as a pure function of the source string, the stage,
and the target dialect.**

`fe/rhi/ShaderDialect.h` introduces `ShaderDialect` (desktop core / ES) and
`ShaderStage` (vertex / fragment), and one free function:

```cpp
std::string translate_shader_source(ShaderDialect dialect, ShaderStage stage, std::string_view source);
```

The properties that make it a seam rather than a helper:

- **It is pure.** No device, no global state, no I/O. The output is a function of
  the inputs, so it is fully unit-testable with no display or GPU.
- **Desktop translation is the identity.** The bytes handed to the desktop driver
  are the bytes the author wrote, so the desktop path cannot regress.
- **ES translation is additive and idempotent.** It rewrites the version directive
  to `#version 300 es` and, for fragment sources that do not already declare a
  precision, injects `precision highp float;` and `precision highp int;`. A source
  that already carries an ES directive and a precision is returned unchanged, so
  translating twice never emits a second version directive or a duplicate
  precision.
- **No renderer names a dialect.** The enum and the function live behind the RHI;
  the backend selects the dialect once, where the device is created (a later queue
  item, `shader-dialect-wiring`). Shader sources stay written once, in the desktop
  dialect.

## Consequences

- The engine keeps a single set of shader sources. The ES path is a translation,
  not a fork, so a shader fix lands once.
- The desktop path is provably unchanged: the identity is asserted by a test that
  compares the translated bytes to the input byte for byte.
- The seam is stage-aware because only fragment shaders need a default precision
  in ES. A caller must therefore say which stage a source belongs to; this is one
  extra argument at the creation path, not a new concept for shader authors.
- The ES translation is deliberately textual, not a parser. It recognises a
  version directive on its own line and the presence of a `precision` declaration,
  and nothing more. A future dialect with a different structure would extend the
  function rather than grow a GLSL front end; the function's purity makes that a
  local change.
- The precision check treats any `precision` token as a declaration. A source that
  mentions the word only inside a comment would suppress the injection; the
  engine's own sources do not, and the cost of being wrong (a compile error under
  ES) is loud rather than silent.

## Alternatives considered

- **Fork each shader into desktop and ES variants.** Rejected: doubles the shader
  surface, drifts, and makes every future shader two shaders.
- **Let each renderer choose a version string.** Rejected: it names the graphics
  API above the RHI, which is exactly the boundary ADR-0010 established.
- **Translate at shader-authoring time (a build step).** Rejected: it adds a code
  generation step to the build and makes the translated source invisible to tests;
  a pure runtime function is simpler and directly testable.
