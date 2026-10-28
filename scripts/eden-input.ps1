[CmdletBinding()]
param(
    [Parameter(Mandatory)][int]$ProcessId,
    [ValidateSet('Start', 'Accept', 'Back', 'Up', 'Down', 'Left', 'Right',
        'Forward', 'Backward', 'StrafeLeft', 'StrafeRight',
        'Jump', 'Inventory', 'Attack', 'Use', 'PreviousItem', 'NextItem',
        'Activate', 'Reload', 'Players', 'LStick', 'RStick')]
    [string[]]$Key = @(),
    [ValidateRange(50, 2000)][int]$HoldMilliseconds = 50,
    [ValidateRange(200, 5000)][int]$IntervalMilliseconds = 1000,
    [ValidateRange(0, 30)][int]$DelaySeconds = 1,
    [switch]$Capture
)

# Drive only an existing CastleMiner Z Eden window. This does not launch an
# emulator, change its configuration, or capture the Windows desktop. The keys
# match the development keyboard mapping: M=Plus, N=Minus, C=A, X=B, V=X, Z=Y,
# Q=L, E=R, R=ZL, T=ZR, arrows=d-pad, and WASD=left stick. In-game action names
# use the Xbox button the game reads after the port's Nintendo-label mapping.
$ErrorActionPreference = 'Stop'
$process = Get-Process -Id $ProcessId -ErrorAction Stop
if ($process.ProcessName -ne 'eden' -or $process.MainWindowHandle -eq [IntPtr]::Zero -or
    $process.MainWindowTitle -notmatch 'CastleMiner Z') {
    throw 'The selected process is not an active CastleMiner Z Eden window.'
}
if (-not ('CmzEdenInput' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class CmzEdenInput {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool ShowWindowAsync(IntPtr window, int command);
    [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint key, uint mapping);
    [DllImport("user32.dll")] public static extern void keybd_event(byte key, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, IntPtr processId);
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint attach, uint attachTo, bool set);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
}
'@
}
function Set-CmzEdenForeground {
    param([IntPtr]$Window)
    [void][CmzEdenInput]::ShowWindowAsync($Window, 9)
    for ($attempt = 0; $attempt -lt 3; $attempt++) {
        [void][CmzEdenInput]::SetForegroundWindow($Window)
        if ([CmzEdenInput]::GetForegroundWindow() -eq $Window) { return $true }
        # Windows only lets the foreground process change focus; temporarily
        # attach to the current foreground thread to satisfy that rule.
        $foreground = [CmzEdenInput]::GetForegroundWindow()
        $foregroundThread = [CmzEdenInput]::GetWindowThreadProcessId($foreground, [IntPtr]::Zero)
        $currentThread = [CmzEdenInput]::GetCurrentThreadId()
        if ($foregroundThread -ne 0 -and $foregroundThread -ne $currentThread -and
            [CmzEdenInput]::AttachThreadInput($currentThread, $foregroundThread, $true)) {
            try {
                [void][CmzEdenInput]::SetForegroundWindow($Window)
            } finally {
                [void][CmzEdenInput]::AttachThreadInput($currentThread, $foregroundThread, $false)
            }
        }
        Start-Sleep -Milliseconds 200
        if ([CmzEdenInput]::GetForegroundWindow() -eq $Window) { return $true }
    }
    return $false
}
if (-not (Set-CmzEdenForeground -Window $process.MainWindowHandle)) {
    Add-Type -AssemblyName UIAutomationClient
    [System.Windows.Automation.AutomationElement]::FromHandle($process.MainWindowHandle).SetFocus()
    Start-Sleep -Milliseconds 200
    if ([CmzEdenInput]::GetForegroundWindow() -ne $process.MainWindowHandle) {
        throw 'Eden did not gain focus; no keys were sent.'
    }
}
$keys = @{ Start = 0x4d; Accept = 0x43; Back = 0x58; Up = 0x26; Down = 0x28; Left = 0x25; Right = 0x27;
    Forward = 0x57; Backward = 0x53; StrafeLeft = 0x41; StrafeRight = 0x44;
    # Xbox actions after Nintendo-label mapping. Eden keyboard codes in parentheses.
    Jump = 0x43; Inventory = 0x5a; Attack = 0x52; Use = 0x54;
    PreviousItem = 0x51; NextItem = 0x45; Activate = 0x58; Reload = 0x56;
    Players = 0x4e; LStick = 0x46; RStick = 0x47 }
foreach ($name in $Key) {
    if ([CmzEdenInput]::GetForegroundWindow() -ne $process.MainWindowHandle) {
        throw 'Eden lost focus; remaining keys were not sent.'
    }
    $flags = if ($name -in @('Up', 'Down', 'Left', 'Right')) { 1 } else { 0 }
    $scan = [byte][CmzEdenInput]::MapVirtualKey($keys[$name], 0)
    [CmzEdenInput]::keybd_event($keys[$name], $scan, $flags, [UIntPtr]::Zero)
    try {
        Start-Sleep -Milliseconds $HoldMilliseconds
    } finally {
        [CmzEdenInput]::keybd_event($keys[$name], $scan, ($flags -bor 2), [UIntPtr]::Zero)
    }
    # Let the game's menu transitions settle before the next button press.
    Start-Sleep -Milliseconds $IntervalMilliseconds
}
if ($DelaySeconds -gt 0) { Start-Sleep -Seconds $DelaySeconds }
if ($Capture) {
    Add-Type -AssemblyName UIAutomationClient
    $window = [System.Windows.Automation.AutomationElement]::FromHandle($process.MainWindowHandle)
    $scope = [System.Windows.Automation.TreeScope]::Descendants
    $nameProperty = [System.Windows.Automation.AutomationElement]::NameProperty
    $tools = $window.FindFirst($scope,
        (New-Object System.Windows.Automation.PropertyCondition($nameProperty, 'Tools')))
    $tools.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern).Expand()
    $item = $window.FindFirst($scope,
        (New-Object System.Windows.Automation.PropertyCondition($nameProperty, 'Capture Screenshot')))
    $item.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
    Write-Host "Screenshot requested for Eden PID $ProcessId. Read the newest PNG in a subsequent call."
}
