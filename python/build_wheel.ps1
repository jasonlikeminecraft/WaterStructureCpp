param(
    [string]$Python = "python",
    [string]$Dll = "build/windows/x64/release/water_structure_shared.dll",
    [string]$Output = "dist/python",
    [switch]$SkipNativeBuild
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$dllPath = Join-Path $root $Dll
$outputPath = Join-Path $root $Output
$stage = Join-Path $root "build/python-wheel"

if (-not $SkipNativeBuild) {
    Push-Location $root
    try { xmake -j 8 water_structure_shared } finally { Pop-Location }
}
if (-not (Test-Path -LiteralPath $dllPath -PathType Leaf)) {
    throw "Native DLL was not found: $dllPath"
}

if (Test-Path -LiteralPath $stage) {
    Remove-Item -LiteralPath $stage -Recurse -Force
}
New-Item -ItemType Directory -Force -Path (Join-Path $stage "water_structure") | Out-Null
Copy-Item -Force (Join-Path $PSScriptRoot "pyproject.toml") $stage
Copy-Item -Force (Join-Path $PSScriptRoot "README.md") $stage
Copy-Item -Force (Join-Path $root "LICENSE") $stage
Copy-Item -Force (Join-Path $PSScriptRoot "water_structure\*") (Join-Path $stage "water_structure") -Exclude "__pycache__"
Copy-Item -Force $dllPath (Join-Path $stage "water_structure\water_structure_shared.dll")
Copy-Item -Recurse -Force (Join-Path $root "assets") (Join-Path $stage "water_structure\assets")

New-Item -ItemType Directory -Force -Path $outputPath | Out-Null
& $Python -m build $stage --wheel --outdir $outputPath
if ($LASTEXITCODE -ne 0) { throw "Wheel build failed" }

$wheel = Get-ChildItem -LiteralPath $outputPath -Filter "water_structure-*.whl" |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $wheel) { throw "Wheel was not produced" }

& $Python -m zipfile -l $wheel.FullName
if ($LASTEXITCODE -ne 0) { throw "Wheel validation failed" }
Write-Host "Built $($wheel.FullName)"
