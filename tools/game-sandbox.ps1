[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('Prepare','Run','Restore')][string]$Action,
    [Parameter(Mandatory)][string]$SessionDirectory,
    [string]$GameDirectory = 'E:/SteamLibrary/steamapps/common/Dyson Sphere Program',
    [string]$CoreDirectory = '',
    [string]$PackageDirectory = '',
    [ValidateRange(60,3600)][int]$MaximumSeconds = 1200
)
# Explicit developer workflow, never called by build/CTest/package. No deployment
# into an existing profile. Configs/path.txt is edited separately and explicitly.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$session = [IO.Path]::GetFullPath($SessionDirectory, $root)
$game = [IO.Path]::GetFullPath($GameDirectory, $root)
$pathFile = Join-Path $game 'Configs/path.txt'
$manifestPath = Join-Path $session 'sandbox.json'
function Get-Hash([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }
function Restore-SandboxPath($Manifest) {
    if (!(Test-Path -LiteralPath $pathFile -PathType Leaf)) { throw 'Owned path override disappeared; do not overwrite another edit.' }
    $current = [IO.File]::ReadAllText($pathFile).Trim().Replace('\','/')
    if ($current -ne $Manifest.OverrideRoot) {
        if ((Get-Hash $pathFile) -eq $Manifest.OriginalSHA256) { return }
        throw 'Game path override changed externally; refusing to overwrite it. Original bytes remain in path.before.'
    }
    $backup = Join-Path $session 'path.before'
    if ((Get-Hash $backup) -ne $Manifest.OriginalSHA256) { throw 'Original path backup hash changed.' }
    # Byte-preserving rollback of the separately edited game config, not a rewrite.
    Copy-Item -LiteralPath $backup -Destination $pathFile
    if ((Get-Hash $pathFile) -ne $Manifest.OriginalSHA256) { throw 'Path restoration verification failed.' }
    [ordered]@{Restored=$true; Path=$pathFile; SHA256=$Manifest.OriginalSHA256; UTC=[DateTime]::UtcNow.ToString('o')} |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $session 'restoration.json') -Encoding utf8NoBOM
}
if ($Action -eq 'Prepare') {
    if (Get-Process -Name DSPGAME -ErrorAction SilentlyContinue) { throw 'Close the game before preparing a sandbox.' }
    if (Test-Path -LiteralPath $session) { throw 'Refusing an existing session directory.' }
    if (!$CoreDirectory -or !$PackageDirectory) { throw 'Prepare requires the exact core and validated package directories.' }
    $core = [IO.Path]::GetFullPath($CoreDirectory, $root)
    $package = [IO.Path]::GetFullPath($PackageDirectory, $root)
    if (!(Test-Path -LiteralPath (Join-Path $core 'BepInEx.Preloader.dll')) -or
        !(Test-Path -LiteralPath (Join-Path $package 'DSPAAMod.dll')) -or
        !(Test-Path -LiteralPath $pathFile -PathType Leaf)) { throw 'Required core/package/original path config is missing.' }
    $null = New-Item -ItemType Directory -Path $session
    Copy-Item -LiteralPath $pathFile -Destination (Join-Path $session 'path.before')
    $coreTarget = Join-Path $session 'BepInEx/core'
    $pluginTarget = Join-Path $session 'BepInEx/plugins/DSPAASR'
    $null = New-Item -ItemType Directory -Path $coreTarget,$pluginTarget,(Join-Path $session 'game-data')
    Get-ChildItem -LiteralPath $core -File | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $coreTarget }
    $copied = @()
    Get-ChildItem -LiteralPath $package -File | ForEach-Object {
        $target = Join-Path $pluginTarget $_.Name
        Copy-Item -LiteralPath $_.FullName -Destination $target
        $hash = Get-Hash $_.FullName
        if ((Get-Hash $target) -ne $hash) { throw 'Sandbox payload copy mismatch.' }
        $copied += [ordered]@{Name=$_.Name; SHA256=$hash}
    }
    $manifest = [ordered]@{GameDirectory=$game; SessionDirectory=$session; OverrideRoot=((Join-Path $session 'game-data').Replace('\','/') + '/');
        OriginalSHA256=(Get-Hash (Join-Path $session 'path.before')); Payload=$copied; UTC=[DateTime]::UtcNow.ToString('o')}
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath -Encoding utf8NoBOM
    $manifest | ConvertTo-Json -Depth 5
    Write-Output 'Prepared only. Explicitly set Configs/path.txt to OverrideRoot, then Run. No game launched.'
    return
}
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifest.GameDirectory -ne $game -or $manifest.SessionDirectory -ne $session) { throw 'Session/game identity mismatch.' }
if ($Action -eq 'Restore') {
    if (Get-Process -Name DSPGAME -ErrorAction SilentlyContinue) { throw 'Close the game before manual path restoration.' }
    Restore-SandboxPath $manifest; return
}
$nameBytes = [Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($game.ToLowerInvariant()))
$mutexName = 'Local\DSPAASR-GameSandbox-' + [Convert]::ToHexString($nameBytes).Substring(0,16)
$mutex = [Threading.Mutex]::new($false, $mutexName)
if (!$mutex.WaitOne(0)) { $mutex.Dispose(); throw 'Another game sandbox owns the temporary path override.' }
$process = $null
try {
    if (Get-Process -Name DSPGAME -ErrorAction SilentlyContinue) { throw 'Refusing to start while any DSP game instance exists.' }
    if ([IO.File]::ReadAllText($pathFile).Trim().Replace('\','/') -ne $manifest.OverrideRoot) {
        throw 'Game data is not redirected to this isolated session; refusing to launch.'
    }
    foreach ($file in $manifest.Payload) {
        if ((Get-Hash (Join-Path $session ('BepInEx/plugins/DSPAASR/' + $file.Name))) -ne $file.SHA256) { throw 'Sandbox payload changed since preparation.' }
    }
    $start = [Diagnostics.ProcessStartInfo]::new((Join-Path $game 'DSPGAME.exe'))
    $start.WorkingDirectory = $game
    $start.UseShellExecute = $false
    # Direct launch must supply the real game's App ID, as Steam normally does.
    # Child-only environment: no Steam/account configuration or steam_appid.txt edits.
    $start.Environment['SteamAppId'] = '1366540'
    $start.Environment['SteamGameId'] = '1366540'
    foreach ($arg in @('--doorstop-enable','true','--doorstop-target',(Join-Path $session 'BepInEx/core/BepInEx.Preloader.dll'),
        '-screen-fullscreen','0','-screen-width','1280','-screen-height','720','-logFile',(Join-Path $session 'Player.log'))) {
        $start.ArgumentList.Add($arg)
    }
    $process = [Diagnostics.Process]::Start($start)
    [ordered]@{PID=$process.Id; Executable=$start.FileName; SessionDirectory=$session; MaximumSeconds=$MaximumSeconds;
        StartedUTC=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json |
        Set-Content -LiteralPath (Join-Path $session 'process.json') -Encoding utf8NoBOM
    Write-Output "Game sandbox PID=$($process.Id); only the temporary data root is active."
    if (!$process.WaitForExit($MaximumSeconds * 1000)) {
        $process.Kill($true)
        $process.WaitForExit()
        throw 'Owned game exceeded its bounded debugging session and was stopped; inspect game logs.'
    }
    [ordered]@{PID=$process.Id; ExitCode=$process.ExitCode; UTC=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json |
        Set-Content -LiteralPath (Join-Path $session 'exit.json') -Encoding utf8NoBOM
    if ($process.ExitCode -ne 0) { throw "Owned game exited $($process.ExitCode); inspect logs before another launch." }
}
finally {
    try {
        if ($null -ne $process -and !$process.HasExited) { $process.Kill($true); $process.WaitForExit() }
        Restore-SandboxPath $manifest
    }
    finally { if ($null -ne $process) { $process.Dispose() }; $mutex.ReleaseMutex(); $mutex.Dispose() }
}
