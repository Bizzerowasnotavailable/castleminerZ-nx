[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$forbiddenNames = @(
    'CastleMinerZ.exe', 'DNA.Common.dll', 'prod.keys', 'title.keys'
)
$forbiddenExtensions = @(
    '.xex', '.xexp', '.xnb', '.xwb', '.xsb', '.xgs', '.stfs', '.live',
    '.pirs', '.god', '.fxb', '.pfx', '.p12', '.pem', '.key', '.keys', '.tik'
)

$candidates = & git -C $repo ls-files --cached --others --exclude-standard
if ($LASTEXITCODE -ne 0) {
    throw "Unable to enumerate repository files"
}

$bad = foreach ($relative in $candidates) {
    $leaf = Split-Path -Leaf $relative
    $extension = [IO.Path]::GetExtension($relative).ToLowerInvariant()
    if ($forbiddenNames -contains $leaf -or $forbiddenExtensions -contains $extension) {
        $relative
        continue
    }

    # GOD/STFS package files commonly have no extension. Check their magic so
    # an accidentally copied package is caught before it is staged.
    $fullPath = Join-Path $repo ($relative -replace '/', [IO.Path]::DirectorySeparatorChar)
    if (Test-Path -LiteralPath $fullPath -PathType Leaf) {
        $stream = [IO.File]::OpenRead($fullPath)
        try {
            if ($stream.Length -ge 4) {
                $magicBytes = New-Object byte[] 4
                [void]$stream.Read($magicBytes, 0, 4)
                $magic = [Text.Encoding]::ASCII.GetString($magicBytes)
                if ($magic -in @('LIVE', 'PIRS', 'CON ')) {
                    $relative
                }
            }
        }
        finally {
            $stream.Dispose()
        }
    }
}

if ($bad) {
    Write-Error ("Forbidden proprietary or secret-looking repository files:`n" +
                 ($bad -join "`n"))
    exit 1
}

Write-Host "Repository check passed: no blocked game-data or key files are visible to Git."
