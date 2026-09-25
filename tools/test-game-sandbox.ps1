# Headless filesystem/launch-refusal tests; never starts DSPGAME or edits its real installation.
$ErrorActionPreference = 'Stop'
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('DSPAASR-sandbox-test-' + [Guid]::NewGuid().ToString('N'))
$runner = Join-Path $PSScriptRoot 'game-sandbox.ps1'
function Require($Condition, [string]$Message) { if (!$Condition) { throw $Message } }
try {
    $game = Join-Path $temporary 'fake-game'
    $core = Join-Path $temporary 'fake-core'
    $package = Join-Path $temporary 'fake-package'
    $session = Join-Path $temporary 'session'
    $null = New-Item -ItemType Directory -Path (Join-Path $game 'Configs'),$core,$package
    $path = Join-Path $game 'Configs/path.txt'
    $original = [byte[]](0xEF,0xBB,0xBF,0x41,0x0D,0x0A)
    [IO.File]::WriteAllBytes($path, $original)
    [IO.File]::WriteAllBytes((Join-Path $core 'BepInEx.Preloader.dll'), [byte[]](1,2,3))
    [IO.File]::WriteAllBytes((Join-Path $package 'DSPAAMod.dll'), [byte[]](4,5,6))
    # Function interception affects only this child script scope: no unrelated running game
    # may make a filesystem unit test nondeterministic; Run still cannot reach Process.Start.
    function Get-Process { param($Name, $ErrorAction); return $null }
    & $runner -Action Prepare -GameDirectory $game -CoreDirectory $core -PackageDirectory $package -SessionDirectory $session | Out-Null
    $beforeHash = (Get-FileHash -LiteralPath $path).Hash
    $manifest = Get-Content (Join-Path $session 'sandbox.json') -Raw | ConvertFrom-Json
    Require ($manifest.OriginalSHA256 -eq $beforeHash) 'Preparation changed original game config'
    $failed = $false
    try { & $runner -Action Run -GameDirectory $game -SessionDirectory $session } catch {
        $failed = $_.Exception.Message -like '*not redirected*'
    }
    Require $failed 'Run did not refuse an unredirected data path before launch'
    Require (!(Test-Path (Join-Path $session 'process.json'))) 'Refused run created a game process receipt'
    [IO.File]::WriteAllText($path, $manifest.OverrideRoot + "`n", [Text.UTF8Encoding]::new($false))
    & $runner -Action Restore -GameDirectory $game -SessionDirectory $session
    Require ((Get-FileHash -LiteralPath $path).Hash -eq $beforeHash) 'Rollback did not preserve original BOM/newline bytes'
    & $runner -Action Restore -GameDirectory $game -SessionDirectory $session
    [IO.File]::WriteAllText($path, 'External owner changed this path')
    $externalHash = (Get-FileHash -LiteralPath $path).Hash
    $failed = $false
    try { & $runner -Action Restore -GameDirectory $game -SessionDirectory $session } catch {
        $failed = $_.Exception.Message -like '*changed externally*'
    }
    Require $failed 'Rollback accepted an external edit'
    Require ((Get-FileHash -LiteralPath $path).Hash -eq $externalHash) 'Rollback overwrote an external edit'
    $early = Join-Path $PSScriptRoot 'game-early.ps1'
    $plugins = Join-Path $game 'DSPGAME_Data/Plugins/x86_64'
    $null = New-Item -ItemType Directory -Path $plugins
    $boot = Join-Path $game 'DSPGAME_Data/boot.config'
    $library = Join-Path $temporary 'bootstrap-source.dll'
    $installed = Join-Path $plugins 'DSPAAPresentBootstrap.dll'
    [IO.File]::WriteAllBytes($library, [byte[]](7,8,9))
    $bootBytes = [byte[]](0xEF,0xBB,0xBF) + [Text.Encoding]::UTF8.GetBytes("test=1`r`n")
    foreach ($hadLibrary in @($false,$true)) {
        $earlySession = Join-Path $temporary "early-$hadLibrary"
        & $runner -Action Prepare -GameDirectory $game -CoreDirectory $core -PackageDirectory $package -SessionDirectory $earlySession | Out-Null
        [IO.File]::WriteAllBytes($boot, $bootBytes)
        if ($hadLibrary) { [IO.File]::WriteAllBytes($installed, [byte[]](10,11,12)) }
        elseif (Test-Path $installed) { Remove-Item -LiteralPath $installed }
        $bootHash = (Get-FileHash $boot).Hash
        $libraryHash = if ($hadLibrary) { (Get-FileHash $installed).Hash } else { '' }
        & $early -Action Prepare -SessionDirectory $earlySession -GameDirectory $game -BootstrapLibrary $library | Out-Null
        $e = Get-Content (Join-Path $earlySession 'early.json') -Raw | ConvertFrom-Json
        Require ((Get-FileHash $boot).Hash -eq $bootHash) 'Early Prepare edited boot.config'
        Require ((Get-FileHash $installed).Hash -eq (Get-FileHash $library).Hash) 'Early binary staging failed'
        [IO.File]::WriteAllBytes($boot, [byte[]]($bootBytes + [Text.Encoding]::UTF8.GetBytes($e.BootSetting + "`r`n")))
        Require ((Get-FileHash $boot).Hash -eq $e.BootActiveSHA256) 'Early active hash lost BOM/CRLF bytes'
        $failed = $false
        try { & $early -Action Run -SessionDirectory $earlySession -GameDirectory $game } catch {
            $failed = $_.Exception.Message -like '*not redirected*'
        }
        Require $failed 'Early Run reached a launch without the isolated game data path'
        Require (!(Test-Path (Join-Path $earlySession 'process.json'))) 'Early refusal wrote a game process receipt'
        Require ((Get-FileHash $boot).Hash -eq $bootHash) 'Early failed-run rollback did not restore exact boot bytes'
        if ($hadLibrary) { Require ((Get-FileHash $installed).Hash -eq $libraryHash) 'Early original DLL was not restored' }
        else { Require (!(Test-Path $installed)) 'Early temporary DLL was not removed' }
        & $early -Action Restore -SessionDirectory $earlySession -GameDirectory $game | Out-Null
        [IO.File]::WriteAllText($boot, 'External boot owner')
        $changedHash = (Get-FileHash $boot).Hash
        $failed = $false
        try { & $early -Action Restore -SessionDirectory $earlySession -GameDirectory $game } catch {
            $failed = $_.Exception.Message -like '*changed externally*'
        }
        Require $failed 'Early rollback did not refuse external boot edits'
        Require ((Get-FileHash $boot).Hash -eq $changedHash) 'Early rollback replaced an external boot edit'
    }
    $conflictSession = Join-Path $temporary 'early-conflict'
    & $runner -Action Prepare -GameDirectory $game -CoreDirectory $core -PackageDirectory $package -SessionDirectory $conflictSession | Out-Null
    [IO.File]::WriteAllBytes($boot, [byte[]]([byte[]](0xEF,0xBB,0xBF) + [Text.Encoding]::UTF8.GetBytes('xrsdk-pre-init-library=OtherProvider')))
    $conflictHash = (Get-FileHash $boot).Hash
    $failed = $false
    try { & $early -Action Prepare -SessionDirectory $conflictSession -GameDirectory $game -BootstrapLibrary $library } catch {
        $failed = $_.Exception.Message -like '*existing PreInit provider*'
    }
    Require $failed 'Early preparation did not reject an existing BOM-prefixed provider'
    Require ((Get-FileHash $boot).Hash -eq $conflictHash) 'Existing PreInit provider was modified'
    # The offline guard is an already transformed private game assembly, not a
    # plugin whose absence would silently allow normal Steam initialization.
    $managed = Join-Path $game 'DSPGAME_Data/Managed'
    $null = New-Item -ItemType Directory -Path $managed
    $offlineAssembly = Join-Path $managed 'Assembly-CSharp.dll'
    $offlineBytes = [byte[]](21,22,23)
    [IO.File]::WriteAllBytes($offlineAssembly, $offlineBytes)
    $offlineHash = (Get-FileHash $offlineAssembly).Hash
    $runtimeReceipt = Join-Path $game 'steam-free-runtime.json'
    @{SourceGameDirectory='different-original-game';SteamFreeAssemblySHA256=$offlineHash} | ConvertTo-Json | Set-Content $runtimeReceipt
    $offlineSession = Join-Path $temporary 'offline-session'
    $failed = $false
    try { & $runner -Action Prepare -GameDirectory $game -CoreDirectory $core -PackageDirectory $package -SessionDirectory $offlineSession } catch {
        $failed = $_.Exception.Message -like '*explicit pinned assembly*'
    }
    Require $failed 'An offline runtime was prepared as an ordinary Steam session'
    & $runner -Action Prepare -GameDirectory $game -CoreDirectory $core -PackageDirectory $package -SessionDirectory $offlineSession -SteamFreeAssemblySha256 $offlineHash | Out-Null
    $offlineManifest = Get-Content (Join-Path $offlineSession 'sandbox.json') -Raw | ConvertFrom-Json
    Require ($offlineManifest.SteamFreeAssemblySha256 -eq $offlineHash) 'Steam-free policy was not persisted'
    foreach ($failure in @('changed','missing','native-library','policy-missing')) {
        [IO.File]::WriteAllBytes($offlineAssembly, $offlineBytes)
        if ($failure -eq 'changed') { [IO.File]::WriteAllBytes($offlineAssembly, [byte[]](24,25)) }
        if ($failure -eq 'missing') { Remove-Item $offlineAssembly }
        $leakedLibrary = Join-Path $plugins 'steam_api64.dll'
        if ($failure -eq 'native-library') { [IO.File]::WriteAllBytes($leakedLibrary, [byte[]](26,27)) }
        if ($failure -eq 'policy-missing') {
            $offlineManifest.PSObject.Properties.Remove('SteamFreeAssemblySha256')
            $offlineManifest | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $offlineSession 'sandbox.json')
        }
        [IO.File]::WriteAllText($path, $offlineManifest.OverrideRoot + "`n", [Text.UTF8Encoding]::new($false))
        $failed = $false
        try { & $runner -Action Run -GameDirectory $game -SessionDirectory $offlineSession } catch {
            $failed = $_.Exception.Message -like '*Steam-free*'
        }
        Require $failed "Offline launch did not refuse $failure without repeated command-line policy"
        Require (!(Test-Path (Join-Path $offlineSession 'process.json'))) 'Unsafe offline run created a process receipt'
        Require ((Get-FileHash $path).Hash -eq $offlineManifest.OriginalSHA256) 'Offline refusal failed path rollback'
        if (Test-Path $leakedLibrary) { Remove-Item $leakedLibrary }
    }
    Write-Output 'Steam-free session pinning, changed/missing assemblies, native-library exclusion and absent-policy refusal passed.'
    Write-Output 'Early overlay: exact BOM/CRLF rollback, absent/existing DLL restoration, failed-run cleanup, idempotence and external/provider edit protection passed.'
    Write-Output 'Sandbox prepare/launch refusal, exact-byte rollback, idempotence and concurrent-edit protection passed; game launches=0.'
}
finally { if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Recurse -Force } }
