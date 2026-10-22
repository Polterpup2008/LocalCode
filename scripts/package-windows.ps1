param(
    [string]$BuildDirectory = 'build\audit-release',
    [string]$RuntimePackage = '',
    [string]$DiscordPackage = '',
    [string]$NSISCompiler = '',
    [string]$MemoryRuntimePackage = '',
    [string]$ReasoningRuntimePackage = ''
)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'msvc-env.ps1')
Push-Location $taskRoot
try {
    $taskBuild = [IO.Path]::GetFullPath($BuildDirectory, $taskRoot)
    if (-not $RuntimePackage) { throw 'Pass -RuntimePackage with the prepared bundled llama.cpp runtime directory.' }
    $taskRuntimePackage = (Resolve-Path -LiteralPath $RuntimePackage).Path
    $taskPlugin = if ($DiscordPackage) { (Resolve-Path -LiteralPath $DiscordPackage).Path } else { '' }
    $taskMemoryPackage = if ($MemoryRuntimePackage) { (Resolve-Path -LiteralPath $MemoryRuntimePackage).Path } else { '' }
    $taskReasoningPackage = if ($ReasoningRuntimePackage) { (Resolve-Path -LiteralPath $ReasoningRuntimePackage).Path } else { '' }
    & cmake -S $taskRoot -B $taskBuild -DCMAKE_BUILD_TYPE=Release -DLOCALCODE_INSTALL_DEVELOPER_TOOLS=OFF "-DLOCALCODE_LLAMA_RUNTIME_PACKAGE=$taskRuntimePackage" "-DLOCALCODE_DISCORD_PACKAGE=$taskPlugin" "-DLOCALCODE_MEMORY_RUNTIME_PACKAGE=$taskMemoryPackage" "-DLOCALCODE_REASONING_RUNTIME_PACKAGE=$taskReasoningPackage"
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
    $taskVersion = (Get-Content -LiteralPath (Join-Path $taskBuild 'LocalCodePackageVersion.txt') -Raw).Trim()
    if ($taskVersion -notmatch '^\d+\.\d+\.\d+$') { throw 'CMake did not provide a valid package version.' }
    & cmake --build $taskBuild --config Release --target localcode localcode-doctor -j 4
    if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
    # A fresh destination prevents stale/private files entering an artifact.
    $taskStage = Join-Path $taskBuild ('package-stage-' + [guid]::NewGuid().ToString('N'))
    & cmake --install $taskBuild --config Release --prefix $taskStage
    if ($LASTEXITCODE -ne 0) { throw 'Install staging failed.' }
    & cpack --config (Join-Path $taskBuild 'CPackConfig.cmake') -G ZIP -C Release
    if ($LASTEXITCODE -ne 0) { throw 'Portable ZIP packaging failed.' }
    $taskPackages = Join-Path $taskBuild 'packages'
    New-Item -ItemType Directory -Path $taskPackages -Force | Out-Null
    function Escape-Nsis([string]$Value) { $Value.Replace('$', '$$').Replace('"', '$\"') }
    $taskCore = [Collections.Generic.List[string]]::new()
    $taskDiscord = [Collections.Generic.List[string]]::new()
    $taskUninstall = [Collections.Generic.List[string]]::new()
    $taskDirectories = [Collections.Generic.HashSet[string]]::new()
    foreach ($taskFile in Get-ChildItem -LiteralPath $taskStage -File -Recurse | Sort-Object FullName) {
        $taskRelative = [IO.Path]::GetRelativePath($taskStage, $taskFile.FullName)
        if ($taskRelative.StartsWith('..') -or [IO.Path]::IsPathRooted($taskRelative)) { throw 'File escaped the staged package.' }
        $taskDirectory = Split-Path $taskRelative -Parent
        $taskDestination = $taskCore
        if ($taskRelative.StartsWith('plugins\discord\')) { $taskDestination = $taskDiscord }
        $taskDestination.Add('SetOutPath "$INSTDIR\' + (Escape-Nsis $taskDirectory) + '"')
        $taskDestination.Add('File "' + (Escape-Nsis $taskFile.FullName) + '"')
        $taskUninstall.Add('Delete "$INSTDIR\' + (Escape-Nsis $taskRelative) + '"')
        while ($taskDirectory) {
            $taskDirectories.Add($taskDirectory) | Out-Null
            $taskDirectory = Split-Path $taskDirectory -Parent
        }
    }
    foreach ($taskDirectory in $taskDirectories | Sort-Object Length -Descending) {
        $taskUninstall.Add('RMDir "$INSTDIR\' + (Escape-Nsis $taskDirectory) + '"')
    }
    if ($taskPlugin -and -not $taskDiscord.Count) {
        throw 'The supplied Discord package produced no installer component.'
    }
    $taskCorePath = Join-Path $taskPackages 'install-core.nsh'
    $taskDiscordPath = Join-Path $taskPackages 'install-discord.nsh'
    $taskUninstallPath = Join-Path $taskPackages 'uninstall-files.nsh'
    [IO.File]::WriteAllLines($taskCorePath, $taskCore, [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllLines($taskDiscordPath, $taskDiscord, [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllLines($taskUninstallPath, $taskUninstall, [Text.UTF8Encoding]::new($false))
    if (-not $NSISCompiler) {
        $taskCompiler = Get-Command makensis.exe -ErrorAction SilentlyContinue
        if ($taskCompiler) { $NSISCompiler = $taskCompiler.Source }
    }
    if (-not (Test-Path -LiteralPath $NSISCompiler -PathType Leaf)) {
        throw "Portable ZIP is ready in $taskPackages. Pass -NSISCompiler with makensis.exe to build the installer."
    }
    $taskInstaller = Join-Path $taskPackages "LocalCode-$taskVersion-Windows-x64-Setup.exe"
    $taskArguments = @('/V2', '/INPUTCHARSET', 'UTF8', "/DLC_OUTPUT=$taskInstaller",
        "/DLC_VERSION=$taskVersion", "/DLC_ICON=$(Join-Path $taskRoot 'packaging\windows\localcode.ico')",
        "/DLC_LICENSE=$(Join-Path $taskRoot 'LICENSE')", "/DLC_FILES_CORE=$taskCorePath",
        "/DLC_FILES_UNINSTALL=$taskUninstallPath")
    if ($taskDiscord.Count) { $taskArguments += "/DLC_FILES_PLUGIN=$taskDiscordPath" }
    $taskArguments += (Join-Path $taskRoot 'packaging\windows\LocalCode.nsi')
    & $NSISCompiler @taskArguments
    if ($LASTEXITCODE -ne 0) { throw 'NSIS installer compilation failed.' }
    Get-FileHash -LiteralPath $taskInstaller -Algorithm SHA256
    Write-Output "Staged files: $taskStage"
} finally { Pop-Location }
