## What and why

<!-- What does this change, and why? Link the issue it closes, if any. -->

## How it was tested

<!-- The build, the tests you ran, and any manual check. -->

## Checklist

- [ ] The build is warning-clean with `-DFE_WARNINGS_AS_ERRORS=ON`.
- [ ] `ctest` passes locally, and new behaviour has a test.
- [ ] `clang-format` has been run on the files I touched (CI pins 23.1.3).
- [ ] Any architectural change is recorded in an ADR under `docs/adr/`.
- [ ] I have not claimed a build or test result I did not actually verify.

<!--
Do not merge the planning documents: docs/STRATEGY.md, docs/ROADMAP.md, and
docs/SHIPPING_QUEUE.md stay three files (a test enforces it).
-->
