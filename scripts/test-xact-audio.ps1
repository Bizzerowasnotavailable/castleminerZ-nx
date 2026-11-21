[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Package,
    [string]$Workspace = (Join-Path $env:LOCALAPPDATA 'Temp\opencode\castleminerz-deps'),
    [int]$WindowMilliseconds = 200
)

# Plays every cue of the game's XACT project through FACT with a live mixer on
# the Linux host (WSL): once with the pinned FAudio as is, and once with
# patches/faudio. Eden never runs the audio mixer past its first callback, so
# faults that only happen while mixing (such as the xWMA music voice with no
# decoder) cannot be seen there.
#
# Needs WSL with gcc, cmake, git and the SDL2 development package, and the
# dependency workspace from setup-dependencies.ps1. The four XACT files are
# extracted into the private workspace for the run and deleted afterwards;
# nothing is written to the repository.
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$Workspace = [IO.Path]::GetFullPath($Workspace)
$repoFull = [IO.Path]::GetFullPath($repo).TrimEnd('\')
if ($Workspace.Equals($repoFull, [StringComparison]::OrdinalIgnoreCase) -or
    $Workspace.StartsWith($repoFull + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The workspace must stay outside the repository.'
}
$Package = (Resolve-Path -LiteralPath $Package -ErrorAction Stop).ProviderPath
$faudio = Join-Path $Workspace 'FAudio'
if (-not (Test-Path -LiteralPath (Join-Path $faudio '.git'))) {
    throw "Missing FAudio checkout: $faudio. Run setup-dependencies.ps1 first."
}

function ConvertTo-WslPath {
    param([string]$Path)
    $result = & wsl.exe --exec wslpath -a -u $Path
    if ($LASTEXITCODE -ne 0) { throw "Could not convert path for WSL: $Path" }
    return ($result -join "`n").Trim()
}

$tool = Join-Path $Workspace 'cmz-host-tools\cmztool'
if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
    $hostBuild = Split-Path -Parent $tool
    New-Item -ItemType Directory -Force -Path $hostBuild | Out-Null
    $common = @('-std=c++17', '-O2', '-I', (ConvertTo-WslPath (Join-Path $repo 'include')))
    foreach ($name in @('stfs.cpp', 'stfs_devoptab.cpp', 'game_image.cpp', 'package_search.cpp')) {
        $common += ConvertTo-WslPath (Join-Path $repo "source\$name")
    }
    & wsl.exe --exec g++ @common (ConvertTo-WslPath (Join-Path $repo 'tools\cmztool.cpp')) -o (ConvertTo-WslPath $tool)
    if ($LASTEXITCODE -ne 0) { throw 'Host cmztool build failed. Install g++ in WSL.' }
}

$data = Join-Path $Workspace ('xact-check-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $data | Out-Null
try {
    $wslTool = ConvertTo-WslPath $tool
    $wslPackage = ConvertTo-WslPath $Package
    $wslData = ConvertTo-WslPath $data
    foreach ($name in @('Sounds.xgs', 'Sounds.xwb', 'SoundsStreaming.xwb', 'Sounds.xsb')) {
        & wsl.exe --exec $wslTool extract $wslPackage "584E07D1/Content/$name" "$wslData/$name"
        if ($LASTEXITCODE -ne 0) { throw "Could not extract $name from the package." }
    }

    $runner = ConvertTo-WslPath (Join-Path $repo 'tools\xact-audio-check\run-check.sh')
    $harness = ConvertTo-WslPath (Join-Path $repo 'tools\xact-audio-check\xact_audio_check.c')
    $patches = ConvertTo-WslPath (Join-Path $repo 'patches\faudio')
    # The runner prints a crash of the unpatched variant on stderr; that is an
    # expected result here, not a failure of this script.
    $previousPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $output = @(& wsl.exe --exec bash $runner (ConvertTo-WslPath $faudio) $patches $harness $wslData /var/tmp/cmz-xact-check $WindowMilliseconds 2>&1 |
        ForEach-Object { "$_" })
    $runnerExit = $LASTEXITCODE
    $ErrorActionPreference = $previousPreference
    $output | ForEach-Object { Write-Host $_ }
    if ($runnerExit -ne 0) { throw "The audio check runner failed (exit $runnerExit)." }
}
finally {
    Remove-Item -LiteralPath $data -Recurse -Force -ErrorAction SilentlyContinue
}

$pristine = $output | Where-Object { $_ -match '^RESULT pristine ' } | Select-Object -First 1
$patched = $output | Where-Object { $_ -match '^RESULT patched ' } | Select-Object -First 1
if (-not $pristine -or -not $patched) { throw 'The audio check did not report both variants.' }

if ($pristine -match 'status=0 ' -and $pristine -match 'cues-with-voice-without-decoder=0 ') {
    Write-Host 'Unpatched FAudio: no voice without a decoder, and no fault. Recheck whether the patch is still needed.'
} else {
    Write-Host "Unpatched FAudio reproduced the fault: $($pristine -replace '^RESULT pristine ', '')"
}
if ($patched -notmatch 'status=0 ' -or $patched -notmatch 'cues-with-voice-without-decoder=0 ') {
    throw "Patched FAudio failed the audio check: $patched"
}
Write-Host 'PASS: every cue played through the patched FAudio with a live mixer; no voice was left without a decoder.'
