param(
    [Parameter(Mandatory=$true)][string]$Bench,
    [Parameter(Mandatory=$true)][string]$Model,
    [string[]]$Threads = @(),
    [ValidateRange(2,20)][int]$Repetitions = 3,
    [ValidateRange(32,8192)][int]$PromptTokens = 512,
    [ValidateRange(16,512)][int]$GeneratedTokens = 64,
    [ValidateRange(0,32768)][int]$HistoryTokens = 0,
    [ValidateSet('Normal','BelowNormal')][string]$Priority = 'Normal',
    [string]$EvidenceRoot = ''
)
$ErrorActionPreference = 'Stop'
$taskBench = (Resolve-Path -LiteralPath $Bench).Path
$taskModel = (Resolve-Path -LiteralPath $Model).Path
$taskModelName = Split-Path -Leaf $taskModel
$taskModelDirectory = Split-Path -Parent $taskModel
if ($taskModelName.Contains(',')) {
    throw 'llama-bench treats commas as model-list separators. Use a model filename without commas, such as model.gguf; this diagnostic does not change LocalCode model selection.'
}
$taskProcessors = @(Get-CimInstance Win32_Processor)
$taskLogical = ($taskProcessors | Measure-Object NumberOfLogicalProcessors -Sum).Sum
$taskPhysical = ($taskProcessors | Measure-Object NumberOfCores -Sum).Sum
$taskExplicitThreads = @(
    foreach ($taskThreadInput in $Threads) {
        foreach ($taskThreadText in $taskThreadInput.Split(',')) {
            $taskThreadValue = 0
            if (-not [int]::TryParse($taskThreadText.Trim(),[Globalization.NumberStyles]::Integer,
                    [Globalization.CultureInfo]::InvariantCulture,[ref]$taskThreadValue)) {
                throw 'Threads must be integers separated by commas, such as 2,4,8.'
            }
            $taskThreadValue
        }
    }
)
$taskCandidates = if ($taskExplicitThreads.Count) { $taskExplicitThreads } else {
    @([Math]::Min(2, $taskLogical), [Math]::Max(1, [Math]::Floor($taskPhysical / 2)),
      [Math]::Max(1, $taskPhysical), $taskLogical)
}
$taskThreads = @($taskCandidates | Select-Object -Unique)
if (-not $taskThreads.Count -or ($taskThreads | Where-Object { $_ -lt 1 -or $_ -gt $taskLogical })) {
    throw "Thread counts must be between 1 and the detected $taskLogical logical CPUs."
}
if (-not $EvidenceRoot) {
    $taskDataRoot = if ($env:LOCALCODE_HOME) { $env:LOCALCODE_HOME } else {
        Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'LocalCode'
    }
    $EvidenceRoot = Join-Path $taskDataRoot 'data\performance'
}
$taskEvidence = Join-Path ([IO.Path]::GetFullPath($EvidenceRoot)) ('cpu-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $taskEvidence -Force | Out-Null
$taskHelpOut = Join-Path $taskEvidence 'runtime-help.stdout.txt'
$taskHelpErr = Join-Path $taskEvidence 'runtime-help.stderr.txt'
$taskHelpProcess = $null
try {
    $taskHelpProcess = Start-Process -FilePath $taskBench -ArgumentList '--help' -PassThru -WindowStyle Hidden `
        -WorkingDirectory (Split-Path -Parent $taskBench) -RedirectStandardOutput $taskHelpOut -RedirectStandardError $taskHelpErr
    $taskHelpHandle = $taskHelpProcess.Handle
    if (-not $taskHelpProcess.WaitForExit(20000)) {
        $taskHelpProcess.Kill()
        $taskHelpProcess.WaitForExit()
        throw 'llama-bench --help timed out.'
    }
    if ($taskHelpProcess.ExitCode -ne 0) { throw 'llama-bench --help failed.' }
    $taskHelp = (Get-Content -LiteralPath $taskHelpOut -Raw -Encoding UTF8) + "`n" +
                (Get-Content -LiteralPath $taskHelpErr -Raw -Encoding UTF8)
} finally {
    if ($taskHelpProcess) { $taskHelpProcess.Dispose() }
}
foreach ($taskFlag in @('--device','--n-gpu-layers','--no-kv-offload','--threads','--n-depth','--output','--repetitions','--prio','--progress')) {
    if (-not $taskHelp.Contains($taskFlag)) { throw "This runtime does not advertise $taskFlag." }
}
Set-Content -LiteralPath (Join-Path $taskEvidence 'runtime-help.txt') -Value $taskHelp -Encoding UTF8
if ($taskModelName -match '[^\x20-\x7e]' -and -not $taskHelp.Contains('LocalCode Windows UTF-8 argv v1')) {
    throw 'This benchmark does not advertise UTF-8 filenames. Choose a compatible benchmark or an ASCII model filename; its containing directories may remain Unicode.'
}
# Keep all weights and KV on CPU; the online Discord GPU runtime stays separate.
# Only the CPU thread count varies within one invocation. Warmup is enabled.
$taskWorkerPriority = if ($Priority -eq 'BelowNormal') { -1 } else { 0 }
# Relative names keep Unicode folders usable even with older narrow-argv tools.
$taskArgumentTokens = @('-m', ('"' + $taskModelName + '"'), '--device', 'none', '-ngl', '0', '-nkvo', '1',
    '-t', ($taskThreads -join ','), '-p', "$PromptTokens", '-n', "$GeneratedTokens",
    '-d', "$HistoryTokens", '-r', "$Repetitions", '--prio', "$taskWorkerPriority", '--progress', '-o', 'json')
if ($taskHelp.Contains('--no-op-offload')) { $taskArgumentTokens += @('--no-op-offload','1') }
if ($taskHelp.Contains('--offline')) { $taskArgumentTokens += '--offline' }
$taskArguments = $taskArgumentTokens -join ' '
$taskOutput = Join-Path $taskEvidence 'samples.json'
# PDH's English counter API avoids localized performance-counter names.
# A query needs two collections; failures remain unavailable, never zero load.
$taskCpuSamplerError = $null
try {
    if (-not ('LocalCode.Diagnostics.CpuLoadSamplerV1' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace LocalCode.Diagnostics {
    public sealed class CpuLoadSamplerV1 : IDisposable {
        [StructLayout(LayoutKind.Explicit, Size=16)]
        private struct Value {
            [FieldOffset(0)] public uint Status;
            [FieldOffset(8)] public double Percent;
        }
        [DllImport("pdh.dll", CharSet=CharSet.Unicode, ExactSpelling=true)]
        private static extern uint PdhOpenQueryW(IntPtr source, UIntPtr data, out IntPtr query);
        [DllImport("pdh.dll", CharSet=CharSet.Unicode, ExactSpelling=true)]
        private static extern uint PdhAddEnglishCounterW(IntPtr query, string path, UIntPtr data, out IntPtr counter);
        [DllImport("pdh.dll", ExactSpelling=true)]
        private static extern uint PdhCollectQueryData(IntPtr query);
        [DllImport("pdh.dll", ExactSpelling=true)]
        private static extern uint PdhGetFormattedCounterValue(IntPtr counter, uint format, IntPtr type, out Value value);
        [DllImport("pdh.dll", ExactSpelling=true)]
        private static extern uint PdhCloseQuery(IntPtr query);
        private IntPtr query, counter;
        public CpuLoadSamplerV1() {
            try {
                Check(PdhOpenQueryW(IntPtr.Zero, UIntPtr.Zero, out query));
                Check(PdhAddEnglishCounterW(query, @"\Processor Information(_Total)\% Processor Time", UIntPtr.Zero, out counter));
                Check(PdhCollectQueryData(query));
            } catch { Dispose(); throw; }
        }
        private static void Check(uint status) {
            if (status != 0) throw new InvalidOperationException("PDH status 0x" + status.ToString("X8"));
        }
        public double Read() {
            if (query == IntPtr.Zero) throw new ObjectDisposedException("CpuLoadSamplerV1");
            Check(PdhCollectQueryData(query));
            Value value;
            Check(PdhGetFormattedCounterValue(counter, 0x200, IntPtr.Zero, out value));
            if ((value.Status != 0 && value.Status != 1) || Double.IsNaN(value.Percent) || Double.IsInfinity(value.Percent) || value.Percent < 0 || value.Percent > 100)
                throw new InvalidOperationException("Invalid PDH counter data");
            return value.Percent;
        }
        public void Dispose() {
            if (query != IntPtr.Zero) { PdhCloseQuery(query); query = IntPtr.Zero; }
        }
    }
}
'@
    }
} catch { $taskCpuSamplerError = $_.Exception.Message }
function Read-TaskCpuLoad($Sampler) {
    if (-not $Sampler) { return $null }
    try { return [Math]::Round($Sampler.Read(),3) } catch { return $null }
}
$taskComputer = Get-CimInstance Win32_ComputerSystem
$taskRecord = [ordered]@{
    status = 'starting'
    utc = [DateTime]::UtcNow.ToString('o')
    cpu = @($taskProcessors | Select-Object Name,NumberOfCores,NumberOfLogicalProcessors)
    cpuLoadAtDetectionPercent = ($taskProcessors | Measure-Object LoadPercentage -Average).Average
    ramBytes = $taskComputer.TotalPhysicalMemory
    runtime = $taskBench
    runtimeSha256 = (Get-FileHash -LiteralPath $taskBench -Algorithm SHA256).Hash
    helperSha256 = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash
    model = $taskModel
    modelSha256 = (Get-FileHash -LiteralPath $taskModel -Algorithm SHA256).Hash
    arguments = $taskArguments
    argumentScope = 'Common requested template; actual command arguments are recorded separately in threadRuns.'
    workingDirectory = $taskModelDirectory
    requestedThreadCounts = $taskThreads
    requestedThreadsInput = @($Threads)
    elapsedSeconds = 0
    observedPeakWorkingSetBytes = 0
    warmup = $true
    requestedProcessPriority = $Priority
    requestedWorkerPriority = $taskWorkerPriority
    observedProcessPriorities = @()
    cpuLoadSource = '\Processor Information(_Total)\% Processor Time via PDH English counter API'
    cpuLoadSamplerInitialisationError = $taskCpuSamplerError
    cpuLoadScope = 'Before each thread-count process; during each process includes the benchmark, warmup and other work. Samples do not identify individual prompt/generation repetitions.'
    threadRuns = @()
    osFileCache = 'uncontrolled; model read for checksum before launch, warmup enabled'
    ttftMeasured = $false
    qualityMeasured = $false
    limits = 'Synthetic llama-bench CPU results on this PC; not Discord latency or another PC. Other applications were not suspended.'
}
$taskEnvironment = Join-Path $taskEvidence 'environment.json'
$taskRecord | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $taskEnvironment -Encoding UTF8
$taskWatch = [Diagnostics.Stopwatch]::StartNew()
$taskProcess = $null
$taskPeakWorkingSet = 0L
$taskPriorities = [Collections.Generic.HashSet[string]]::new()
$taskAllSamples = [Collections.Generic.List[object]]::new()
$taskCpuSampler = $null
try {
    $taskRecord.status = 'running'
    foreach ($taskThreadCount in $taskThreads) {
        $taskRunDirectory = Join-Path $taskEvidence ('threads-' + $taskThreadCount)
        New-Item -ItemType Directory -Path $taskRunDirectory | Out-Null
        $taskRunTokens = $taskArgumentTokens.Clone()
        $taskRunTokens[9] = [string]$taskThreadCount
        $taskRunArguments = $taskRunTokens -join ' '
        $taskRunOutput = Join-Path $taskRunDirectory 'samples.json'
        $taskRun = [ordered]@{ threads=$taskThreadCount; arguments=$taskRunArguments; status='starting';
            cpuLoadBeforePercent=$null; cpuLoadSamples=@(); elapsedSeconds=0; counterError=$taskCpuSamplerError }
        $taskRecord.threadRuns += $taskRun
        $taskCpuSampler = $null
        if (-not $taskCpuSamplerError) {
            try { $taskCpuSampler = [LocalCode.Diagnostics.CpuLoadSamplerV1]::new() }
            catch { $taskRun.counterError = $_.Exception.Message }
        }
        if ($taskCpuSampler) {
            Start-Sleep -Milliseconds 1000
            $taskRun.cpuLoadBeforePercent = Read-TaskCpuLoad $taskCpuSampler
        }
        $taskRunWatch = [Diagnostics.Stopwatch]::StartNew()
        $taskProcess = Start-Process -FilePath $taskBench -ArgumentList $taskRunArguments -PassThru -WindowStyle Hidden -WorkingDirectory $taskModelDirectory `
            -RedirectStandardOutput $taskRunOutput -RedirectStandardError (Join-Path $taskRunDirectory 'runtime.log')
        $taskProcessHandle = $taskProcess.Handle
        $taskProcess.PriorityClass = $Priority
        $taskRun.status = 'running'
        do {
            $taskExited = $taskProcess.WaitForExit(1000)
            $taskRun.cpuLoadSamples += [ordered]@{ elapsedSeconds=[Math]::Round($taskRunWatch.Elapsed.TotalSeconds,3);
                utc=[DateTime]::UtcNow.ToString('o'); systemCpuPercent=(Read-TaskCpuLoad $taskCpuSampler) }
            $taskProcess.Refresh()
            if (-not $taskExited) {
                # The child may finish while the CPU counter is being collected.
                try {
                    $taskPeakWorkingSet = [Math]::Max($taskPeakWorkingSet, $taskProcess.PeakWorkingSet64)
                    $taskObservedPriority = $taskProcess.PriorityClass
                    if ($null -ne $taskObservedPriority) {
                        $taskPriorities.Add($taskObservedPriority.ToString()) | Out-Null
                    } elseif (-not $taskProcess.HasExited) {
                        throw 'Benchmark process priority was unavailable while it was running.'
                    }
                } catch {
                    if (-not $taskProcess.HasExited) { throw }
                }
                if ($taskProcess.HasExited) { $taskExited = $true }
            }
            if ($taskWatch.Elapsed.TotalMinutes -gt 15) { throw 'CPU benchmark exceeded its 15-minute bound.' }
        } while (-not $taskExited)
        if ($taskProcess.ExitCode -ne 0) { throw "CPU benchmark exited with $($taskProcess.ExitCode); see $taskRunDirectory/runtime.log." }
        $taskParsedSamples = Get-Content -LiteralPath $taskRunOutput -Raw -Encoding UTF8 | ConvertFrom-Json
        foreach ($taskSample in @($taskParsedSamples)) { $taskAllSamples.Add($taskSample) }
        $taskRunWatch.Stop()
        $taskRun.elapsedSeconds = [Math]::Round($taskRunWatch.Elapsed.TotalSeconds,3)
        $taskRun.status = 'complete'
        if ($taskCpuSampler) { $taskCpuSampler.Dispose(); $taskCpuSampler = $null }
        $taskProcess.Dispose(); $taskProcess = $null
        Write-Output "Completed CPU measurement with $taskThreadCount threads."
    }
    $taskSamples = @($taskAllSamples.ToArray())
    if (-not $taskSamples.Count -or ($taskSamples | Where-Object {
        $null -eq $_.n_gpu_layers -or $_.n_gpu_layers -ne 0 -or $_.avg_ts -le 0 -or
        $_.n_threads -notin $taskThreads
    })) {
        throw 'No valid CPU-only measurements were produced.'
    }
    ConvertTo-Json -InputObject $taskSamples -Depth 8 | Set-Content -LiteralPath $taskOutput -Encoding UTF8
    $taskRecord.status = 'complete'
} catch {
    $taskRecord.status = 'failed'
    $taskRecord.error = $_.Exception.Message
    if ($taskRun -and $taskRun.status -ne 'complete') { $taskRun.status = 'failed' }
    throw
} finally {
    if ($taskProcess -and -not $taskProcess.HasExited) { $taskProcess.Kill(); $taskProcess.WaitForExit() }
    if ($taskCpuSampler) { $taskCpuSampler.Dispose() }
    $taskWatch.Stop()
    $taskRecord.elapsedSeconds = [Math]::Round($taskWatch.Elapsed.TotalSeconds,3)
    $taskRecord.observedPeakWorkingSetBytes = $taskPeakWorkingSet
    $taskRecord.observedProcessPriorities = @($taskPriorities)
    $taskRecord | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $taskEnvironment -Encoding UTF8
    if ($taskProcess) { $taskProcess.Dispose() }
}
$taskSamples | Select-Object n_threads,n_prompt,n_gen,n_depth,avg_ts,stddev_ts | Format-Table -AutoSize
Write-Output "Raw measurements and provenance: $taskEvidence"
