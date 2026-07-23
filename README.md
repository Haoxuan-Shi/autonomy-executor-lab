# Autonomy Executor Lab

[English](README.md) | [简体中文](README.zh-CN.md)

Autonomy Executor Lab is a deterministic C++17 callback-scheduling and trace-diagnosis laboratory. It answers an engineering question that average-latency dashboards hide: under the same callback workload, which policy preserves control deadlines and input freshness, which samples are superseded, and why did a particular callback fail?

It models a non-preemptive executor with FIFO, fixed-priority, earliest-deadline-first, and latest-only dispatch. Every result retains per-job wait, response, data age, outcome, and a concise diagnosis. It is a portable experiment harness, not a real-time operating system or a proof of schedulability.

Prerequisites are CMake 3.20+, Ninja, and a C++17 compiler. The supplied build scripts and commands deliberately select the Ninja generator.

## Build and run

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
.\build\ael-sim.exe examples\overload.csv --policy latest-only --trace trace.csv
python tools\compare_policies.py .\build\ael-sim.exe examples\overload.csv
```

For multi-config generators, omit `-DCMAKE_BUILD_TYPE` and pass `--config Release` to build, test, and install commands. The CSV header is fixed and documented in [docs/workload-format.md](docs/workload-format.md).

Workloads are read in binary mode on every platform. LF and CRLF records are handled explicitly; embedded or bare carriage returns and byte `0x1A` (CTRL-Z) are rejected, so Windows text-mode EOF behavior cannot silently accept only a valid prefix.

The equivalent clean build entry points are `powershell -File scripts/build.ps1` and `sh scripts/build.sh`.

`cmake --install build --prefix install` installs the library, CLI, headers, and package metadata. The minimal independent consumer in `tests/install_consumer` verifies `find_package(ael CONFIG REQUIRED)` and the exported `ael::ael` target using only the install prefix:

```powershell
cmake -S tests/install_consumer -B consumer-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PWD/install"
cmake --build consumer-build
.\consumer-build\ael-package-consumer.exe
```

Use `--strict` when deadline misses or stale rejections should fail CI. The normal CLI exits successfully after producing a valid analysis even when the analyzed workload is unsafe; malformed inputs always exit with code `2`.

## Engineering contribution

- One trace ties scheduling order to freshness and deadline contracts instead of reporting aggregate utilization alone; a monotonic trace ordinal keeps simultaneous zero-duration outcomes in actual scheduler order.
- Latest-only shedding remains auditable: superseded samples are records, not invisible deletions.
- Counts, makespan, maximum response, and maximum data age are derived from the
  completed trace in one pass, so aggregate JSON cannot omit superseded rows.
- A versioned SHA-256 trace fingerprint binds the event ordinal and every workload, timing, outcome, and diagnosis field so experiment drift is detectable.
- `latest-only` requires sequence numbers to increase strictly per stream in release order; malformed or reordered stream histories fail closed.
- The library API, CLI, install target, adverse tests, and portable CMake project make policy comparisons reproducible without ROS or DDS.

See [docs/architecture.md](docs/architecture.md) for assumptions and non-goals.
