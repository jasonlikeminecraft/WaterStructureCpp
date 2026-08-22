# Conversion optimization workflow

The conversion matrix is generated from the audited registry rather than a
second hand-maintained list:

```text
water_structure_cli formats
water_structure_cli matrix
water_structure_cli matrix --all
```

`matrix` includes file-to-file, structure-to-world, and world-to-structure
edges. Unsupported targets remain explicit capability errors; the project does
not invent writers which are absent from the Go reference implementation.

## Parallel agents

Agents must not share a worktree, `.xmake` state, build directory, output world,
or temporary directory. On Windows, prepare/build an isolated agent with:

```powershell
tools/agent_build.ps1 -Agent nbt -Target water_structure_tests -Mode release
```

The helper creates sibling `WaterStructureCpp-wt`, `WaterStructureCpp-build`,
and `WaterStructureCpp-tmp` roots. Xmake configuration/package bootstrap is
serialized; compilation uses the agent's independent worktree and may run in
parallel. Public headers, the format registry, world adapter, C API, CLI, and
`xmake.lua` remain integration-owner files. The integration owner merges agent
commits serially and performs the canonical Debug/Release builds.

## Memory-limited validation

Build `tools/limited_runner`, then run every large conversion as a separate
process. The runner never allows a limit above 500 MiB:

```text
go build -trimpath -o limited_runner ./tools/limited_runner
limited_runner -limit-mib 500 -repeat 3 -output result.json -- <command> <args>
```

Windows uses a Job Object assigned before the child resumes. Linux/Termux uses
RLIMIT_AS plus cgroup v2 when available, with process-group monitoring as the
fallback. Large manifest runs emit streaming digests and logs to files; they do
not retain the complete manifest in memory.

Acceptance uses three Release runs per edge, median wall time, maximum peak
memory, semantic chunk/NBT hashes, and explicit lossiness metadata. Large
fixtures run serially by default; small-fixture process concurrency is chosen
from available physical memory while each process remains independently capped.
