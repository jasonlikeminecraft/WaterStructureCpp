param(
    [string]$BuildDir = "build/cmake-release",
    [string]$Prefix = "dist/install"
)
$ErrorActionPreference = "Stop"
cmake -S . -B $BuildDir -DCMAKE_BUILD_TYPE=Release -DWATER_STRUCTURE_BUILD_SHARED=ON -DWATER_STRUCTURE_BUILD_TOOLS=ON
cmake --build $BuildDir --config Release --parallel
cmake --install $BuildDir --config Release --prefix $Prefix
