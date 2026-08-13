param(
    [string]$Version = "0.1.0",
    [string]$Output = "dist/nuget"
)
$ErrorActionPreference = "Stop"
& (Join-Path $PSScriptRoot "..\cmake\copy_runtime.ps1") -Destination (Join-Path $PSScriptRoot "..\dist\runtime")
New-Item -ItemType Directory -Force -Path $Output | Out-Null
nuget pack (Join-Path $PSScriptRoot "WaterStructure.Native.nuspec") -Version $Version -OutputDirectory $Output
