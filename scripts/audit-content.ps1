[CmdletBinding()]
param(
    [Parameter(Mandatory)][string[]]$Asset,
    [string]$Workspace = (Join-Path $env:LOCALAPPDATA 'Temp\opencode\castleminerz-deps')
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$Workspace = [IO.Path]::GetFullPath($Workspace)
$repoFull = [IO.Path]::GetFullPath($repo).TrimEnd('\')
if ($Workspace.Equals($repoFull, [StringComparison]::OrdinalIgnoreCase) -or
    $Workspace.StartsWith($repoFull + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Content audit build outputs must stay outside the repository.'
}
$build = Join-Path $Workspace 'content-audit-build'
& dotnet build (Join-Path $repo 'tools\content-audit\Cmz.ContentAudit.csproj') `
    -c Release --nologo "-p:FnaSource=$(Join-Path $Workspace 'FNA')" `
    "-p:BaseOutputPath=$(Join-Path $build 'bin')\" `
    "-p:BaseIntermediateOutputPath=$(Join-Path $build 'obj')\"
if ($LASTEXITCODE -ne 0) { throw 'Content audit did not build.' }
& dotnet (Join-Path $build 'bin\Release\net8.0\Cmz.ContentAudit.dll') @Asset
if ($LASTEXITCODE -ne 0) { throw 'Content audit failed.' }
