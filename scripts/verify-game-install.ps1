[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$SdRoot
)

$ErrorActionPreference = 'Stop'
$SdRoot = [IO.Path]::GetFullPath($SdRoot)
$manifestPath = Join-Path $SdRoot 'switch\castleminerz\build-manifest.json'
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw 'Missing build-manifest.json. Run scripts/prepare-game.ps1 with your own package and copy the entire prepared switch folder.'
}
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$expected = @(
    'switch/castleminerz.nro',
    'switch/castleminerz/game/CastleMinerZ.stfs',
    'switch/castleminerz/managed/CastleMinerZ.exe',
    'switch/castleminerz/managed/DNA.Common.dll'
)
if ($manifest.format -ne 1 -or @($manifest.files).Count -ne $expected.Count) {
    throw 'Unsupported or incomplete build manifest; regenerate with scripts/prepare-game.ps1.'
}
foreach ($path in $expected) {
    # Read only fixed installation paths, never arbitrary paths from a manifest.
    $records = @($manifest.files | Where-Object { $_.path -ceq $path })
    if ($records.Count -ne 1) { throw "Missing or duplicate manifest entry: $path" }
    $record = $records[0]
    $fullPath = Join-Path $SdRoot $path
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
        throw "Missing installation file: $path. Copy the entire matching prepared switch folder, not only the NRO/package."
    }
    $file = Get-Item -LiteralPath $fullPath
    try {
        $hash = (Get-FileHash -LiteralPath $fullPath -Algorithm SHA256 -ErrorAction Stop).Hash
    } catch {
        throw "Cannot read installation file: $path. Close the running game/Eden before verification and check file permissions. $($_.Exception.Message)"
    }
    if ($file.Length -eq 0 -or $file.Length -ne $record.length -or
        $hash -cne $record.sha256) {
        throw "Build mismatch or damaged file: $path. Restore it from the SAME prepared build; do not substitute the original extracted assemblies."
    }
}
$extras = @(Get-ChildItem -LiteralPath (Join-Path $SdRoot 'switch\castleminerz\game') -Recurse -File -Force |
    Where-Object { $_.FullName -ne (Join-Path $SdRoot 'switch\castleminerz\game\CastleMinerZ.stfs') })
if ($extras.Count -ne 0) {
    throw 'Other files remain under switch/castleminerz/game. Move old packages out of that folder so discovery cannot select a different package.'
}
if (Test-Path -LiteralPath (Join-Path $SdRoot 'switch\castleminerz-logs\aot_config.ini')) {
    Write-Warning 'An SD aot_config.ini overrides the embedded config. Check it for stale assembly paths if launch fails.'
}
Write-Host 'Verified: package, NRO and both processed managed assemblies match the private build manifest.'
