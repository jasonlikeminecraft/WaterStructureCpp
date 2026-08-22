param(
    [string]$Python = "python",
    [switch]$TestPyPI
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$uploadDirectory = Join-Path $root "dist\python-upload"
[void](New-Item -ItemType Directory -Force -Path $uploadDirectory)
# Only clear wheel files in the dedicated upload directory. dist/python may
# intentionally contain previously released versions and must never be passed
# wholesale to twine.
Get-ChildItem -LiteralPath $uploadDirectory -Filter "*.whl" -File |
    ForEach-Object { Remove-Item -LiteralPath $_.FullName -Force }
& (Join-Path $PSScriptRoot "build_wheel.ps1") `
    -Python $Python `
    -Output $uploadDirectory
$repositoryArgs = if ($TestPyPI) { @("--repository", "testpypi") } else { @() }
$wheels = Get-ChildItem -LiteralPath $uploadDirectory -Filter "*.whl" -File
if (-not $wheels) { throw "No wheels found for upload" }
$wheelPaths = @($wheels | ForEach-Object { $_.FullName })
& $Python -m twine check @wheelPaths
if ($LASTEXITCODE -ne 0) { throw "twine check failed" }
& $Python -m twine upload @repositoryArgs @wheelPaths
if ($LASTEXITCODE -ne 0) { throw "twine upload failed" }
