[CmdletBinding()]
param(
    [string]$Workspace = (Join-Path $env:LOCALAPPDATA 'Temp\opencode\castleminerz-deps')
)

# Test the real orchestration and verification scripts with synthetic package
# and compiler boundaries. No game files, downloads, Switch SDK or AOT needed.
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$Workspace = [IO.Path]::GetFullPath($Workspace)
$repoFull = [IO.Path]::GetFullPath($repo).TrimEnd('\')
if ($Workspace.Equals($repoFull, [StringComparison]::OrdinalIgnoreCase) -or
    $Workspace.StartsWith($repoFull + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Test outputs must stay outside the repository.'
}
$test = Join-Path $Workspace ('preparation-tests-' + [guid]::NewGuid().ToString('N'))
$fixture = Join-Path $test 'source fixture'
$scripts = Join-Path $fixture 'scripts'
$deps = Join-Path $test 'build workspace'
New-Item -ItemType Directory -Force -Path $scripts, $deps | Out-Null
foreach ($name in @('prepare-game.ps1', 'verify-game-install.ps1')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $scripts
}
$mockTool = Join-Path $test 'mock cmztool.ps1'
@'
param([string]$Command, [string]$Package, [string]$InternalPath, [string]$Destination)
if ((Get-Content -LiteralPath $Package -Raw) -cne 'synthetic supported package') {
    $global:LASTEXITCODE = 3
    return
}
if ($Command -eq 'extract') {
    if ($InternalPath -notin @('584E07D1/CastleMinerZ.exe', '584E07D1/DNA.Common.dll')) {
        throw "Unexpected extraction: $InternalPath"
    }
    [IO.File]::WriteAllText($Destination, "original:$InternalPath")
}
$global:LASTEXITCODE = 0
'@ | Set-Content -LiteralPath $mockTool
@'
param($ManagedSource, $Workspace, $DevkitPro, $SdRoot, $OutputDirectory)
foreach ($name in @('CastleMinerZ.exe', 'DNA.Common.dll')) {
    if ((Get-Content -LiteralPath (Join-Path $ManagedSource $name) -Raw) -cne "original:584E07D1/$name") {
        throw "The build did not receive the original package assembly: $name"
    }
}
if (Test-Path -LiteralPath (Join-Path $Workspace 'fail-build')) { throw 'Synthetic compilation failure' }
$managed = Join-Path $SdRoot 'switch\castleminerz\managed'
New-Item -ItemType Directory -Force -Path $managed, $OutputDirectory | Out-Null
[IO.File]::WriteAllText((Join-Path $OutputDirectory 'castleminerz.nro'), 'synthetic AOT image')
foreach ($name in @('CastleMinerZ.exe', 'DNA.Common.dll')) {
    if ($name -eq 'DNA.Common.dll' -and (Test-Path -LiteralPath (Join-Path $Workspace 'omit-dna'))) { continue }
    [IO.File]::WriteAllText((Join-Path $managed $name), "processed:$name")
}
'@ | Set-Content -LiteralPath (Join-Path $scripts 'build-game.ps1')
@'
param($Workspace, [switch]$InstallWslDotnet)
if (-not $InstallWslDotnet) { throw 'Preparation must request the pinned WSL SDK.' }
[IO.File]::WriteAllText((Join-Path $Workspace 'setup-called'), 'yes')
'@ | Set-Content -LiteralPath (Join-Path $scripts 'setup-dependencies.ps1')
@'
param($Workspace, $DevkitPro)
if (-not (Test-Path -LiteralPath (Join-Path $Workspace 'setup-called'))) { throw 'Setup must precede native build.' }
[IO.File]::WriteAllText((Join-Path $Workspace 'native-called'), 'yes')
'@ | Set-Content -LiteralPath (Join-Path $scripts 'build-native-dependencies.ps1')
$prepare = Join-Path $scripts 'prepare-game.ps1'
$verify = Join-Path $scripts 'verify-game-install.ps1'
$package = Join-Path $test 'user package without extension'
[IO.File]::WriteAllText($package, 'synthetic supported package')
$output = Join-Path $test 'prepared sd'
$arguments = @{ Package = $package; Workspace = $deps; CmzTool = $mockTool }
$script:checks = 0
function Assert-Fails {
    param([scriptblock]$Action, [string]$Expected)
    try { & $Action } catch {
        if ($_.Exception.Message -notlike "*$Expected*") { throw "Wrong failure (expected '$Expected'): $_" }
        $script:checks++
        return
    }
    throw "Expected failure: $Expected"
}

& $prepare @arguments -OutputRoot $output
foreach ($marker in @('setup-called', 'native-called')) {
    if (-not (Test-Path -LiteralPath (Join-Path $deps $marker))) { throw "Default preparation did not run $marker" }
    $script:checks++
}
$arguments.DependenciesReady = $true
& $verify -SdRoot $output
if ((Get-Content -LiteralPath (Join-Path $output 'switch\castleminerz\managed\DNA.Common.dll') -Raw) -cne 'processed:DNA.Common.dll') {
    throw 'The installation contained the original DLL instead of the processed DLL.'
}
$script:checks++
Assert-Fails { & $prepare @arguments -OutputRoot $output } 'already exists'
Assert-Fails { & $prepare @arguments -OutputRoot (Join-Path $fixture 'private output') } 'outside the repository'
Assert-Fails { & $prepare @arguments -OutputRoot (Join-Path $deps 'cmz-game-stage\output') } 'inside cmz-game-stage'

# The completed tree must work after transfer, with no original-machine paths.
$transferred = Join-Path $test 'other pc sd'
Move-Item -LiteralPath $output -Destination $transferred
& $verify -SdRoot $transferred
$script:checks++
$dna = Join-Path $transferred 'switch\castleminerz\managed\DNA.Common.dll'
$correctDna = [IO.File]::ReadAllBytes($dna)
Remove-Item -LiteralPath $dna
Assert-Fails { & $verify -SdRoot $transferred } 'Missing installation file'
[IO.File]::WriteAllText($dna, 'original extracted DNA.Common.dll')
Assert-Fails { & $verify -SdRoot $transferred } 'Build mismatch'
[IO.File]::WriteAllBytes($dna, $correctDna)
$locked = [IO.File]::Open($dna, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
try {
    Assert-Fails { & $verify -SdRoot $transferred } 'Close the running game/Eden'
} finally { $locked.Dispose() }
$nro = Join-Path $transferred 'switch\castleminerz.nro'
$correctNro = [IO.File]::ReadAllBytes($nro)
[IO.File]::WriteAllText($nro, 'NRO from another build')
Assert-Fails { & $verify -SdRoot $transferred } 'Build mismatch'
[IO.File]::WriteAllBytes($nro, $correctNro)
$old = Join-Path $transferred 'switch\castleminerz\game\old-package'
[IO.File]::WriteAllText($old, 'old package')
Assert-Fails { & $verify -SdRoot $transferred } 'Other files remain'
Remove-Item -LiteralPath $old
$manifest = Join-Path $transferred 'switch\castleminerz\build-manifest.json'
$correctManifest = [IO.File]::ReadAllText($manifest)
$bad = $correctManifest | ConvertFrom-Json
$bad.files[3].path = '../../outside.dll'
$bad | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifest
Assert-Fails { & $verify -SdRoot $transferred } 'Missing or duplicate manifest entry'
[IO.File]::WriteAllText($manifest, $correctManifest)
Remove-Item -LiteralPath $manifest
Assert-Fails { & $verify -SdRoot $transferred } 'Missing build-manifest'
[IO.File]::WriteAllText($manifest, $correctManifest)
& $verify -SdRoot $transferred

[IO.File]::WriteAllText($package, 'unsupported package')
Assert-Fails { & $prepare @arguments -OutputRoot $output } 'cmztool verify failed'
if (Test-Path -LiteralPath $output) { throw 'An invalid package published an installation.' }
[IO.File]::WriteAllText($package, 'synthetic supported package')
$failure = Join-Path $deps 'fail-build'
[IO.File]::WriteAllText($failure, '')
Assert-Fails { & $prepare @arguments -OutputRoot $output } 'Synthetic compilation failure'
Remove-Item -LiteralPath $failure
if (Test-Path -LiteralPath $output) { throw 'A failed build published stale outputs.' }
$omit = Join-Path $deps 'omit-dna'
[IO.File]::WriteAllText($omit, '')
Assert-Fails { & $prepare @arguments -OutputRoot $output } 'Missing build output'
if (Test-Path -LiteralPath $output) { throw 'An incomplete build published an installation.' }
Remove-Item -LiteralPath $omit
Write-Host "PASS: $script:checks synthetic preparation/transfer/failure checks. Fixtures: $test"
