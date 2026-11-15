# Live demonstration and viva guide

## Five-minute presentation

**1. State the problem.** Under CPU contention, a process can remain alive but take longer to receive enough CPU service. A CPU utilization graph alone cannot show whether responsiveness improved after intervention.

**2. Build and run.**

```bash
make clean
make
make demo
```

Explain the printed probe PID/workload evidence in `results/report.json`. These are real Linux PIDs, not generated display values. All children compete on one permitted CPU; this makes the contention visible on a multi-core laptop too.

**3. Explain each phase as it prints.**

- Baseline: the probe performs a small periodic CPU job with no generated competitors.
- Contention: workers consume CPU, response rises, and repeated windows correlate delay with CPU usage.
- Recovery: terminal lines show actual nice changes for the owned workers.
- Verification: the same probe continues and its before/after measurements are compared.

**4. Show the evidence.**

```bash
python3 -m json.tool results/report.json
head results/samples.csv
```

Read `contention_detected`, `priority_adjustment_applied`, and `recovery_verified` separately. Show `workload_evidence`: nice before/after, actual CPU tick progression, and process states. A completed experiment with `recovery_verified: false` is an honest measured outcome.

**5. Run the control.**

```bash
./build/freezedoctor --no-recovery --report results/control.json --csv results/control.csv
```

No priorities are changed. Regardless of natural fluctuations, it cannot claim recovery was verified. Compare the two runs and explain that shared host load prevents precise repeatability.

## Troubleshooting

| Observation | Explanation / next action |
| --- | --- |
| `g++` or `make` missing | Install `build-essential` on Debian/Ubuntu. |
| Affinity error | Use the default CPU or select one from `/proc/self/status`'s `Cpus_allowed_list`. |
| Cannot open report path | Create its parent directory and check write access. |
| No detection | Increase `--seconds` to 10 or use more workers, and check unrelated host load. Keep unsuccessful measurements. |
| Detection but no verified recovery | Inspect actual nice changes, sample counts, and mean/p95. The host may limit the effect; do not substitute expected values. |
| Workers already have nice 19 | Start from a normal-priority shell; no priority reduction is available from nice 19. |
| `setpriority` denied | Check container/security restrictions. Use an ordinary Linux session; root is not a normal requirement. |

## Viva questions

**Why pin to one CPU?** Otherwise Linux can distribute workers across cores and avoid meaningful contention with the probe.

**Why use a baseline?** Absolute timing varies by machine. A measured baseline gives the threshold a local reference, with a 1 ms absolute margin to avoid tiny fluctuations triggering detection.

**Why both CPU and delay?** High CPU usage by itself is not evidence of poor responsiveness; delay alone may have another cause. Correlation with controlled competitors strengthens the diagnosis.

**Why multiple samples?** Three consecutive half-second windows reject isolated spikes.

**Does nice 19 mean the process stops?** No. It lowers relative priority under normal fair scheduling. The workers remain alive and can still consume idle CPU.

**Why not restore nice 0?** Ordinary users can lower priority but generally cannot raise it again. Workers are disposable and cleaned up; no pre-existing process state is changed.

**Is this a new scheduler?** No. It is a Linux user-space observation and recovery experiment using existing scheduler APIs.

**What are the latency units?** Milliseconds, from scheduled release to completion. The job uses approximately 2 ms of actual process CPU time; elapsed response can be much higher under contention.

**Why no `exec()`?** `fork()` directly launches small C++ worker routines, avoiding an external executable dependency. It still demonstrates genuine process creation/control; the synopsis allows mechanisms such as fork/exec rather than requiring a particular process launch pairing.
