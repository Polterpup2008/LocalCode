# Drives the LocalCode window from outside: focuses it, types a prompt into the
# composer, sends it with Ctrl+Enter, and captures the window at intervals.
#
# This exists so that "the GUI was verified" can mean something. Running the
# engine headlessly proves the engine; it says nothing about whether the live
# thinking indicator appears, whether the footer renders, or whether a timer
# stops when it should.
#
# Safety: keystrokes go to whatever window has focus, so the script refuses to
# type unless LocalCode is genuinely in the foreground. Never widen that check.

param(
    [Parameter(Mandatory = $true)][string]$Prompt,
    [string]$ShotPrefix = "",
    [int]$SettleSeconds = 2,
    [int[]]$CaptureAt = @(),
    [switch]$StopAfterFirstShot,
    [int]$StopAfterSeconds = 0
)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class GuiDrive {
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr c);
}
"@

try { [GuiDrive]::SetThreadDpiAwarenessContext([IntPtr](-4)) | Out-Null } catch { }

$process = Get-Process -Name LocalCode -ErrorAction Stop |
    Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $process) { throw "LocalCode has no visible window." }
$handle = $process.MainWindowHandle

# Windows refuses SetForegroundWindow to a background process unless the calling
# thread shares an input queue with the target's.
[GuiDrive]::ShowWindow($handle, 9) | Out-Null
$targetThread = [GuiDrive]::GetWindowThreadProcessId($handle, [ref]([uint32]0))
$ourThread = [GuiDrive]::GetCurrentThreadId()
[GuiDrive]::AttachThreadInput($ourThread, $targetThread, $true) | Out-Null
[GuiDrive]::BringWindowToTop($handle) | Out-Null
[GuiDrive]::SetForegroundWindow($handle) | Out-Null
[GuiDrive]::AttachThreadInput($ourThread, $targetThread, $false) | Out-Null
Start-Sleep -Milliseconds 700

# Refuse to type into somebody else's window.
$foreground = [GuiDrive]::GetForegroundWindow()
if ($foreground -ne $handle) {
    throw "LocalCode is not in the foreground (got $foreground, wanted $handle). Refusing to send keystrokes."
}

Add-Type -AssemblyName System.Windows.Forms

# Focus the composer through the application's own shortcut rather than guessing
# at coordinates: View -> Focus the prompt is bound to Ctrl+L.
[System.Windows.Forms.SendKeys]::SendWait("^l")
Start-Sleep -Milliseconds 250
[System.Windows.Forms.SendKeys]::SendWait("^a")
[System.Windows.Forms.SendKeys]::SendWait("{DEL}")
Start-Sleep -Milliseconds 150

# SendKeys treats these as control characters.
$escaped = $Prompt -replace '([+^%~(){}\[\]])', '{$1}'
[System.Windows.Forms.SendKeys]::SendWait($escaped)
Start-Sleep -Milliseconds 400
[System.Windows.Forms.SendKeys]::SendWait("^{ENTER}")
Write-Host "sent: $Prompt"

function Capture([string]$path) {
    & (Join-Path $PSScriptRoot "capture-window.ps1") -ProcessName LocalCode -OutputPath $path
}

$elapsed = 0
foreach ($at in $CaptureAt) {
    Start-Sleep -Seconds ([Math]::Max(0, $at - $elapsed))
    $elapsed = $at
    if ($ShotPrefix) { Capture "$ShotPrefix-t${at}.png" }
    if ($StopAfterFirstShot) {
        [System.Windows.Forms.SendKeys]::SendWait("{ESC}")
        Write-Host "sent Esc at t=$at"
        $StopAfterFirstShot = $false
    }
}

if ($StopAfterSeconds -gt 0) {
    Start-Sleep -Seconds ([Math]::Max(0, $StopAfterSeconds - $elapsed))
    [System.Windows.Forms.SendKeys]::SendWait("{ESC}")
    Write-Host "sent Esc at t=$StopAfterSeconds"
}

Start-Sleep -Seconds $SettleSeconds
if ($ShotPrefix) { Capture "$ShotPrefix-final.png" }
