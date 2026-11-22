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
if (-not (Get-Content -LiteralPath $Package -Raw).StartsWith('synthetic supported package')) {
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
# The mock build embeds the processed assemblies in the NRO, so the SD card
# gets only the NRO and the package.
@'
param($ManagedSource, $Workspace, $DevkitPro, $OutputDirectory, $Icon)
$received = if ([string]::IsNullOrWhiteSpace($Icon)) { 'none' } else { [Convert]::ToBase64String([IO.File]::ReadAllBytes($Icon)) }
[IO.File]::WriteAllText((Join-Path $Workspace 'icon-received'), $received)
foreach ($name in @('CastleMinerZ.exe', 'DNA.Common.dll')) {
    if ((Get-Content -LiteralPath (Join-Path $ManagedSource $name) -Raw) -cne "original:584E07D1/$name") {
        throw "The build did not receive the original package assembly: $name"
    }
}
if (Test-Path -LiteralPath (Join-Path $Workspace 'fail-build')) { throw 'Synthetic compilation failure' }
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
if (-not (Test-Path -LiteralPath (Join-Path $Workspace 'omit-nro'))) {
    [IO.File]::WriteAllText((Join-Path $OutputDirectory 'castleminerz.nro'), 'synthetic AOT image with embedded game assemblies')
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

# A synthetic LIVE header: the content type (00000002) and title ID (584E07D2)
# sit at the same big-endian offsets as in a real STFS package.
$package = Join-Path $test 'user package without extension'
$header = [byte[]]::new(0x400)
[Text.Encoding]::ASCII.GetBytes('synthetic supported package').CopyTo($header, 0)
[Array]::Copy([byte[]](0x00, 0x00, 0x00, 0x02), 0, $header, 0x344, 4)
[Array]::Copy([byte[]](0x58, 0x4E, 0x07, 0xD2), 0, $header, 0x360, 4)
[IO.File]::WriteAllBytes($package, $header)

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

# The SD layout is exactly the NRO plus the GOD-style package folder.
$god = Join-Path $output 'switch\castleminerz\game\584E07D2\00000002\user package without extension'
if (-not (Test-Path -LiteralPath $god -PathType Leaf)) { throw 'The package was not placed in the GOD-style folder.' }
$script:checks++
if (Test-Path -LiteralPath (Join-Path $output 'switch\castleminerz\managed')) {
    throw 'The SD layout still contains a managed folder.'
}
$script:checks++
& $verify -SdRoot $output
Assert-Fails { & $prepare @arguments -OutputRoot $output } 'already exists'
Assert-Fails { & $prepare @arguments -OutputRoot (Join-Path $fixture 'private output') } 'outside the repository'
Assert-Fails { & $prepare @arguments -OutputRoot (Join-Path $deps 'cmz-game-stage\output') } 'inside cmz-game-stage'

# The completed tree must work after transfer, with no original-machine paths.
$transferred = Join-Path $test 'other pc sd'
Move-Item -LiteralPath $output -Destination $transferred
& $verify -SdRoot $transferred
$script:checks++
$godFile = Join-Path $transferred 'switch\castleminerz\game\584E07D2\00000002\user package without extension'
$correctGod = [IO.File]::ReadAllBytes($godFile)
Remove-Item -LiteralPath $godFile
Assert-Fails { & $verify -SdRoot $transferred } 'Missing installation file'
[IO.File]::WriteAllText($godFile, 'a different package')
Assert-Fails { & $verify -SdRoot $transferred } 'Build mismatch'
[IO.File]::WriteAllBytes($godFile, $correctGod)

$locked = [IO.File]::Open($godFile, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
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
$bad.files[1].path = '../../outside.dll'
$bad | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifest
Assert-Fails { & $verify -SdRoot $transferred } 'must list exactly one NRO and one GOD-style package'
[IO.File]::WriteAllText($manifest, $correctManifest)
Remove-Item -LiteralPath $manifest
Assert-Fails { & $verify -SdRoot $transferred } 'Missing build-manifest'
[IO.File]::WriteAllText($manifest, $correctManifest)

# A leftover managed folder from an older build warns but does not block.
$staleManaged = Join-Path $transferred 'switch\castleminerz\managed'
New-Item -ItemType Directory -Path $staleManaged | Out-Null
& $verify -SdRoot $transferred 3>$null
$script:checks++
Remove-Item -LiteralPath $staleManaged -Recurse
& $verify -SdRoot $transferred
$script:checks++

# The hbmenu icon: none without a thumbnail, otherwise the package's thumbnail
# PNG, and an explicit -Icon always wins.
$iconReceived = Join-Path $deps 'icon-received'
if ([IO.File]::ReadAllText($iconReceived) -cne 'none') { throw 'A package without a thumbnail produced an icon.' }
$script:checks++
Add-Type -AssemblyName System.Drawing
function New-PngBytes {
    param([System.Drawing.Color]$Color)
    $bitmap = [System.Drawing.Bitmap]::new(64, 64)
    try {
        for ($x = 0; $x -lt 64; $x++) { for ($y = 0; $y -lt 64; $y++) { $bitmap.SetPixel($x, $y, $Color) } }
        $memory = [IO.MemoryStream]::new()
        try {
            $bitmap.Save($memory, [System.Drawing.Imaging.ImageFormat]::Png)
            return , $memory.ToArray()
        } finally { $memory.Dispose() }
    } finally { $bitmap.Dispose() }
}
$thumbnail = New-PngBytes ([System.Drawing.Color]::Red)
$iconHeader = [byte[]]::new(0x971A)
[Array]::Copy($header, $iconHeader, $header.Length)
$size = $thumbnail.Length
$sizeBytes = [byte[]](
    (($size -shr 24) -band 0xFF), (($size -shr 16) -band 0xFF), (($size -shr 8) -band 0xFF), ($size -band 0xFF))
[Array]::Copy($sizeBytes, 0, $iconHeader, 0x1712, 4)
[Array]::Copy($thumbnail, 0, $iconHeader, 0x171A, $size)
[IO.File]::WriteAllBytes($package, $iconHeader)
$iconOutput = Join-Path $test 'prepared sd with icon'
& $prepare @arguments -OutputRoot $iconOutput
if ([IO.File]::ReadAllText($iconReceived) -cne [Convert]::ToBase64String($thumbnail)) {
    throw 'The build did not receive the package thumbnail as its icon.'
}
$script:checks++
$override = Join-Path $test 'my icon.png'
[IO.File]::WriteAllBytes($override, (New-PngBytes ([System.Drawing.Color]::Blue)))
& $prepare @arguments -OutputRoot (Join-Path $test 'prepared sd with override') -Icon $override
if ([IO.File]::ReadAllText($iconReceived) -cne [Convert]::ToBase64String([IO.File]::ReadAllBytes($override))) {
    throw 'An explicit -Icon did not override the package thumbnail.'
}
$script:checks++
[IO.File]::WriteAllBytes($package, $header)

[IO.File]::WriteAllText($package, 'unsupported package')
Assert-Fails { & $prepare @arguments -OutputRoot $output } 'cmztool verify failed'
if (Test-Path -LiteralPath $output) { throw 'An invalid package published an installation.' }
[IO.File]::WriteAllBytes($package, $header)
$failure = Join-Path $deps 'fail-build'
[IO.File]::WriteAllText($failure, '')
Assert-Fails { & $prepare @arguments -OutputRoot $output } 'Synthetic compilation failure'
Remove-Item -LiteralPath $failure
if (Test-Path -LiteralPath $output) { throw 'A failed build published stale outputs.' }
$omit = Join-Path $deps 'omit-nro'
[IO.File]::WriteAllText($omit, '')
Assert-Fails { & $prepare @arguments -OutputRoot $output } 'did not produce an NRO'
if (Test-Path -LiteralPath $output) { throw 'An incomplete build published an installation.' }
Remove-Item -LiteralPath $omit
Write-Host "PASS: $script:checks synthetic preparation/transfer/failure checks. Fixtures: $test"
