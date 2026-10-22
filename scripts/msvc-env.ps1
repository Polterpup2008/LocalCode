# Imports the MSVC x64 developer environment into the current PowerShell session.
# Used by the build helpers and by CI-equivalent local runs.
param([string]$Arch = "x64")

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found; install Visual Studio 2022." }

$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw "No Visual Studio installation with the C++ toolset was found." }

# Visual Studio names these vcvars64 / vcvars32 / vcvarsarm64, not vcvarsx64.
$script = switch ($Arch) {
    "x64"   { "vcvars64.bat" }
    "x86"   { "vcvars32.bat" }
    "arm64" { "vcvarsarm64.bat" }
    default { "vcvars$Arch.bat" }
}
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\$script"
if (-not (Test-Path $vcvars)) { throw "$script not found at $vcvars" }

# Run vcvars in cmd and import the resulting environment.
$output = & "${env:ComSpec}" /s /c "`"$vcvars`" && set" 2>&1
foreach ($line in $output) {
    if ($line -match '^([^=]+)=(.*)$') {
        Set-Item -Path "Env:\$($matches[1])" -Value $matches[2] -ErrorAction SilentlyContinue
    }
}

# CMake and Ninja ship with Visual Studio; prefer them when nothing else is on PATH.
$vsCMake = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
$vsNinja = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
if (Test-Path $vsCMake) { $env:PATH = "$vsCMake;$env:PATH" }
if (Test-Path $vsNinja) { $env:PATH = "$vsNinja;$env:PATH" }

Write-Host "MSVC $Arch environment ready (VS at $vsPath)"
