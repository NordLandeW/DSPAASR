[CmdletBinding()]
param([Parameter(Mandatory)][string]$Bootstrap, [Parameter(Mandatory)][string]$Probe,
      [Parameter(Mandatory)][string]$Fixture, [Parameter(Mandatory)][string]$OutputRoot)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Join-Path ([IO.Path]::GetFullPath($OutputRoot)) ('bootstrap gate ' + [Guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $root
$exe = Join-Path $root 'DSPGAME.exe'
$core = Join-Path $root 'BepInEx/core'
$plugin = Join-Path $root 'BepInEx/plugins/DSPAASR'
$config = Join-Path $root 'BepInEx/config'
$null = New-Item -ItemType Directory -Path $core,$plugin,$config
$preloader = Join-Path $core 'BepInEx.Preloader.dll'
$native = Join-Path $plugin 'DSPAANative.dll'
$managed = Join-Path $plugin 'DSPAAMod.dll'
$optin = Join-Path $config 'dspaa.present.optin'
Copy-Item -LiteralPath $Probe -Destination $exe
Copy-Item -LiteralPath $Fixture -Destination $native
[IO.File]::WriteAllBytes($preloader, [byte[]](1,2,3))
[IO.File]::WriteAllBytes($managed, [byte[]](4,5,6))
$nativeHash = (Get-FileHash -LiteralPath $native).Hash
$managedHash = (Get-FileHash -LiteralPath $managed).Hash
$valid = "DSPAASR-PRESENT-1`nmode=trace`nnative-sha256=$nativeHash`nmanaged-sha256=$managedHash`n"
function Set-Manifest([string]$Value) { [IO.File]::WriteAllText($optin, $Value, [Text.UTF8Encoding]::new($false)) }
$script:cases = 0
function Run-Case([string]$Name, [bool]$Expected, [string[]]$Arguments) {
    $start = [Diagnostics.ProcessStartInfo]::new($exe)
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.WorkingDirectory = $root
    foreach ($a in @($Bootstrap,$native,$Expected.ToString().ToLowerInvariant()) + $Arguments) { $start.ArgumentList.Add($a) }
    $p = [Diagnostics.Process]::Start($start)
    try {
        if (!$p.WaitForExit(5000)) { $p.Kill($true); $p.WaitForExit(); throw "Gate case hung: $Name" }
        if ($p.ExitCode) { throw "Gate case failed: $Name (exit $($p.ExitCode))" }
    } finally { $p.Dispose() }
    $script:cases++
    Write-Output "PASS $Name"
}
$on = @('--doorstop-enable','true','--doorstop-target',$preloader)
try {
    Set-Manifest $valid
    Run-Case 'exact enabled profile, spaces, ABI and payload hashes' $true $on
    Run-Case 'vanilla has no native load' $false @()
    Run-Case 'explicitly disabled Doorstop' $false @('--doorstop-enable','false','--doorstop-target',$preloader)
    Run-Case 'ambiguous duplicate enable rejected' $false ($on + @('--doorstop-enable','true'))
    Run-Case 'relative preloader rejected' $false @('--doorstop-enable','true','--doorstop-target','BepInEx/core/BepInEx.Preloader.dll')
    Remove-Item -LiteralPath $optin
    Run-Case 'other profile without opt-in' $false $on
    Set-Manifest ($valid.Replace('DSPAASR-PRESENT-1','DSPAASR-PRESENT-99'))
    Run-Case 'unknown ABI rejected before native load' $false $on
    Set-Manifest ($valid.Replace('mode=trace','mode=unknown'))
    Run-Case 'unknown mode rejected before native load' $false $on
    Set-Manifest ($valid.Replace($nativeHash,('0'*64)))
    Run-Case 'native hash mismatch rejected before native load' $false $on
    Set-Manifest ($valid.Replace($managedHash,('0'*64)))
    Run-Case 'managed hash mismatch rejected before native load' $false $on
    Set-Manifest $valid
    Move-Item -LiteralPath $managed -Destination "$managed.disabled"
    Run-Case 'disabled managed plugin rejected' $false $on
    Move-Item -LiteralPath "$managed.disabled" -Destination $managed
    $duplicate = Join-Path $root 'BepInEx/plugins/NordLandeW-DSPAASR'
    $null = New-Item -ItemType Directory -Path $duplicate
    Copy-Item -LiteralPath $managed -Destination $duplicate
    Run-Case 'two active plugin directories rejected' $false $on
    Remove-Item -LiteralPath $duplicate -Recurse
    Set-Manifest ($valid.Replace("`n","`r`n"))
    Run-Case 'CRLF opt-in remains valid' $true $on
    Set-Manifest ($valid.Replace('mode=trace','mode=present'))
    Run-Case 'presentation mode reaches the native ABI as one' $true ($on + @('--test-presentation'))
    Write-Output "Bootstrap profile gate: $script:cases cases passed; no window or real game files used."
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force
}
