param(
    [Parameter(Mandatory = $true)][string]$GoManifest,
    [Parameter(Mandatory = $true)][string]$CppManifest,
    [string]$InputPath,
    [string]$GoManifestTool,
    [string]$CppManifestTool,
    [string]$StreamDiffTool,
    [string]$LimitedRunner,
    [ValidateRange(64, 500)][int]$MemoryLimitMiB = 500,
    # Round-trip comparisons intentionally use different input files. Their
    # semantic manifests must match even though the source byte hashes cannot.
    [switch]$IgnoreInputSha
)

# The top-level chunks and block_entities arrays can contain millions of
# records. Keep this wrapper small: the Go helper reads JSON tokens and does
# an ordered merge without ConvertFrom-Json on either large array. The JSON
# report is bounded (at most 100 differences), so parsing it here is safe.
$goPath = (Resolve-Path -LiteralPath $GoManifest -ErrorAction Stop).Path
$cppPath = (Resolve-Path -LiteralPath $CppManifest -ErrorAction Stop).Path
if ([string]::IsNullOrWhiteSpace($StreamDiffTool)) {
    $nativeName = if ($env:OS -eq 'Windows_NT') { 'stream_manifest_diff.exe' } else { 'stream_manifest_diff' }
    $StreamDiffTool = Join-Path $PSScriptRoot ("stream_manifest_diff\$nativeName")
}
if ([string]::IsNullOrWhiteSpace($GoManifestTool)) {
    $GoManifestTool = Join-Path $PSScriptRoot 'go_manifest\go_manifest.exe'
}
if ([string]::IsNullOrWhiteSpace($CppManifestTool)) {
    $CppManifestTool = Join-Path (Split-Path $PSScriptRoot -Parent) 'build\windows\x64\release\cpp_manifest.exe'
}

$token = [Guid]::NewGuid().ToString('N')
$reportPath = Join-Path ([IO.Path]::GetTempPath()) "water-structure-manifest-diff-$token.json"
$limitedReportPath = Join-Path ([IO.Path]::GetTempPath()) "water-structure-manifest-limit-$token.json"
$runnerExit = 2

function Invoke-LimitedTool {
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [string]$WorkingDirectory,
        [string]$RunnerReportPath
    )
    if ([string]::IsNullOrWhiteSpace($LimitedRunner)) {
        if ([string]::IsNullOrWhiteSpace($WorkingDirectory)) {
            & $Executable @Arguments 2>&1 | Out-Null
        } else {
            Push-Location $WorkingDirectory
            try { & $Executable @Arguments 2>&1 | Out-Null } finally { Pop-Location }
        }
        return $LASTEXITCODE
    }
    if (-not (Test-Path -LiteralPath $LimitedRunner -PathType Leaf)) {
        throw "limited_runner not found: $LimitedRunner"
    }
    $limitArgs = @(
        '-limit-mib', [string]$MemoryLimitMiB,
        '-output', $RunnerReportPath
    )
    if (-not [string]::IsNullOrWhiteSpace($WorkingDirectory)) {
        $limitArgs += @('-cwd', $WorkingDirectory)
    }
    $limitArgs += @('--', $Executable)
    $limitArgs += $Arguments
    & $LimitedRunner @limitArgs | Out-Null
    $limitedExit = $LASTEXITCODE
    if (-not (Test-Path -LiteralPath $RunnerReportPath -PathType Leaf)) {
        throw "limited_runner did not produce report (exit code $limitedExit)"
    }
    $limitReport = Get-Content -Raw -Encoding UTF8 -LiteralPath $RunnerReportPath | ConvertFrom-Json
    $run = @($limitReport.runs)[0]
    if ($null -eq $run) { throw 'limited_runner report contains no run' }
    if ([bool]$run.memory_limit_exceeded) {
        throw "manifest helper exceeded ${MemoryLimitMiB} MiB"
    }
    if ([string]$run.termination -ne 'exited') {
        throw "manifest helper terminated: $($run.termination)"
    }
    return [int]$run.exit_code
}

try {
    if (Test-Path -LiteralPath $StreamDiffTool -PathType Leaf) {
        $streamArguments = @(
            '--go', $goPath, '--cpp', $cppPath, '--report', $reportPath
        )
        if ($IgnoreInputSha) { $streamArguments += '--ignore-input-sha' }
        $runnerExit = Invoke-LimitedTool $StreamDiffTool $streamArguments '' $limitedReportPath
    } elseif (Get-Command go -ErrorAction SilentlyContinue) {
        # go run keeps the same CLI usable on Linux, macOS and Termux when no
        # generated native helper has been installed beside this script.
        $goArguments = @(
            'run', '.', '--go', $goPath, '--cpp', $cppPath, '--report', $reportPath
        )
        if ($IgnoreInputSha) { $goArguments += '--ignore-input-sha' }
        $runnerExit = Invoke-LimitedTool 'go' $goArguments `
            (Join-Path $PSScriptRoot 'stream_manifest_diff') $limitedReportPath
    } else {
        throw "stream_manifest_diff not found and Go is not installed: $StreamDiffTool"
    }
    if (-not (Test-Path -LiteralPath $reportPath -PathType Leaf)) {
        throw "stream_manifest_diff did not produce a report (exit code $runnerExit)"
    }
    $report = Get-Content -Raw -Encoding UTF8 -LiteralPath $reportPath | ConvertFrom-Json
} finally {
    Remove-Item -LiteralPath $reportPath, $limitedReportPath -Force -ErrorAction SilentlyContinue
}

if ($runnerExit -eq 2) {
    throw "stream_manifest_diff failed to parse manifests"
}

$differences = [System.Collections.Generic.List[string]]::new()
$firstBlockMismatch = $null
function Add-Difference {
    param([string]$Path, $GoValue, $CppValue)
    $script:differences.Add("${Path}: Go=$GoValue C++=$CppValue")
}
if ($null -ne $report.differences) {
    foreach ($difference in @($report.differences)) {
        if ($null -ne $difference) { $differences.Add([string]$difference) }
    }
}
if ($null -ne $report.first_block_mismatch) {
    $firstBlockMismatch = [pscustomobject]@{
        ChunkX = [int]$report.first_block_mismatch.chunk_x
        ChunkZ = [int]$report.first_block_mismatch.chunk_z
        SubY = [int]$report.first_block_mismatch.sub_y
        Layer = [int]$report.first_block_mismatch.layer
    }
}

function Expand-BlockMismatch {
    param($Mismatch)
    if ([string]::IsNullOrWhiteSpace($InputPath)) {
        Write-Warning "First block-layer mismatch: chunk=($($Mismatch.ChunkX),$($Mismatch.ChunkZ)) subchunk=$($Mismatch.SubY) layer=$($Mismatch.Layer). Pass -InputPath to locate the first block coordinate."
        return
    }
    if (-not (Test-Path -LiteralPath $GoManifestTool -PathType Leaf)) {
        Write-Warning "Go manifest tool not found; cannot expand block coordinate: $GoManifestTool"
        return
    }
    if (-not (Test-Path -LiteralPath $CppManifestTool -PathType Leaf)) {
        Write-Warning "C++ manifest tool not found; cannot expand block coordinate: $CppManifestTool"
        return
    }

    $detailToken = [Guid]::NewGuid().ToString('N')
    $goDetailPath = Join-Path ([IO.Path]::GetTempPath()) "water-structure-go-detail-$detailToken.json"
    $cppDetailPath = Join-Path ([IO.Path]::GetTempPath()) "water-structure-cpp-detail-$detailToken.json"
    $goLimitPath = Join-Path ([IO.Path]::GetTempPath()) "water-structure-go-detail-limit-$detailToken.json"
    $cppLimitPath = Join-Path ([IO.Path]::GetTempPath()) "water-structure-cpp-detail-limit-$detailToken.json"
    try {
        $goDetailExit = Invoke-LimitedTool $GoManifestTool @(
            $InputPath, $goDetailPath, '--detail', [string]$Mismatch.ChunkX,
            [string]$Mismatch.ChunkZ, [string]$Mismatch.SubY, [string]$Mismatch.Layer
        ) '' $goLimitPath
        if ($goDetailExit -ne 0) { throw "Go manifest detail failed with exit code $goDetailExit" }
        $cppDetailExit = Invoke-LimitedTool $CppManifestTool @(
            $InputPath, $cppDetailPath, '--detail', [string]$Mismatch.ChunkX,
            [string]$Mismatch.ChunkZ, [string]$Mismatch.SubY, [string]$Mismatch.Layer
        ) '' $cppLimitPath
        if ($cppDetailExit -ne 0) { throw "C++ manifest detail failed with exit code $cppDetailExit" }
        # Detail is one 16x16x16 layer, not an unbounded manifest array.
        $goDetail = Get-Content -Raw -Encoding UTF8 -LiteralPath $goDetailPath | ConvertFrom-Json
        $cppDetail = Get-Content -Raw -Encoding UTF8 -LiteralPath $cppDetailPath | ConvertFrom-Json
        $goCells = @($goDetail.detail)
        $cppCells = @($cppDetail.detail)
        $count = [Math]::Max($goCells.Count, $cppCells.Count)
        for ($index = 0; $index -lt $count; $index++) {
            $goCell = if ($index -lt $goCells.Count) { $goCells[$index] } else { $null }
            $cppCell = if ($index -lt $cppCells.Count) { $cppCells[$index] } else { $null }
            if (($null -eq $goCell) -or ($null -eq $cppCell) -or
                ([string]$goCell.state_sha256 -cne [string]$cppCell.state_sha256)) {
                $cell = if ($null -ne $goCell) { $goCell } else { $cppCell }
                $path = "chunks[$($Mismatch.ChunkX),$($Mismatch.ChunkZ)].subchunks[y=$($Mismatch.SubY)].layer$($Mismatch.Layer).blocks[x=$($cell.x),y=$($cell.y),z=$($cell.z)]"
                $goState = if ($null -eq $goCell) { '<missing>' } else { $goCell | Select-Object name, version, states | ConvertTo-Json -Compress -Depth 8 }
                $cppState = if ($null -eq $cppCell) { '<missing>' } else { $cppCell | Select-Object name, version, states | ConvertTo-Json -Compress -Depth 8 }
                Add-Difference "$path.state" $goState $cppState
                return
            }
        }
        Write-Warning "All detailed blocks match in the first mismatched layer; verify that both manifests were generated by the current tools."
    } finally {
        Remove-Item -LiteralPath $goDetailPath, $cppDetailPath,
            $goLimitPath, $cppLimitPath -Force -ErrorAction SilentlyContinue
    }
}

if ($differences.Count -ne 0) {
    if ($null -ne $firstBlockMismatch) { Expand-BlockMismatch $firstBlockMismatch }
    $reportedCount = [uint64]$report.difference_count
    if ($reportedCount -eq 0) { $reportedCount = $differences.Count }
    $limit = [Math]::Min($differences.Count, 100)
    Write-Error "manifest mismatch ($reportedCount differences):`n$($differences.GetRange(0, $limit) -join "`n")"
    exit 1
}
if ($runnerExit -ne 0) {
    Write-Error "manifest mismatch (stream_manifest_diff exit code $runnerExit)"
    exit 1
}
Write-Output "manifest match: $GoManifest == $CppManifest"
