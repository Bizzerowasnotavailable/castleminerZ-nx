[CmdletBinding()]
param(
    [string]$Workspace = (Join-Path $env:LOCALAPPDATA "Temp\opencode\castleminerz-deps"),
    [switch]$VerifyOnly,
    [switch]$InstallWslDotnet
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$lockPath = Join-Path $repo "dependencies.lock.json"
$lock = Get-Content -LiteralPath $lockPath -Raw | ConvertFrom-Json
$Workspace = [IO.Path]::GetFullPath($Workspace)
$repoFull = [IO.Path]::GetFullPath($repo).TrimEnd('\')

if ($Workspace.Equals($repoFull, [StringComparison]::OrdinalIgnoreCase) -or
    $Workspace.StartsWith($repoFull + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw "The dependency workspace must be outside the source repository."
}

New-Item -ItemType Directory -Force -Path $Workspace | Out-Null

function Invoke-Git {
    param([Parameter(Mandatory)][string[]]$GitArgs)

    & git @GitArgs
    if ($LASTEXITCODE -ne 0) {
        throw "git failed: git $($GitArgs -join ' ')"
    }
}

function Assert-Hash {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Algorithm,
        [Parameter(Mandatory)][string]$Expected
    )

    $actual = (Get-FileHash -LiteralPath $Path -Algorithm $Algorithm).Hash
    if (-not $actual.Equals($Expected, [StringComparison]::OrdinalIgnoreCase)) {
        throw "$Algorithm mismatch for $Path. Expected $Expected, got $actual."
    }
}

function Get-Download {
    param(
        [Parameter(Mandatory)][string]$Url,
        [Parameter(Mandatory)][string]$Destination,
        [Parameter(Mandatory)][string]$Algorithm,
        [Parameter(Mandatory)][string]$ExpectedHash
    )

    if (Test-Path -LiteralPath $Destination -PathType Leaf) {
        Assert-Hash $Destination $Algorithm $ExpectedHash
        Write-Host "Verified $(Split-Path -Leaf $Destination)"
        return
    }
    if ($VerifyOnly) {
        throw "Missing dependency archive: $Destination"
    }

    $partial = "$Destination.partial"
    if (Test-Path -LiteralPath $partial) {
        Remove-Item -LiteralPath $partial -Force
    }
    & curl.exe -L --fail --retry 3 --output $partial $Url
    if ($LASTEXITCODE -ne 0) {
        throw "Download failed: $Url"
    }
    Assert-Hash $partial $Algorithm $ExpectedHash
    Move-Item -LiteralPath $partial -Destination $Destination
    Write-Host "Downloaded and verified $(Split-Path -Leaf $Destination)"
}

function Assert-SafeZip {
    param([Parameter(Mandatory)][string]$Path)

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($Path)
    try {
        foreach ($entry in $zip.Entries) {
            $name = $entry.FullName.Replace('\', '/')
            if ([IO.Path]::IsPathRooted($name) -or $name -match '(^|/)\.\.(/|$)') {
                throw "Unsafe path in archive $Path`: $name"
            }
        }
    }
    finally {
        $zip.Dispose()
    }
}

foreach ($dependency in $lock.gitDependencies) {
    $path = Join-Path $Workspace $dependency.directory
    if (-not (Test-Path -LiteralPath (Join-Path $path '.git'))) {
        if ($VerifyOnly) {
            throw "Missing Git dependency: $path"
        }
        New-Item -ItemType Directory -Force -Path $path | Out-Null
        Invoke-Git @('-C', $path, 'init')
        Invoke-Git @('-C', $path, 'remote', 'add', 'origin', $dependency.url)
    }

    $origin = (& git -C $path remote get-url origin).Trim()
    if ($LASTEXITCODE -ne 0 -or
        -not $origin.TrimEnd('/').TrimEnd('.git').Equals(
            $dependency.url.TrimEnd('/').TrimEnd('.git'),
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Unexpected origin for $($dependency.name): $origin"
    }

    $head = (& git -C $path rev-parse HEAD 2>$null).Trim()
    if ($head -ne $dependency.commit) {
        if ($VerifyOnly) {
            throw "$($dependency.name) is at $head; expected $($dependency.commit)."
        }
        $trackedChanges = & git -C $path status --porcelain --untracked-files=no
        if ($trackedChanges) {
            throw "$($dependency.name) has tracked changes; refusing to change revisions."
        }
        Invoke-Git @('-C', $path, 'fetch', '--depth', '1', 'origin', $dependency.commit)
        Invoke-Git @('-C', $path, 'checkout', '--detach', $dependency.commit)
    }

    if ($dependency.recursive) {
        if (-not $VerifyOnly) {
            Invoke-Git @('-C', $path, 'submodule', 'update', '--init', '--recursive', '--depth', '1')
        }
        $missingSubmodule = & git -C $path submodule status --recursive |
            Where-Object { $_ -match '^-' }
        if ($missingSubmodule) {
            throw "$($dependency.name) has uninitialized submodules."
        }
    }
    Write-Host "Verified $($dependency.name) at $($dependency.commit)"
}

foreach ($archive in $lock.archives) {
    $archivePath = Join-Path $Workspace $archive.file
    Get-Download $archive.url $archivePath 'SHA256' $archive.sha256
    $destination = Join-Path $Workspace $archive.extractTo

    if (-not (Test-Path -LiteralPath $destination -PathType Container)) {
        if ($VerifyOnly) {
            throw "Missing extracted dependency: $destination"
        }
        Assert-SafeZip $archivePath
        Expand-Archive -LiteralPath $archivePath -DestinationPath $destination
        Write-Host "Extracted $($archive.name)"
    }
    foreach ($required in $archive.requiredFiles) {
        $requiredPath = Join-Path $destination ($required -replace '/', '\')
        if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
            throw "$($archive.name) extraction is incomplete: missing $required"
        }
    }
}

if ($InstallWslDotnet) {
    $dotnet = $lock.wslDotnet
    $archivePath = Join-Path $Workspace $dotnet.file
    $destination = Join-Path $Workspace $dotnet.extractTo
    $dotnetExecutable = Join-Path $destination 'dotnet'

    if (-not (Test-Path -LiteralPath $dotnetExecutable -PathType Leaf)) {
        Get-Download $dotnet.url $archivePath 'SHA512' $dotnet.sha512
        if ($VerifyOnly) {
            throw "Missing WSL .NET SDK at $destination"
        }
        if (Test-Path -LiteralPath $destination) {
            throw "Incomplete WSL .NET directory exists at $destination; inspect it before removing it."
        }
        New-Item -ItemType Directory -Path $destination | Out-Null
        & "$env:SystemRoot\System32\tar.exe" -xzf $archivePath -C $destination
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to extract the WSL .NET SDK."
        }
    }

    $wslDotnet = (& wsl.exe --exec wslpath -a -u $dotnetExecutable).Trim()
    $version = (& wsl.exe --exec env DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 $wslDotnet --version).Trim()
    if ($LASTEXITCODE -ne 0 -or $version -ne $dotnet.version) {
        throw "Expected WSL .NET $($dotnet.version), got '$version'."
    }
    Write-Host "Verified WSL .NET SDK $version"
}

Write-Host "Dependency workspace is ready: $Workspace"
