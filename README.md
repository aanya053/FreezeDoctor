# FreezeDoctor

**Detect CPU scheduling delay. Identify contention. Apply a priority adjustment. Measure recovery.**

A working **C++17** Linux Operating Systems project based on the supplied *FreezeDoctor OS-Based Detection and Recovery of Process Scheduling Starvation in Linux* synopsis. It uses real processes and Linux APIs, not simulated metrics. No root access or third-party libraries are needed.

```text
Latency probe + CPU workers (one allowed CPU)
               ↓
/proc observations + monotonic timing
               ↓
Persistent delay AND measured workload CPU usage
               ↓
setpriority(): lower owned test workers' priority
               ↓
Same response measurement → before/after verification
```

## Run the working model

On Linux (including WSL2 or a Linux VM), install G++, Make, and Python 3 for tests. On Debian/Ubuntu:

```bash
sudo apt-get update
sudo apt-get install build-essential python3 git
git clone https://github.com/asgt2408/freezedoctor.git
cd freezedoctor
make
make demo
```

The default demonstration takes about **12 seconds**. It prints live samples and writes `results/report.json` and `results/samples.csv`. Run it as a normal user. The `sudo` above is only for installing packages.

```bash
make test                         # real-process integration tests
./build/freezedoctor --help
./build/freezedoctor --seconds 6 --workers 4
./build/freezedoctor --no-recovery # counterfactual: same experiment, no renicing
```

`--seconds` is the duration of **each** of three phases (2–60 seconds); `--workers` accepts 1–16. `--cpu N` selects a CPU from the current affinity mask. Defaults select the first permitted CPU. `--report PATH` and `--csv PATH` choose output files; their parent directories must exist. Outputs overwrite existing files at those paths. Press Ctrl+C to stop and reap the experiment's children.

## C++ lab implementation

The program is in `src/freezedoctor.cpp` and compiles with `g++ -std=c++17`. It uses `std::array` for fixed process/sample storage, `std::vector` and `std::sort` for statistics, C++ file/string streams for `/proc` parsing, `std::atomic` for publishing probe samples, and `std::unique_ptr` with a custom deleter for shared-memory cleanup. Linux scheduling and process functions such as `fork()`, `setpriority()`, and `sched_setaffinity()` are called directly from C++.

To compile directly for your lab:

```bash
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic src/freezedoctor.cpp -o freezedoctor
./freezedoctor
```

## What it demonstrates

1. **A — Baseline:** a latency probe runs without FreezeDoctor's CPU workers.
2. **B — Contention:** four CPU-bound processes compete with the probe on the same CPU. The parent samples Linux `/proc/<pid>/stat` and `/proc/<pid>/schedstat`.
3. **C — Recovery and verification:** after persistent correlated delay is detected, `setpriority()` changes all owned workers to nice 19. The probe continues unchanged; measured mean and p95 response times determine the result.

The probe performs approximately **2 ms of CPU work** per release, normally every **10 ms**. Its response metric is wall time from scheduled release to completion. This includes wake-up delay, CPU service, and preemption while doing work. The raw CSV also records wake-up delay separately. Missed releases are skipped so backlog does not grow indefinitely.

The detector requires **three consecutive 500 ms windows** whose mean response exceeds `max(1.5 × baseline mean, baseline mean + 1 ms)` while workers consume at least **30% of one CPU**. The highest-CPU worker in the most recent qualifying window is recorded as an identified contributor. All generated workers are known competing workloads, so recovery adjusts each of them.

`RECOVERY VERIFIED` requires detection, successful priority reduction of every worker, at least 10 before and after samples, a **mean response reduction greater than 10%**, and a **lower p95**. Otherwise the program reports `RECOVERY NOT VERIFIED`. Measurements are never replaced by expected values.

## Evidence and project documentation

- [Demonstration script and viva guide](docs/DEMONSTRATION.md)
- [Design, OS concepts, and limitations](docs/DESIGN.md)
- [Actual measured example report](docs/example-report.json)
- [Raw samples from that example](docs/example-samples.csv)
- [Full example terminal session](docs/example-run.txt)

The committed example is a single run on the development Linux host. Rerunning produces new host-dependent numbers. This project demonstrates **excessive scheduling delay under CPU contention**, not proof of indefinite starvation; Linux's fair scheduler normally continues giving runnable processes CPU service.

## Process boundaries and cleanup

Only processes forked by this invocation are priority targets. No external PID is accepted. Processes are not killed as a recovery technique: termination is used only to clean up this experiment's probe and workers when it ends or is interrupted. `waitpid()` reaps every child. Children also request `PR_SET_PDEATHSIG` so an unexpectedly terminated controller does not leave CPU workers running. No kernel changes, real-time scheduling policy, or privileged priority boosts are used. Raising nice from 0 to 19 is ordinarily permitted for an owned process; resetting it to 0 may require privilege, so the disposable workers are reaped instead.

Exit codes: `0` means the experiment completed, **not necessarily verified recovery**; read the JSON boolean `recovery_verified` for that. `2` means invalid arguments, `1` means an operational error, and `130` means interrupted. An interrupted baseline may leave incomplete output files.

## Requirements and portability

Linux with `/proc`, `sched_setaffinity`, POSIX clocks, and lock-free shared integer atomics; G++ with C++17 support; Make. Python 3 is needed only for tests. Native macOS/Windows are unsupported; use Linux/WSL2. Kernel policies, containers, CPU quotas, existing load, inherited nice values, and scheduler configuration can limit reproducibility. The tool reports the measured outcome even if recovery does not improve latency.
