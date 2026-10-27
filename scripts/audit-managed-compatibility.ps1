[CmdletBinding()]
param(
    [Parameter(Mandatory)][string[]]$Assembly,
    [string]$Workspace = (Join-Path $env:LOCALAPPDATA "Temp\opencode\castleminerz-deps")
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$fna = Join-Path $Workspace 'FNA'
$cecil = Join-Path $Workspace 'mono-cecil'
$fnaProject = Join-Path $fna 'FNA.Core.csproj'
$fnaDll = Join-Path $fna 'bin\Release\net8.0\FNA.dll'
$cecilProject = Join-Path $cecil 'Mono.Cecil.csproj'
$cecilDll = Join-Path $cecil 'bin\Release\net8.0\Mono.Cecil.dll'
$auditProject = Join-Path $repo 'tools\managed-audit\Cmz.ManagedAudit.csproj'
$cecilOverrides = Join-Path $repo 'tools\managed-audit\Mono.Cecil.overrides'
$auditBuild = Join-Path $Workspace 'managed-audit-build'
$compatProject = Join-Path $repo 'compat\managed\CastleMinerZ.XnaCompat.csproj'
$compatBuild = Join-Path $Workspace 'xna-compat-build'

foreach ($path in @($fnaProject, $cecilProject, $auditProject, $cecilOverrides, $compatProject) + $Assembly) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing managed-audit input: $path"
    }
}

& dotnet build $fnaProject -c Release --nologo
if ($LASTEXITCODE -ne 0) {
    throw "FNA managed build failed."
}

& dotnet build $cecilProject -c Release --nologo `
    '-p:TargetFrameworks=net8.0' `
    '-p:EnableSourceControlManagerQueries=false' `
    "-p:CecilOverrides=$cecilOverrides"
if ($LASTEXITCODE -ne 0) {
    throw "Mono.Cecil build failed."
}

$auditBin = Join-Path $auditBuild 'bin'
$auditObj = Join-Path $auditBuild 'obj'
& dotnet build $auditProject -c Release --nologo `
    "-p:MonoCecilPath=$cecilDll" `
    "-p:BaseOutputPath=$auditBin\" `
    "-p:BaseIntermediateOutputPath=$auditObj\"
if ($LASTEXITCODE -ne 0) {
    throw "Managed compatibility audit build failed."
}

$auditDll = Join-Path $auditBin 'Release\net8.0\Cmz.ManagedAudit.dll'

& dotnet build $compatProject -c Release --nologo `
    "-p:FnaPath=$fnaDll" `
    "-p:BaseOutputPath=$(Join-Path $compatBuild 'bin')\" `
    "-p:BaseIntermediateOutputPath=$(Join-Path $compatBuild 'obj')\"
if ($LASTEXITCODE -ne 0) {
    throw "CastleMinerZ.XnaCompat build failed."
}
$compatDll = Join-Path $compatBuild 'bin\Release\net8.0\CastleMinerZ.XnaCompat.dll'

& dotnet $auditDll --reference $fnaDll --reference $compatDll @Assembly
if ($LASTEXITCODE -ne 0) {
    throw "Managed compatibility audit failed."
}
