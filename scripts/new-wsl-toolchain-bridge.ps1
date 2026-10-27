[CmdletBinding()]
param(
    [string]$DevkitPro = "C:\devkitPro",
    [string]$Workspace = (Join-Path $env:LOCALAPPDATA "Temp\opencode\castleminerz-deps")
)

$ErrorActionPreference = "Stop"
$compilerBin = Join-Path $DevkitPro "devkitA64\bin"
$output = Join-Path $Workspace "wsl-devkita64-bin"
if (-not (Test-Path -LiteralPath $compilerBin -PathType Container)) {
    throw "devkitA64 was not found at $compilerBin"
}

New-Item -ItemType Directory -Force -Path $output | Out-Null
$template = @'
#!/bin/bash
translated=()
for arg in "$@"; do
    case "$arg" in
        /*) translated+=("$(wslpath -w "$arg")") ;;
        *) translated+=("$arg") ;;
    esac
done
exec "__EXECUTABLE__" "${translated[@]}"
'@

foreach ($executable in Get-ChildItem -LiteralPath $compilerBin -Filter 'aarch64-none-elf-*.exe') {
    $wslExecutable = (& wsl.exe --exec wslpath -a -u $executable.FullName).Trim()
    if ($LASTEXITCODE -ne 0) {
        throw "Could not convert $($executable.FullName) to a WSL path."
    }
    $content = $template.Replace('__EXECUTABLE__', $wslExecutable).Replace("`r`n", "`n")
    [IO.File]::WriteAllText(
        (Join-Path $output $executable.BaseName),
        $content,
        (New-Object Text.UTF8Encoding($false)))
}

$wslOutput = (& wsl.exe --exec wslpath -a -u $output).Trim()
$assembler = "$wslOutput/aarch64-none-elf-as"
$version = & wsl.exe --exec $assembler --version
if ($LASTEXITCODE -ne 0) {
    throw "The WSL devkitA64 bridge failed its assembler smoke test."
}

Write-Host ($version | Select-Object -First 1)
Write-Host "Created WSL devkitA64 bridge: $output"
