# Runs the frozen Fibonacci benchmark and preserves everything needed to trust
# the numbers afterwards.
#
# Written because a set of raw baseline logs was cleaned out of a temporary
# directory once their metrics had been extracted, leaving figures that could no
# longer be re-derived. Results now go to a stable directory with the
# configuration that produced them: model file and hash, LocalCode commit,
# runtime build, environment overrides, timestamps, and the raw logs themselves.
#
# The benchmark itself is unchanged and must stay that way - changing it to make
# a number look better makes the number worthless.
param(
    [Parameter(Mandatory = $true)][string]$Model,
    [Parameter(Mandatory = $true)][string]$Label,
    [string]$Server  = "L:\ai\qwen38\runtimes\llama-b10655-cuda13.3\llama-server.exe",
    [int]   $Runs    = 10,
    [string]$ResultsRoot = "L:\ai\benchmark-results",
    # Extra environment for the spawned llama-server, "NAME=VALUE". Recorded in
    # the manifest so a configuration cannot be forgotten between series.
    [string[]]$Env = @(),
    [switch]$Trace,
    # Run even though another process is on the GPU. Only for a machine where
    # that is genuinely acceptable - it is not, for anything being compared.
    [switch]$AllowSharedGpu
)

$ErrorActionPreference = "Stop"

# --- The GPU has to be ours ------------------------------------------------
# A series was once started while a game was running. Decode fell from 29.4 to
# 10.6 tok/s and every throughput figure in the metric set was void, which is
# only discoverable afterwards by noticing the numbers look wrong. Whether the
# GPU was shared is a fact about the measurement, so it is checked before the
# series and recorded either way.
function Get-GpuState {
    $state = [ordered]@{ available = $false }
    try {
        $prev = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        $q = & nvidia-smi --query-gpu=name,memory.total,memory.used,driver_version `
                          --format=csv,noheader,nounits 2>&1 | Select-Object -First 1
        $apps = & nvidia-smi --query-compute-apps=pid,process_name `
                             --format=csv,noheader 2>&1
        $ErrorActionPreference = $prev
    } catch { $ErrorActionPreference = "Continue"; return $state }

    if (-not $q -or "$q" -match "not found|NVIDIA-SMI has failed") { return $state }
    $parts = ("$q" -split ',').Trim()
    $state.available      = $true
    $state.name           = $parts[0]
    $state.memory_total_mib = [int]$parts[1]
    $state.memory_used_mib  = [int]$parts[2]
    $state.driver_version   = $parts[3]
    $state.processes = @(
        foreach ($a in $apps) {
            $line = "$a".Trim()
            if ($line -and $line -notmatch "^\s*$" -and $line -notmatch "No running processes") {
                $f = ($line -split ',').Trim()
                [ordered]@{ pid = $f[0]; name = (Split-Path $f[1] -Leaf) }
            }
        })
    return $state
}

$gpu = Get-GpuState
if ($gpu.available -and $gpu.processes.Count -gt 0) {
    $who = ($gpu.processes | ForEach-Object { "$($_.name) (pid $($_.pid))" }) -join ", "
    $msg = "the GPU is already in use by: $who - $($gpu.memory_used_mib) of " +
           "$($gpu.memory_total_mib) MiB used. Timings measured against a shared " +
           "GPU are not comparable with anything."
    if ($AllowSharedGpu) {
        Write-Warning "$msg Continuing because -AllowSharedGpu was passed."
    } else {
        throw "$msg Close it, or pass -AllowSharedGpu to measure anyway."
    }
}

$DOCTOR = "L:\ai\LocalCode\build\dev\bin\localcode-doctor.exe"
$PROMPT = "Write a C++ program that prints the first ten Fibonacci numbers starting from 0, one per line. Build it and verify it actually prints them."

foreach ($p in @($Model, $Server, $DOCTOR)) {
    if (-not (Test-Path $p)) { throw "missing: $p" }
}

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$safe  = ($Label -replace '[^A-Za-z0-9._-]', '-')
$dir   = Join-Path $ResultsRoot "$stamp-$safe"
New-Item -ItemType Directory -Path (Join-Path $dir "logs") -Force | Out-Null
if ($Trace) { New-Item -ItemType Directory -Path (Join-Path $dir "traces") -Force | Out-Null }

Write-Output "results -> $dir"

# --- Manifest: what produced these numbers -------------------------------
$commit = (& git -C "L:\ai\LocalCode" rev-parse HEAD 2>$null)
$dirty  = (& git -C "L:\ai\LocalCode" status --porcelain 2>$null | Measure-Object -Line).Lines
# llama-server prints its version to stderr. Under $ErrorActionPreference=Stop
# a native command's stderr becomes a terminating NativeCommandError, so this is
# captured with the preference relaxed rather than by redirecting.
$serverVersion = "unknown"
try {
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $serverVersion = (& $Server --version 2>&1 | ForEach-Object { "$_" } | Select-Object -First 2) -join " "
    $ErrorActionPreference = $prev
} catch { $ErrorActionPreference = "Continue" }

Write-Output "hashing the model (this takes a moment for a multi-GB file) ..."
$modelHash = (Get-FileHash $Model -Algorithm SHA256).Hash

foreach ($pair in $Env) {
    $name, $value = $pair -split '=', 2
    Set-Item -Path "Env:\$name" -Value $value
}
if ($Trace) { $env:LOCALCODE_TRACE_DIR = (Join-Path $dir "traces") }

$manifest = [ordered]@{
    label            = $Label
    started_utc      = (Get-Date).ToUniversalTime().ToString("o")
    runs_requested   = $Runs
    benchmark_prompt = $PROMPT
    model_path       = $Model
    model_sha256     = $modelHash
    model_bytes      = (Get-Item $Model).Length
    server_path      = $Server
    server_version   = $serverVersion
    localcode_commit = $commit
    localcode_dirty_files = $dirty
    doctor_flags     = "--yes --max-steps 30"
    environment      = $Env
    tracing          = [bool]$Trace
    host             = $env:COMPUTERNAME
    gpu_at_start     = $gpu
    gpu_shared_allowed = [bool]$AllowSharedGpu
}
$manifest | ConvertTo-Json -Depth 6 | Out-File (Join-Path $dir "manifest.json") -Encoding utf8

# --- The runs -------------------------------------------------------------
$SB = Join-Path $env:TEMP "lc-bench-$stamp"
New-Item -ItemType Directory -Path $SB -Force | Out-Null

foreach ($i in 1..$Runs) {
    $ws = Join-Path $SB ("run{0:d2}" -f $i)
    if (Test-Path $ws) { Remove-Item -Recurse -Force $ws }
    New-Item -ItemType Directory -Path $ws | Out-Null

    # The fixture, byte for byte what every previous series used.
    @"
# Conventions

- C++ only. Build with CMake and the default generator; do not pass -G Ninja.
- Console programs, runnable from a command line so their output can be checked.
"@ | Out-File -FilePath (Join-Path $ws "AGENTS.md") -Encoding utf8 -NoNewline
    "# fib" | Out-File -FilePath (Join-Path $ws "README.md") -Encoding utf8 -NoNewline

    $log = Join-Path $dir "logs\run$i.log"
    & $DOCTOR agent $ws $Model $PROMPT --yes --max-steps 30 --server $Server *>&1 |
        Tee-Object -FilePath $log | Out-Null

    $verdict = (Select-String -Path $log -Pattern "^  (completed|verification) " |
                ForEach-Object { $_.Line.Trim() }) -join "  "
    Write-Output ("run {0,2}: {1}" -f $i, $verdict)
}

foreach ($pair in $Env) {
    $name, $null = $pair -split '=', 2
    Remove-Item -Path "Env:\$name" -ErrorAction SilentlyContinue
}
Remove-Item Env:\LOCALCODE_TRACE_DIR -ErrorAction SilentlyContinue

$manifest.finished_utc = (Get-Date).ToUniversalTime().ToString("o")
# Recorded again at the end: a process that arrived midway through would not
# show up in the starting check, and a series is only clean if it was clean
# throughout.
$manifest.gpu_at_end = Get-GpuState
$manifest | ConvertTo-Json -Depth 6 | Out-File (Join-Path $dir "manifest.json") -Encoding utf8

Write-Output ""
Write-Output "raw logs preserved in $dir\logs"
Write-Output "BENCHMARK_DONE $dir"
