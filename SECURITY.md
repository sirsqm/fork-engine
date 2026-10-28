# Security Policy

Fork Engine is pre-1.0 and has no released version yet, so `main` is the only
supported version. Fixes land there; there is no back-port branch to maintain.

## Reporting a vulnerability

Please report privately rather than in a public issue:

- **Preferred:** GitHub's private vulnerability reporting — the repository's
  **Security** tab, then **Report a vulnerability**. That keeps the report, the
  discussion, and the fix in one place and out of the public tracker.
- **Or email** [sirsamyoudev@gmail.com](mailto:sirsamyoudev@gmail.com) with a
  description and, if you can, a minimal reproduction.

We will acknowledge the report within a few days, keep you posted as we work on
it, and credit you in the fix unless you would rather stay anonymous. Please give
us a chance to fix it before disclosing it publicly.

## What is in scope

A game engine loads content it did not write, and that is where the security
surface is:

- **Asset decoding** — `GltfLoader` (`.gltf`/`.glb`), PNG, WAV, and OBJ. A
  malformed file that makes a loader read out of bounds, corrupt memory, or
  otherwise misbehave is a vulnerability, even though it needs a hostile file.
- **Pack loading** — `PackFile` / `PackSource`. A crafted pack that escapes the
  mounted root (path traversal) or over-reads a length field is a vulnerability.
- Anything else that lets untrusted content corrupt memory, escape a path, or
  run code the game did not ask for.

The report is most useful with a minimal reproduction: the smallest file or
pack that shows the problem.

## What is out of scope

- **Running gameplay modules.** `platform::DynamicLibrary` loads native C++ the
  game asked for ([ADR-0061](docs/adr/0061-cpp-gameplay-modules-hot-reload.md)).
  A loaded module is native code with the game's own privileges by design, so a
  module doing what its author wrote is not a vulnerability.
- **Crashing on your own changes.** A crash caused by editing the engine's
  source, or by an API caller violating a documented precondition, is a bug
  report (see [CONTRIBUTING.md](CONTRIBUTING.md)), not a security report.
- **Missing hardening with no demonstrated impact.**

## How we handle it

The pure-logic test suite runs under AddressSanitizer and UndefinedBehaviorSanitizer
in CI ([ADR-0035](docs/adr/0035-ci-quality-gates.md)), and every fix gets a test
that would have caught it. A security fix is no different: we will add a targeted
regression test alongside it.
