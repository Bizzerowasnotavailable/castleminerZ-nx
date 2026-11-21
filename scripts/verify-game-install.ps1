[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$SdRoot
)

$ErrorActionPreference = 'Stop'
$SdRoot = [IO.Path]::GetFullPath($SdRoot)
$manifestPath = Join-Path $SdRoot 'switch\castleminerz\build-manifest.json'
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw 'Missing build-manifest.json. Run scripts/prepare-game.ps1 with your own package and copy the prepared switch folder.'
}
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$files = @($manifest.files)
if ($manifest.format -ne 1 -or $files.Count -ne 2) {
    throw 'Unsupported or incomplete build manifest; regenerate with scripts/prepare-game.ps1.'
}

# Only two install shapes are accepted: the NRO, and one GOD-style package at
# switch/castleminerz/game/<title ID>/<content type>/<file>. Arbitrary paths in
# a manifest are never trusted.
$nroRecords = @($files | Where-Object { $_.path -ceq 'switch/castleminerz.nro' })
$packageRecords = @($files | Where-Object {
    $_.path -cmatch '^switch/castleminerz/game/[0-9A-F]{8}/[0-9A-F]{8}/[^/]+$'
})
if ($nroRecords.Count -ne 1 -or $packageRecords.Count -ne 1) {
    throw 'The manifest must list exactly one NRO and one GOD-style package.'
}

foreach ($record in @($nroRecords[0], $packageRecords[0])) {
    $path = $record.path
    $fullPath = Join-Path $SdRoot ($path -replace '/', '\')
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
        throw "Missing installation file: $path. Copy switch\castleminerz.nro and the switch\castleminerz folder from the same prepared build."
    }
    $file = Get-Item -LiteralPath $fullPath
    try {
        $hash = (Get-FileHash -LiteralPath $fullPath -Algorithm SHA256 -ErrorAction Stop).Hash
    } catch {
        throw "Cannot read installation file: $path. Close the running game/Eden before verification and check file permissions. $($_.Exception.Message)"
    }
    if ($file.Length -eq 0 -or $file.Length -ne $record.length -or
        $hash -cne $record.sha256) {
        throw "Build mismatch or damaged file: $path. Restore it from the SAME prepared build."
    }
}

$packagePath = Join-Path $SdRoot ($packageRecords[0].path -replace '/', '\')
$extras = @(Get-ChildItem -LiteralPath (Join-Path $SdRoot 'switch\castleminerz\game') -Recurse -File -Force |
    Where-Object { $_.FullName -ne $packagePath })
if ($extras.Count -ne 0) {
    throw 'Other files remain under switch\castleminerz\game. Move old packages out of that folder so discovery cannot select a different package.'
}
if (Test-Path -LiteralPath (Join-Path $SdRoot 'switch\castleminerz\managed')) {
    Write-Warning 'switch\castleminerz\managed is from an older build and is no longer read. Delete it so it cannot be mistaken for the current game code.'
}
if (Test-Path -LiteralPath (Join-Path $SdRoot 'switch\castleminerz-logs\aot_config.ini')) {
    Write-Warning 'An SD aot_config.ini overrides the embedded config. Check it for stale assembly paths if launch fails.'
}
Write-Host 'Verified: the NRO and the GOD-style package match the private build manifest.'
