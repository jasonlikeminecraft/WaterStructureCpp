param(
    [Parameter(Mandatory = $true)][string]$FixturePath,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [string]$Converter,
    [string]$CppManifest,
    [string]$LimitedRunner,
    [string]$AssetsPath,
    [ValidateRange(64, 500)][int]$MemoryLimitMiB = 500,
    [int]$Threads = 1,
    [switch]$KeepOutput
)

# For every verified writer B, execute and semantically compare both cycles:
#
#   A -> B -> A
#   B -> A -> B
#
# A must itself have a verified writer. Use a deliberately small fixture so
# protocol loss declarations remain visible in the capability matrix while the
# writer smoke/semantic check stays deterministic. Every native child is placed
# below the same hard 500 MiB process limit.

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$diffTool = Join-Path $PSScriptRoot 'diff_manifest.ps1'

function Resolve-Tool {
    param([string]$Configured, [string[]]$Candidates, [string]$Name)
    if (-not [string]::IsNullOrWhiteSpace($Configured)) {
        if (-not (Test-Path -LiteralPath $Configured -PathType Leaf)) {
            throw "$Name not found: $Configured"
        }
        return (Resolve-Path -LiteralPath $Configured).Path
    }
    foreach ($candidate in $Candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    throw "$Name not found; pass -$Name explicitly"
}

$Converter = Resolve-Tool $Converter @(
    (Join-Path $projectRoot 'build\windows\x64\release\water_structure_cli.exe'),
    (Join-Path $projectRoot 'build\windows\x64\debug\water_structure_cli.exe')
) 'Converter'
$CppManifest = Resolve-Tool $CppManifest @(
    (Join-Path $projectRoot 'build\windows\x64\release\cpp_manifest.exe'),
    (Join-Path $projectRoot 'build\windows\x64\debug\cpp_manifest.exe')
) 'CppManifest'
$LimitedRunner = Resolve-Tool $LimitedRunner @(
    (Join-Path $PSScriptRoot 'limited_runner\limited_runner.exe'),
    (Join-Path $PSScriptRoot 'limited_runner\limited_runner')
) 'LimitedRunner'
if (-not (Test-Path -LiteralPath $diffTool -PathType Leaf)) {
    throw "diff tool not found: $diffTool"
}
if ([string]::IsNullOrWhiteSpace($AssetsPath)) {
    $AssetsPath = Join-Path $projectRoot 'assets'
}
$AssetsPath = (Resolve-Path -LiteralPath $AssetsPath -ErrorAction Stop).Path
if (-not (Test-Path -LiteralPath (Join-Path $AssetsPath 'block_mappings_v1.json') -PathType Leaf)) {
    throw "runtime assets not found: $AssetsPath"
}
$fixture = (Resolve-Path -LiteralPath $FixturePath -ErrorAction Stop).Path
if (-not (Test-Path -LiteralPath $fixture -PathType Leaf)) {
    throw "fixture is not a regular file: $fixture"
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$outputRoot = (Resolve-Path -LiteralPath $OutputDirectory).Path

function Invoke-Limited {
    param(
        [string]$Name,
        [string]$Executable,
        [string[]]$Arguments,
        [string]$LogDirectory
    )
    New-Item -ItemType Directory -Force -Path $LogDirectory | Out-Null
    $safeName = $Name -replace '[^A-Za-z0-9_.-]', '_'
    $reportPath = Join-Path $LogDirectory "$safeName.limited.json"
    $runnerArguments = @(
        '-limit-mib', [string]$MemoryLimitMiB,
        '-output', $reportPath,
        '--', $Executable
    ) + $Arguments
    & $LimitedRunner @runnerArguments | Out-Null
    $runnerExit = $LASTEXITCODE
    if (-not (Test-Path -LiteralPath $reportPath -PathType Leaf)) {
        throw "${Name}: limited_runner did not produce a report (exit $runnerExit)"
    }
    $report = Get-Content -Raw -Encoding UTF8 -LiteralPath $reportPath | ConvertFrom-Json
    $run = @($report.runs)[0]
    if ($null -eq $run) { throw "${Name}: runner report contains no run" }
    if ([bool]$run.memory_limit_exceeded) {
        throw "${Name}: process exceeded ${MemoryLimitMiB} MiB"
    }
    if ([string]$run.termination -ne 'exited') {
        throw "${Name}: process termination=$($run.termination)"
    }
    if ([int]$run.exit_code -ne 0) {
        $details = if ($run.stderr_path -and (Test-Path -LiteralPath $run.stderr_path -PathType Leaf)) {
            (Get-Content -Raw -Encoding UTF8 -LiteralPath $run.stderr_path).Trim()
        } else { '' }
        throw "${Name}: exit code $($run.exit_code): $details"
    }
    return [pscustomobject]@{
        Run = $run
        StdoutPath = [string]$run.stdout_path
        PeakRssMiB = [Math]::Round(([double]$run.memory.peak_rss_bytes / 1MB), 2)
        DurationMs = [double]$run.duration_ms
    }
}

function Get-ExtensionMap {
    $result = @{}
    $lines = @(& $Converter formats --writers-only 2>&1)
    if ($LASTEXITCODE -ne 0) { throw 'CLI formats --writers-only failed' }
    foreach ($line in $lines) {
        if ($line -match '^\s*(\S+)\s+(yes|pending)\s+(yes|pending)\s+(yes|pending)\s+(yes|pending)\s+(.+?)\s*$') {
            $extension = (($Matches[6] -split ',')[0]).Trim()
            if (-not $extension.StartsWith('.')) { $extension = ".$extension" }
            $result[$Matches[1]] = $extension
        }
    }
    return $result
}

function Compare-Manifests {
    param([string]$Expected, [string]$Actual, [string]$LogPath)
    & $diffTool -GoManifest $Expected -CppManifest $Actual `
        -LimitedRunner $LimitedRunner -MemoryLimitMiB $MemoryLimitMiB -IgnoreInputSha `
        1> $LogPath 2>&1
    if ($LASTEXITCODE -ne 0) {
        $details = if (Test-Path -LiteralPath $LogPath -PathType Leaf) {
            (Get-Content -Raw -Encoding UTF8 -LiteralPath $LogPath).Trim()
        } else { '' }
        throw "semantic manifest mismatch: $details"
    }
}

$inspection = Invoke-Limited 'inspect-source' $Converter @(
    'inspect', $fixture, '--assets', $AssetsPath
) $outputRoot
if (-not (Test-Path -LiteralPath $inspection.StdoutPath -PathType Leaf)) {
    throw 'source inspection stdout is missing'
}
$inspectText = Get-Content -Raw -Encoding UTF8 -LiteralPath $inspection.StdoutPath
if ($inspectText -notmatch '(?m)^format:\s*(\S+)\s*$') {
    throw 'cannot parse source format from CLI inspect output'
}
$sourceFormat = $Matches[1]

$matrixLines = @(& $Converter matrix --all 2>&1)
if ($LASTEXITCODE -ne 0) { throw 'CLI matrix --all failed' }
$matrix = @($matrixLines | ConvertFrom-Csv)
$writers = @($matrix | Where-Object {
    $_.direction -eq 'world-to-structure' -and $_.supported -eq 'yes'
} | ForEach-Object target | Sort-Object -Unique)
if ($writers.Count -eq 0) { throw 'capability matrix contains no verified writers' }
if ($sourceFormat -notin $writers) {
    throw "source format $sourceFormat has no verified writer; A -> B -> A cannot be tested"
}
$extensions = Get-ExtensionMap
if (-not $extensions.ContainsKey($sourceFormat)) {
    throw "cannot determine extension for source format $sourceFormat"
}

$sourceManifest = Join-Path $outputRoot 'source.manifest.json'
$sourceManifestRun = Invoke-Limited 'manifest-source' $CppManifest @(
    $fixture, $sourceManifest
) $outputRoot

$status = [ordered]@{}
$passed = 0
$failed = 0
foreach ($target in $writers) {
    $safeTarget = $target -replace '[^A-Za-z0-9_.-]', '_'
    $caseOutput = Join-Path $outputRoot $safeTarget
    New-Item -ItemType Directory -Force -Path $caseOutput | Out-Null
    try {
        if (-not $extensions.ContainsKey($target)) {
            throw "cannot determine extension for target $target"
        }
        $bFirst = Join-Path $caseOutput "first-B$($extensions[$target])"
        $aReturned = Join-Path $caseOutput "returned-A$($extensions[$sourceFormat])"
        $bReturned = Join-Path $caseOutput "returned-B$($extensions[$target])"
        $bFirstManifest = Join-Path $caseOutput 'first-B.manifest.json'
        $aReturnedManifest = Join-Path $caseOutput 'returned-A.manifest.json'
        $bReturnedManifest = Join-Path $caseOutput 'returned-B.manifest.json'
        $runs = [System.Collections.Generic.List[object]]::new()

        Write-Output "RUN  $sourceFormat -> $target -> $sourceFormat"
        [void]$runs.Add((Invoke-Limited 'convert-A-B' $Converter @(
            'convert', $fixture, $bFirst, '--format', $target,
            '--threads', [string]([Math]::Max(1, $Threads)),
            '--assets', $AssetsPath, '--quiet'
        ) $caseOutput))
        [void]$runs.Add((Invoke-Limited 'manifest-B-first' $CppManifest @(
            $bFirst, $bFirstManifest
        ) $caseOutput))
        [void]$runs.Add((Invoke-Limited 'convert-B-A' $Converter @(
            'convert', $bFirst, $aReturned, '--format', $sourceFormat,
            '--threads', [string]([Math]::Max(1, $Threads)),
            '--assets', $AssetsPath, '--quiet'
        ) $caseOutput))
        [void]$runs.Add((Invoke-Limited 'manifest-A-returned' $CppManifest @(
            $aReturned, $aReturnedManifest
        ) $caseOutput))
        Compare-Manifests $sourceManifest $aReturnedManifest (Join-Path $caseOutput 'A-B-A.diff.log')

        Write-Output "RUN  $target -> $sourceFormat -> $target"
        [void]$runs.Add((Invoke-Limited 'convert-A-B-returned' $Converter @(
            'convert', $aReturned, $bReturned, '--format', $target,
            '--threads', [string]([Math]::Max(1, $Threads)),
            '--assets', $AssetsPath, '--quiet'
        ) $caseOutput))
        [void]$runs.Add((Invoke-Limited 'manifest-B-returned' $CppManifest @(
            $bReturned, $bReturnedManifest
        ) $caseOutput))
        Compare-Manifests $bFirstManifest $bReturnedManifest (Join-Path $caseOutput 'B-A-B.diff.log')

        $status[$target] = [ordered]@{
            result = 'passed'
            source_format = $sourceFormat
            target_format = $target
            max_peak_rss_mib = ($runs | ForEach-Object PeakRssMiB | Measure-Object -Maximum).Maximum
            total_child_duration_ms = ($runs | ForEach-Object DurationMs | Measure-Object -Sum).Sum
            completed_at = [DateTimeOffset]::Now.ToString('o')
        }
        $passed++
        Write-Output "PASS $target"
        if (-not $KeepOutput) {
            Remove-Item -LiteralPath $bFirst, $aReturned, $bReturned,
                $bFirstManifest, $aReturnedManifest, $bReturnedManifest `
                -Force -ErrorAction SilentlyContinue
        }
    } catch {
        $status[$target] = [ordered]@{
            result = 'failed'
            source_format = $sourceFormat
            target_format = $target
            error = $_.Exception.Message
            completed_at = [DateTimeOffset]::Now.ToString('o')
        }
        $failed++
        Write-Output "FAIL $target $($_.Exception.Message)"
    }
    [IO.File]::WriteAllText(
        (Join-Path $outputRoot 'status.json'),
        (($status | ConvertTo-Json -Depth 10) + [Environment]::NewLine),
        [Text.UTF8Encoding]::new($false))
}

$summary = [ordered]@{
    source_format = $sourceFormat
    writer_count = $writers.Count
    passed = $passed
    failed = $failed
    memory_limit_mib = $MemoryLimitMiB
    source_inspect_peak_rss_mib = $inspection.PeakRssMiB
    source_manifest_peak_rss_mib = $sourceManifestRun.PeakRssMiB
}
[IO.File]::WriteAllText(
    (Join-Path $outputRoot 'summary.json'),
    (($summary | ConvertTo-Json -Depth 8) + [Environment]::NewLine),
    [Text.UTF8Encoding]::new($false))
Write-Output "SUMMARY passed=$passed failed=$failed total=$($writers.Count)"
if ($failed -ne 0) { exit 1 }
exit 0
