param(
    [Parameter(Mandatory = $true)][string]$CaseFile,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [string]$OracleFixtureDirectory,
    [string]$Converter,
    [string]$WorldCompare,
    [string]$LimitedRunner,
    [ValidateRange(64, 500)][int]$MemoryLimitMiB = 500,
    [int]$Threads = 1,
    [int]$StartChunkX = 0,
    [int]$StartSubChunkY = -4,
    [int]$StartChunkZ = 0,
    [switch]$RetryFailed,
    [switch]$Force,
    [switch]$KeepOutput
)

# Runs every reader fixture through both Bedrock world sinks and compares the
# resulting worlds semantically. Each inspect/conversion process is wrapped by
# limited_runner; no process may request more than the 500 MiB acceptance cap.

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$worldExportTool = Join-Path $PSScriptRoot 'test_world_export.ps1'
$assetsDirectory = Join-Path $projectRoot 'assets'

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
$WorldCompare = Resolve-Tool $WorldCompare @(
    (Join-Path $projectRoot 'build\windows\x64\release\world_compare.exe'),
    (Join-Path $projectRoot 'build\windows\x64\debug\world_compare.exe')
) 'WorldCompare'
$LimitedRunner = Resolve-Tool $LimitedRunner @(
    (Join-Path $PSScriptRoot 'limited_runner\limited_runner.exe'),
    (Join-Path $PSScriptRoot 'limited_runner\limited_runner')
) 'LimitedRunner'
foreach ($required in $worldExportTool, (Join-Path $assetsDirectory 'block_mappings_v1.json')) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "required file not found: $required"
    }
}

$casePath = (Resolve-Path -LiteralPath $CaseFile -ErrorAction Stop).Path
$caseDirectory = Split-Path $casePath -Parent
$decodedCases = Get-Content -Raw -Encoding UTF8 -LiteralPath $casePath | ConvertFrom-Json
$cases = @($decodedCases)
if ($cases.Count -eq 0) { throw 'case file is empty' }
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$outputRoot = (Resolve-Path -LiteralPath $OutputDirectory).Path
$statusPath = Join-Path $outputRoot 'status.json'
$status = [ordered]@{}
if ((Test-Path -LiteralPath $statusPath -PathType Leaf) -and -not $Force) {
    $loaded = Get-Content -Raw -Encoding UTF8 -LiteralPath $statusPath | ConvertFrom-Json
    foreach ($property in $loaded.PSObject.Properties) {
        $status[$property.Name] = $property.Value
    }
}

function Save-Status {
    $temporary = "$statusPath.tmp"
    [IO.File]::WriteAllText(
        $temporary,
        (($status | ConvertTo-Json -Depth 10) + [Environment]::NewLine),
        [Text.UTF8Encoding]::new($false))
    Move-Item -LiteralPath $temporary -Destination $statusPath -Force
}

function Resolve-CaseInput {
    param([string]$InputPath)
    if ($InputPath.StartsWith('oracle:', [StringComparison]::OrdinalIgnoreCase)) {
        if ([string]::IsNullOrWhiteSpace($OracleFixtureDirectory)) {
            throw "case uses oracle: but OracleFixtureDirectory is missing: $InputPath"
        }
        return Join-Path $OracleFixtureDirectory $InputPath.Substring(7)
    }
    if ($InputPath.StartsWith('project:', [StringComparison]::OrdinalIgnoreCase)) {
        return Join-Path $projectRoot $InputPath.Substring(8)
    }
    if ([IO.Path]::IsPathRooted($InputPath)) { return $InputPath }
    return Join-Path $caseDirectory $InputPath
}

function Get-LimitedInspection {
    param([string]$InputPath, [string]$Name, [string]$CaseOutput)
    $reportPath = Join-Path $CaseOutput 'inspect.limited.json'
    $arguments = @(
        '-limit-mib', [string]$MemoryLimitMiB,
        '-output', $reportPath,
        '--', $Converter,
        'inspect', $InputPath, '--assets', $assetsDirectory
    )
    & $LimitedRunner @arguments | Out-Null
    $runnerExit = $LASTEXITCODE
    if (-not (Test-Path -LiteralPath $reportPath -PathType Leaf)) {
        throw "${Name}: inspect runner did not produce a report (exit $runnerExit)"
    }
    $report = Get-Content -Raw -Encoding UTF8 -LiteralPath $reportPath | ConvertFrom-Json
    $run = @($report.runs)[0]
    if ($null -eq $run) { throw "${Name}: inspect report contains no run" }
    if ([bool]$run.memory_limit_exceeded) {
        throw "${Name}: inspect exceeded ${MemoryLimitMiB} MiB"
    }
    if ([int]$run.exit_code -ne 0) {
        $details = if ($run.stderr_path -and (Test-Path -LiteralPath $run.stderr_path -PathType Leaf)) {
            (Get-Content -Raw -Encoding UTF8 -LiteralPath $run.stderr_path).Trim()
        } else { '' }
        throw "${Name}: inspect failed with exit code $($run.exit_code): $details"
    }
    if (-not $run.stdout_path -or -not (Test-Path -LiteralPath $run.stdout_path -PathType Leaf)) {
        throw "${Name}: inspect stdout is missing"
    }
    $text = Get-Content -Raw -Encoding UTF8 -LiteralPath $run.stdout_path
    if ($text -notmatch '(?m)^size:\s*(\d+)x(\d+)x(\d+)\s*$') {
        throw "${Name}: cannot parse size from inspect output"
    }
    $width = [int64]$Matches[1]
    $height = [int64]$Matches[2]
    $length = [int64]$Matches[3]
    if ($width -le 0 -or $height -le 0 -or $length -le 0) {
        throw "${Name}: inspect returned a non-positive size"
    }
    if ($text -notmatch '(?m)^structure -> world:\s*yes\s*$') {
        throw "${Name}: source is not marked structure-to-world capable"
    }
    return [pscustomobject]@{
        Width = $width
        Height = $height
        Length = $length
        PeakRssMiB = [Math]::Round(([double]$run.memory.peak_rss_bytes / 1MB), 2)
        DurationMs = [double]$run.duration_ms
    }
}

$passed = 0
$failed = 0
foreach ($case in $cases) {
    $name = [string]$case.name
    if ([string]::IsNullOrWhiteSpace($name) -or $name -notmatch '^[A-Za-z0-9_.-]+$') {
        throw "invalid case name: $name"
    }
    if (-not $Force -and $status.Contains($name)) {
        if ($status[$name].result -eq 'passed') {
            Write-Output "SKIP $name passed"
            $passed++
            continue
        }
        if ($status[$name].result -eq 'failed' -and -not $RetryFailed) {
            Write-Output "SKIP $name failed"
            $failed++
            continue
        }
    }

    $inputPath = Resolve-CaseInput ([string]$case.input)
    $caseOutput = Join-Path $outputRoot $name
    New-Item -ItemType Directory -Force -Path $caseOutput | Out-Null
    try {
        if (-not (Test-Path -LiteralPath $inputPath)) {
            throw "input not found: $inputPath"
        }
        Write-Output "RUN  $name inspect"
        $inspection = Get-LimitedInspection $inputPath $name $caseOutput
        $minX = [int64]$StartChunkX * 16
        $minY = [int64]$StartSubChunkY * 16
        $minZ = [int64]$StartChunkZ * 16
        $maxX = $minX + $inspection.Width - 1
        $maxY = $minY + $inspection.Height - 1
        $maxZ = $minZ + $inspection.Length - 1
        foreach ($value in $minX, $minY, $minZ, $maxX, $maxY, $maxZ) {
            if ($value -lt [int]::MinValue -or $value -gt [int]::MaxValue) {
                throw "computed comparison bounds exceed int32"
            }
        }
        Write-Output "RUN  $name directory/.mcworld"
        $worldArguments = @{
            InputPath = $inputPath
            OutputDirectory = $caseOutput
            MinX = [int]$minX
            MinY = [int]$minY
            MinZ = [int]$minZ
            MaxX = [int]$maxX
            MaxY = [int]$maxY
            MaxZ = [int]$maxZ
            Converter = $Converter
            WorldCompare = $WorldCompare
            LimitedRunner = $LimitedRunner
            MemoryLimitMiB = $MemoryLimitMiB
            Threads = [Math]::Max(1, $Threads)
            KeepOutput = [bool]$KeepOutput
        }
        & $worldExportTool @worldArguments |
            Out-File -LiteralPath (Join-Path $caseOutput 'world-export.log') -Encoding utf8

        $summary = Get-ChildItem -LiteralPath $caseOutput -Recurse -Filter summary.json |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ($null -eq $summary) { throw 'world export summary is missing' }
        $worldResult = Get-Content -Raw -Encoding UTF8 -LiteralPath $summary.FullName | ConvertFrom-Json
        $status[$name] = [ordered]@{
            result = 'passed'
            input = $inputPath
            size = @($inspection.Width, $inspection.Height, $inspection.Length)
            inspect_duration_ms = $inspection.DurationMs
            inspect_peak_rss_mib = $inspection.PeakRssMiB
            directory_duration_ms = [double]$worldResult.directory_duration_ms
            archive_duration_ms = [double]$worldResult.archive_duration_ms
            directory_peak_rss_mib = [double]$worldResult.directory_peak_rss_mib
            archive_peak_rss_mib = [double]$worldResult.archive_peak_rss_mib
            completed_at = [DateTimeOffset]::Now.ToString('o')
        }
        $passed++
        Write-Output "PASS $name"
    } catch {
        $status[$name] = [ordered]@{
            result = 'failed'
            input = $inputPath
            error = $_.Exception.Message
            completed_at = [DateTimeOffset]::Now.ToString('o')
        }
        $failed++
        Write-Output "FAIL $name $($_.Exception.Message)"
    }
    Save-Status
}

Write-Output "SUMMARY passed=$passed failed=$failed total=$($cases.Count)"
if ($failed -ne 0) { exit 1 }
exit 0
