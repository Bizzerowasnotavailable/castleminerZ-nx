[CmdletBinding()]
param(
    [string]$Workspace = (Join-Path $env:LOCALAPPDATA 'Temp\opencode\castleminerz-deps'),
    [string]$Output = ''
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$Workspace = [IO.Path]::GetFullPath($Workspace)
if (-not $Output) {
    $Output = Join-Path $Workspace 'replacement-effects'
}
$Output = [IO.Path]::GetFullPath($Output)
$repoFull = [IO.Path]::GetFullPath($repo).TrimEnd('\')
foreach ($path in @($Workspace, $Output)) {
    if ($path.Equals($repoFull, [StringComparison]::OrdinalIgnoreCase) -or
        $path.StartsWith($repoFull + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Effect compiler outputs must stay outside the repository.'
    }
}

$build = Join-Path $Workspace 'effect-compiler-build'
& dotnet build (Join-Path $repo 'tools\effect-compiler\Cmz.EffectCompiler.csproj') `
    -c Release --nologo `
    "-p:BaseOutputPath=$(Join-Path $build 'bin')\" `
    "-p:BaseIntermediateOutputPath=$(Join-Path $build 'obj')\"
if ($LASTEXITCODE -ne 0) {
    throw 'Effect compiler build failed.'
}

$compiler = Join-Path $build 'bin\Release\net8.0\Cmz.EffectCompiler.dll'
$sources = @(Get-ChildItem -LiteralPath (Join-Path $repo 'shaders\effects') -Filter '*.fx' -File |
    Sort-Object Name)
if ($sources.Count -ne 10) {
    throw "Expected 10 project-authored effects, found $($sources.Count)."
}
New-Item -ItemType Directory -Force -Path $Output | Out-Null
foreach ($source in $sources) {
    & dotnet $compiler $source.FullName (Join-Path $Output ($source.BaseName + '.fxb'))
    if ($LASTEXITCODE -ne 0) {
        throw "Effect compilation failed: $($source.Name)"
    }
}
Write-Host "Compiled $($sources.Count) project-authored replacement effects in $Output"
