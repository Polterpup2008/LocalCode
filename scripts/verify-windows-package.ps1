param(
    [Parameter(Mandatory=$true)][string]$Installer,
    [string]$SmokeModel='',
    [string]$ExpectedWorkerSha256='',
    [string]$ExpectedCacheHelperSha256='',
    [string]$ExpectedCpuHelperSha256='',
    [string]$ExpectedMemoryCoreSha256='',
    [string]$ExpectedReasoningCommonSha256='',
    [switch]$VerifyDiscordPlugin
)
$ErrorActionPreference = 'Stop'
if (($ExpectedWorkerSha256 -or $ExpectedCacheHelperSha256) -and -not $VerifyDiscordPlugin) { throw 'Worker/cache expectations require -VerifyDiscordPlugin.' }
if ($ExpectedWorkerSha256 -and $ExpectedWorkerSha256 -notmatch '^[0-9a-fA-F]{64}$') { throw 'ExpectedWorkerSha256 must be a SHA256 digest.' }
if ($ExpectedCacheHelperSha256 -and $ExpectedCacheHelperSha256 -notmatch '^[0-9a-fA-F]{64}$') { throw 'ExpectedCacheHelperSha256 must be a SHA256 digest.' }
if ($ExpectedCpuHelperSha256 -and $ExpectedCpuHelperSha256 -notmatch '^[0-9a-fA-F]{64}$') { throw 'ExpectedCpuHelperSha256 must be a SHA256 digest.' }
if ($ExpectedMemoryCoreSha256 -and $ExpectedMemoryCoreSha256 -notmatch '^[0-9a-fA-F]{64}$') { throw 'ExpectedMemoryCoreSha256 must be a SHA256 digest.' }
if ($ExpectedReasoningCommonSha256 -and $ExpectedReasoningCommonSha256 -notmatch '^[0-9a-fA-F]{64}$') { throw 'ExpectedReasoningCommonSha256 must be a SHA256 digest.' }
$taskRoot = Split-Path $PSScriptRoot -Parent
$taskInstaller = (Resolve-Path -LiteralPath $Installer).Path
$taskEvidence = Join-Path $taskRoot ('build\installer-smoke-' + [guid]::NewGuid().ToString('N'))
$taskInstall = Join-Path $taskEvidence 'Fresh PC é'
$taskUser = Join-Path $taskEvidence 'User data é'
$taskExpectedRoot = [IO.Path]::GetFullPath((Join-Path $taskRoot 'build')) + [IO.Path]::DirectorySeparatorChar
if (-not ([IO.Path]::GetFullPath($taskInstall)).StartsWith($taskExpectedRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Smoke installation escaped the workspace.' }
New-Item -ItemType Directory -Path $taskEvidence -Force | Out-Null
$taskInstallKey = 'HKCU:\Software\LocalCode'
$taskUninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\LocalCode'
$taskHadInstallKey = Test-Path -LiteralPath $taskInstallKey
$taskShortcutDirectory = Join-Path ([Environment]::GetFolderPath('Programs')) 'LocalCode'
if (Test-Path -LiteralPath $taskShortcutDirectory) { throw 'Fresh-install smoke requires an unused LocalCode shortcut folder; existing shortcuts will not be replaced.' }
if (Test-Path -LiteralPath $taskUninstallKey) { throw 'Fresh-install smoke requires no existing LocalCode uninstall registration.' }
$taskPreviousDir = (Get-ItemProperty -LiteralPath $taskInstallKey -ErrorAction SilentlyContinue).InstallDir
$taskPreviousUninstall = Get-ItemProperty -LiteralPath $taskUninstallKey -ErrorAction SilentlyContinue
$taskEnvironment = @{}
foreach ($taskKey in @('PATH','LOCALCODE_HOME','LCD_HOME','QT_PLUGIN_PATH','QT_QPA_PLATFORM_PLUGIN_PATH','QT_QPA_PLATFORM','LOCALCODE_LLAMA_SERVER','LOCALCODE_IK_LLAMA_SERVER','LOCALCODE_SKILLS_DIR','DISCORD_BOT_TOKEN')) {
    $taskEnvironment[$taskKey] = [Environment]::GetEnvironmentVariable($taskKey, 'Process')
}
function Run-Owned([string]$Executable, [string]$Arguments, [string]$Log, [string]$CompilerRuntimeRoot='', [int]$TimeoutMilliseconds=45000) {
    $taskChild = Start-Process -FilePath $Executable -ArgumentList $Arguments -PassThru -WindowStyle Hidden -RedirectStandardOutput $Log -RedirectStandardError ($Log + '.stderr')
    if ($CompilerRuntimeRoot) {
        try {
            $taskRequired=@('msvcp140.dll','vcruntime140.dll','vcruntime140_1.dll','qt6core.dll','qt6gui.dll','qt6widgets.dll')
            $taskRuntimeModules=@{}
            $taskDeadline=[DateTime]::UtcNow.AddSeconds(5)
            $taskRuntimePrefix=[IO.Path]::GetFullPath($CompilerRuntimeRoot).TrimEnd('\')+'\'
            while ([DateTime]::UtcNow -lt $taskDeadline -and -not $taskChild.HasExited) {
                try {
                    $taskObserved=Get-Process -Id $taskChild.Id -ErrorAction Stop
                    if ($taskObserved.StartTime -ne $taskChild.StartTime) { throw 'Package-check process identity changed.' }
                    foreach ($taskModule in $taskObserved.Modules) {
                        $taskModuleName=$taskModule.ModuleName.ToLowerInvariant()
                        if ($taskRequired -contains $taskModuleName) {
                            if (-not $taskModule.FileName.StartsWith($taskRuntimePrefix,[StringComparison]::OrdinalIgnoreCase)) {
                                throw "Required runtime loaded outside the installation: $taskModuleName"
                            }
                            $taskRuntimeModules[$taskModuleName]=$taskModule.FileName
                        }
                    }
                    if ($taskRuntimeModules.Count -eq $taskRequired.Count) { break }
                } catch [System.ComponentModel.Win32Exception] {
                    # Retry only while the loader initializes this owned child.
                }
                Start-Sleep -Milliseconds 50
            }
            if ($taskRuntimeModules.Count -ne $taskRequired.Count) { throw 'Required Qt/compiler runtime modules were not observed in the installation.' }
            $taskRuntimeModules | ConvertTo-Json | Set-Content -LiteralPath ($Log+'.modules.json')
        } catch {
            if (-not $taskChild.HasExited) { $taskChild.Kill(); $taskChild.WaitForExit() }
            throw
        }
    }
    if (-not $taskChild.WaitForExit($TimeoutMilliseconds)) { $taskChild.Kill(); throw "Package check timed out: $Executable" }
    if ($taskChild.ExitCode -ne 0) { throw "Package check failed ($($taskChild.ExitCode)): $Executable" }
}
try {
    Run-Owned $taskInstaller "/S /D=$taskInstall" (Join-Path $taskEvidence 'install.log')
    [Environment]::SetEnvironmentVariable('PATH', "$env:WINDIR\System32", 'Process')
    foreach ($taskKey in @('QT_PLUGIN_PATH','QT_QPA_PLATFORM_PLUGIN_PATH','QT_QPA_PLATFORM','LOCALCODE_LLAMA_SERVER','LOCALCODE_IK_LLAMA_SERVER','LOCALCODE_SKILLS_DIR','DISCORD_BOT_TOKEN')) {
        [Environment]::SetEnvironmentVariable($taskKey, $null, 'Process')
    }
    $env:LOCALCODE_HOME = $taskUser
    $env:LCD_HOME = Join-Path $taskUser 'discord-one'
    Run-Owned (Join-Path $taskInstall 'bin\localcode-doctor.exe') 'skills' (Join-Path $taskEvidence 'skills.log')
    $taskRuntime = Join-Path $taskInstall 'bin\llama.cpp.localcode'
    $taskRuntimeManifest = Get-Content -LiteralPath (Join-Path $taskRuntime 'manifest.json') -Raw | ConvertFrom-Json
    if ($taskRuntimeManifest.id -ne 'localcode.runtime') { throw 'Bundled runtime identity is missing.' }
    foreach ($taskRuntimeFile in $taskRuntimeManifest.files) {
        if ($taskRuntimeFile.name -notmatch '^[A-Za-z0-9][A-Za-z0-9_.-]*$') { throw 'Unsafe bundled runtime file name.' }
        $taskRuntimePath = Join-Path $taskRuntime $taskRuntimeFile.name
        if ((Get-FileHash -LiteralPath $taskRuntimePath -Algorithm SHA256).Hash -ne $taskRuntimeFile.sha256) {
            throw "Installed runtime digest mismatch: $($taskRuntimeFile.name)"
        }
    }
    Run-Owned (Join-Path $taskInstall 'bin\localcode-doctor.exe') 'backends' (Join-Path $taskEvidence 'backends.log')
    if (-not (Select-String -LiteralPath (Join-Path $taskEvidence 'backends.log') -SimpleMatch 'llama.cpp.localcode bundled with LocalCode' -Quiet)) {
        throw 'A fresh installation did not discover its bundled default runtime.'
    }
    if (Get-ChildItem -LiteralPath $taskInstall -Recurse -File | Where-Object {
        $_.Extension -in @('.sqlite3','.gguf','.jsonl','.log') -or $_.Name -match '^localcode_.*tests[.]exe$'
    }) { throw 'The installation contains private data, model weights or test outputs.' }
    if (Test-Path -LiteralPath (Join-Path $taskInstall 'share\localcode\tools')) {
        throw 'The normal package contains developer support tools.'
    }
    $taskNativeSmoke = $false
    if ($SmokeModel) {
        $taskSmokeModel = (Resolve-Path -LiteralPath $SmokeModel).Path
        $taskSmokeProject = Join-Path $taskEvidence 'Native model smoke'
        New-Item -ItemType Directory -Path $taskSmokeProject | Out-Null
        $taskNativeArguments = "agent `"$taskSmokeProject`" `"$taskSmokeModel`" `"Reply with exactly READY. Do not call tools.`" --ctx 12288 --gpu-layers -1 --max-steps 2 --time-budget 2 --read-only"
        Run-Owned (Join-Path $taskInstall 'bin\localcode-doctor.exe') $taskNativeArguments (Join-Path $taskEvidence 'native-model.log') '' 150000
        if (-not (Select-String -LiteralPath (Join-Path $taskEvidence 'native-model.log') -Pattern '\bREADY\b' -Quiet)) {
            throw 'The bundled runtime did not return the requested literal model reply.'
        }
        $taskNativeSmoke = $true
    }
    [pscustomobject]@{ bundledRuntimeVerified=$true; nativeInferenceChecked=$taskNativeSmoke;
        defaultServer=(Join-Path $taskRuntime 'llama-server.exe'); runtimeFiles=$taskRuntimeManifest.files.Count } |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $taskEvidence 'runtime-result.json')
    $taskGuiArguments = "--diagnostic-snapshot `"$(Join-Path $taskEvidence 'startup.png')`""
    if ($VerifyDiscordPlugin) { $taskGuiArguments = '--plugins ' + $taskGuiArguments }
    Run-Owned (Join-Path $taskInstall 'bin\LocalCode.exe') $taskGuiArguments (Join-Path $taskEvidence 'gui.log') (Join-Path $taskInstall 'bin')
    if (-not (Test-Path -LiteralPath (Join-Path $taskEvidence 'startup.png') -PathType Leaf)) { throw 'Installed GUI startup snapshot is missing.' }
    $taskWorkerHash = $null
    $taskHelperHash = $null
    if ($VerifyDiscordPlugin) {
        if (-not (Select-String -LiteralPath (Join-Path $taskEvidence 'gui.log') -Pattern '^plugin panel initialized$' -Quiet)) {
            throw 'The installed native panel did not initialize the bundled Discord worker.'
        }
        $taskGuiConfig = Join-Path $taskUser 'data\plugins\discord\instances\default\config\bot.toml'
        if (-not (Test-Path -LiteralPath $taskGuiConfig -PathType Leaf)) { throw 'The native panel did not create its private bot home.' }
        Run-Owned (Join-Path $taskInstall 'plugins\discord\lcd.exe') 'plugin init' (Join-Path $taskEvidence 'plugin-init.json')
        Run-Owned (Join-Path $taskInstall 'plugins\discord\lcd.exe') 'plugin settings' (Join-Path $taskEvidence 'plugin-settings.json')
        $taskSettings = Get-Content -LiteralPath (Join-Path $taskEvidence 'plugin-settings.json') -Raw | ConvertFrom-Json
        if (-not $taskSettings.ok -or $taskSettings.result.guildIds -or $taskSettings.result.tokenConfigured) { throw 'The fresh package contains unexpected bot configuration.' }
        $taskWorkerHash=(Get-FileHash -LiteralPath (Join-Path $taskInstall 'plugins\discord\lcd.exe')).Hash
        if ($ExpectedWorkerSha256 -and $taskWorkerHash -ne $ExpectedWorkerSha256) { throw 'Installed worker hash differs from the intended build.' }
        $taskHelper=Join-Path $taskInstall 'plugins\discord\benchmark-prompt-cache.ps1'
        $taskHelperHash=if (Test-Path -LiteralPath $taskHelper -PathType Leaf) { (Get-FileHash -LiteralPath $taskHelper).Hash } else { $null }
        if ($ExpectedCacheHelperSha256 -and $taskHelperHash -ne $ExpectedCacheHelperSha256) { throw 'Installed cache helper is missing or differs from the intended package.' }
    }
    $taskCpuHelper = Join-Path $taskInstall 'share\localcode\tools\benchmark-cpu.ps1'
    $taskCpuHelperHash = if (Test-Path -LiteralPath $taskCpuHelper -PathType Leaf) { (Get-FileHash -LiteralPath $taskCpuHelper).Hash } else { $null }
    if ($ExpectedCpuHelperSha256 -and ($taskCpuHelperHash -ne $ExpectedCpuHelperSha256 -or
        -not (Test-Path -LiteralPath (Join-Path $taskInstall 'share\localcode\tools\CPU_MEASUREMENT.md') -PathType Leaf))) {
        throw 'Installed CPU helper or guide is missing or differs from the intended package.'
    }
    $taskAppHash=(Get-FileHash -LiteralPath (Join-Path $taskInstall 'bin\LocalCode.exe')).Hash
    $taskMemoryCore = Join-Path $taskInstall 'bin\runtime-memory\llama.dll'
    $taskMemoryHash = if (Test-Path -LiteralPath $taskMemoryCore -PathType Leaf) { (Get-FileHash -LiteralPath $taskMemoryCore).Hash } else { $null }
    if ($ExpectedMemoryCoreSha256 -and $taskMemoryHash -ne $ExpectedMemoryCoreSha256) { throw 'Installed RAM runtime core is missing or differs from the qualified package.' }
    if ($ExpectedMemoryCoreSha256) {
        foreach ($taskMemoryNotice in @('manifest.json','LICENSE','README.md','windows-mmap-working-set.patch')) {
            if (-not (Test-Path -LiteralPath (Join-Path $taskInstall "bin\runtime-memory\$taskMemoryNotice") -PathType Leaf)) { throw 'Installed RAM runtime provenance is incomplete.' }
        }
    }
    $taskInstallerHash=(Get-FileHash -LiteralPath $taskInstaller).Hash
    $taskReasoningDirectory=Join-Path $taskInstall 'bin\runtime-reasoning'
    $taskReasoningCommon=Join-Path $taskReasoningDirectory 'llama-common.dll'
    $taskReasoningHash=if (Test-Path -LiteralPath $taskReasoningCommon -PathType Leaf) { (Get-FileHash -LiteralPath $taskReasoningCommon).Hash } else { $null }
    if ($ExpectedReasoningCommonSha256) {
        if ($taskReasoningHash -ne $ExpectedReasoningCommonSha256) { throw 'Installed reasoning repair differs from the qualified package.' }
        foreach ($taskReasoningNotice in @('manifest.json','LICENSE','README.md','reasoning-prefill.patch','OPENSSL-LICENSE.txt')) {
            if (-not (Test-Path -LiteralPath (Join-Path $taskReasoningDirectory $taskReasoningNotice) -PathType Leaf)) { throw 'Installed reasoning runtime provenance is incomplete.' }
        }
        $taskReasoningManifest=Get-Content -LiteralPath (Join-Path $taskReasoningDirectory 'manifest.json') -Raw | ConvertFrom-Json
        foreach ($taskReasoningFile in $taskReasoningManifest.files) {
            if ($taskReasoningFile.name -notmatch '^[A-Za-z0-9][A-Za-z0-9_.-]*\.dll$') { throw 'Invalid reasoning runtime file name.' }
            $taskReasoningPath=Join-Path $taskReasoningDirectory $taskReasoningFile.name
            if ((Get-FileHash -LiteralPath $taskReasoningPath).Hash -ne $taskReasoningFile.sha256) { throw 'Installed reasoning dependency differs from its manifest.' }
        }
    }
    $taskDatabase = Join-Path $taskUser 'data\localcode.sqlite3'
    if (-not (Test-Path -LiteralPath $taskDatabase -PathType Leaf)) { throw 'First launch did not create its private conversation/settings database.' }
    $taskAdded = Join-Path $taskInstall 'preserve-me.txt'
    Set-Content -LiteralPath $taskAdded -Value 'Added after installation; must survive uninstall.'
    $taskPersonal = Join-Path $taskUser 'keep-private.txt'
    Set-Content -LiteralPath $taskPersonal -Value 'Independent user data.'
    $taskDataBeforeUpdate = @{}
    foreach ($taskFile in Get-ChildItem -LiteralPath $taskUser -Recurse -File) {
        $taskDataBeforeUpdate[$taskFile.FullName] = (Get-FileHash -LiteralPath $taskFile.FullName).Hash
    }
    Run-Owned $taskInstaller "/S /D=$taskInstall" (Join-Path $taskEvidence 'update.log')
    foreach ($taskEntry in $taskDataBeforeUpdate.GetEnumerator()) {
        if (-not (Test-Path -LiteralPath $taskEntry.Key -PathType Leaf) -or
            (Get-FileHash -LiteralPath $taskEntry.Key).Hash -ne $taskEntry.Value) {
            throw 'Installer update modified or removed persistent user data.'
        }
    }
    if (-not (Test-Path -LiteralPath $taskAdded -PathType Leaf)) { throw 'Installer update removed a user-added installation file.' }
    $taskVersion = (Get-Item -LiteralPath (Join-Path $taskInstall 'bin\LocalCode.exe')).VersionInfo.ProductVersion
    $taskRegisteredVersion = (Get-ItemProperty -LiteralPath $taskUninstallKey).DisplayVersion
    if (-not $taskVersion -or $taskRegisteredVersion -ne $taskVersion) { throw 'Installed executable and uninstall entry versions differ.' }
    Run-Owned (Join-Path $taskInstall 'bin\LocalCode.exe') "--diagnostic-snapshot `"$(Join-Path $taskEvidence 'restart.png')`"" (Join-Path $taskEvidence 'restart.log') (Join-Path $taskInstall 'bin')
    if (-not (Test-Path -LiteralPath (Join-Path $taskEvidence 'restart.png') -PathType Leaf)) { throw 'Updated application did not restart.' }
    Run-Owned (Join-Path $taskInstall 'Uninstall.exe') "/S _?=$taskInstall" (Join-Path $taskEvidence 'uninstall.log')
    if (Test-Path -LiteralPath (Join-Path $taskInstall 'bin\LocalCode.exe')) { throw 'Uninstaller left the installed application.' }
    if ($ExpectedMemoryCoreSha256 -and (Test-Path -LiteralPath $taskMemoryCore)) { throw 'Uninstaller left its RAM runtime core.' }
    if ($ExpectedReasoningCommonSha256 -and (Test-Path -LiteralPath $taskReasoningCommon)) { throw 'Uninstaller left its reasoning runtime.' }
    if ($ExpectedCpuHelperSha256 -and (Test-Path -LiteralPath $taskCpuHelper)) { throw 'Uninstaller left the installed CPU helper.' }
    if (-not (Test-Path -LiteralPath $taskAdded) -or -not (Test-Path -LiteralPath $taskPersonal) -or -not (Test-Path -LiteralPath $taskDatabase)) { throw 'Uninstaller removed a file outside its installed manifest.' }
    [pscustomobject]@{ passed=$true; installer=$taskInstaller; installerSha256=$taskInstallerHash; installedAppSha256=$taskAppHash; installedWorkerSha256=$taskWorkerHash; cacheHelperSha256=$taskHelperHash; cpuHelperSha256=$taskCpuHelperHash; memoryCoreSha256=$taskMemoryHash; reasoningCommonSha256=$taskReasoningHash; isolatedPath=$taskInstall; evidence=$taskEvidence; version=$taskVersion; nativeGuiInitialised=$true; discordPluginChecked=[bool]$VerifyDiscordPlugin; updatePreservedUserData=$true; restartPassed=$true; restrictedPath=$true; nonAsciiPath=$true; addedFilePreserved=$true; userDataPreserved=$true } |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $taskEvidence 'result.json')
    Write-Output "Package checks passed. Evidence: $taskEvidence"
} finally {
    foreach ($taskEntry in $taskEnvironment.GetEnumerator()) { [Environment]::SetEnvironmentVariable($taskEntry.Key, $taskEntry.Value, 'Process') }
    if ($null -ne $taskPreviousDir) {
        New-Item -Path $taskInstallKey -Force | Out-Null
        Set-ItemProperty -LiteralPath $taskInstallKey -Name InstallDir -Value $taskPreviousDir
    }
    if ($taskHadInstallKey -and -not (Test-Path -LiteralPath $taskInstallKey)) { New-Item -Path $taskInstallKey -Force | Out-Null }
    if ($taskPreviousUninstall) {
        New-Item -Path $taskUninstallKey -Force | Out-Null
        foreach ($taskProperty in $taskPreviousUninstall.PSObject.Properties) {
            if (-not $taskProperty.Name.StartsWith('PS')) { Set-ItemProperty -LiteralPath $taskUninstallKey -Name $taskProperty.Name -Value $taskProperty.Value }
        }
    }
}
