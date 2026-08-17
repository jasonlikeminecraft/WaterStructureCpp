param(
    [string]$Python = "python",
    [string]$NativeLibrary = "",
    [string]$Output = "dist/python",
    [switch]$SkipNativeBuild
)

$arguments = @((Join-Path $PSScriptRoot "build_wheel.py"), "--output", $Output)
if ($NativeLibrary) { $arguments += @("--native-library", $NativeLibrary) }
if ($SkipNativeBuild) { $arguments += "--skip-native-build" }
& $Python @arguments
if ($LASTEXITCODE -ne 0) { throw "Wheel build failed" }
