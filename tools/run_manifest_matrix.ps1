param(
    [Parameter(Mandatory = $true)][string]$CaseFile,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [string]$OracleFixtureDirectory,
    [string]$GoManifestTool,
    [string]$CppManifestTool,
    [int]$MemoryLimitMiB = 1024,
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
$diffTool = Join-Path $PSScriptRoot 'diff_manifest.ps1'

foreach ($tool in $GoManifestTool, $CppManifestTool, $diffTool) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
        throw "tool not found: $tool"
    }
}
if ($MemoryLimitMiB -lt 128) {
    throw 'MemoryLimitMiB must be at least 128'
}

$casePath = (Resolve-Path -LiteralPath $CaseFile).Path
$caseDirectory = Split-Path $casePath -Parent
$decodedCases = Get-Content -Raw -Encoding UTF8 -LiteralPath $casePath | ConvertFrom-Json
$cases = if ($decodedCases -is [System.Array]) { @($decodedCases) } else { @($decodedCases) }
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

function Quote-ProcessArgument {
    param([string]$Value)
    return '"' + $Value.Replace('\', '\').Replace('"', '\"') + '"'
}

function Invoke-MonitoredProcess {
    param(
        [string]$Executable,
        [string[]]$Arguments,
        [string]$StandardErrorPath,
        [int]$LimitMiB
    )
    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $Executable
    $startInfo.Arguments = (($Arguments | ForEach-Object { Quote-ProcessArgument $_ }) -join ' ')
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardError = $true
	$startInfo.EnvironmentVariables['WATERSTRUCTURE_MANIFEST_MEMORY_MIB'] = [string]$LimitMiB
    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    if (-not $process.Start()) {
        throw "failed to start $Executable"
    }
    $peakWorkingSet = 0L
    $peakPrivateMemory = 0L
    $exceeded = $false
    while (-not $process.HasExited) {
        $process.Refresh()
        $peakWorkingSet = [Math]::Max($peakWorkingSet, $process.WorkingSet64)
        $peakPrivateMemory = [Math]::Max($peakPrivateMemory, $process.PrivateMemorySize64)
        if ([Math]::Max($process.WorkingSet64, $process.PrivateMemorySize64) -gt $LimitMiB * 1MB) {
            $exceeded = $true
            try {
                $process.Kill($true)
            } catch {
                # Windows PowerShell 5.1 has no Kill(bool) overload. taskkill is
                # restricted to the exact PID and recursively terminates its children.
                & "$env:SystemRoot\System32\taskkill.exe" /PID $process.Id /T /F 2>$null | Out-Null
                if (-not $process.HasExited) {
                    $process.Kill()
                }
            }
            break
        }
        Start-Sleep -Milliseconds 100
    }
    $process.WaitForExit()
    $stderr = $process.StandardError.ReadToEnd()
    [System.IO.File]::WriteAllText($StandardErrorPath, $stderr, [System.Text.UTF8Encoding]::new($false))
    if ($exceeded) {
        throw "$([System.IO.Path]::GetFileName($Executable)) exceeded ${LimitMiB} MiB and was terminated"
    }
    return [pscustomobject]@{
        ExitCode = $process.ExitCode
        PeakWorkingSetMiB = [Math]::Round($peakWorkingSet / 1MB, 2)
        PeakPrivateMemoryMiB = [Math]::Round($peakPrivateMemory / 1MB, 2)
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

    if (-not $Force -and (Test-Path -LiteralPath $goOutput -PathType Leaf) -and
        (Test-Path -LiteralPath $cppOutput -PathType Leaf)) {
        try {
            & $diffTool -GoManifest $goOutput -CppManifest $cppOutput |
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
        $goResult = Invoke-MonitoredProcess $GoManifestTool $goArguments $goError $MemoryLimitMiB
        if ($goResult.ExitCode -notin 0, 2) {
            throw "go_manifest exit code $($goResult.ExitCode)"
        }
        if (Get-Process -Name go_manifest -ErrorAction SilentlyContinue) {
            throw 'residual go_manifest process detected'
        }
        Write-Output "RUN  $name C++"
        $cppResult = Invoke-MonitoredProcess $CppManifestTool $cppArguments $cppError $MemoryLimitMiB
        if ($cppResult.ExitCode -notin 0, 2) {
            throw "cpp_manifest exit code $($cppResult.ExitCode)"
        }
        & $diffTool -GoManifest $goOutput -CppManifest $cppOutput |
            Out-File -LiteralPath (Join-Path $outputRoot "$name.diff.txt") -Encoding utf8
        $status[$name] = [ordered]@{
            result = 'passed'
            input = $inputPath
            format = [string]$case.format
            go_peak_working_set_mib = $goResult.PeakWorkingSetMiB
            go_peak_private_memory_mib = $goResult.PeakPrivateMemoryMiB
            cpp_peak_working_set_mib = $cppResult.PeakWorkingSetMiB
            cpp_peak_private_memory_mib = $cppResult.PeakPrivateMemoryMiB
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
