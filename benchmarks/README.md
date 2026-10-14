# Benchmarks

A headless performance benchmark and the CI regression gate that protects it.
Design and trade-offs: [ADR-0033](../docs/adr/0033-performance-regression-gate.md).

## What it measures

`fork_bench` builds a synthetic 2D scene and runs a fixed number of frames
through the engine's real `Application` frame loop — the actual `renderer2d` and
`physics2d` systems, not a stand-in. It reads the frame timings the `Profiler`
already records and reports percentiles:

- **p50** — a typical frame. This is the value the CI gate compares.
- **p95 / p99** — the worst frames. Reported for a human, not gated.

Three named scenes:

| scene           | sprites | static colliders | dynamic bodies |
| --------------- | ------: | ---------------: | -------------: |
| `sprite_heavy`  |    2000 |              500 |            200 |
| `physics_heavy` |     200 |             1000 |           1500 |
| `mixed`         |    1000 |              750 |            750 |

## Running it

The benchmark needs a graphics context (a real GL context, hidden). On a
machine with no display, run it under a virtual one:

```sh
# From the repository root, after a Release build:
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel

# With a display:
./build/bin/fork_bench --scene sprite_heavy

# Headless (Linux), via Xvfb + Mesa's software driver:
xvfb-run -a -s "-screen 0 1280x720x24" ./build/bin/fork_bench --scene sprite_heavy
```

Options:

```
--scene NAME     sprite_heavy (default), physics_heavy, mixed
--sprites N      sprite count (overrides the scene preset)
--bricks N       static collider count
--bodies N       dynamic body count
--frames N       frames to run (default 240)
--warmup N       frames to discard before measuring (default 20)
--json           emit JSON instead of CSV
--out PATH       also write the result block to a file
--verbose        log at debug level
```

Output (CSV):

```
# fork_engine benchmark
# scene=sprite_heavy sprites=2000 bricks=500 bodies=200 frames=300 warmup=60
metric,p50_ms,p95_ms,p99_ms,mean_ms,min_ms,max_ms,samples
frame_total,1.9663,2.0230,2.0281,1.9660,1.8971,2.0291,64
frame_update,0.9974,1.0075,1.0126,0.9972,0.9828,1.0175,64
frame_render,0.1814,0.2252,0.2332,0.1895,0.1641,0.2374,64
```

## The regression gate

`check_regression.py` compares a run against `baseline.json` and exits non-zero
on a regression. Only the p50 is gated, with a 25% tolerance and a 0.5 ms floor,
so the gate survives shared-runner noise while still catching a real slowdown
(the reasoning is in the ADR).

```sh
# Check the current build against the recorded baseline:
python3 benchmarks/check_regression.py \
  --baseline benchmarks/baseline.json \
  --current benchmark.json

# Re-record the baseline after an intentional change:
python3 benchmarks/check_regression.py \
  --baseline benchmarks/baseline.json \
  --current benchmark.json --update
```

The baseline is a number from one machine, so it must be recorded on the machine
that checks it. It is recorded on the CI runner (the `Benchmark regression gate`
job's `benchmark-result` artifact), which is why it reads higher than a local
run: a shared runner is slower and noisier than a developer's machine, and a
baseline taken locally would make the gate fail on the first pull request. CI
then compares like against like — the same runner image, the same Release build —
which is exactly what a regression gate needs.

To re-record it, run the job on `main`, download its `benchmark-result` artifact,
and copy its `benchmark.json` over `benchmarks/baseline.json`. A local `--update`
is for experimenting only; do not commit a locally recorded baseline.

## Sanitizers

The suite also runs under AddressSanitizer + UndefinedBehaviorSanitizer +
LeakSanitizer in a dedicated CI job ([ADR-0035](../docs/adr/0035-ci-quality-gates.md)).
The pure-logic tests (`fork_engine_tests`, `fork_editor_tests`) are checked with
full LeakSanitizer; the graphics tests run with the leak checker off, because the
software GL driver leaks memory no engine code owns. To run it locally:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DFE_ENABLE_ASAN=ON
cmake --build build --parallel
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 ./build/bin/fork_engine_tests
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 ./build/bin/fork_editor_tests
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
  xvfb-run -a -s "-screen 0 640x480x24" ./build/bin/fork_graphics_tests
```
