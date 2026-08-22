param(
    [Parameter(Mandatory = $true)][ValidatePattern('^[A-Za-z0-9_-]+$')][string]$Agent,
    [string]$Target = 'water_structure_tests',
    [ValidateSet('debug', 'release')][string]$Mode = 'release',
    [ValidateRange(1, 64)][int]$Jobs = 4,
    [string]$Branch,
    [switch]$ConfigureOnly
)

$ErrorActionPreference = 'Stop'
$repoRoot = (& git rev-parse --show-toplevel).Trim()
if (-not $repoRoot) { throw 'not inside a git worktree' }
$repoRoot = (Resolve-Path -LiteralPath $repoRoot).Path
$parent = Split-Path $repoRoot -Parent
$repoName = Split-Path $repoRoot -Leaf
$worktreeRoot = Join-Path $parent "$repoName-wt"
$buildRoot = Join-Path $parent "$repoName-build\agents"
$temporaryRoot = Join-Path $parent "$repoName-tmp\agents"
$worktree = Join-Path $worktreeRoot $Agent
$buildDirectory = Join-Path $buildRoot "$Agent-$Mode"
$temporaryDirectory = Join-Path $temporaryRoot $Agent
if ([string]::IsNullOrWhiteSpace($Branch)) { $Branch = "agent/$Agent" }

New-Item -ItemType Directory -Force -Path $worktreeRoot, $buildRoot, $temporaryDirectory | Out-Null
if (-not (Test-Path -LiteralPath $worktree -PathType Container)) {
    $existingBranch = & git show-ref --verify --quiet "refs/heads/$Branch"
    if ($LASTEXITCODE -eq 0) {
        & git worktree add $worktree $Branch
    } else {
        & git worktree add -b $Branch $worktree HEAD
    }
    if ($LASTEXITCODE -ne 0) { throw "git worktree add failed for $Agent" }
}

# xmake package installation uses a global cache. Serialize configuration so
# two fresh agents cannot install the same dependency concurrently. Compiles
# run after the mutex is released and use independent worktree/.xmake/build
# state, so they may proceed in parallel.
$mutexName = 'Local\WaterStructureCpp-XMake-Package-Bootstrap'
$mutex = [System.Threading.Mutex]::new($false, $mutexName)
try {
    if (-not $mutex.WaitOne([TimeSpan]::FromMinutes(30))) {
        throw 'timed out waiting for the xmake package/configure lock'
    }
    try {
        Push-Location $worktree
        & xmake f -m $Mode -o $buildDirectory -y
        if ($LASTEXITCODE -ne 0) { throw "xmake configure failed for $Agent" }
    } finally {
        Pop-Location
        $mutex.ReleaseMutex()
    }

    if (-not $ConfigureOnly) {
        Push-Location $worktree
        try {
            $env:WATER_STRUCTURE_AGENT_TMP = $temporaryDirectory
            & xmake b -j $Jobs $Target
            if ($LASTEXITCODE -ne 0) { throw "xmake build failed for $Agent/$Target" }
        } finally {
            Remove-Item Env:\WATER_STRUCTURE_AGENT_TMP -ErrorAction SilentlyContinue
            Pop-Location
        }
    }
} finally {
    $mutex.Dispose()
}

[pscustomobject]@{
    Agent = $Agent
    Branch = $Branch
    Worktree = $worktree
    BuildDirectory = $buildDirectory
    TemporaryDirectory = $temporaryDirectory
    Target = $Target
    Mode = $Mode
}
