param(
    [Parameter(Mandatory = $true)][string]$InputPath,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [Parameter(Mandatory = $true)][int]$MinX,
    [Parameter(Mandatory = $true)][int]$MinY,
    [Parameter(Mandatory = $true)][int]$MinZ,
    [Parameter(Mandatory = $true)][int]$MaxX,
    [Parameter(Mandatory = $true)][int]$MaxY,
    [Parameter(Mandatory = $true)][int]$MaxZ,
    [string]$Start = '0,-4,0',
    [string]$Converter,
    [string]$WorldCompare,
    [string]$LimitedRunner,
    [ValidateRange(64, 500)][int]$MemoryLimitMiB = 500,
    [int]$Threads = 1,
    [switch]$KeepOutput
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent

function Resolve-Tool {
    param(
        [string]$Configured,
        [string[]]$Candidates,
        [string]$Name
    )
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

$inputResolved = (Resolve-Path -LiteralPath $InputPath -ErrorAction Stop).Path
$assetsDirectory = Join-Path $projectRoot 'assets'
if (-not (Test-Path -LiteralPath (Join-Path $assetsDirectory 'block_mappings_v1.json') -PathType Leaf)) {
    throw "runtime assets not found: $assetsDirectory"
}
$root = [System.IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $root | Out-Null
$runDirectory = Join-Path $root ("world-export-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $runDirectory | Out-Null
$directoryWorld = Join-Path $runDirectory 'directory-world'
$archiveWorld = Join-Path $runDirectory 'packed.mcworld'

function Invoke-Limited {
    param(
        [string]$Label,
        [string[]]$Arguments
    )
    $stdoutLog = Join-Path $runDirectory "$Label.stdout.log"
    $stderrLog = Join-Path $runDirectory "$Label.stderr.log"
    $reportPath = Join-Path $runDirectory "$Label.limited.json"
    $runnerArguments = @(
        '-limit-mib', [string]$MemoryLimitMiB,
        '-output', $reportPath,
        '--', $Converter
    ) + $Arguments
    & $LimitedRunner @runnerArguments 1> $stdoutLog 2> $stderrLog
    $runnerExit = $LASTEXITCODE
    if (-not (Test-Path -LiteralPath $reportPath -PathType Leaf)) {
        throw "limited_runner did not produce a report (exit code $runnerExit)"
    }
    $report = Get-Content -Raw -Encoding UTF8 -LiteralPath $reportPath | ConvertFrom-Json
    $run = @($report.runs)[0]
    if ($null -eq $run) { throw 'limited_runner report contains no run' }
    if ([bool]$run.memory_limit_exceeded) {
        throw "conversion exceeded ${MemoryLimitMiB} MiB"
    }
    if ([int]$run.exit_code -ne 0) {
        $details = if (Test-Path -LiteralPath $stderrLog -PathType Leaf) {
            (Get-Content -Raw -Encoding UTF8 -LiteralPath $stderrLog).Trim()
        } else { '' }
        throw "conversion failed with exit code $($run.exit_code): $details"
    }
    return $run
}

function Invoke-Compare {
    param([string]$Source, [string]$Target)
    $stdoutLog = Join-Path $runDirectory 'compare.stdout.log'
    $stderrLog = Join-Path $runDirectory 'compare.stderr.log'
    & $WorldCompare $Source $Target $MinX $MinY $MinZ $MaxX $MaxY $MaxZ '--assets' $assetsDirectory 1> $stdoutLog 2> $stderrLog
    if ($LASTEXITCODE -ne 0) {
        $details = if (Test-Path -LiteralPath $stderrLog -PathType Leaf) {
            (Get-Content -Raw -Encoding UTF8 -LiteralPath $stderrLog).Trim()
        } else { '' }
        throw "world_compare failed: $details"
    }
    Get-Content -Raw -Encoding UTF8 -LiteralPath $stdoutLog
}

$common = @(
    '--quiet', '--threads', [string]([Math]::Max(1, $Threads)),
    '--assets', $assetsDirectory, '--start', $Start
)
Write-Output "RUN directory world: $directoryWorld"
$directoryRun = Invoke-Limited 'directory' (@('to-world', $inputResolved, $directoryWorld) + $common)
Write-Output "RUN packed mcworld: $archiveWorld"
$archiveRun = Invoke-Limited 'archive' (@('to-world', $inputResolved, $archiveWorld) + $common)

Write-Output 'COMPARE packed mcworld -> directory world'
$comparison = Invoke-Compare $archiveWorld $directoryWorld
Write-Output $comparison.TrimEnd()

$summary = [ordered]@{
    input = $inputResolved
    directory_world = $directoryWorld
    packed_mcworld = $archiveWorld
    bounds = [ordered]@{ min = @($MinX, $MinY, $MinZ); max = @($MaxX, $MaxY, $MaxZ) }
    memory_limit_mib = $MemoryLimitMiB
    start = $Start
    directory_duration_ms = [double]$directoryRun.duration_ms
    archive_duration_ms = [double]$archiveRun.duration_ms
    directory_peak_rss_mib = [Math]::Round(([double]$directoryRun.memory.peak_rss_bytes / 1MB), 2)
    archive_peak_rss_mib = [Math]::Round(([double]$archiveRun.memory.peak_rss_bytes / 1MB), 2)
    outputs_retained = [bool]$KeepOutput
    result = 'passed'
}
$summaryPath = Join-Path $runDirectory 'summary.json'
[System.IO.File]::WriteAllText(
    $summaryPath,
    (($summary | ConvertTo-Json -Depth 8) + [Environment]::NewLine),
    [System.Text.UTF8Encoding]::new($false))
Write-Output "PASS world export semantic comparison"
Write-Output "summary: $summaryPath"
if (-not $KeepOutput) {
    # The run directory is unique and was created by this script. Remove only
    # the generated world payloads; retain the bounded report/logs and summary
    # for CI diagnostics without leaving multi-gigabyte test artifacts.
    Remove-Item -LiteralPath $directoryWorld, $archiveWorld -Recurse -Force -ErrorAction SilentlyContinue
    Write-Output 'generated worlds removed; pass -KeepOutput to retain them'
}
