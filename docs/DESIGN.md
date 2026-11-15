# Design and synopsis mapping

The supplied synopsis is treated as a project specification. Its numerical examples are illustrative; they are not implementation inputs or claimed measurements. The original PDF and student identifiers are not redistributed in this public repository.

| Synopsis module | Implementation | OS concept |
| --- | --- | --- |
| Controlled workload | `spawn()` forks CPU-bound workers and a periodic CPU-service probe | Process creation; affinity; CPU contention |
| Observation | `observe()` parses live `/proc/PID/stat`, reads `schedstat`, and derives CPU utilization from tick deltas | Process state, CPU accounting, scheduler wait |
| Analyzer | `phase()` correlates per-window response means with measured workload CPU; requires three consecutive abnormal windows | Scheduling delay and persistent evidence |
| Recovery | `main()` calls and checks `getpriority()` / `setpriority()` on child workers | Nice weights and scheduling priority |
| Verification | `stats()` computes mean, nearest-rank p95, maximum, and sample count using identical metrics | Quantitative before/after experiment |
| Reporting | Live terminal output, sample CSV, structured JSON | Auditable experimental evidence |

## Data and execution

The probe writes measurements to a fixed-size `MAP_SHARED` anonymous mapping. It publishes each complete sample using a release store on a lock-free C11 atomic counter; the parent uses acquire loads. This is a single-writer append-only buffer. At most 18,000 periodic releases occur in a 3 × 60-second run, below the 131,072-sample capacity. No sample is read before publication.

Probe and workers share one CPU selected from `sched_getaffinity()`. This deliberately creates contention even on a multi-core machine. The controller is not pinned; it may still execute on the test CPU if the scheduler chooses. CPU tick deltas divided by actual elapsed monotonic time give utilization as a percentage of one logical CPU. Small accounting errors are expected from tick granularity.

Each 500 ms window reports mean/p95 response and total CPU usage of the generated workload. The CSV repeats that window's aggregate CPU figure on each probe measurement; it is not an instantaneous per-probe CPU reading. Phase summaries use exactly the published samples written to that phase's CSV rows. Tiny transition gaps can omit samples between windows/phases. Initial contention samples and initial verification samples can overlap phase transitions slightly because work already released may finish after the boundary.

The baseline-relative threshold is calculated once, before introducing workers. Detection is evaluated during contention, with intervention at the end of that fixed observation period; it is not an immediate reaction to the first detection. This preserves a complete before interval for comparison. The report includes the maximum streak, selected contributor PID/CPU, monotonic intervention timestamp (zero if none), per-worker before/after nice/state/CPU ticks, and optional cumulative probe scheduler wait. `schedstat` counters can be zero or unavailable; diagnosis does not depend on their availability.

No third-party process is scanned or modified. The experiment's known processes provide the candidate set. A particular highest-CPU worker is evidence of a contributor, not proof it is the sole cause of all observed delay. Lowering every known competitor's priority provides a controlled collective intervention.

## Honest interpretation

- A successful `setpriority()` call alone does not establish recovery. The same measured response metric must improve, with both mean and p95 criteria.
- CPU contention generally degrades responsiveness rather than causing literal unbounded starvation under the fair scheduler.
- Per-run p95 and a 10% mean difference are pragmatic demonstration criteria, not a statistical causal proof. Repeat independent runs for academic evaluation.
- Sleep wake-up delay is not pure run-queue wait. Timer resolution, cgroups, virtualization, scheduling, controller activity, and unrelated system load contribute. CPU-service timing separately uses `CLOCK_PROCESS_CPUTIME_ID`.
- Absolute values vary by Linux scheduler/version (including CFS/EEVDF behavior), nice inheritance, background load, and CPU quotas. No recovery claim is forced when the evidence is insufficient.
- The workers continue doing CPU work after intervention. Nice changes CPU allocation relative to runnable peers, not a hard CPU cap. Workload CPU can remain high while the probe becomes more responsive.
- The no-recovery run is a counterfactual control. It cannot report recovery verified, even if latency improves naturally.
- CI tests validate lifecycle, accounting, report consistency, control behavior, and argument handling. They intentionally do not require particular latency numbers on shared runners.

## Evaluation plan

Run `make demo` at least three times and save reports under distinct paths. Then run an equivalent `--no-recovery` experiment. Compare baseline, before, and after means/p95, samples, worker CPU evidence, and actual nice changes. Record unsuccessful runs as well as successful ones. For longer experiments use `--seconds 10`. Preserve the same CPU, worker count, and environment between comparisons.

## References

- Linux man-pages: `fork(2)`, `waitpid(2)`, `sched_setaffinity(2)`, `getpriority(2)`, `setpriority(2)`, `clock_gettime(2)`, `clock_nanosleep(2)`, `proc_pid_stat(5)`, `prctl(2)`.
- Linux kernel scheduler documentation: https://docs.kernel.org/scheduler/
- Linux man-pages project: https://www.kernel.org/doc/man-pages/
