param(
    [string]$BuildDir = "build/windows/x64/release",
    [string]$Destination = "dist/runtime"
)
$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force -Path $Destination | Out-Null
Copy-Item -Force "$BuildDir/water_structure_shared.dll" $Destination
Copy-Item -Force "$BuildDir/water_structure_shared.lib" $Destination
if (Test-Path -LiteralPath "$BuildDir/water_structure.lib") {
    Copy-Item -Force "$BuildDir/water_structure.lib" (Join-Path $Destination "water_structure_static.lib")
}
Copy-Item -Recurse -Force assets "$Destination/assets"
$dependencyNames = @("brotli", "zlib", "minizip")
$searchRoots = @(
    (Join-Path (Get-Location) ".xmake"),
    (Join-Path $env:USERPROFILE ".xmake"),
    (Join-Path $env:LOCALAPPDATA "xmake")
)
foreach ($root in $searchRoots) {
    if (-not (Test-Path -LiteralPath $root)) { continue }
    Get-ChildItem -LiteralPath $root -Recurse -File -Filter "*.dll" -ErrorAction SilentlyContinue |
        Where-Object {
            $dllName = $_.Name.ToLowerInvariant()
            $dependencyNames | Where-Object { $dllName.Contains($_) }
        } |
        ForEach-Object { Copy-Item -Force $_.FullName $Destination }
}
