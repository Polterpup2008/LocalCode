param(
    [Parameter(Mandatory=$true)][string]$SourceDirectory,
    [Parameter(Mandatory=$true)][string]$BuildDirectory
)
$ErrorActionPreference = 'Stop'
$taskSource = (Resolve-Path -LiteralPath $SourceDirectory).Path
$taskBuild = [IO.Path]::GetFullPath($BuildDirectory)
$taskCommit = '184f6536024668e8908ab8c86033d56708e62b40'
$taskPatch = Join-Path $PSScriptRoot 'windows-working-set.patch'
if (Test-Path -LiteralPath $taskBuild) { throw 'Choose a new build directory.' }
if ((git -C $taskSource rev-parse HEAD).Trim() -ne $taskCommit) { throw 'Use an unmodified llama.cpp b11829 checkout at the pinned commit.' }
if (git -C $taskSource status --porcelain) { throw 'The source checkout must be clean; use a dedicated checkout.' }
. (Join-Path $PSScriptRoot '..\..\scripts\msvc-env.ps1')
git -C $taskSource apply --check -- $taskPatch
if ($LASTEXITCODE -ne 0) { throw 'Patch check failed.' }
git -C $taskSource apply -- $taskPatch
if ($LASTEXITCODE -ne 0) { throw 'Patch application failed.' }
Copy-Item -LiteralPath (Join-Path $PSScriptRoot '..\..\LICENSE') -Destination (Join-Path $taskSource 'LICENSE-LOCALCODE-MODIFICATIONS.txt')
$taskArguments = @('-S',$taskSource,'-B',$taskBuild,'-G','Ninja',
    '-DCMAKE_BUILD_TYPE=Release','-DGGML_NATIVE=OFF','-DGGML_BACKEND_DL=ON',
    '-DGGML_OPENMP=OFF','-DGGML_CPU_ALL_VARIANTS=OFF','-DLLAMA_BUILD_IS_DEV=OFF',
    '-DLLAMA_BUILD_COMMON=OFF','-DLLAMA_BUILD_TESTS=OFF',
    '-DLLAMA_BUILD_TOOLS=OFF','-DLLAMA_BUILD_SERVER=OFF','-DLLAMA_BUILD_EXAMPLES=OFF',
    '-DLLAMA_BUILD_APP=OFF','-DLLAMA_OPENSSL=OFF')
cmake @taskArguments
if ($LASTEXITCODE -ne 0) { throw 'Core configure failed.' }
cmake --build $taskBuild --target llama --parallel 2
if ($LASTEXITCODE -ne 0) { throw 'Core build failed.' }
$taskTests = Join-Path $taskBuild 'mapped-page-tests'
New-Item -ItemType Directory -Path $taskTests | Out-Null
Copy-Item -LiteralPath (Join-Path $taskBuild 'bin\ggml-base.dll') -Destination $taskTests
$taskExecutable = Join-Path $taskTests 'test-mapped-pages.exe'
$taskCompile = @('/nologo','/O2','/MD','/EHsc','/std:c++17','/DGGML_SHARED','/D_WIN32_WINNT=0x0602',
    "/I$taskSource\src","/I$taskSource\include","/I$taskSource\ggml\include",
    (Join-Path $PSScriptRoot 'test_mmap_working_set.cpp'),(Join-Path $taskSource 'src\llama-mmap.cpp'),
    "/Fe:$taskExecutable","/Fo:$taskTests\",'/link',(Join-Path $taskBuild 'ggml\src\ggml-base.lib'),'psapi.lib')
cl @taskCompile
if ($LASTEXITCODE -ne 0) { throw 'Mapped-page test compilation failed.' }
& $taskExecutable (Join-Path $taskTests 'fixture.bin') (Join-Path $taskTests 'results.json')
if ($LASTEXITCODE -ne 0) { throw 'Mapped-page correctness failed.' }
$taskReceipt = [ordered]@{ upstream_commit=$taskCommit; patch_sha256=(Get-FileHash $taskPatch).Hash.ToLowerInvariant();
    core_sha256=(Get-FileHash (Join-Path $taskBuild 'bin\llama.dll')).Hash.ToLowerInvariant();
    configure=$taskArguments; native_mapped_page_tests='passed';
    scope='Core build and mapped-page tests only. Run live model qualification and matched timing before distribution.' }
[IO.File]::WriteAllText((Join-Path $taskBuild 'build-receipt.json'),($taskReceipt | ConvertTo-Json -Depth 5)+"`n",[Text.UTF8Encoding]::new($false))
Get-FileHash -LiteralPath (Join-Path $taskBuild 'bin\llama.dll')
