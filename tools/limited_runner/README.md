# limited_runner

`limited_runner` runs a conversion or benchmark in a separate process with the
strongest memory enforcement available on the host and streams child output to
disk. It is intentionally independent from the C++ build, so Go and C++ oracle
tools can be measured with the same wrapper.

## Build

```text
go build -trimpath -o limited_runner ./tools/limited_runner
```

On Windows the child is created suspended, assigned to a Job Object with both
per-process and process-tree memory limits plus `KILL_ON_JOB_CLOSE`, then resumed.
On Linux/Termux the helper waits on an inherited startup gate while the parent
places it in a private cgroup v2 (`memory.max`), so target-command allocations
cannot race the attach. `RLIMIT_AS` remains an additional per-process guard. If
cgroup v2 is unavailable, the runner keeps the rlimit, samples `/proc`, and
recursively terminates the process group and descendants; it never claims that
the cgroup hard limit is active. Cgroup directories are drained and removed after every
run.

Darwin and the BSDs do not depend on a normally absent `/proc`. They stream the
native `ps` process table and retain only PID/PPID/process-group, RSS, VSZ and
cumulative CPU fields for the selected descendant tree. This supplies bounded
peak monitoring and recursive termination tracking, but the per-process rlimit
plus a sampled tree is not an atomic process-tree memory ceiling. Darwin,
FreeBSD, NetBSD and DragonFly use `RLIMIT_AS`; OpenBSD, which does not expose
`RLIMIT_AS`, uses its closest enforceable guard, `RLIMIT_DATA`.

## Usage

```text
limited_runner -output result.json -- water_structure_bench input.schem
limited_runner -repeat 3 -warmup 1 -cwd build -- ./cpp_manifest input.schem result.json
limited_runner -limit-mib 500 -timeout 30m -- go_manifest input.schem go.json
```

The default and maximum limit is **500 MiB**.  `stdout` and `stderr` are never
buffered in the runner; they are written to `<output>.stdout.txt` and
`<output>.stderr.txt` (with `.run-NNN` suffixes for repeated runs).  Use
`-stdout-dir` to put those files on a scratch volume.  Numeric `*_ms`, `*_mib`,
`*_bytes`, `*_count`, and `checksum` key/value lines from stdout are included
in each result as phase metrics.

The JSON report contains per-run wall time, exit/signal/termination reason,
working-set/RSS/private/virtual peaks, output sizes, backend used, cumulative
user/system CPU time and CPU utilization (when the platform exposes reliable
counters), and an aggregate median/max summary. Every run explicitly records
`hard_tree_limit` and `enforcement`: Windows Job Objects and successfully
created cgroup-v2 groups report `true`; an rlimit plus `/proc`/`ps` monitoring
reports `false`. BSD `ps` exposes cumulative total CPU but no portable
user/system split, so `cpu_total_seconds` is populated while
`cpu_split_available` remains false. Zero is never used to imply that an
unavailable CPU counter was measured. Exit status is 0 only when every
requested run exits successfully; 125 denotes a memory-limit termination.
