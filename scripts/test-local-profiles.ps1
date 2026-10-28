[CmdletBinding()]
param(
    [string]$Workspace = (Join-Path $env:LOCALAPPDATA 'Temp\opencode\castleminerz-deps')
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$Workspace = [IO.Path]::GetFullPath($Workspace)
if ($Workspace.StartsWith([IO.Path]::GetFullPath($repo).TrimEnd('\') + '\',
        [StringComparison]::OrdinalIgnoreCase) -or $Workspace -eq $repo) {
    throw 'Profile test outputs must stay outside the repository.'
}
$build = Join-Path $Workspace 'local-profile-tests-build'
$fna = Join-Path $Workspace 'FNA\bin\Release\net8.0\FNA.dll'
$compat = Join-Path $Workspace 'xna-compat-build\bin\Release\net8.0\CastleMinerZ.XnaCompat.dll'
& dotnet build (Join-Path $repo 'tools\local-profile-tests\Cmz.LocalProfileTests.csproj') `
    -c Release --nologo "-p:FnaPath=$fna" "-p:CompatPath=$compat" `
    "-p:BaseOutputPath=$(Join-Path $build 'bin')\" `
    "-p:BaseIntermediateOutputPath=$(Join-Path $build 'obj')\"
if ($LASTEXITCODE -ne 0) { throw 'Profile tests did not build.' }
$test = Join-Path $build 'bin\Release\net8.0\Cmz.LocalProfileTests.dll'
foreach ($mode in @('selected', 'fallback')) {
    & dotnet $test $mode (Join-Path $build 'fixtures')
    if ($LASTEXITCODE -ne 0) { throw "Profile tests failed: $mode" }
}
