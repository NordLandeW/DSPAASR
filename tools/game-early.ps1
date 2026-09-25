[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('Prepare','Run','Restore')][string]$Action,
    [Parameter(Mandatory)][string]$SessionDirectory,
    [string]$GameDirectory = 'E:/SteamLibrary/steamapps/common/Dyson Sphere Program',
    [string]$BootstrapLibrary = '',
    [ValidateRange(60,3600)][int]$MaximumSeconds = 600
)
# Optional early-entry overlay for an already prepared, isolated game sandbox.
# Prepare backs up bytes and stages only the binary. The boot.config text edit
# remains a separate explicit operation; Run verifies its exact expected hash.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot -Parent
$session = [IO.Path]::GetFullPath($SessionDirectory, $project)
$game = [IO.Path]::GetFullPath($GameDirectory, $project)
$boot = Join-Path $game 'DSPGAME_Data/boot.config'
$target = Join-Path $game 'DSPGAME_Data/Plugins/x86_64/DSPAAPresentBootstrap.dll'
$record = Join-Path $session 'early.json'
function Get-Hash([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }
function Assert-NoGame {
    if (Get-Process -Name DSPGAME -ErrorAction SilentlyContinue) { throw 'Close all DSP game processes before changing the early-load overlay.' }
}
function Restore-Early($m) {
    Assert-NoGame
    if ((Get-Hash (Join-Path $session 'boot.before')) -ne $m.BootOriginalSHA256) { throw 'Boot backup changed.' }
    $current = Get-Hash $boot
    if ($current -ne $m.BootOriginalSHA256) {
        if ($current -ne $m.BootActiveSHA256) { throw 'boot.config changed externally; refusing rollback over another edit.' }
        Copy-Item -LiteralPath (Join-Path $session 'boot.before') -Destination $boot
    }
    if ((Get-Hash $boot) -ne $m.BootOriginalSHA256) { throw 'Boot rollback did not restore exact bytes.' }
    if (Test-Path -LiteralPath $target -PathType Leaf) {
        $hash = Get-Hash $target
        if ($m.BootstrapExisted -and $hash -eq $m.BootstrapOriginalSHA256) {
            # Already restored.
        } elseif ($hash -eq $m.BootstrapActiveSHA256) {
            if ($m.BootstrapExisted) {
                $before = Join-Path $session 'bootstrap.before.dll'
                if ((Get-Hash $before) -ne $m.BootstrapOriginalSHA256) { throw 'Bootstrap backup changed.' }
                Copy-Item -LiteralPath $before -Destination $target
            } else { Remove-Item -LiteralPath $target }
        } else { throw 'Early-load library changed externally; refusing to replace it.' }
    } elseif ($m.BootstrapExisted) { throw 'Original early-load library disappeared; refusing an unproven rollback.' }
    if ($m.BootstrapExisted -and (Get-Hash $target) -ne $m.BootstrapOriginalSHA256) { throw 'Library rollback hash mismatch.' }
    [ordered]@{Restored=$true;BootSHA256=$m.BootOriginalSHA256;BootstrapRestored=$true;UTC=[DateTime]::UtcNow.ToString('o')} |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $session 'early-restoration.json') -Encoding utf8NoBOM
}
$sandbox = Get-Content -LiteralPath (Join-Path $session 'sandbox.json') -Raw | ConvertFrom-Json
if ($sandbox.GameDirectory -ne $game -or $sandbox.SessionDirectory -ne $session) { throw 'Sandbox/early-overlay identity mismatch.' }
if ($Action -eq 'Prepare') {
    Assert-NoGame
    if (Test-Path -LiteralPath $record) { throw 'Early overlay already prepared; restore or use a new sandbox.' }
    if (!$BootstrapLibrary) { throw 'Prepare requires the exact built bootstrap library.' }
    $source = [IO.Path]::GetFullPath($BootstrapLibrary, $project)
    if (!(Test-Path -LiteralPath $source -PathType Leaf) -or !(Test-Path -LiteralPath $boot -PathType Leaf) -or
        !(Test-Path -LiteralPath (Split-Path $target -Parent) -PathType Container)) { throw 'Missing bootstrap input or game layout.' }
    $bytes = [IO.File]::ReadAllBytes($boot)
    $text = [Text.UTF8Encoding]::new($false,$true).GetString($bytes)
    if ($text.Contains([char]0) -or $text.TrimStart([char]0xFEFF) -match '(?im)^\s*xrsdk-pre-init-library\s*=') {
        throw 'Non-UTF8 boot config or an existing PreInit provider; do not replace it.'
    }
    $nl = if ($text.Contains("`r`n")) {"`r`n"} else {"`n"}
    $setting = 'xrsdk-pre-init-library=DSPAAPresentBootstrap'
    $append = $(if ($text.Length -and !$text.EndsWith("`n")) {$nl} else {''}) + $setting + $nl
    $activeBytes = [byte[]]($bytes + [Text.Encoding]::UTF8.GetBytes($append))
    Copy-Item -LiteralPath $boot -Destination (Join-Path $session 'boot.before')
    $existed = Test-Path -LiteralPath $target -PathType Leaf
    $oldHash = ''
    if ($existed) {
        Copy-Item -LiteralPath $target -Destination (Join-Path $session 'bootstrap.before.dll')
        $oldHash = Get-Hash $target
    }
    $m = [ordered]@{GameDirectory=$game;SessionDirectory=$session;BootSetting=$setting;
        BootOriginalSHA256=(Get-Hash (Join-Path $session 'boot.before'));
        BootActiveSHA256=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($activeBytes));
        BootstrapExisted=$existed;BootstrapOriginalSHA256=$oldHash;BootstrapActiveSHA256=(Get-Hash $source);
        UTC=[DateTime]::UtcNow.ToString('o')}
    $m | ConvertTo-Json | Set-Content -LiteralPath $record -Encoding utf8NoBOM
    $staged = $target + '.stage-' + [Guid]::NewGuid().ToString('N')
    try {
        Copy-Item -LiteralPath $source -Destination $staged
        if ((Get-Hash $staged) -ne $m.BootstrapActiveSHA256) { throw 'Staged bootstrap hash mismatch.' }
        if ($existed) {
            if ((Get-Hash $target) -ne $oldHash) { throw 'Bootstrap changed before atomic installation.' }
        } elseif (Test-Path -LiteralPath $target) { throw 'Bootstrap appeared before atomic installation.' }
        [IO.File]::Move($staged, $target, $true)
        if ((Get-Hash $target) -ne $m.BootstrapActiveSHA256) { throw 'Installed bootstrap hash mismatch.' }
    } catch { Restore-Early $m; throw }
    finally { if (Test-Path -LiteralPath $staged) { Remove-Item -LiteralPath $staged } }
    $m | ConvertTo-Json
    Write-Output 'Binary staged with exact rollback. Explicitly append BootSetting to boot.config before Run.'
    return
}
$m = Get-Content -LiteralPath $record -Raw | ConvertFrom-Json
if ($m.GameDirectory -ne $game -or $m.SessionDirectory -ne $session) { throw 'Early-overlay identity mismatch.' }
if ($Action -eq 'Restore') { Restore-Early $m; return }
try {
    if ((Get-Hash $boot) -ne $m.BootActiveSHA256 -or (Get-Hash $target) -ne $m.BootstrapActiveSHA256) {
        throw 'Early-load overlay does not match the separately authorized config and pinned binary.'
    }
    & (Join-Path $PSScriptRoot 'game-sandbox.ps1') -Action Run -SessionDirectory $session -GameDirectory $game -MaximumSeconds $MaximumSeconds
} finally { Restore-Early $m }
