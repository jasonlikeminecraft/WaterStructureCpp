param(
    [Parameter(Mandatory = $true)][string]$GoManifest,
    [Parameter(Mandatory = $true)][string]$CppManifest,
    [string]$InputPath,
    [string]$GoManifestTool,
    [string]$CppManifestTool
)

if ([string]::IsNullOrWhiteSpace($GoManifestTool)) {
    $GoManifestTool = Join-Path $PSScriptRoot 'go_manifest\go_manifest.exe'
}
if ([string]::IsNullOrWhiteSpace($CppManifestTool)) {
    $CppManifestTool = Join-Path (Split-Path $PSScriptRoot -Parent) 'build\windows\x64\release\cpp_manifest.exe'
}

$go = Get-Content -Raw -Encoding UTF8 -LiteralPath $GoManifest | ConvertFrom-Json
$cpp = Get-Content -Raw -Encoding UTF8 -LiteralPath $CppManifest | ConvertFrom-Json
$differences = [System.Collections.Generic.List[string]]::new()
$firstBlockMismatch = $null

function Add-Difference {
    param([string]$Path, $GoValue, $CppValue)
    $script:differences.Add("${Path}: Go=$GoValue C++=$CppValue")
}

function Compare-Scalar {
    param([string]$Path, $GoValue, $CppValue)
    if ([string]$GoValue -cne [string]$CppValue) {
        Add-Difference $Path $GoValue $CppValue
    }
}

function Compare-Array {
    param([string]$Path, $GoValue, $CppValue)
    $goArray = @($GoValue)
    $cppArray = @($CppValue)
    if ($goArray.Count -ne $cppArray.Count) {
        Add-Difference "$Path.length" $goArray.Count $cppArray.Count
        return
    }
    for ($index = 0; $index -lt $goArray.Count; $index++) {
        Compare-Scalar "${Path}[$index]" $goArray[$index] $cppArray[$index]
    }
}

function Compare-NbtFields {
    param([string]$Path, $GoFields, $CppFields)
    $goByPath = @{}
    foreach ($field in @($GoFields)) { $goByPath[[string]$field.path] = $field }
    $cppByPath = @{}
    foreach ($field in @($CppFields)) { $cppByPath[[string]$field.path] = $field }
    $fieldPaths = @($goByPath.Keys + $cppByPath.Keys | Sort-Object -Unique)
    foreach ($fieldPath in $fieldPaths) {
        $current = "$Path.nbt$fieldPath"
        if (-not $goByPath.ContainsKey($fieldPath)) {
            Add-Difference "$current.presence" $false $true
            continue
        }
        if (-not $cppByPath.ContainsKey($fieldPath)) {
            Add-Difference "$current.presence" $true $false
            continue
        }
        Compare-Scalar "$current.type" $goByPath[$fieldPath].type $cppByPath[$fieldPath].type
        Compare-Scalar "$current.value_sha256" $goByPath[$fieldPath].value_sha256 $cppByPath[$fieldPath].value_sha256
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

    $token = [Guid]::NewGuid().ToString('N')
    $goDetailPath = Join-Path ([IO.Path]::GetTempPath()) "water-structure-go-detail-$token.json"
    $cppDetailPath = Join-Path ([IO.Path]::GetTempPath()) "water-structure-cpp-detail-$token.json"
    try {
        & $GoManifestTool $InputPath $goDetailPath '--detail' $Mismatch.ChunkX $Mismatch.ChunkZ $Mismatch.SubY $Mismatch.Layer
        if ($LASTEXITCODE -ne 0) { throw "Go manifest detail failed with exit code $LASTEXITCODE" }
        & $CppManifestTool $InputPath $cppDetailPath '--detail' $Mismatch.ChunkX $Mismatch.ChunkZ $Mismatch.SubY $Mismatch.Layer
        if ($LASTEXITCODE -ne 0) { throw "C++ manifest detail failed with exit code $LASTEXITCODE" }
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
        Remove-Item -LiteralPath $goDetailPath, $cppDetailPath -Force -ErrorAction SilentlyContinue
    }
}

Compare-Scalar 'schema' $go.schema $cpp.schema
Compare-Scalar 'block_hash_algorithm' $go.block_hash_algorithm $cpp.block_hash_algorithm
Compare-Scalar 'nbt_hash_algorithm' $go.nbt_hash_algorithm $cpp.nbt_hash_algorithm
Compare-Scalar 'input_sha256' $go.input_sha256 $cpp.input_sha256

if (($null -ne $go.error) -or ($null -ne $cpp.error)) {
    if (($null -eq $go.error) -or ($null -eq $cpp.error)) {
        Add-Difference 'error.presence' ($null -ne $go.error) ($null -ne $cpp.error)
    } else {
        Compare-Scalar 'error.category' $go.error.category $cpp.error.category
        Compare-Scalar 'error.offset' $go.error.offset $cpp.error.offset
        Compare-Scalar 'error.command_index' $go.error.command_index $cpp.error.command_index
        Compare-Array 'error.coordinate' $go.error.coordinate $cpp.error.coordinate
    }
} else {
    Compare-Scalar 'format' $go.format $cpp.format
    Compare-Array 'size' $go.size $cpp.size
    Compare-Array 'offset' $go.offset $cpp.offset
    Compare-Scalar 'non_air_blocks' $go.non_air_blocks $cpp.non_air_blocks

    $goChunks = @{}
    foreach ($chunk in @($go.chunks)) { $goChunks["$($chunk.x),$($chunk.z)"] = $chunk }
    $cppChunks = @{}
    foreach ($chunk in @($cpp.chunks)) { $cppChunks["$($chunk.x),$($chunk.z)"] = $chunk }
    $chunkKeys = @($goChunks.Keys + $cppChunks.Keys | Sort-Object -Unique)
    foreach ($key in $chunkKeys) {
        $path = "chunks[$key]"
        if (-not $goChunks.ContainsKey($key)) { Add-Difference "$path.presence" $false $true; continue }
        if (-not $cppChunks.ContainsKey($key)) { Add-Difference "$path.presence" $true $false; continue }
        $goSubChunks = @{}
        foreach ($subChunk in @($goChunks[$key].subchunks)) { $goSubChunks[[string]$subChunk.y] = $subChunk }
        $cppSubChunks = @{}
        foreach ($subChunk in @($cppChunks[$key].subchunks)) { $cppSubChunks[[string]$subChunk.y] = $subChunk }
        $subChunkKeys = @($goSubChunks.Keys + $cppSubChunks.Keys | Sort-Object {[int]$_} -Unique)
        foreach ($subKey in $subChunkKeys) {
            $subPath = "$path.subchunks[y=$subKey]"
            if (-not $goSubChunks.ContainsKey($subKey)) { Add-Difference "$subPath.presence" $false $true; continue }
            if (-not $cppSubChunks.ContainsKey($subKey)) { Add-Difference "$subPath.presence" $true $false; continue }
            foreach ($layer in 0, 1) {
                $property = "layer${layer}_sha256"
                $goHash = $goSubChunks[$subKey].$property
                $cppHash = $cppSubChunks[$subKey].$property
                if (($null -eq $script:firstBlockMismatch) -and ([string]$goHash -cne [string]$cppHash)) {
                    $coordinates = $key -split ',', 2
                    $script:firstBlockMismatch = [pscustomobject]@{
                        ChunkX = [int]$coordinates[0]
                        ChunkZ = [int]$coordinates[1]
                        SubY = [int]$subKey
                        Layer = $layer
                    }
                }
                Compare-Scalar "$subPath.$property" $goHash $cppHash
            }
        }
    }

    $goEntities = @($go.block_entities | Sort-Object x, y, z)
    $cppEntities = @($cpp.block_entities | Sort-Object x, y, z)
    if ($goEntities.Count -ne $cppEntities.Count) {
        Add-Difference 'block_entities.length' $goEntities.Count $cppEntities.Count
    }
    $entityCount = [Math]::Min($goEntities.Count, $cppEntities.Count)
    for ($index = 0; $index -lt $entityCount; $index++) {
        $goEntity = $goEntities[$index]
        $cppEntity = $cppEntities[$index]
        $path = "block_entities[$index]"
        Compare-Scalar "$path.x" $goEntity.x $cppEntity.x
        Compare-Scalar "$path.y" $goEntity.y $cppEntity.y
        Compare-Scalar "$path.z" $goEntity.z $cppEntity.z
        if (($goEntity.x -eq $cppEntity.x) -and ($goEntity.y -eq $cppEntity.y) -and ($goEntity.z -eq $cppEntity.z)) {
            $path = "block_entities[x=$($goEntity.x),y=$($goEntity.y),z=$($goEntity.z)]"
        }
        Compare-Scalar "$path.nbt_sha256" $goEntity.nbt_sha256 $cppEntity.nbt_sha256
        if ([string]$goEntity.nbt_sha256 -cne [string]$cppEntity.nbt_sha256) {
            Compare-NbtFields $path $goEntity.nbt_fields $cppEntity.nbt_fields
        }
    }
}

if ($differences.Count -ne 0) {
    if ($null -ne $firstBlockMismatch) {
        Expand-BlockMismatch $firstBlockMismatch
    }
    $limit = [Math]::Min($differences.Count, 100)
    Write-Error "manifest mismatch ($($differences.Count) differences):`n$($differences.GetRange(0, $limit) -join "`n")"
    exit 1
}
Write-Output "manifest match: $GoManifest == $CppManifest"
