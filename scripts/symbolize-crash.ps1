<#
.SYNOPSIS
    Maps the castleminerz+0x... frames in an Atmosphere crash report to functions.

.DESCRIPTION
    Atmosphere reports PC, LR and return addresses as offsets into the NRO module.
    The NRO has no symbols, but the unstripped castleminerz.elf that build-game.ps1
    links first does, and its virtual addresses start at the same zero base. This
    resolves each offset with devkitA64's addr2line, which runs natively on Windows,
    so no WSL is needed.

    The ELF must come from the same build as the crashed NRO. It is kept in the
    build workspace at <Workspace>\cmz-game-stage\native\aot\castleminerz.elf. An
    ELF from another build resolves the same offsets to unrelated functions.

    A PC of 0000000000000000 has no module offset. It means a call through a null
    pointer. The frame labelled LR is the instruction that made that call.

.PARAMETER Report
    Atmosphere crash report, for example 01736465908_01007b5017a12000.log.

.PARAMETER Elf
    Unstripped castleminerz.elf from the build that produced the crashed NRO.

.PARAMETER DevkitPro
    devkitPro root. addr2line is taken from its devkitA64 bin directory.

.EXAMPLE
    ./scripts/symbolize-crash.ps1 -Report C:\logs\01736465908_01007b5017a12000.log `
        -Elf "$env:LOCALAPPDATA\Temp\opencode\castleminerz-deps\cmz-game-stage\native\aot\castleminerz.elf"
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Report,
    [Parameter(Mandatory)][string]$Elf,
    [string]$DevkitPro = 'C:\devkitPro'
)

$ErrorActionPreference = 'Stop'

$addr2line = Join-Path $DevkitPro 'devkitA64\bin\aarch64-none-elf-addr2line.exe'
if (-not (Test-Path -LiteralPath $addr2line -PathType Leaf)) {
    throw "addr2line not found: $addr2line (install devkitA64 or pass -DevkitPro)"
}
if (-not (Test-Path -LiteralPath $Report -PathType Leaf)) {
    throw "Crash report not found: $Report"
}
if (-not (Test-Path -LiteralPath $Elf -PathType Leaf)) {
    throw "ELF not found: $Elf"
}

# Frame lines look like:
#   LR:                      0000000ffe0b8f00 (castleminerz + 0x22b6f00)
#   ReturnAddress[03]:       0000000ffe056e94 (castleminerz + 0x2254e94)
# A null PC has no module offset, so it does not match and is reported below.
# Only the crashed thread is read: the other threads are listed afterwards under
# "Thread Report" and are not the thread that faulted.
$frames = @()
$nullPc = $false
$inCrashedThread = $false
foreach ($line in Get-Content -LiteralPath $Report) {
    if ($line -match '^Crashed Thread Info:') {
        $inCrashedThread = $true
        continue
    }
    if ($line -match '^(Module Info|Thread Report):') {
        $inCrashedThread = $false
    }
    if (-not $inCrashedThread) {
        continue
    }
    if ($line -match '^\s*PC:\s+0{16}\s*$') {
        $nullPc = $true
    }
    if ($line -match '^\s*(PC|LR|FP|SP|ReturnAddress\[\d+\]):.*\(castleminerz \+ 0x([0-9A-Fa-f]+)\)') {
        $frames += [pscustomobject]@{ Label = $Matches[1]; Offset = $Matches[2].ToLowerInvariant() }
    }
}
if ($frames.Count -eq 0) {
    throw 'No castleminerz frames found in the report.'
}

$addresses = @($frames | ForEach-Object { '0x' + $_.Offset })
# -f prints the function name, then file:line, for each address in order.
$resolved = & $addr2line -f -C -e $Elf @addresses
if ($LASTEXITCODE -ne 0) {
    throw "addr2line failed with exit code $LASTEXITCODE"
}
$resolved = @($resolved)
if ($resolved.Count -ne ($frames.Count * 2)) {
    throw "addr2line returned $($resolved.Count) lines for $($frames.Count) addresses"
}

for ($index = 0; $index -lt $frames.Count; $index++) {
    $frame = $frames[$index]
    $function = $resolved[$index * 2]
    $location = $resolved[$index * 2 + 1]
    '{0,-18} +0x{1,-10} {2}  [{3}]' -f $frame.Label, $frame.Offset, $function, $location
}

if ($nullPc) {
    Write-Host ''
    Write-Host 'Crashed thread PC is 0: a call through a null pointer. The LR frame above is the call site.'
}
