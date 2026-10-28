# Test that the three planning documents stay three documents, run by ctest in
# script mode.
#
# The planning docs were merged into a single docs/PLAN.md twice, and both times
# it was split back out: the pillars (why), the milestones (sequence and status),
# and the shipping queue (what next) answer different questions and are read
# separately, and one file grew too large to read. A later session re-merged them
# on the strength of a remembered instruction, which is exactly the failure this
# guards against: the decision to keep them separate is written down in AGENTS.md
# and checked here, so a silent re-merge fails a test instead of landing.
#
# Run by tests/CMakeLists.txt:
#   cmake -DREPO_ROOT=<path> -P cmake/tests/test_planning_docs.cmake

if(NOT DEFINED REPO_ROOT)
  message(FATAL_ERROR "REPO_ROOT must point at the repository root")
endif()

set(_docs
  "${REPO_ROOT}/docs/STRATEGY.md"
  "${REPO_ROOT}/docs/ROADMAP.md"
  "${REPO_ROOT}/docs/SHIPPING_QUEUE.md"
)

# The merged file must not come back.
if(EXISTS "${REPO_ROOT}/docs/PLAN.md")
  message(FATAL_ERROR
    "docs/PLAN.md exists again. The strategy, roadmap and shipping queue are "
    "three documents by decision (AGENTS.md) and were split back out twice. Do "
    "not merge them; edit the three files in place.")
endif()

# Each document must exist and carry real content, so a stub or an accidental
# truncation is caught too.
foreach(_doc IN LISTS _docs)
  if(NOT EXISTS "${_doc}")
    message(FATAL_ERROR "${_doc} is missing; the three planning documents must exist.")
  endif()
  file(READ "${_doc}" _text)
  string(REGEX REPLACE "\n$" "" _text "${_text}")
  string(REPLACE "\n" ";" _lines "${_text}")
  list(LENGTH _lines _count)
  if(_count LESS 40)
    message(FATAL_ERROR
      "${_doc} has only ${_count} lines; it looks truncated or stubbed.")
  endif()
endforeach()

# CONTRIBUTING.md is the human-facing record of the decision; it must keep
# naming the three files and must keep the never-merge rule.
file(READ "${REPO_ROOT}/CONTRIBUTING.md" _contrib)
foreach(_name STRATEGY.md ROADMAP.md SHIPPING_QUEUE.md)
  if(NOT _contrib MATCHES "${_name}")
    message(FATAL_ERROR "CONTRIBUTING.md no longer names docs/${_name}.")
  endif()
endforeach()
if(NOT _contrib MATCHES "Never merge them into")
  message(FATAL_ERROR
    "CONTRIBUTING.md no longer states that the planning documents must never be "
    "merged. The unmerge decision must stay recorded or it will be undone again.")
endif()

# No tracked file may point at a merged plan: the three documents are the only
# valid targets, so a lingering link or breadcrumb to one merged file is stale.
# Prose that *names* the old merged file in backticks while explaining this rule
# (AGENTS.md, CONTRIBUTING.md) is not a pointer and is not matched: a pointer
# looks like `docs/PLAN.md,` or `](docs/PLAN.md)`.
file(GLOB_RECURSE _tracked
  "${REPO_ROOT}/CMakeLists.txt"
  "${REPO_ROOT}/*.md"
  "${REPO_ROOT}/cmake/*.cmake"
  "${REPO_ROOT}/cmake/**/*.cmake"
  "${REPO_ROOT}/docs/*.md"
  "${REPO_ROOT}/docs/adr/*.md"
  "${REPO_ROOT}/engine/*"
  "${REPO_ROOT}/engine/**/*"
  "${REPO_ROOT}/editor/*"
  "${REPO_ROOT}/editor/**/*"
  "${REPO_ROOT}/examples/*"
  "${REPO_ROOT}/examples/**/*"
  "${REPO_ROOT}/tests/*"
  "${REPO_ROOT}/tests/**/*"
  "${REPO_ROOT}/tools/*"
  "${REPO_ROOT}/tools/**/*"
)
foreach(_f IN LISTS _tracked)
  # This script names the merged file in its own failure messages, so skip itself.
  if(_f STREQUAL CMAKE_CURRENT_LIST_FILE)
    continue()
  endif()
  if(_f MATCHES "/build" OR _f MATCHES "third_party/")
    continue()
  endif()
  file(READ "${_f}" _body)
  if(_body MATCHES "\\]\\([^)]*PLAN\\.md" OR _body MATCHES "PLAN\\.md[,)]")
    message(FATAL_ERROR
      "${_f} still points at a merged plan; retarget it to STRATEGY.md, "
      "ROADMAP.md, or SHIPPING_QUEUE.md.")
  endif()
endforeach()

message(STATUS "planning docs: three documents intact, no merged plan")
