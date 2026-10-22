# Captures a screenshot of a window by process name. Used to verify the
# interface renders, and to produce the screenshots the README references.
#
# The process must be made DPI-aware before it asks Windows for a window rect:
# a DPI-unaware caller is handed virtualised coordinates, and the resulting
# bitmap is a crop of the real window rather than the whole of it.
param(
    [Parameter(Mandatory = $true)][string]$ProcessName,
    [Parameter(Mandatory = $true)][string]$OutputPath
)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win32Capture {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr c);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }
}
"@

# DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == -4. Falls back to the older
# process-wide call on Windows versions that do not have the thread-level one.
try { [Win32Capture]::SetThreadDpiAwarenessContext([IntPtr](-4)) | Out-Null } catch { }
try { [Win32Capture]::SetProcessDPIAware() | Out-Null } catch { }

$process = Get-Process -Name $ProcessName -ErrorAction Stop |
    Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $process) { throw "No visible window found for process '$ProcessName'." }

$handle = $process.MainWindowHandle
[Win32Capture]::ShowWindow($handle, 9) | Out-Null      # SW_RESTORE
[Win32Capture]::SetForegroundWindow($handle) | Out-Null
Start-Sleep -Milliseconds 900

$rect = New-Object Win32Capture+RECT
[Win32Capture]::GetWindowRect($handle, [ref]$rect) | Out-Null

$width = $rect.Right - $rect.Left
$height = $rect.Bottom - $rect.Top
if ($width -le 0 -or $height -le 0) { throw "The window reported a zero size." }

# PrintWindow asks the window to draw itself into our bitmap, rather than copying
# whatever pixels happen to be on screen at those coordinates. Windows refuses
# SetForegroundWindow to a background process, so a screen copy silently captures
# whatever is in front instead - which is both wrong and a way to photograph
# something the user never meant to share. PW_RENDERFULLCONTENT (2) is required
# for hardware-composited content.
$bitmap = New-Object System.Drawing.Bitmap $width, $height
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$hdc = $graphics.GetHdc()
$printed = [Win32Capture]::PrintWindow($handle, $hdc, 2)
$graphics.ReleaseHdc($hdc)
if (-not $printed) {
    $graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bitmap.Size)
    Write-Warning "PrintWindow failed; fell back to a screen copy, which may include other windows."
}
$graphics.Dispose()

$directory = Split-Path -Parent $OutputPath
if ($directory -and -not (Test-Path $directory)) { New-Item -ItemType Directory -Force $directory | Out-Null }

$bitmap.Save($OutputPath, [System.Drawing.Imaging.ImageFormat]::Png)
$bitmap.Dispose()
Write-Host "Captured ${width}x${height} to $OutputPath"
