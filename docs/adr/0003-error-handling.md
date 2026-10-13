# ADR-0003: Errors as values, not exceptions

- **Status:** Accepted
- **Date:** 2026-01

## Context

The engine must handle failure everywhere: a missing asset, a malformed scene
file, a failed shader compile, an unsupported graphics feature. It needs a
consistent policy for how failure is reported, because mixing policies is how
error handling rots.

Two mainstream options exist. **Exceptions** are idiomatic C++ and propagate
failure automatically, but they impose hidden control flow, cost unwinding on
hot paths, and are disabled or discouraged in some engine and platform contexts.
**Error codes / expected values** make failure explicit in signatures but require
callers to check, and can be verbose.

A game engine has specific pressures: rendering and streaming are hot paths that
must not pay for unwinding; failures are usually expected and local (a file did
not exist); and the codebase is large enough that invisible control flow becomes
a maintenance hazard.

## Decision

Represent recoverable failure as a **value**:

- `fe::Error` is a lightweight message; `fe::Result<T>` is a value-or-error.
- Functions that can fail for ordinary reasons return `Error` or `Result<T>`.
- **No exceptions are thrown** anywhere in the engine.
- Assertions are reserved for programming errors and are not an error-handling
  mechanism:
  - `FE_ASSERT` — a debug contract, compiled out in release.
  - `FE_CHECK` — an always-on contract, for cheap preconditions on public API
    boundaries and for untrusted data (files, scripts, network).

`Result<T>` is designed so an error cannot be silently ignored without a
deliberate act: the success flag is an explicit `operator bool`.

## Consequences

**Easier:**

- Failure modes are visible in signatures; a reader can see what can fail.
- Hot paths pay nothing for error handling.
- No hidden control flow; debugging is straightforward.
- Works in environments where exceptions are disabled.

**Harder:**

- Callers must handle errors explicitly, which is more code than letting an
  exception fly.
- Deeply nested calls need propagation through return values (mitigated by
  returning early and by `Result`'s ergonomics).
- A truly exceptional, unrecoverable condition still terminates the process via
  an assertion, which is deliberate.

## Alternatives considered

- **Exceptions throughout.** Rejected: hidden control flow, unwinding cost on
  hot paths, and inconsistency with engine/platform conventions.
- **Bare error codes and out-parameters.** Rejected as error-prone and easy to
  ignore; `Result<T>` gives the explicitness without the footguns.
- **A third-party `expected`/`outcome` library.** Rejected for now to keep the
  dependency surface small; `Result<T>` is small enough to own. This can be
  revisited if the standard `std::expected` becomes uniformly available.
