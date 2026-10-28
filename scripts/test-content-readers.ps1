[CmdletBinding()]
param(
    [string]$Workspace = (Join-Path $env:LOCALAPPDATA 'Temp\opencode\castleminerz-deps')
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$Workspace = [IO.Path]::GetFullPath($Workspace)
$repoFull = [IO.Path]::GetFullPath($repo).TrimEnd('\')
if ($Workspace.Equals($repoFull, [StringComparison]::OrdinalIgnoreCase) -or
    $Workspace.StartsWith($repoFull + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Content reader test outputs must stay outside the repository.'
}
& (Join-Path $PSScriptRoot 'apply-dependency-patches.ps1') -Workspace $Workspace -Dependency FNA
$build = Join-Path $Workspace 'content-reader-tests-build'
$source = Join-Path $Workspace 'FNA'
& dotnet build (Join-Path $repo 'tools\content-reader-tests\Cmz.ContentReaderTests.csproj') `
    -c Release --nologo "-p:FnaSource=$source" `
    "-p:BaseOutputPath=$(Join-Path $build 'bin')\" `
    "-p:BaseIntermediateOutputPath=$(Join-Path $build 'obj')\"
if ($LASTEXITCODE -ne 0) { throw 'Content reader tests did not build.' }
& dotnet (Join-Path $build 'bin\Release\net8.0\Cmz.ContentReaderTests.dll')
if ($LASTEXITCODE -ne 0) { throw 'Content reader tests failed.' }
