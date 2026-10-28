[CmdletBinding()]
param(
    [string]$Workspace = (Join-Path $env:LOCALAPPDATA 'Temp\opencode\castleminerz-deps'),
    [switch]$SkipCompile,
    [string]$GlslangValidator = ''
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$Workspace = [IO.Path]::GetFullPath($Workspace)
$repoFull = [IO.Path]::GetFullPath($repo).TrimEnd('\')
if ($Workspace.Equals($repoFull, [StringComparison]::OrdinalIgnoreCase) -or
    $Workspace.StartsWith($repoFull + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Effect test outputs must stay outside the repository.'
}
if (-not $SkipCompile) {
    & (Join-Path $PSScriptRoot 'build-effects.ps1') -Workspace $Workspace
}
$build = Join-Path $Workspace 'effect-check-build'
& (Join-Path $PSScriptRoot 'apply-dependency-patches.ps1') -Workspace $Workspace -Dependency MojoShader
New-Item -ItemType Directory -Force -Path $build | Out-Null
$source = (& wsl.exe --exec wslpath -a -u (Join-Path $repo 'tools\effect-check\main.c')).Trim()
$mojo = (& wsl.exe --exec wslpath -a -u (Join-Path $Workspace 'FNA3D\MojoShader')).Trim()
$wslBuild = (& wsl.exe --exec wslpath -a -u $build).Trim()
# MojoShader's standalone build normally generates this header with CMake.
[IO.File]::WriteAllText((Join-Path $build 'mojoshader_version.h'),
    "#define MOJOSHADER_VERSION 1`n#define MOJOSHADER_CHANGESET `"cmz-host-check`"`n")
& wsl.exe --exec cc -std=gnu99 -O2 -Wall -Wextra -Werror -DMOJOSHADER_EFFECT_SUPPORT `
    -DMOJOSHADER_DEPTH_CLIPPING -DMOJOSHADER_FLIP_RENDERTARGET `
    "-I$mojo" "-I$wslBuild" -c $source -o "$wslBuild/main.o"
if ($LASTEXITCODE -ne 0) {
    throw 'Project effect validator compilation failed.'
}
# Upstream sources use GNU extensions and have their own warning policy.
$arguments = @('--exec', 'cc', '-std=gnu99', '-O2', '-Wno-format-truncation',
    '-DMOJOSHADER_EFFECT_SUPPORT', '-DMOJOSHADER_DEPTH_CLIPPING', '-DMOJOSHADER_FLIP_RENDERTARGET',
    "-I$mojo", "-I$wslBuild", "$wslBuild/main.o")
foreach ($file in @('mojoshader.c', 'mojoshader_common.c', 'mojoshader_effects.c',
    'profiles/mojoshader_profile_arb1.c', 'profiles/mojoshader_profile_bytecode.c',
    'profiles/mojoshader_profile_d3d.c', 'profiles/mojoshader_profile_hlsl.c',
    'profiles/mojoshader_profile_glsl.c', 'profiles/mojoshader_profile_metal.c',
    'profiles/mojoshader_profile_spirv.c', 'profiles/mojoshader_profile_common.c')) {
    $arguments += "$mojo/$file"
}
$arguments += @('-lm', '-o', "$wslBuild/cmz-effect-check")
& wsl.exe @arguments
if ($LASTEXITCODE -ne 0) {
    throw 'Host effect validator build failed.'
}

$contracts = Get-Content -LiteralPath (Join-Path $repo 'shaders\effects\contracts.json') -Raw |
    ConvertFrom-Json
foreach ($contract in $contracts) {
    $path = Join-Path $Workspace "replacement-effects\$($contract.name).fxb"
    $wslPath = (& wsl.exe --exec wslpath -a -u $path).Trim()
    $checkArguments = @('--exec', "$wslBuild/cmz-effect-check", $wslPath)
    if ($GlslangValidator) {
        $shaderDirectory = Join-Path $build $contract.name
        New-Item -ItemType Directory -Force -Path $shaderDirectory | Out-Null
        Get-ChildItem -LiteralPath $shaderDirectory -File |
            Where-Object { $_.Name -match '^\d+\.(vert|frag)$' } |
            Remove-Item -Force
        $checkArguments += (& wsl.exe --exec wslpath -a -u $shaderDirectory).Trim()
    }
    $report = @(& wsl.exe @checkArguments)
    if ($LASTEXITCODE -ne 0) {
        throw "MojoShader GLSL ES 3 translation failed: $($contract.name)"
    }
    [IO.File]::WriteAllLines((Join-Path $build "$($contract.name).txt"), $report)
    $parameters = @($report | Where-Object { $_ -match '^parameter ' } |
        ForEach-Object { ($_ -split ' ')[1] })
    if (@(Compare-Object @($contract.parameters) $parameters).Count -ne 0) {
        throw "Effect parameter names changed: $($contract.name)"
    }
    $techniques = @($report | Where-Object { $_ -match '^technique ' } |
        ForEach-Object { $_.Substring(10) })
    if (($techniques -join "`n") -cne ($contract.techniques -join "`n")) {
        throw "Effect technique/pass order changed: $($contract.name)"
    }
    foreach ($shape in $contract.shapes) {
        if (-not ($report -ccontains ("parameter " + $shape))) {
            throw "Effect parameter layout changed: $($contract.name): $shape"
        }
    }
    if ($GlslangValidator) {
        foreach ($shader in Get-ChildItem -LiteralPath $shaderDirectory -File |
            Where-Object { $_.Extension -in @('.vert', '.frag') }) {
            $shaderPath = (& wsl.exe --exec wslpath -a -u $shader.FullName).Trim()
            & wsl.exe --exec $GlslangValidator $shaderPath
            if ($LASTEXITCODE -ne 0) {
                throw "Strict GLSL ES compilation failed: $($contract.name)/$($shader.Name)"
            }
        }
    }
    Write-Host "$($contract.name): $($report[0]); interface verified"
}
Write-Host 'All 10 effects passed MojoShader parsing, GLSL ES 3 translation and interface checks.'
if ($GlslangValidator) { Write-Host 'All translated shader stages also passed strict glslang compilation.' }
