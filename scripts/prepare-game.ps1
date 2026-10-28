[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Package,
    [string]$OutputRoot = '',
    [string]$Workspace = (Join-Path $env:LOCALAPPDATA 'Temp\opencode\castleminerz-deps'),
    [string]$DevkitPro = 'C:\devkitPro',
    [string]$CmzTool = '',
    [switch]$DependenciesReady
)

# Source-only distribution: derive a complete private installation from the
# user's own supported STFS package, never from someone else's NRO/assemblies.
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$Workspace = [IO.Path]::GetFullPath($Workspace)
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $Workspace 'prepared-sd'
}
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
$Package = (Resolve-Path -LiteralPath $Package -ErrorAction Stop).ProviderPath
if (-not (Test-Path -LiteralPath $Package -PathType Leaf)) {
    throw '-Package must name a single LIVE/STFS package file, not a directory.'
}

function Test-InDirectory {
    param([string]$Path, [string]$Directory)
    $directoryFull = [IO.Path]::GetFullPath($Directory).TrimEnd('\', '/')
    return $Path.Equals($directoryFull, [StringComparison]::OrdinalIgnoreCase) -or
        $Path.StartsWith($directoryFull + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase)
}

foreach ($path in @($Workspace, $OutputRoot)) {
    if (Test-InDirectory $path $repo) {
        throw 'The dependency workspace and prepared SD output must stay outside the repository.'
    }
}
if (Test-InDirectory $OutputRoot (Join-Path $Workspace 'cmz-game-stage')) {
    throw 'OutputRoot must not be inside cmz-game-stage; game builds replace that directory.'
}
if (Test-InDirectory $Workspace $OutputRoot) {
    throw 'OutputRoot must not contain the dependency workspace.'
}
if (Test-InDirectory $Package (Join-Path $Workspace 'cmz-game-stage')) {
    throw 'The input package must not be inside cmz-game-stage; game builds replace that directory.'
}
if (Test-Path -LiteralPath $OutputRoot) {
    throw "OutputRoot already exists: $OutputRoot. Choose a new directory; existing installations are never overwritten."
}
if (-not [string]::IsNullOrWhiteSpace($CmzTool)) {
    $CmzTool = (Resolve-Path -LiteralPath $CmzTool -ErrorAction Stop).ProviderPath
    if (-not (Test-Path -LiteralPath $CmzTool -PathType Leaf)) {
        throw '-CmzTool must name a built host cmztool executable.'
    }
}

function ConvertTo-WslPath {
    param([string]$Path)
    $result = & wsl.exe --exec wslpath -a -u $Path
    if ($LASTEXITCODE -ne 0) { throw "Could not convert path for WSL: $Path" }
    return ($result -join "`n").Trim()
}

# Use WSL's host C++ compiler by default, not devkitA64 (which produces Switch
# executables). No separate CMake installation in WSL is required. A separately
# built Windows cmztool may be supplied instead.
if ([string]::IsNullOrWhiteSpace($CmzTool)) {
    $hostBuild = Join-Path $Workspace 'cmz-host-tools'
    New-Item -ItemType Directory -Force -Path $hostBuild | Out-Null
    $wslHostBuild = ConvertTo-WslPath $hostBuild
    $common = @('-std=c++17', '-O2', '-Wall', '-Wextra', '-Wpedantic', '-I',
        (ConvertTo-WslPath (Join-Path $repo 'include')))
    foreach ($name in @('stfs.cpp', 'stfs_devoptab.cpp', 'game_image.cpp', 'package_search.cpp')) {
        $common += ConvertTo-WslPath (Join-Path $repo "source\$name")
    }
    & wsl.exe --exec g++ @common (ConvertTo-WslPath (Join-Path $repo 'tools\cmztool.cpp')) -o "$wslHostBuild/cmztool"
    if ($LASTEXITCODE -ne 0) { throw 'Host tool build failed. Install g++ in WSL, or supply a Windows -CmzTool executable.' }
    & wsl.exe --exec g++ @common (ConvertTo-WslPath (Join-Path $repo 'tests\stfs_tests.cpp')) -o "$wslHostBuild/stfs_tests"
    if ($LASTEXITCODE -ne 0) { throw 'Host parser test build failed.' }
    # The synthetic parser test creates a temporary file in its cwd.
    & wsl.exe --cd $wslHostBuild --exec "$wslHostBuild/stfs_tests"
    if ($LASTEXITCODE -ne 0) { throw 'Host parser tests failed.' }
    $script:wslTool = "$wslHostBuild/cmztool"
}

function Invoke-PackageTool {
    param([string]$Command, [string]$InternalPath = '', [string]$Destination = '')
    $toolArgs = @($Command, $Package)
    if ($Command -eq 'extract') { $toolArgs += @($InternalPath, $Destination) }
    $global:LASTEXITCODE = 0
    if ([string]::IsNullOrWhiteSpace($CmzTool)) {
        $toolArgs[1] = ConvertTo-WslPath $Package
        if ($Command -eq 'extract') { $toolArgs[3] = ConvertTo-WslPath $Destination }
        & wsl.exe --exec $script:wslTool @toolArgs
    } else {
        & $CmzTool @toolArgs
    }
    if ($LASTEXITCODE -ne 0) {
        throw "cmztool $Command failed. Supply the supported CastleMiner Z LIVE/STFS package; no installation was produced."
    }
}

# Reject wrong/unsupported packages before downloading dependencies or doing
# any game compilation. verify checks structure/layout, not Xbox signatures.
Invoke-PackageTool 'verify'
$packageHash = (Get-FileHash -LiteralPath $Package -Algorithm SHA256).Hash
if (-not $DependenciesReady) {
    & (Join-Path $PSScriptRoot 'setup-dependencies.ps1') -Workspace $Workspace -InstallWslDotnet
    & (Join-Path $PSScriptRoot 'build-native-dependencies.ps1') -Workspace $Workspace -DevkitPro $DevkitPro
}

# A unique run directory prevents a failed build from accidentally packaging
# stale outputs. Retain it on failure for diagnostics; never modify a live SD.
$run = Join-Path $Workspace ('cmz-prepare-' + [guid]::NewGuid().ToString('N'))
$originals = Join-Path $run 'original-managed'
$buildOutput = Join-Path $run 'build'
$sd = Join-Path $run 'sd'
New-Item -ItemType Directory -Force -Path $originals, $sd | Out-Null
foreach ($name in @('CastleMinerZ.exe', 'DNA.Common.dll')) {
    Invoke-PackageTool 'extract' "584E07D1/$name" (Join-Path $originals $name)
}

& (Join-Path $PSScriptRoot 'build-game.ps1') -ManagedSource $originals `
    -Workspace $Workspace -DevkitPro $DevkitPro -SdRoot $sd -OutputDirectory $buildOutput

$nro = Join-Path $buildOutput 'castleminerz.nro'
if (-not (Test-Path -LiteralPath $nro -PathType Leaf)) {
    throw "Game build did not produce an NRO. Private diagnostics: $run"
}
Copy-Item -LiteralPath $nro -Destination (Join-Path $sd 'switch\castleminerz.nro')
$game = Join-Path $sd 'switch\castleminerz\game'
New-Item -ItemType Directory -Path $game | Out-Null
Copy-Item -LiteralPath $Package -Destination (Join-Path $game 'CastleMinerZ.stfs')

$paths = @(
    'switch/castleminerz.nro',
    'switch/castleminerz/game/CastleMinerZ.stfs',
    'switch/castleminerz/managed/CastleMinerZ.exe',
    'switch/castleminerz/managed/DNA.Common.dll'
)
$files = foreach ($path in $paths) {
    $fullPath = Join-Path $sd $path
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
        throw "Missing build output: $path. No complete installation was produced. Private diagnostics: $run"
    }
    $file = Get-Item -LiteralPath $fullPath -ErrorAction Stop
    if ($file.PSIsContainer -or $file.Length -eq 0) { throw "Missing/empty build output: $path" }
    [ordered]@{
        path = $path
        length = $file.Length
        sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
    }
}
if ($files[1].sha256 -cne $packageHash) {
    throw 'The input package changed during preparation. No installation was published; retry with a stable package file.'
}
$manifest = [ordered]@{
    format = 1
    description = 'Private matching package, NRO and processed managed assemblies. Do not redistribute.'
    files = @($files)
}
$manifestPath = Join-Path $sd 'switch\castleminerz\build-manifest.json'
[IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 5) + "`n",
    (New-Object Text.UTF8Encoding($false)))
& (Join-Path $PSScriptRoot 'verify-game-install.ps1') -SdRoot $sd

# Publish only after all four outputs pass verification. An absent destination
# also prevents accidental merges with old packages, saves, or config overrides.
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $OutputRoot) | Out-Null
if (Test-Path -LiteralPath $OutputRoot) { throw "OutputRoot appeared during the build: $OutputRoot" }
Move-Item -LiteralPath $sd -Destination $OutputRoot
Write-Host "Prepared private SD folder: $OutputRoot"
Write-Host 'Copy its entire switch folder to your SD root (or the destination Eden SDMC directory).'
Write-Host 'Do not mix NRO/managed files from other builds or publish these outputs.'
