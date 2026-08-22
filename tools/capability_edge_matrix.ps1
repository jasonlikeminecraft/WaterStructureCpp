<#
Validate the registry's directed conversion capability matrix and, optionally,
execute a small fixture against every currently verified writer.

The CLI is the source of truth.  This keeps the check useful for both release
and debug builds without linking a second copy of the registry into a test
binary.  Matrix validation is cheap and does not open any structure files;
fixture execution is opt-in because a complete 36 x 11 edge run can be
expensive on large fixtures.

Examples (PowerShell):

  .\tools\capability_edge_matrix.ps1 `
    -CliPath .\build\windows\x64\release\water_structure_cli.exe

  .\tools\capability_edge_matrix.ps1 `
    -CliPath .\build\windows\x64\release\water_structure_cli.exe `
    -FixturePath .\tmp-cli-test.schem -AssetsPath .\assets `
    -OutputDirectory .\build\edge-results -Execute -ProbeUnsupported

Every edge process can be wrapped by limited_runner.  Pass -LimitedRunner to
enforce the same per-process memory ceiling used by the manifest matrix.
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$CliPath,
    [string]$FixturePath,
    [string]$AssetsPath,
    [string]$OutputDirectory,
    [string]$LimitedRunner,
    [ValidateRange(64, 500)]
    [int]$MemoryLimitMiB = 500,
    [switch]$Execute,
    [switch]$ProbeUnsupported,
    [string]$ReportPath
)

$ErrorActionPreference = 'Stop'

function Resolve-ExistingFile {
    param([Parameter(Mandatory = $true)][string]$Path, [string]$Label = 'file')
    $resolved = Resolve-Path -LiteralPath $Path -ErrorAction Stop
    if (-not (Test-Path -LiteralPath $resolved.Path -PathType Leaf)) {
        throw "$Label is not a regular file: $Path"
    }
    return $resolved.Path
}

$CliPath = Resolve-ExistingFile $CliPath 'CLI'
if ($Execute -or $ProbeUnsupported) {
    if ([string]::IsNullOrWhiteSpace($FixturePath)) {
        throw '-FixturePath is required with -Execute or -ProbeUnsupported'
    }
    $FixturePath = Resolve-ExistingFile $FixturePath 'fixture'
    if ([string]::IsNullOrWhiteSpace($AssetsPath)) {
        throw '-AssetsPath is required with -Execute or -ProbeUnsupported'
    }
    $AssetsPath = Resolve-Path -LiteralPath $AssetsPath -ErrorAction Stop | Select-Object -ExpandProperty Path
    if (-not (Test-Path -LiteralPath $AssetsPath -PathType Container)) {
        throw "assets path is not a directory: $AssetsPath"
    }
    if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
        throw '-OutputDirectory is required with -Execute or -ProbeUnsupported'
    }
    New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
    $OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
}

if (-not [string]::IsNullOrWhiteSpace($LimitedRunner)) {
    $LimitedRunner = Resolve-ExistingFile $LimitedRunner 'limited_runner'
}

function Invoke-Captured {
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [string]$StdoutPath,
        [string]$StderrPath
    )
    # Invocation with an argument array avoids shell re-parsing paths (in
    # particular fixtures containing spaces or non-ASCII characters).
    # Native stderr is represented as an ErrorRecord by Windows PowerShell.
    # Do not let the script-wide Stop policy turn an expected unsupported-edge
    # diagnostic into an exception before we can inspect its exit code.
    $previousErrorAction = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $text = @(& $Executable @Arguments 2>&1)
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorAction
    }
    $stdout = [System.Collections.Generic.List[string]]::new()
    $stderr = [System.Collections.Generic.List[string]]::new()
    foreach ($line in $text) {
        # The merged stream loses the original stream type for native tools;
        # retaining all text is preferable to dropping a diagnostic.  A caller
        # can still inspect the captured files and process exit code.
        [void]$stdout.Add([string]$line)
    }
    if ($StdoutPath) {
        [IO.File]::WriteAllText($StdoutPath, ($stdout -join [Environment]::NewLine) + [Environment]::NewLine)
    }
    if ($StderrPath) {
        [IO.File]::WriteAllText($StderrPath, ($stderr -join [Environment]::NewLine))
    }
    return [pscustomobject]@{
        ExitCode = [int]$exitCode
        Output = ($stdout -join [Environment]::NewLine)
        Stderr = ($stderr -join [Environment]::NewLine)
    }
}

function Invoke-EdgeProcess {
    param(
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$StdoutPath,
        [Parameter(Mandatory = $true)][string]$StderrPath,
        [Parameter(Mandatory = $true)][string]$Name
    )
    if ([string]::IsNullOrWhiteSpace($LimitedRunner)) {
        $run = Invoke-Captured $CliPath $Arguments $StdoutPath $StderrPath
        return [pscustomobject]@{
            ExitCode = $run.ExitCode
            Output = $run.Output
            Termination = 'none'
            MemoryExceeded = $false
            PeakRssMiB = $null
        }
    }

    $runnerReport = "$StdoutPath.limited.json"
    $runnerArguments = @('-limit-mib', [string]$MemoryLimitMiB,
                         '-output', $runnerReport, '--', $CliPath) + $Arguments
    $previousErrorAction = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $runnerOutput = @(& $LimitedRunner @runnerArguments 2>&1)
        $runnerExit = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorAction
    }
    if (-not (Test-Path -LiteralPath $runnerReport -PathType Leaf)) {
        throw "${Name}: limited_runner did not write a report (exit $runnerExit)"
    }
    $report = Get-Content -Raw -Encoding UTF8 -LiteralPath $runnerReport | ConvertFrom-Json
    $run = @($report.runs)[0]
    if ($null -eq $run) { throw "${Name}: limited_runner report has no run" }
    $diagnostic = ''
    if ($run.stderr_path -and (Test-Path -LiteralPath $run.stderr_path -PathType Leaf)) {
        $diagnostic = Get-Content -Raw -Encoding UTF8 -LiteralPath $run.stderr_path
        Copy-Item -LiteralPath $run.stderr_path -Destination $StderrPath -Force
    } else {
        [IO.File]::WriteAllText($StderrPath, '')
    }
    [IO.File]::WriteAllText($StdoutPath, ($runnerOutput -join [Environment]::NewLine) + [Environment]::NewLine)
    return [pscustomobject]@{
        ExitCode = [int]$run.exit_code
        Output = ($runnerOutput -join [Environment]::NewLine)
        Termination = [string]$run.termination
        MemoryExceeded = [bool]$run.memory_limit_exceeded
        PeakRssMiB = [Math]::Round(([double]$run.memory.peak_rss_bytes / 1MB), 2)
        Diagnostic = $diagnostic
    }
}

function Read-MatrixRows {
    param([Parameter(Mandatory = $true)][string[]]$Lines)
    $rows = [System.Collections.Generic.List[object]]::new()
    foreach ($line in $Lines) {
        if ([string]::IsNullOrWhiteSpace($line) -or $line -match '^source,target,direction,') { continue }
        # reason is deliberately the final field.  Joining the tail keeps the
        # checker tolerant of future diagnostics that contain commas.
        $fields = $line -split ',', 7
        if ($fields.Count -lt 7) { throw "malformed matrix row: $line" }
        [void]$rows.Add([pscustomobject]@{
            Source = $fields[0].Trim()
            Target = $fields[1].Trim()
            Direction = $fields[2].Trim()
            Supported = $fields[3].Trim().ToLowerInvariant()
            Streaming = $fields[4].Trim().ToLowerInvariant()
            Lossy = $fields[5].Trim().ToLowerInvariant()
            Reason = $fields[6].Trim()
        })
    }
    return @($rows)
}

function Add-Failure {
    param([System.Collections.Generic.List[string]]$Failures, [string]$Message)
    [void]$Failures.Add($Message)
}

$matrixText = @(& $CliPath matrix --all 2>&1)
$matrixExit = $LASTEXITCODE
if ($matrixExit -ne 0) {
    throw "CLI matrix --all failed with exit code $matrixExit`n$($matrixText -join [Environment]::NewLine)"
}
$rows = Read-MatrixRows $matrixText
$failures = [System.Collections.Generic.List[string]]::new()

$fileRows = @($rows | Where-Object Direction -eq 'file-to-file')
$toWorldRows = @($rows | Where-Object Direction -eq 'structure-to-world')
$fromWorldRows = @($rows | Where-Object Direction -eq 'world-to-structure')
if ($toWorldRows.Count -eq 0 -or $fromWorldRows.Count -eq 0) {
    Add-Failure $failures 'matrix is missing one or more world directions'
}

$formatNames = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
foreach ($row in $toWorldRows) { [void]$formatNames.Add($row.Source) }
foreach ($row in $fromWorldRows) { [void]$formatNames.Add($row.Target) }
if ($formatNames.Count -eq 0) { Add-Failure $failures 'matrix contains no format names' }
$expectedCount = $formatNames.Count

if ($fileRows.Count -ne $expectedCount * $expectedCount) {
    Add-Failure $failures "file-to-file row count $($fileRows.Count) != $expectedCount x $expectedCount"
}
if ($toWorldRows.Count -ne $expectedCount) {
    Add-Failure $failures "structure-to-world row count $($toWorldRows.Count) != $expectedCount"
}
if ($fromWorldRows.Count -ne $expectedCount) {
    Add-Failure $failures "world-to-structure row count $($fromWorldRows.Count) != $expectedCount"
}

$reader = @{}
$writer = @{}
foreach ($row in $toWorldRows) {
    if ($row.Target -ne 'MCWorld') { Add-Failure $failures "unexpected structure-to-world target: $($row.Target)" }
    if ($reader.ContainsKey($row.Source)) { Add-Failure $failures "duplicate reader capability: $($row.Source)" }
    $reader[$row.Source] = $row.Supported -eq 'yes'
    if ($row.Supported -notin @('yes', 'no')) { Add-Failure $failures "invalid supported value: $($row.Source)" }
    if ($row.Supported -eq 'no' -and [string]::IsNullOrWhiteSpace($row.Reason)) {
        Add-Failure $failures "missing reason for structure-to-world failure: $($row.Source)"
    }
}
foreach ($row in $fromWorldRows) {
    if ($row.Source -ne 'MCWorld') { Add-Failure $failures "unexpected world-to-structure source: $($row.Source)" }
    if ($writer.ContainsKey($row.Target)) { Add-Failure $failures "duplicate writer capability: $($row.Target)" }
    $writer[$row.Target] = $row.Supported -eq 'yes'
    if ($row.Supported -notin @('yes', 'no')) { Add-Failure $failures "invalid supported value: $($row.Target)" }
    if ($row.Supported -eq 'no' -and [string]::IsNullOrWhiteSpace($row.Reason)) {
        Add-Failure $failures "missing reason for world-to-structure failure: $($row.Target)"
    }
}

$seenEdges = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
foreach ($row in $fileRows) {
    if (-not $formatNames.Contains($row.Source) -or -not $formatNames.Contains($row.Target)) {
        Add-Failure $failures "unknown format in file edge: $($row.Source) -> $($row.Target)"
        continue
    }
    $key = "$($row.Source)`n$($row.Target)"
    if (-not $seenEdges.Add($key)) { Add-Failure $failures "duplicate file edge: $($row.Source) -> $($row.Target)" }
    $expected = $reader[$row.Source] -and $writer[$row.Target]
    $actual = $row.Supported -eq 'yes'
    if ($actual -ne $expected) {
        Add-Failure $failures "capability mismatch $($row.Source) -> $($row.Target): expected $expected, got $actual"
        continue
    }
    if ($actual) {
        if (-not [string]::IsNullOrWhiteSpace($row.Reason)) {
            Add-Failure $failures "supported edge has a failure reason: $($row.Source) -> $($row.Target)"
        }
    } else {
        $hasReaderFailure = -not $reader[$row.Source]
        $hasWriterFailure = -not $writer[$row.Target]
        if ($hasReaderFailure -and $row.Reason -notmatch 'no verified reader') {
            Add-Failure $failures "missing reader reason $($row.Source) -> $($row.Target): $($row.Reason)"
        } elseif (-not $hasReaderFailure -and $hasWriterFailure -and
                  $row.Reason -notmatch 'no verified writer') {
            Add-Failure $failures "missing writer reason $($row.Source) -> $($row.Target): $($row.Reason)"
        } elseif (-not $hasReaderFailure -and -not $hasWriterFailure) {
            Add-Failure $failures "unsupported edge has neither capability cause: $($row.Source) -> $($row.Target)"
        }
    }
}
if ($seenEdges.Count -ne $expectedCount * $expectedCount) {
    Add-Failure $failures "unique file edges $($seenEdges.Count) != $expectedCount x $expectedCount"
}

$executed = [System.Collections.Generic.List[object]]::new()
if ($Execute -or $ProbeUnsupported) {
    # Parse the human-readable formats table only to obtain a target extension;
    # capability itself is taken exclusively from matrix rows above.
    $formatLines = @(& $CliPath formats --writers-only 2>&1)
    $extensions = @{}
    foreach ($line in $formatLines) {
        if ($line -match '^\s*(\S+)\s+(yes|pending)\s+(yes|pending)\s+(yes|pending)\s+(yes|pending)\s+(.+?)\s*$') {
            $extensions[$Matches[1]] = (($Matches[5] -split ',')[0]).Trim()
        }
    }
    $writerNames = @($writer.Keys | Where-Object { $writer[$_] }) | Sort-Object
    foreach ($target in $writerNames) {
        if (-not $Execute) { break }
        $extension = if ($extensions.ContainsKey($target)) { $extensions[$target] } else { '.out' }
        if (-not $extension.StartsWith('.')) { $extension = ".$extension" }
        $safeTarget = $target -replace '[^A-Za-z0-9_.-]', '_'
        $output = Join-Path $OutputDirectory "edge-$safeTarget$extension"
        $stdoutPath = Join-Path $OutputDirectory "edge-$safeTarget.stdout.txt"
        $stderrPath = Join-Path $OutputDirectory "edge-$safeTarget.stderr.txt"
        $args = @('convert', $FixturePath, $output, '--format', $target,
                  '--assets', $AssetsPath, '--quiet')
        $run = Invoke-EdgeProcess $args $stdoutPath $stderrPath "edge $target"
        $ok = $run.ExitCode -eq 0 -and (Test-Path -LiteralPath $output -PathType Leaf)
        if ($run.MemoryExceeded) { $ok = $false }
        [void]$executed.Add([pscustomobject]@{
            Source = $FixturePath; Target = $target; Supported = $true; Passed = $ok
            ExitCode = $run.ExitCode; Output = $output; PeakRssMiB = $run.PeakRssMiB
            Termination = $run.Termination
        })
        if (-not $ok) { Add-Failure $failures "valid edge execution failed: fixture -> $target (exit $($run.ExitCode))" }
    }

    if ($ProbeUnsupported) {
        $unsupported = @($writer.Keys | Where-Object { -not $writer[$_] } | Sort-Object | Select-Object -First 1)
        if ($unsupported.Count -eq 0) {
            Add-Failure $failures 'cannot probe unsupported writer: every format is marked writable'
        } else {
            $target = [string]$unsupported[0]
            $output = Join-Path $OutputDirectory "edge-unsupported-$target.out"
            $stdoutPath = Join-Path $OutputDirectory "edge-unsupported-$target.stdout.txt"
            $stderrPath = Join-Path $OutputDirectory "edge-unsupported-$target.stderr.txt"
            $run = Invoke-EdgeProcess @('convert', $FixturePath, $output, '--format', $target,
                                        '--assets', $AssetsPath, '--quiet') `
                $stdoutPath $stderrPath "unsupported edge $target"
            $diagnostic = "$($run.Output)`n$(Get-Content -Raw -Encoding UTF8 -LiteralPath $stderrPath -ErrorAction SilentlyContinue)"
            $ok = $run.ExitCode -ne 0 -and $diagnostic -match '(?i)capability error|verified writer|writer'
            [void]$executed.Add([pscustomobject]@{
                Source = $FixturePath; Target = $target; Supported = $false; Passed = $ok
                ExitCode = $run.ExitCode; Output = $output; PeakRssMiB = $run.PeakRssMiB
                Termination = $run.Termination
            })
            if (-not $ok) { Add-Failure $failures "unsupported writer probe did not return a capability error: $target (exit $($run.ExitCode))" }
        }
    }
}

$summary = [ordered]@{
    formats = $expectedCount
    file_to_file_edges = $fileRows.Count
    valid_file_to_file_edges = @($fileRows | Where-Object Supported -eq 'yes').Count
    reader_count = @($reader.Keys | Where-Object { $reader[$_] }).Count
    writer_count = @($writer.Keys | Where-Object { $writer[$_] }).Count
    executed_edges = @($executed).Count
    executed_passed = @($executed | Where-Object Passed).Count
    failures = @($failures)
}
if (-not [string]::IsNullOrWhiteSpace($ReportPath)) {
    $reportParent = Split-Path -Parent $ReportPath
    if ($reportParent) { New-Item -ItemType Directory -Force -Path $reportParent | Out-Null }
    # Resolve-Path returns $null for a new report file.  Normalize the target
    # directly so first-run reports work as well as overwriting an existing
    # report.
    $reportFullPath = [IO.Path]::GetFullPath($ReportPath)
    [IO.File]::WriteAllText($reportFullPath,
        (($summary | ConvertTo-Json -Depth 8) + [Environment]::NewLine),
        [Text.UTF8Encoding]::new($false))
}

Write-Output ("capability matrix: formats={0} file_edges={1} valid={2} readers={3} writers={4}" -f
    $summary.formats, $summary.file_to_file_edges, $summary.valid_file_to_file_edges,
    $summary.reader_count, $summary.writer_count)
if ($executed.Count -gt 0) {
    Write-Output ("fixture edges: passed={0} total={1}" -f $summary.executed_passed, $summary.executed_edges)
}
if ($failures.Count -gt 0) {
    foreach ($failure in $failures) { Write-Output "FAIL: $failure" }
    exit 1
}
Write-Output 'PASS: capability matrix and requested edge probes are consistent.'
exit 0
