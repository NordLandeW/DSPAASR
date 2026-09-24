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
    Write-Output 'Sandbox prepare/launch refusal, exact-byte rollback, idempotence and concurrent-edit protection passed; game launches=0.'
}
finally { if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Recurse -Force } }
