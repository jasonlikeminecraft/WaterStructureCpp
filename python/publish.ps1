param(
    [string]$Python = "python",
    [switch]$TestPyPI
)

$ErrorActionPreference = "Stop"
& (Join-Path $PSScriptRoot "build_wheel.ps1") -Python $Python
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$repositoryArgs = if ($TestPyPI) { @("--repository", "testpypi") } else { @() }
$wheels = Get-ChildItem -LiteralPath (Join-Path $root "dist\python") -Filter "*.whl"
if (-not $wheels) { throw "No wheels found for upload" }
$wheelPaths = @($wheels | ForEach-Object { $_.FullName })
& $Python -m twine check @wheelPaths
if ($LASTEXITCODE -ne 0) { throw "twine check failed" }
& $Python -m twine upload @repositoryArgs @wheelPaths
if ($LASTEXITCODE -ne 0) { throw "twine upload failed" }
