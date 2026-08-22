param(
    [Parameter(Mandatory = $true)][string]$CaseFile,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [string]$OracleFixtureDirectory,
    [string]$GoManifestTool,
    [string]$CppManifestTool,
    [string]$LimitedRunner,
    # Every manifest process is subject to the same hard acceptance budget.
    # Larger values are intentionally rejected instead of silently falling
    # back to an unbounded polling run.
    [int]$MemoryLimitMiB = 500,
    [switch]$RetryFailed,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
if ([string]::IsNullOrWhiteSpace($GoManifestTool)) {
    $GoManifestTool = Join-Path $PSScriptRoot 'go_manifest\go_manifest.exe'
}
if ([string]::IsNullOrWhiteSpace($CppManifestTool)) {
    $CppManifestTool = Join-Path $projectRoot 'build\windows\x64\release\cpp_manifest.exe'
}
$runnerCandidates = @(
    (Join-Path $PSScriptRoot 'limited_runner\limited_runner.exe'),
    (Join-Path $PSScriptRoot 'limited_runner\limited_runner')
)
if ([string]::IsNullOrWhiteSpace($LimitedRunner)) {
    $LimitedRunner = $runnerCandidates | Where-Object {
        Test-Path -LiteralPath $_ -PathType Leaf
    } | Select-Object -First 1
}
$diffTool = Join-Path $PSScriptRoot 'diff_manifest.ps1'

foreach ($tool in $GoManifestTool, $CppManifestTool, $diffTool) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
        throw "tool not found: $tool"
    }
}
if ([string]::IsNullOrWhiteSpace($LimitedRunner) -or
    -not (Test-Path -LiteralPath $LimitedRunner -PathType Leaf)) {
    throw 'limited_runner is required: build tools/limited_runner first or pass -LimitedRunner'
}
$LimitedRunner = (Resolve-Path -LiteralPath $LimitedRunner).Path
if ($MemoryLimitMiB -lt 64 -or $MemoryLimitMiB -gt 500) {
    throw 'MemoryLimitMiB must be between 64 and 500'
}

$casePath = (Resolve-Path -LiteralPath $CaseFile).Path
$caseDirectory = Split-Path $casePath -Parent
$decodedCases = Get-Content -Raw -Encoding UTF8 -LiteralPath $casePath | ConvertFrom-Json
$cases = @($decodedCases)
if ($cases.Count -eq 0) {
    throw 'case file is empty'
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$outputRoot = (Resolve-Path -LiteralPath $OutputDirectory).Path
$statusPath = Join-Path $outputRoot 'status.json'
$status = [ordered]@{}
if ((Test-Path -LiteralPath $statusPath) -and -not $Force) {
    $loaded = Get-Content -Raw -Encoding UTF8 -LiteralPath $statusPath | ConvertFrom-Json
    foreach ($property in $loaded.PSObject.Properties) {
        $status[$property.Name] = $property.Value
    }
}

function Resolve-CaseInput {
    param([string]$InputPath)
    if ($InputPath.StartsWith('oracle:', [System.StringComparison]::OrdinalIgnoreCase)) {
        if ([string]::IsNullOrWhiteSpace($OracleFixtureDirectory)) {
            throw "case uses oracle: but OracleFixtureDirectory is missing: $InputPath"
        }
        return Join-Path $OracleFixtureDirectory $InputPath.Substring(7)
    }
    if ($InputPath.StartsWith('project:', [System.StringComparison]::OrdinalIgnoreCase)) {
        return Join-Path $projectRoot $InputPath.Substring(8)
    }
    if ([System.IO.Path]::IsPathRooted($InputPath)) {
        return $InputPath
    }
    return Join-Path $caseDirectory $InputPath
}

function Invoke-MonitoredProcess {
    param(
        [string]$Executable,
        [string[]]$Arguments,
        [string]$StandardErrorPath,
        [int]$LimitMiB,
        [string[]]$Environment = @()
    )
    $reportPath = "$StandardErrorPath.limited.json"
    $runnerArgs = @('-limit-mib', [string]$LimitMiB, '-output', $reportPath)
    foreach ($entry in $Environment) { $runnerArgs += @('-env', $entry) }
    $runnerArgs += @('--', $Executable)
    $runnerArgs += $Arguments
    & $LimitedRunner @runnerArgs | Out-Null
    $runnerExit = $LASTEXITCODE
    if (-not (Test-Path -LiteralPath $reportPath -PathType Leaf)) {
        throw "limited runner did not produce a report (exit code $runnerExit)"
    }
    $runnerReport = Get-Content -Raw -Encoding UTF8 -LiteralPath $reportPath | ConvertFrom-Json
    $runnerRun = @($runnerReport.runs)[0]
    if ($null -eq $runnerRun) { throw "limited runner report has no runs (exit code $runnerExit)" }
    if ($runnerRun.stderr_path -and (Test-Path -LiteralPath $runnerRun.stderr_path -PathType Leaf)) {
        Copy-Item -LiteralPath $runnerRun.stderr_path -Destination $StandardErrorPath -Force
    } else {
        [System.IO.File]::WriteAllText($StandardErrorPath, '', [System.Text.UTF8Encoding]::new($false))
    }
    return [pscustomobject]@{
        ExitCode = [int]$runnerRun.exit_code
        PeakWorkingSetMiB = [Math]::Round(([double]$runnerRun.memory.peak_rss_bytes / 1MB), 2)
        PeakPrivateMemoryMiB = [Math]::Round(([double]$runnerRun.memory.peak_private_bytes / 1MB), 2)
        DurationMs = [double]$runnerRun.duration_ms
        Termination = [string]$runnerRun.termination
        MemoryExceeded = [bool]$runnerRun.memory_limit_exceeded
        CpuUtilization = [double]$runnerRun.cpu_utilization_percent
        RunnerExitCode = $runnerExit
        Backend = [string]$runnerRun.backend
    }
}

function Save-Status {
    $temporary = "$statusPath.tmp"
    [System.IO.File]::WriteAllText(
        $temporary,
        (($status | ConvertTo-Json -Depth 8) + [Environment]::NewLine),
        [System.Text.UTF8Encoding]::new($false))
    Move-Item -LiteralPath $temporary -Destination $statusPath -Force
}

$passed = 0
$failed = 0
foreach ($case in $cases) {
    $name = [string]$case.name
    if ([string]::IsNullOrWhiteSpace($name) -or $name -notmatch '^[A-Za-z0-9_.-]+$') {
        throw "invalid case name: $name"
    }
    $retryingFailed = $false
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
        $retryingFailed = $status[$name].result -eq 'failed' -and $RetryFailed
    }
    $inputPath = Resolve-CaseInput ([string]$case.input)
    if (-not (Test-Path -LiteralPath $inputPath)) {
        $status[$name] = [ordered]@{ result = 'failed'; error = "input not found: $inputPath" }
        Save-Status
        Write-Output "FAIL $name input not found"
        $failed++
        continue
    }

    $goOutput = Join-Path $outputRoot "$name.go.json"
    $cppOutput = Join-Path $outputRoot "$name.cpp.json"
    $goError = Join-Path $outputRoot "$name.go.stderr.txt"
    $cppError = Join-Path $outputRoot "$name.cpp.stderr.txt"
    $goArguments = @($inputPath, $goOutput)
    $cppArguments = @($inputPath, $cppOutput)
    if (-not [string]::IsNullOrWhiteSpace([string]$case.format)) {
        $goArguments += @('--format', [string]$case.format)
        $cppArguments += @('--format', [string]$case.format)
    }

    if (-not $Force -and -not $retryingFailed -and
        (Test-Path -LiteralPath $goOutput -PathType Leaf) -and
        (Test-Path -LiteralPath $cppOutput -PathType Leaf)) {
        try {
            & $diffTool -GoManifest $goOutput -CppManifest $cppOutput `
                -LimitedRunner $LimitedRunner -MemoryLimitMiB $MemoryLimitMiB |
                Out-File -LiteralPath (Join-Path $outputRoot "$name.diff.txt") -Encoding utf8
            $status[$name] = [ordered]@{
                result = 'passed'
                input = $inputPath
                format = [string]$case.format
                reused_existing_manifests = $true
                completed_at = [DateTimeOffset]::Now.ToString('o')
            }
            $passed++
            Save-Status
            Write-Output "PASS $name reused existing manifests"
            continue
        } catch {
            $status[$name] = [ordered]@{
                result = 'failed'
                input = $inputPath
                format = [string]$case.format
                reused_existing_manifests = $true
                error = $_.Exception.Message
                completed_at = [DateTimeOffset]::Now.ToString('o')
            }
            $failed++
            Save-Status
            Write-Output "FAIL $name reused existing manifests: $($_.Exception.Message)"
            continue
        }
    }

    try {
        Write-Output "RUN  $name Go"
        $goResult = Invoke-MonitoredProcess $GoManifestTool $goArguments $goError $MemoryLimitMiB @(
            "WATERSTRUCTURE_MANIFEST_MEMORY_MIB=$MemoryLimitMiB",
            "GOMEMLIMIT=$([Math]::Max(64, $MemoryLimitMiB - 32))MiB"
        )
        if ($goResult.MemoryExceeded) {
            throw "go_manifest exceeded ${MemoryLimitMiB} MiB ($($goResult.Backend))"
        }
        if ($goResult.ExitCode -notin 0, 2) {
            throw "go_manifest exit code $($goResult.ExitCode)"
        }
        Write-Output "RUN  $name C++"
        $cppResult = Invoke-MonitoredProcess $CppManifestTool $cppArguments $cppError $MemoryLimitMiB
        if ($cppResult.MemoryExceeded) {
            throw "cpp_manifest exceeded ${MemoryLimitMiB} MiB ($($cppResult.Backend))"
        }
        if ($cppResult.ExitCode -notin 0, 2) {
            throw "cpp_manifest exit code $($cppResult.ExitCode)"
        }
        & $diffTool -GoManifest $goOutput -CppManifest $cppOutput `
            -LimitedRunner $LimitedRunner -MemoryLimitMiB $MemoryLimitMiB |
            Out-File -LiteralPath (Join-Path $outputRoot "$name.diff.txt") -Encoding utf8
        $status[$name] = [ordered]@{
            result = 'passed'
            input = $inputPath
            format = [string]$case.format
            go_peak_working_set_mib = $goResult.PeakWorkingSetMiB
            go_peak_private_memory_mib = $goResult.PeakPrivateMemoryMiB
            cpp_peak_working_set_mib = $cppResult.PeakWorkingSetMiB
            cpp_peak_private_memory_mib = $cppResult.PeakPrivateMemoryMiB
            go_duration_ms = $goResult.DurationMs
            cpp_duration_ms = $cppResult.DurationMs
            go_termination = $goResult.Termination
            cpp_termination = $cppResult.Termination
            completed_at = [DateTimeOffset]::Now.ToString('o')
        }
        $passed++
        Write-Output "PASS $name GoPeak=$($goResult.PeakWorkingSetMiB)MiB CppPeak=$($cppResult.PeakWorkingSetMiB)MiB"
    } catch {
        $status[$name] = [ordered]@{
            result = 'failed'
            input = $inputPath
            format = [string]$case.format
            error = $_.Exception.Message
            completed_at = [DateTimeOffset]::Now.ToString('o')
        }
        $failed++
        Write-Output "FAIL $name $($_.Exception.Message)"
    }
    Save-Status
}

Write-Output "SUMMARY passed=$passed failed=$failed total=$($cases.Count)"
if ($failed -ne 0) {
    exit 1
}
exit 0
