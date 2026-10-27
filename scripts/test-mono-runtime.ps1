[CmdletBinding()]
param(
    [string]$DevkitPro = "C:\devkitPro",
    [string]$Workspace = (Join-Path $env:LOCALAPPDATA "Temp\opencode\castleminerz-deps"),
    [switch]$SkipInterpreter,
    [switch]$SkipAot
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$monoSource = Join-Path $Workspace 'mono-nx'
$sdk = Join-Path $Workspace 'mono-nx-sdk-rel-3'
$monoRoot = Join-Path $sdk 'dotnet_runtime'
$icuRoot = Join-Path $sdk 'icu\libnx'
$dotnetRoot = Join-Path $Workspace 'dotnet-9-linux'
$bridge = Join-Path $Workspace 'wsl-devkita64-bin'
$bash = Join-Path $DevkitPro 'msys2\usr\bin\bash.exe'

foreach ($required in @(
    (Join-Path $monoSource 'native\interpreter\Makefile'),
    (Join-Path $monoSource 'native\aot\build_aot.sh'),
    (Join-Path $monoRoot 'artifacts\obj\mono\libnx.arm64.Debug\out\lib\libmonosgen-2.0.a'),
    (Join-Path $icuRoot 'lib\libicuuc.a'),
    $bash
)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Missing runtime prerequisite: $required. Run setup-dependencies.ps1 first."
    }
}

function ConvertTo-MsysPath {
    param([Parameter(Mandatory)][string]$Path)

    $result = [IO.Path]::GetFullPath($Path).Replace('\', '/')
    if ($result -match '^([A-Za-z]):/(.*)$') {
        return "/$($Matches[1].ToLower())/$($Matches[2])"
    }
    return $result
}

function ConvertTo-WslPath {
    param([Parameter(Mandatory)][string]$Path)

    $result = (& wsl.exe --exec wslpath -a -u ([IO.Path]::GetFullPath($Path))).Trim()
    if ($LASTEXITCODE -ne 0) {
        throw "Could not convert $Path to a WSL path."
    }
    return $result
}

function Quote-Sh {
    param([Parameter(Mandatory)][string]$Value)
    return "'" + $Value.Replace("'", "'`"'`"'") + "'"
}

function Invoke-MsysMake {
    param([Parameter(Mandatory)][string]$Directory)

    $msysDevkit = ConvertTo-MsysPath $DevkitPro
    $msysDirectory = ConvertTo-MsysPath $Directory
    $msysMonoRoot = ConvertTo-MsysPath $monoRoot
    $msysIcuRoot = ConvertTo-MsysPath $icuRoot
    $command = "set -e; " +
        "export DEVKITPRO='$msysDevkit'; " +
        "export DEVKITA64='$msysDevkit/devkitA64'; " +
        "export PATH='$msysDevkit/tools/bin:$msysDevkit/devkitA64/bin:/usr/bin:`$PATH'; " +
        "export MONO_NX_ROOT='$msysMonoRoot'; " +
        "export ICU_NX_INSTALL_DIR='$msysIcuRoot'; " +
        "cd '$msysDirectory'; make clean; make -j4"
    & $bash -lc $command
    if ($LASTEXITCODE -ne 0) {
        throw "mono-nx make failed in $Directory"
    }
}

if (-not $SkipInterpreter) {
    $interpreter = Join-Path $monoSource 'native\interpreter'
    Invoke-MsysMake $interpreter
    $interpreterNro = Join-Path $interpreter 'mono_nx.nro'
    Write-Host "Interpreter: $interpreterNro"
    Write-Host "SHA-256: $((Get-FileHash $interpreterNro -Algorithm SHA256).Hash)"
}

if (-not $SkipAot) {
    $dotnet = Join-Path $dotnetRoot 'dotnet'
    if (-not (Test-Path -LiteralPath $dotnet -PathType Leaf)) {
        throw "Missing WSL .NET SDK. Run setup-dependencies.ps1 -InstallWslDotnet."
    }
    & (Join-Path $PSScriptRoot 'new-wsl-toolchain-bridge.ps1') `
        -DevkitPro $DevkitPro -Workspace $Workspace

    $wslDotnetRoot = ConvertTo-WslPath $dotnetRoot
    $wslBridge = ConvertTo-WslPath $bridge
    $wslMonoRoot = ConvertTo-WslPath $monoRoot
    $wslIcuRoot = ConvertTo-WslPath $icuRoot
    $wslDevkit = ConvertTo-WslPath $DevkitPro
    $aot = Join-Path $monoSource 'native\aot'
    $wslAot = ConvertTo-WslPath $aot
    $runner = Join-Path $Workspace 'run-mono-nx-aot-smoke.sh'
    $wslRunner = ConvertTo-WslPath $runner
    $hostPath = "$wslBridge`:$wslDotnetRoot`:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

    $lines = @(
        '#!/bin/bash',
        'set -euo pipefail',
        'export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1',
        'export DOTNET_CLI_TELEMETRY_OPTOUT=1',
        'export DOTNET_NOLOGO=1',
        "export DOTNET_ROOT=$(Quote-Sh $wslDotnetRoot)",
        "export PATH=$(Quote-Sh $hostPath)",
        "export DEVKITPRO=$(Quote-Sh $wslDevkit)",
        "export MONO_NX_ROOT=$(Quote-Sh $wslMonoRoot)",
        "export ICU_NX_INSTALL_DIR=$(Quote-Sh $wslIcuRoot)",
        "cd $(Quote-Sh $wslAot)",
        'bash ./build_aot.sh'
    )
    [IO.File]::WriteAllText(
        $runner,
        ($lines -join "`n") + "`n",
        (New-Object Text.UTF8Encoding($false)))

    & wsl.exe --exec bash $wslRunner
    if ($LASTEXITCODE -ne 0) {
        throw "mono-nx WSL AOT compilation failed."
    }
    Invoke-MsysMake $aot

    $aotNro = Join-Path $aot 'aot_example.nro'
    Write-Host "AOT: $aotNro"
    Write-Host "SHA-256: $((Get-FileHash $aotNro -Algorithm SHA256).Hash)"
}
