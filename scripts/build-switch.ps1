[CmdletBinding()]
param(
    [string]$DevkitPro = "C:\devkitPro",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$bash = Join-Path $DevkitPro "msys2\usr\bin\bash.exe"
if (-not (Test-Path -LiteralPath $bash)) {
    throw "devkitPro bash was not found at $bash"
}

$msysRepo = $repo -replace '\\', '/'
$msysDevkit = $DevkitPro -replace '\\', '/'
if ($msysRepo -match '^([A-Za-z]):/(.*)$') {
    $msysRepo = "/$($Matches[1].ToLower())/$($Matches[2])"
}
if ($msysDevkit -match '^([A-Za-z]):/(.*)$') {
    $msysDevkit = "/$($Matches[1].ToLower())/$($Matches[2])"
}

$make = if ($Clean) { "make clean && make all -j`$(nproc)" } else { "make all -j`$(nproc)" }
$command = "export DEVKITPRO='$msysDevkit'; " +
            "export DEVKITA64='$msysDevkit/devkitA64'; " +
            "export PATH='$msysDevkit/tools/bin:$msysDevkit/devkitA64/bin:/usr/bin:`$PATH'; " +
            "cd '$msysRepo'; $make"

& $bash -lc $command
if ($LASTEXITCODE -ne 0) {
    throw "Switch build failed with exit code $LASTEXITCODE"
}

Write-Host "Built $(Join-Path $repo 'castleminerz-nx.nro')"
