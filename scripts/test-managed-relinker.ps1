[CmdletBinding()]
param(
    [string]$Workspace = (Join-Path $env:LOCALAPPDATA "Temp\opencode\castleminerz-deps"),
    [string]$Scratch = (Join-Path $env:LOCALAPPDATA "Temp\opencode\castleminerz-analysis\relinker-tests")
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$fna = Join-Path $Workspace 'FNA'
$cecil = Join-Path $Workspace 'mono-cecil'
$fnaProject = Join-Path $fna 'FNA.Core.csproj'
$fnaDll = Join-Path $fna 'bin\Release\net8.0\FNA.dll'
$cecilProject = Join-Path $cecil 'Mono.Cecil.csproj'
$cecilDll = Join-Path $cecil 'bin\Release\net8.0\Mono.Cecil.dll'
$cecilOverrides = Join-Path $repo 'tools\managed-audit\Mono.Cecil.overrides'
$compatProject = Join-Path $repo 'compat\managed\CastleMinerZ.XnaCompat.csproj'
$relinkProject = Join-Path $repo 'tools\managed-relinker\Cmz.ManagedRelink.csproj'
$testProject = Join-Path $repo 'tools\managed-relinker-tests\Cmz.ManagedRelinkTests.csproj'
$compatBuild = Join-Path $Workspace 'xna-compat-build'
$relinkBuild = Join-Path $Workspace 'managed-relink-build'
$testBuild = Join-Path $Workspace 'managed-relink-tests-build'

foreach ($path in @($fnaProject, $cecilProject, $compatProject, $relinkProject, $testProject, $cecilOverrides)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing managed relinker test input: $path"
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

& dotnet build $compatProject -c Release --nologo `
    "-p:FnaPath=$fnaDll" `
    "-p:BaseOutputPath=$(Join-Path $compatBuild 'bin')\" `
    "-p:BaseIntermediateOutputPath=$(Join-Path $compatBuild 'obj')\"
if ($LASTEXITCODE -ne 0) {
    throw "CastleMinerZ.XnaCompat build failed."
}
$compatDll = Join-Path $compatBuild 'bin\Release\net8.0\CastleMinerZ.XnaCompat.dll'

& dotnet build $relinkProject -c Release --nologo `
    "-p:MonoCecilPath=$cecilDll" `
    "-p:BaseOutputPath=$(Join-Path $relinkBuild 'bin')\" `
    "-p:BaseIntermediateOutputPath=$(Join-Path $relinkBuild 'obj')\"
if ($LASTEXITCODE -ne 0) {
    throw "Managed relinker build failed."
}
$relinkDll = Join-Path $relinkBuild 'bin\Release\net8.0\Cmz.ManagedRelink.dll'

& dotnet build $testProject -c Release --nologo `
    "-p:MonoCecilPath=$cecilDll" `
    "-p:BaseOutputPath=$(Join-Path $testBuild 'bin')\" `
    "-p:BaseIntermediateOutputPath=$(Join-Path $testBuild 'obj')\"
if ($LASTEXITCODE -ne 0) {
    throw "Managed relinker test harness build failed."
}
$testDll = Join-Path $testBuild 'bin\Release\net8.0\Cmz.ManagedRelinkTests.dll'

& dotnet $testDll $relinkDll $fnaDll $compatDll $Scratch
if ($LASTEXITCODE -ne 0) {
    throw "Managed relinker tests failed."
}
