[CmdletBinding()]
param(
    [string]$DevkitPro = "C:\devkitPro",
    [string]$Workspace = (Join-Path $env:LOCALAPPDATA "Temp\opencode\castleminerz-deps"),
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
& (Join-Path $PSScriptRoot 'apply-dependency-patches.ps1') -Workspace $Workspace -Dependency FNA3D
& (Join-Path $PSScriptRoot 'apply-dependency-patches.ps1') -Workspace $Workspace -Dependency MojoShader
$bash = Join-Path $DevkitPro "msys2\usr\bin\bash.exe"
if (-not (Test-Path -LiteralPath $bash -PathType Leaf)) {
    throw "devkitPro bash was not found at $bash"
}

function ConvertTo-MsysPath {
    param([Parameter(Mandatory)][string]$Path)

    $result = [IO.Path]::GetFullPath($Path).Replace('\', '/')
    if ($result -match '^([A-Za-z]):/(.*)$') {
        return "/$($Matches[1].ToLower())/$($Matches[2])"
    }
    return $result
}

function Invoke-SwitchCMake {
    param(
        [Parameter(Mandatory)][string]$Source,
        [Parameter(Mandatory)][string[]]$Options
    )

    if (-not (Test-Path -LiteralPath (Join-Path $Source 'CMakeLists.txt') -PathType Leaf)) {
        throw "Dependency source is missing: $Source"
    }
    $build = Join-Path $Source 'build-switch'
    if ($Clean -and (Test-Path -LiteralPath $build)) {
        Remove-Item -LiteralPath $build -Recurse -Force
    }

    $msysDevkit = ConvertTo-MsysPath $DevkitPro
    $msysSource = ConvertTo-MsysPath $Source
    $optionText = $Options -join ' '
    $command = "set -e; " +
        "export DEVKITPRO='$msysDevkit'; " +
        "export DEVKITA64='$msysDevkit/devkitA64'; " +
        "export PATH='$msysDevkit/tools/bin:$msysDevkit/devkitA64/bin:/usr/bin:`$PATH'; " +
        "cd '$msysSource'; " +
        "cmake -S . -B build-switch -DCMAKE_TOOLCHAIN_FILE='$msysDevkit/cmake/Switch.cmake' " +
        "-DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF $optionText; " +
        "cmake --build build-switch --parallel 4"

    & $bash -lc $command
    if ($LASTEXITCODE -ne 0) {
        throw "Switch dependency build failed in $Source"
    }
}

Invoke-SwitchCMake (Join-Path $Workspace 'FNA3D') @(
    '-DBUILD_SDL3=OFF',
    '-DTRACING_SUPPORT=OFF',
    '-DMOJOSHADER_STATIC_SPIRVCROSS=OFF'
)
Invoke-SwitchCMake (Join-Path $Workspace 'FAudio') @(
    '-DBUILD_SDL3=OFF',
    '-DBUILD_TESTS=OFF',
    '-DBUILD_UTILS=OFF'
)

$outputs = @(
    (Join-Path $Workspace 'FNA3D\build-switch\libFNA3D.a'),
    (Join-Path $Workspace 'FNA3D\build-switch\libmojoshader.a'),
    (Join-Path $Workspace 'FAudio\build-switch\libFAudio.a')
)
foreach ($output in $outputs) {
    if (-not (Test-Path -LiteralPath $output -PathType Leaf)) {
        throw "Expected static library was not produced: $output"
    }
    $hash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash
    Write-Host "Built $output ($hash)"
}
