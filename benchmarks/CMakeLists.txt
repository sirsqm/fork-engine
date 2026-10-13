# The runtime performance benchmark (see docs/adr/0033-performance-regression-gate.md).
# It links the whole engine because it measures the real
# renderer and physics systems, and it runs headless, so it needs a GL context
# (real or llvmpipe under Xvfb) exactly like the graphics tests.
add_executable(fork_bench scene_bench.cpp)
target_link_libraries(fork_bench PRIVATE FE::engine fe_warnings)
target_compile_features(fork_bench PRIVATE cxx_std_20)
set_target_properties(fork_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# A fast smoke run so `ctest` exercises the benchmark's own plumbing (it must
# start, simulate, and emit a parseable block) without measuring anything the CI
# gate would compare. The gate itself is a separate, deliberate step: see
# benchmarks/check_regression.py and .github/workflows/ci.yml.
add_test(NAME benchmark_smoke COMMAND fork_bench --scene mixed --frames 30 --warmup 5)
