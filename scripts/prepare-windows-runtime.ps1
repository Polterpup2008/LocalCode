param(
    [Parameter(Mandatory=$true)][string]$SourceDirectory,
    [string]$OutputDirectory = 'build\localcode-runtime-1.0',
    [string]$CompilerRuntimeDirectory = 'build\audit-release\bin',
    [Parameter(Mandatory=$true)][string]$LlamaLicense,
    [string]$NvidiaLicense = '',
    [string]$NvidiaLicenseSource = 'https://docs.nvidia.com/cuda/archive/13.3.0/eula/index.html',
    [ValidatePattern('^b[0-9]+$')][string]$UpstreamRevision = 'b10655',
    [ValidatePattern('^([a-fA-F0-9]{40})?$')][string]$UpstreamCommit = '',
    [string]$OptimizationReceipt = '',
    [string]$OptimizationPatch = ''
)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path $PSScriptRoot -Parent
$taskSource = (Resolve-Path -LiteralPath $SourceDirectory).Path
$taskCompiler = [IO.Path]::GetFullPath($CompilerRuntimeDirectory, $taskRoot)
$taskOutput = [IO.Path]::GetFullPath($OutputDirectory, $taskRoot)
if (Test-Path -LiteralPath $taskOutput) { throw 'Choose a new runtime output directory; existing files are never overwritten.' }
if (-not (Test-Path -LiteralPath $taskCompiler -PathType Container)) { throw 'Compiler runtime directory is missing.' }

function Assert-X64([string]$Path) {
    $taskStream = [IO.File]::OpenRead($Path)
    $taskReader = [IO.BinaryReader]::new($taskStream)
    try {
        if ($taskReader.ReadUInt16() -ne 0x5A4D) { throw "Not a PE file: $Path" }
        $taskStream.Position = 0x3C
        $taskPeOffset = $taskReader.ReadInt32()
        if ($taskPeOffset -lt 64 -or $taskPeOffset -gt $taskStream.Length - 26) { throw 'Invalid PE offset.' }
        $taskStream.Position = $taskPeOffset
        if ($taskReader.ReadUInt32() -ne 0x4550 -or $taskReader.ReadUInt16() -ne 0x8664) { throw "Not a Windows x64 binary: $Path" }
        $taskStream.Position = $taskPeOffset + 24
        if ($taskReader.ReadUInt16() -ne 0x20B) { throw "Not a PE32+ binary: $Path" }
    } finally { $taskReader.Dispose() }
}

$taskRequired = @('llama-server.exe','llama-server-impl.dll','llama-common.dll','llama.dll',
    'mtmd.dll','ggml.dll','ggml-base.dll','libomp.dll')
$taskCpu = @(Get-ChildItem -LiteralPath $taskSource -File |
    Where-Object { $_.Name -match '^ggml-cpu(?:-[A-Za-z0-9_-]+)?\.dll$' } |
    Sort-Object Name | ForEach-Object { $_.Name })
if (-not ($taskCpu -contains 'ggml-cpu-x64.dll')) { throw 'The portable x64 CPU backend is missing.' }
$taskCuda = @('ggml-cuda.dll','cublas64_13.dll','cublasLt64_13.dll','cudart64_13.dll')
$taskHasCuda = @($taskCuda | Where-Object { Test-Path -LiteralPath (Join-Path $taskSource $_) -PathType Leaf }).Count -gt 0
if ($taskHasCuda) { $taskRequired += $taskCuda }
$taskHasVulkan = Test-Path -LiteralPath (Join-Path $taskSource 'ggml-vulkan.dll') -PathType Leaf
if ($taskHasVulkan) {
    $taskRequired += 'ggml-vulkan.dll'
}
$taskRequired += $taskCpu
$taskMsvc = @('concrt140.dll','msvcp140.dll','msvcp140_1.dll','msvcp140_2.dll',
    'msvcp140_atomic_wait.dll','msvcp140_codecvt_ids.dll','vcruntime140.dll','vcruntime140_1.dll')
$taskInputs = @()
foreach ($taskName in $taskRequired) {
    $taskPath = Join-Path $taskSource $taskName
    if (-not (Test-Path -LiteralPath $taskPath -PathType Leaf)) { throw "Required runtime file is missing: $taskName" }
    Assert-X64 $taskPath
    $taskInputs += [pscustomobject]@{ name=$taskName; path=$taskPath; component='llama.cpp runtime' }
}
foreach ($taskName in $taskMsvc) {
    $taskPath = Join-Path $taskCompiler $taskName
    if (-not (Test-Path -LiteralPath $taskPath -PathType Leaf)) { throw "Required compiler runtime is missing: $taskName" }
    Assert-X64 $taskPath
    $taskInputs += [pscustomobject]@{ name=$taskName; path=$taskPath; component='MSVC redistributable' }
}
$taskMit = (Resolve-Path -LiteralPath $LlamaLicense).Path
if ((Get-Content -LiteralPath $taskMit -Raw) -notmatch 'MIT License') { throw 'Select the actual llama.cpp MIT license.' }
$taskLlvm = Join-Path $taskSource 'LICENSE-LLVM-OpenMP'
if (-not (Test-Path -LiteralPath $taskLlvm -PathType Leaf) -or
    (Get-Content -LiteralPath $taskLlvm -Raw) -notmatch 'LLVM Project') { throw 'The actual LLVM/OpenMP notice is missing.' }
$taskInputs += [pscustomobject]@{ name='LICENSE'; path=$taskMit; component='llama.cpp MIT notice' }
$taskInputs += [pscustomobject]@{ name='LICENSE-LLVM-OpenMP'; path=$taskLlvm; component='LLVM/OpenMP notice' }
$taskOptimization = $null
if ($OptimizationReceipt -or $OptimizationPatch) {
    if (-not $OptimizationReceipt -or -not $OptimizationPatch) { throw 'Supply both the optimization receipt and source patch.' }
    $taskReceiptPath = (Resolve-Path -LiteralPath $OptimizationReceipt).Path
    $taskPatchPath = (Resolve-Path -LiteralPath $OptimizationPatch).Path
    $taskOptimization = Get-Content -LiteralPath $taskReceiptPath -Raw | ConvertFrom-Json
    if ($taskOptimization.format -ne 1 -or $taskOptimization.id -ne 'localcode.windows-working-set' -or
        $taskOptimization.upstream_commit -ne $UpstreamCommit -or
        $taskOptimization.core_sha256 -ne (Get-FileHash -LiteralPath (Join-Path $taskSource 'llama.dll')).Hash.ToLowerInvariant() -or
        $taskOptimization.patch_sha256 -ne (Get-FileHash -LiteralPath $taskPatchPath).Hash.ToLowerInvariant() -or
        $taskOptimization.qualification_passed -ne $true -or $taskOptimization.memory_gate_passed -ne $true) {
        throw 'Optimization receipt does not bind this source, patch, qualified core and passed memory gate.'
    }
    $taskInputs += [pscustomobject]@{ name='localcode-optimization.json'; path=$taskReceiptPath; component='optimization provenance and qualification' }
    $taskInputs += [pscustomobject]@{ name='windows-working-set.patch'; path=$taskPatchPath; component='LocalCode source additions; upstream portions retain MIT terms' }
    $taskInputs += [pscustomobject]@{ name='LICENSE-LOCALCODE-MODIFICATIONS.txt'; path=(Join-Path $taskRoot 'LICENSE'); component='proprietary terms for LocalCode additions only' }
}
if ($taskHasCuda) {
    if (-not $NvidiaLicense -or -not (Test-Path -LiteralPath $NvidiaLicense -PathType Leaf)) { throw 'CUDA DLLs are present: provide the real NVIDIA CUDA EULA/redistribution notice; GPU files will not be silently omitted.' }
    $taskNotice = Get-Content -LiteralPath $NvidiaLicense -Raw
    $taskNoticeUrl = [uri]$NvidiaLicenseSource
    if ($taskNotice -notmatch 'Distribution Requirements' -or $taskNotice -notmatch 'cublas' -or
        $taskNotice -notmatch 'cudart' -or $taskNoticeUrl.Scheme -ne 'https' -or
        $taskNoticeUrl.Host -notin @('docs.nvidia.com','developer.nvidia.com','download.nvidia.com')) {
        throw 'Select the full CUDA EULA including redistribution terms from an official NVIDIA source.'
    }
    $taskInputs += [pscustomobject]@{ name='NVIDIA-CUDA-EULA.md'; path=(Resolve-Path -LiteralPath $NvidiaLicense).Path; component='NVIDIA CUDA notice' }
}
if (($taskInputs.name | Select-Object -Unique).Count -ne $taskInputs.Count) { throw 'Duplicate payload name.' }
New-Item -ItemType Directory -Path $taskOutput | Out-Null
$taskRecords = @()
foreach ($taskInput in $taskInputs) {
    $taskBefore = Get-Item -LiteralPath $taskInput.path
    $taskSourceHash = (Get-FileHash -LiteralPath $taskInput.path -Algorithm SHA256).Hash.ToLowerInvariant()
    $taskDestination = Join-Path $taskOutput $taskInput.name
    Copy-Item -LiteralPath $taskInput.path -Destination $taskDestination
    $taskHash = (Get-FileHash -LiteralPath $taskDestination -Algorithm SHA256).Hash.ToLowerInvariant()
    $taskAfter = Get-Item -LiteralPath $taskInput.path
    if ($taskHash -ne $taskSourceHash -or $taskBefore.Length -ne $taskAfter.Length -or
        $taskBefore.LastWriteTimeUtc -ne $taskAfter.LastWriteTimeUtc) { throw "Source/copy identity changed: $($taskInput.name). Partial output is preserved." }
    $taskVersion = (Get-Item -LiteralPath $taskDestination).VersionInfo
    $taskRecords += [ordered]@{ name=$taskInput.name; sha256=$taskHash; bytes=$taskAfter.Length;
        component=$taskInput.component; file_version=$taskVersion.FileVersion; product_version=$taskVersion.ProductVersion }
}
$taskNvidiaText = @"
NVIDIA CUDA redistribution notice source
$NvidiaLicenseSource
Retrieved for LocalCode packaging on $([DateTime]::UtcNow.ToString('yyyy-MM-dd')).

The full supplied EULA, CUDA supplement and third-party attachments govern the
CUDA components. Attachment A lists cudart, cublas and cublasLt, including
version/architecture-specific filenames. The distribution requirements include
material additional application functionality, access to SDK portions by the
application, consistent distribution terms, and no unlisted developer tools.
The CUDA supplement licenses CUDA application development for NVIDIA GPU systems.
The supplied text does not grant blanket permission for unrelated components.
NVIDIA GPU drivers are not included; this preparation does not install a driver.
MIT and LLVM/OpenMP are separate component licenses; this does not waive NVIDIA
distribution requirements for the package.
"@
$taskReadme = @"
# LocalCode bundled Windows x64 runtime

This is a selected, byte-verified copy of an existing local llama.cpp runtime,
declared upstream revision $UpstreamRevision (source commit $UpstreamCommit).
Source distribution directory: $taskSource
The source path is preparation provenance, never a runtime requirement.
No runtime was rebuilt, downloaded, benchmarked or executed by this script.
$(if ($taskOptimization) { 'The core includes the LocalCode Windows mapped-file working-set change. See localcode-optimization.json and windows-working-set.patch for its measured scope and provenance. No universal speed or compatibility claim is made.' } else { 'No custom optimization or target-PC compatibility/performance is claimed.' })

The payload includes the server and its implementation/common/core/multimodal
libraries, every supplied ggml CPU backend, OpenMP, and the available complete
CUDA dependency group, plus the Vulkan backend when present. It excludes benchmark/CLI/RPC helper programs, model
weights, model profiles, settings, logs and user conversations. MSVC runtime DLLs
are copied beside the server so its loader does not need a developer PATH.

llama.cpp is covered by LICENSE (MIT); OpenMP by LICENSE-LLVM-OpenMP.
$(if ($taskOptimization) { 'LocalCode additions are covered separately by LICENSE-LOCALCODE-MODIFICATIONS.txt. That license does not restrict rights in the original MIT-licensed portions.' })
CUDA DLLs, when present, have separate NVIDIA-CUDA-EULA.md and source notice;
their distribution remains subject to those terms. MSVC DLLs are Microsoft
redistributables and retain their own binary copyright/version information.
The operating system and compatible GPU driver remain machine prerequisites.
The manifest lists every payload file except the manifest itself, which cannot
contain its own hash. Source binaries remain untouched. Installed model loading
and clean-PATH startup are separate release checks, not performed here.
"@
$taskGenerated = [ordered]@{ 'README.md'=$taskReadme }
if ($taskHasCuda) { $taskGenerated['NVIDIA-CUDA-SOURCE.txt'] = $taskNvidiaText }
foreach ($taskName in $taskGenerated.Keys) {
    $taskPath = Join-Path $taskOutput $taskName
    [IO.File]::WriteAllText($taskPath, $taskGenerated[$taskName] + "`n", [Text.UTF8Encoding]::new($false))
    $taskRecords += [ordered]@{ name=$taskName; sha256=(Get-FileHash -LiteralPath $taskPath -Algorithm SHA256).Hash.ToLowerInvariant(); bytes=(Get-Item -LiteralPath $taskPath).Length; component='package documentation' }
}
if ($taskRecords.Count -gt 96) { throw 'Runtime payload exceeds the packaging file-count bound.' }
$taskManifest = [ordered]@{ format=1; id='localcode.runtime'; architecture='windows-x64';
    upstream_revision=$UpstreamRevision; upstream_commit=$UpstreamCommit;
    source_directory=$taskSource; source_kind=$(if ($taskOptimization) { 'upstream runtime with qualified LocalCode core modification' } else { 'existing local runtime distribution, copied unchanged' });
    custom_optimization_claimed=($null -ne $taskOptimization); runtime_execution_tested=$false;
    optimization=$(if ($taskOptimization) { $taskOptimization.id } else { $null });
    cuda_included=$taskHasCuda; vulkan_included=$taskHasVulkan; cpu_backend_count=$taskCpu.Count;
    license_status='notices included; redistribution subject to supplied component terms';
    nvidia_license_source=$(if ($taskHasCuda) { $NvidiaLicenseSource } else { $null });
    files=@($taskRecords | Sort-Object { $_.name }) }
$taskManifestPath = Join-Path $taskOutput 'manifest.json'
[IO.File]::WriteAllText($taskManifestPath, ($taskManifest | ConvertTo-Json -Depth 8) + "`n", [Text.UTF8Encoding]::new($false))
[pscustomobject]@{ output=$taskOutput; manifest=$taskManifestPath; manifestSha256=(Get-FileHash -LiteralPath $taskManifestPath -Algorithm SHA256).Hash.ToLowerInvariant(); files=$taskRecords.Count; cudaIncluded=$taskHasCuda; vulkanIncluded=$taskHasVulkan; cpuBackends=$taskCpu.Count; runtimeExecuted=$false }
