param(
    [string]$Dll = "../build/windows/x64/release/water_structure_shared.dll",
    [string]$Output = "../dist/python"
)
$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$stage = Join-Path $root "build/python-wheel"
if (Test-Path -LiteralPath $stage) {
    Remove-Item -LiteralPath $stage -Recurse -Force
}
New-Item -ItemType Directory -Force -Path (Join-Path $stage "water_structure") | Out-Null
Copy-Item -Force (Join-Path $PSScriptRoot "pyproject.toml") $stage
Copy-Item -Force (Join-Path $PSScriptRoot "water_structure.py") $stage
Copy-Item -Force (Join-Path $PSScriptRoot "water_structure\__init__.py") (Join-Path $stage "water_structure\__init__.py")
Copy-Item -Force (Join-Path $root $Dll) (Join-Path $stage "water_structure\water_structure_shared.dll")
Copy-Item -Recurse -Force (Join-Path $root "assets") (Join-Path $stage "water_structure\assets")
$outputPath = Join-Path $root $Output
New-Item -ItemType Directory -Force -Path $outputPath | Out-Null
python -m build $stage --wheel --outdir $outputPath
