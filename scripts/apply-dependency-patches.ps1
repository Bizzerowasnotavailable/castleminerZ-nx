[CmdletBinding()]
param(
    [string]$Workspace = (Join-Path $env:LOCALAPPDATA 'Temp\opencode\castleminerz-deps'),
    [ValidateSet('All', 'FNA', 'FNA3D', 'MojoShader', 'FAudio', 'mono-nx')]
    [string]$Dependency = 'All'
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$targets = @(
    @{ Name = 'FNA'; Source = 'FNA'; Patches = 'patches\fna' },
    @{ Name = 'FNA3D'; Source = 'FNA3D'; Patches = 'patches\fna3d' },
    @{ Name = 'MojoShader'; Source = 'FNA3D\MojoShader'; Patches = 'patches\mojoshader' },
    @{ Name = 'FAudio'; Source = 'FAudio'; Patches = 'patches\faudio' },
    @{ Name = 'mono-nx'; Source = 'mono-nx'; Patches = 'patches\mono-nx' }
)
foreach ($target in $targets) {
    if ($Dependency -ne 'All' -and $Dependency -ne $target.Name) { continue }
    $source = Join-Path $Workspace $target.Source
    foreach ($patch in Get-ChildItem -LiteralPath (Join-Path $repo $target.Patches) -Filter '*.patch' -File |
        Sort-Object Name) {
        # Committed patches use LF. In a CRLF checkout git rejects their context
        # lines, so git always reads an LF copy kept in the workspace.
        $normalizedDirectory = Join-Path $Workspace 'normalized-patches'
        New-Item -ItemType Directory -Force -Path $normalizedDirectory | Out-Null
        $patchFile = Join-Path $normalizedDirectory ($target.Name + '-' + $patch.Name)
        [IO.File]::WriteAllText($patchFile, ([IO.File]::ReadAllText($patch.FullName) -replace "`r`n", "`n"),
            (New-Object Text.UTF8Encoding($false)))
        # A failed reverse check is expected on a pristine checkout. Windows
        # PowerShell can turn redirected native stderr into a terminating error.
        $previousPreference = $ErrorActionPreference
        try {
            $ErrorActionPreference = 'Continue'
            & git -C $source apply --reverse --check $patchFile 2>$null
            $alreadyApplied = $LASTEXITCODE -eq 0
        } finally {
            $ErrorActionPreference = $previousPreference
        }
        if ($alreadyApplied) {
            Write-Host "$($target.Name) patch already applied: $($patch.Name)"
            continue
        }
        & git -C $source apply --check $patchFile
        if ($LASTEXITCODE -ne 0) {
            throw "$($target.Name) source does not match $($patch.Name); inspect local dependency changes."
        }
        & git -C $source apply $patchFile
        if ($LASTEXITCODE -ne 0) {
            throw "$($target.Name) patch failed: $($patch.Name)"
        }
        Write-Host "Applied $($target.Name) patch: $($patch.Name)"
    }
}
