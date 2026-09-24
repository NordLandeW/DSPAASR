[CmdletBinding()]
param(
    [ValidateSet('rel', 'dev')][string[]]$RuntimeVariants = @('rel'),
    [switch]$IsolateDriverProfile,
    [switch]$BridgeSwitch,
    [switch]$Typeless,
    [switch]$JitterStability,
    [switch]$SuperResolution,
    [string]$OutputDirectory = ''
)
$ErrorActionPreference = 'Stop'
if ($Typeless -and !$BridgeSwitch) { throw 'Typeless validation currently requires -BridgeSwitch.' }
if ($JitterStability -and ($BridgeSwitch -or $Typeless -or $SuperResolution)) { throw 'JitterStability is a separate static-image comparison.' }
if ($SuperResolution -and ($BridgeSwitch -or $Typeless)) { throw 'SuperResolution is a separate native bridge matrix.' }
$root = Split-Path $PSScriptRoot -Parent
$probe = Join-Path $root 'build/native/Release/ngx-probe.exe'
$profileTool = Join-Path $root 'build/native/Release/driver-profile.exe'
if (!(Test-Path -LiteralPath $probe)) { throw 'Build the Release probe first.' }
if ($IsolateDriverProfile -and !(Test-Path -LiteralPath $profileTool)) {
    throw 'Build DSPAA_BUILD_DRIVER_TOOL first; isolation must be explicitly authorized.'
}
if (!$OutputDirectory) { $OutputDirectory = Join-Path $root ('artifacts/presets-' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
$output = [IO.Path]::GetFullPath($OutputDirectory, $root)
if (Test-Path -LiteralPath $output) { throw "Output directory already exists: $output" }
$null = New-Item -ItemType Directory -Path $output
$isolated = $false
$results = @()
$failed = $false
$mutex = [Threading.Mutex]::new($false, 'Local\DSPAAMod-NGX-PresetProbes')
# The eight-stage SR matrix measured about 14.4s on the reference GPU; allow
# startup/model-load headroom without turning elapsed time into a correctness test.
$guardSeconds = if ($SuperResolution) { 30 } else { 15 }
if (!$mutex.WaitOne(0)) { $mutex.Dispose(); throw 'Another preset probe matrix is active.' }
try {
    if ($IsolateDriverProfile) {
        & $profileTool isolate (Join-Path $output 'driver-before.nip') *> (Join-Path $output 'driver-isolate.log')
        if ($LASTEXITCODE -ne 0) { throw 'Driver isolation failed; inspect driver-isolate.log.' }
        $isolated = $true
    }
    foreach ($variant in $RuntimeVariants) {
        $runtime = Join-Path $root "external/ngx/runtime/$variant"
        $presets = if ($BridgeSwitch -or $JitterStability -or $SuperResolution) { @('K') } else { @('E', 'F', 'K', 'L', 'M') }
        foreach ($preset in $presets) {
            $directory = Join-Path $output "$variant-$preset"
            $null = New-Item -ItemType Directory -Path $directory
            $arguments = '"' + $runtime + '" "' + $directory + '" ' + $preset
            if ($BridgeSwitch) { $arguments += ' --bridge-switch'; if ($Typeless) { $arguments += '-typeless' } }
            if ($JitterStability) { $arguments += ' --jitter-stability' }
            if ($SuperResolution) { $arguments += ' --bridge-super-resolution' }
            $errorFile = if ($BridgeSwitch -or $SuperResolution) { 'stderr.log' } else { 'ngx.log' }
            $timer = [Diagnostics.Stopwatch]::StartNew()
            $process = Start-Process -FilePath $probe -ArgumentList $arguments -NoNewWindow -PassThru `
                -RedirectStandardOutput (Join-Path $directory 'output.log') `
                -RedirectStandardError (Join-Path $directory $errorFile)
            try {
                # Deadlock/process isolation only, never an image quality or performance assertion.
                if (!$process.WaitForExit($guardSeconds * 1000)) {
                    $process.Kill($true)
                    $process.WaitForExit()
                    throw "Probe $variant/$preset exceeded the $guardSeconds-second process guard."
                }
                $process.Refresh()
                $record = [ordered]@{ Runtime = $variant; Preset = $preset; ExitCode = $process.ExitCode; Seconds = $timer.Elapsed.TotalSeconds }
                $results += $record
                Write-Output ($record | ConvertTo-Json -Compress)
                Get-Content -LiteralPath (Join-Path $directory 'output.log') -Tail 4
                if ($process.ExitCode -ne 0) { $failed = $true }
            } finally { $process.Dispose() }
        }
    }
} finally {
    try {
        # Restore first: a failure to write machine results must not skip driver cleanup.
        if ($isolated) {
            & $profileTool restore *> (Join-Path $output 'driver-restored.log')
            if ($LASTEXITCODE -ne 0) {
                throw "DRIVER RESTORATION FAILED. Inspect $output/driver-restored.log before further work."
            }
            Get-Content -LiteralPath (Join-Path $output 'driver-restored.log')
        }
    } finally {
        $mutex.ReleaseMutex()
        $mutex.Dispose()
        $results | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $output 'results.json') -Encoding utf8
    }
}
if ($failed) { throw "One or more preset probes were not verified. Inspect $output." }
Write-Output "Preset probes passed: $output"
