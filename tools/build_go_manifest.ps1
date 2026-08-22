param(
    [Parameter(Mandatory = $true)][string]$OracleRoot,
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'
$toolRoot = Join-Path $PSScriptRoot 'go_manifest'
$oracle = (Resolve-Path -LiteralPath $OracleRoot).Path
if (Test-Path -LiteralPath (Join-Path $oracle 'modules\WaterStructure\go.mod') -PathType Leaf) {
    $waterRoot = Join-Path $oracle 'modules\WaterStructure'
} elseif (Test-Path -LiteralPath (Join-Path $oracle 'go.mod') -PathType Leaf) {
    $waterRoot = $oracle
} else {
    throw "OracleRoot must be Fatalder or its modules/WaterStructure directory: $oracle"
}
$bdumpRoot = Join-Path $waterRoot 'modules\bdump'
if (-not (Test-Path -LiteralPath (Join-Path $bdumpRoot 'go.mod') -PathType Leaf)) {
    throw "Go oracle bdump module not found: $bdumpRoot"
}
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $executable = if ($env:OS -eq 'Windows_NT') { 'go_manifest.exe' } else { 'go_manifest' }
    $OutputPath = Join-Path $toolRoot $executable
}
$output = [IO.Path]::GetFullPath($OutputPath)
$outputDirectory = Split-Path $output -Parent
[IO.Directory]::CreateDirectory($outputDirectory) | Out-Null

$temporary = Join-Path ([IO.Path]::GetTempPath()) (
    'water-structure-go-work-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($temporary) | Out-Null
$workFile = Join-Path $temporary 'go.work'
$previousWork = $env:GOWORK
try {
    Push-Location $temporary
    try {
        & go work init $toolRoot
        if ($LASTEXITCODE -ne 0) { throw "go work init failed with exit code $LASTEXITCODE" }
        & go work edit "-replace=github.com/Yeah114/WaterStructure@v0.0.0=$waterRoot"
        if ($LASTEXITCODE -ne 0) { throw "go work WaterStructure replace failed with exit code $LASTEXITCODE" }
        & go work edit "-replace=github.com/Yeah114/bdump@v0.0.0-00010101000000-000000000000=$bdumpRoot"
        if ($LASTEXITCODE -ne 0) { throw "go work bdump replace failed with exit code $LASTEXITCODE" }
    } finally {
        Pop-Location
    }
    $env:GOWORK = $workFile
    Push-Location $toolRoot
    try {
        & go build -trimpath -o $output .
        if ($LASTEXITCODE -ne 0) { throw "go_manifest build failed with exit code $LASTEXITCODE" }
    } finally {
        Pop-Location
    }
    Write-Output $output
} finally {
    if ($null -eq $previousWork) { Remove-Item Env:GOWORK -ErrorAction SilentlyContinue }
    else { $env:GOWORK = $previousWork }
    Remove-Item -LiteralPath $temporary -Recurse -Force -ErrorAction SilentlyContinue
}
