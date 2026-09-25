[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Plugin,
    [Parameter(Mandatory)][string]$Preloader,
    [string]$BepInExPath = '',
    [string]$GameManagedPath = $env:DSP_GAME_MANAGED_PATH,
    [string]$OutputRoot = ''
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot -Parent
if (!$BepInExPath) { $BepInExPath = Join-Path $project 'external/bepinex-minimum/core' }
if (!$OutputRoot) { $OutputRoot = Join-Path $project 'build/Testing' }
if (!$GameManagedPath) { throw 'Specify the unmodified game Managed directory with -GameManagedPath.' }
$Plugin = (Resolve-Path -LiteralPath $Plugin).Path
$Preloader = (Resolve-Path -LiteralPath $Preloader).Path
$BepInExPath = (Resolve-Path -LiteralPath $BepInExPath).Path
$GameManagedPath = (Resolve-Path -LiteralPath $GameManagedPath).Path
foreach ($name in @('BepInEx.dll', 'Mono.Cecil.dll', 'MonoMod.Utils.dll')) {
    if (!(Test-Path -LiteralPath (Join-Path $BepInExPath $name) -PathType Leaf)) { throw "Missing real BepInEx dependency: $name" }
}
if (!(Test-Path -LiteralPath (Join-Path $GameManagedPath 'Assembly-CSharp.dll') -PathType Leaf)) {
    throw 'GameManagedPath is not a game Managed directory.'
}
# Use a fresh pwsh process: never reuse a previously loaded BepInEx/patcher copy.
$ownedNames = @('BepInEx', 'DSPAASR.Preloader', 'DSPAAMod', 'Mono.Cecil', 'MonoMod.Utils')
foreach ($assembly in [AppDomain]::CurrentDomain.GetAssemblies()) {
    if ($assembly.GetName().Name -in $ownedNames) { throw 'Run test-preloader.ps1 in a fresh pwsh process.' }
}
$root = Join-Path ([IO.Path]::GetFullPath($OutputRoot)) ('preloader helpers ' + [Guid]::NewGuid().ToString('N'))
$core = Join-Path $root 'BepInEx/core'
$inputs = Join-Path $root 'inputs'
$null = New-Item -ItemType Directory -Path $core, $inputs
$resolver = $null
$script:preloaderCases = 0
function Assert-True([bool]$Condition, [string]$Message) { if (!$Condition) { throw $Message } }
function Invoke-Helper([Reflection.MethodInfo]$Method, [object[]]$Values) {
    # Command output can retain a PSObject wrapper inside object[]. Reflection
    # requires the underlying CLR value, including the string[] Paths argument.
    $raw = [object[]]::new($Values.Length)
    for ($i = 0; $i -lt $Values.Length; $i++) {
        if ($null -eq $Values[$i]) { $raw[$i] = $null }
        else { $raw[$i] = $Values[$i].PSObject.BaseObject }
    }
    try { return $Method.Invoke($null, $raw) }
    catch { throw $_.Exception.GetBaseException() }
}
function Assert-Rejected([scriptblock]$Action, [type]$ExpectedType, [string]$ExpectedText) {
    $failure = $null
    try { $null = & $Action } catch { $failure = $_.Exception.GetBaseException() }
    Assert-True ($null -ne $failure) 'Expected the real helper to reject the fixture.'
    Assert-True ($ExpectedType.IsAssignableFrom($failure.GetType())) "Unexpected rejection type: $($failure.GetType().FullName): $($failure.Message)"
    Assert-True ($failure.Message.Contains($ExpectedText)) "Unexpected rejection reason: $($failure.Message)"
}
function Passed([string]$Name) { $script:preloaderCases++; Write-Output "PASS $Name" }
function New-Payload([string]$Name) {
    $plugins = Join-Path $root "payloads/$Name/BepInEx/plugins"
    $directory = Join-Path $plugins 'AnotherAuthor-UnrelatedFolder/nested payload'
    $null = New-Item -ItemType Directory -Path $directory
    Copy-Item -LiteralPath $pluginCopy -Destination (Join-Path $directory 'DSPAAMod.dll')
    # FindRuntimeDirectory only checks this file's existence. Deliberately not
    # loadable: this helper suite must never call Initialize or LoadLibrary.
    [IO.File]::WriteAllBytes((Join-Path $directory 'DSPAANative.dll'), [byte[]](0, 1, 2, 3))
    return @{ Plugins = $plugins; Directory = $directory }
}
function Config-State([string]$Path) {
    $exists = [IO.File]::Exists($Path)
    return @{
        Exists = $exists
        ParentExists = [IO.Directory]::Exists([IO.Path]::GetDirectoryName($Path))
        Bytes = $(if ($exists) { [Convert]::ToBase64String([IO.File]::ReadAllBytes($Path)) } else { '' })
        Modified = $(if ($exists) { [IO.File]::GetLastWriteTimeUtc($Path).Ticks } else { 0L })
    }
}
function Check-Config([string]$Name, [AllowNull()][string]$Content, [int]$Expected, [bool]$Invalid = $false, [switch]$Absent) {
    $path = Join-Path $root "configs/$Name/dspaa.mod.cfg"
    if (!$Absent) {
        $null = New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($path))
        [IO.File]::WriteAllText($path, $Content, [Text.UTF8Encoding]::new($false))
        [IO.File]::SetLastWriteTimeUtc($path, [DateTime]::new(2020, 1, 2, 3, 4, 5, [DateTimeKind]::Utc))
    }
    $before = Config-State $path
    if ($Invalid) {
        Assert-Rejected { Invoke-Helper $readBackend ([object[]]@($path)) } ([IO.InvalidDataException]) 'Unknown saved FrameGeneration.Backend'
    } else {
        $actual = Invoke-Helper $readBackend ([object[]]@($path))
        Assert-True ($actual -eq $Expected) "Incorrect backend for ${Name}: $actual, expected $Expected"
    }
    $after = Config-State $path
    foreach ($key in @('Exists', 'ParentExists', 'Bytes', 'Modified')) {
        Assert-True ($before[$key] -ceq $after[$key]) "ReadBackend changed $key for $Name"
    }
    Passed "config $Name; bytes, mtime and absence preserved"
}
try {
    Copy-Item -LiteralPath $Plugin -Destination (Join-Path $inputs 'DSPAAMod.dll')
    Copy-Item -LiteralPath $Preloader -Destination (Join-Path $inputs 'DSPAASR.Preloader.dll')
    $pluginCopy = Join-Path $inputs 'DSPAAMod.dll'
    foreach ($name in @('DSPAAMod.dll', 'DSPAASR.Preloader.dll')) {
        Write-Output ("Input $name SHA256=" + (Get-FileHash -LiteralPath (Join-Path $inputs $name) -Algorithm SHA256).Hash)
    }
    foreach ($file in Get-ChildItem -LiteralPath $BepInExPath -Filter '*.dll' -File) {
        Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $core $file.Name)
    }
    # Load immutable copies from bytes so Windows does not retain file handles
    # during finally cleanup. Resolve only real BepInEx/game dependencies.
    $resolver = [ResolveEventHandler]{
        param($sender, $eventArgs)
        $name = [Reflection.AssemblyName]::new($eventArgs.Name).Name
        foreach ($loaded in [AppDomain]::CurrentDomain.GetAssemblies()) {
            if ($loaded.GetName().Name -eq $name) { return $loaded }
        }
        if ($name -eq 'DSPAAMod') { throw 'The Unity-dependent DSPAAMod assembly must not be loaded by helper tests.' }
        $path = Join-Path $core ($name + '.dll')
        if (![IO.File]::Exists($path)) {
            $source = Join-Path $GameManagedPath ($name + '.dll')
            if (![IO.File]::Exists($source)) { return $null }
            $path = Join-Path $inputs ($name + '.dll')
            if (![IO.File]::Exists($path)) { [IO.File]::Copy($source, $path) }
        }
        return [Reflection.Assembly]::Load([IO.File]::ReadAllBytes($path))
    }
    [AppDomain]::CurrentDomain.add_AssemblyResolve($resolver)
    $null = [Reflection.Assembly]::Load([IO.File]::ReadAllBytes((Join-Path $core 'Mono.Cecil.dll')))
    $null = [Reflection.Assembly]::Load([IO.File]::ReadAllBytes((Join-Path $core 'MonoMod.Utils.dll')))
    $bep = [Reflection.Assembly]::Load([IO.File]::ReadAllBytes((Join-Path $core 'BepInEx.dll')))
    # ConfigFile's real static CoreConfig may save BepInEx.cfg on first use.
    # Initialize BepInEx Paths to this fixture before touching ConfigFile.
    $flags = [Reflection.BindingFlags]'Static,NonPublic'
    $setPaths = $bep.GetType('BepInEx.Paths', $true).GetMethod('SetExecutablePath', $flags)
    Assert-True ($null -ne $setPaths) 'BepInEx Paths initialization API was not found.'
    $pathArgs = [object[]]::new(4)
    $pathArgs[0] = Join-Path $root 'DSPGAME.exe'
    $pathArgs[1] = Join-Path $root 'BepInEx'
    $pathArgs[2] = $GameManagedPath
    $pathArgs[3] = [string[]]@($GameManagedPath)
    $null = Invoke-Helper $setPaths $pathArgs
    $patcher = [Reflection.Assembly]::Load([IO.File]::ReadAllBytes((Join-Path $inputs 'DSPAASR.Preloader.dll')))
    $type = $patcher.GetType('DSPAASR.Preloader.Patcher', $true)
    $findRuntime = $type.GetMethod('FindRuntimeDirectory', $flags)
    $readBackend = $type.GetMethod('ReadBackend', $flags)
    Assert-True ($null -ne $findRuntime -and $null -ne $readBackend) 'Production preloader helpers were not found.'

    $fixture = New-Payload 'arbitrary-author'
    $found = Invoke-Helper $findRuntime ([object[]]@($fixture.Plugins))
    Assert-True ([string]::Equals($found, [IO.Path]::GetFullPath($fixture.Directory), [StringComparison]::OrdinalIgnoreCase)) 'The arbitrary author/nested payload was not selected.'
    Passed 'arbitrary author and nested payload directory'

    $fixture = New-Payload 'duplicate'
    $duplicate = Join-Path $fixture.Plugins 'AnotherAuthor-Duplicate'
    $null = New-Item -ItemType Directory -Path $duplicate
    Copy-Item -LiteralPath $pluginCopy -Destination (Join-Path $duplicate 'DSPAAMod.dll')
    Assert-Rejected { Invoke-Helper $findRuntime ([object[]]@($fixture.Plugins)) } ([InvalidOperationException]) 'Multiple enabled'
    Passed 'duplicate enabled plugin rejected'

    foreach ($suffix in @('old', 'disabled')) {
        $fixture = New-Payload "disabled-managed-$suffix"
        $managed = Join-Path $fixture.Directory 'DSPAAMod.dll'
        Move-Item -LiteralPath $managed -Destination "$managed.$suffix"
        Assert-Rejected { Invoke-Helper $findRuntime ([object[]]@($fixture.Plugins)) } ([IO.FileNotFoundException]) 'disabled or absent'
        Passed "managed .$suffix file is not an enabled plugin"
    }
    $fixture = New-Payload 'missing-native'
    Remove-Item -LiteralPath (Join-Path $fixture.Directory 'DSPAANative.dll')
    Assert-Rejected { Invoke-Helper $findRuntime ([object[]]@($fixture.Plugins)) } ([IO.FileNotFoundException]) 'disabled or missing'
    Passed 'missing native payload rejected'
    $fixture = New-Payload 'disabled-native'
    $native = Join-Path $fixture.Directory 'DSPAANative.dll'
    Move-Item -LiteralPath $native -Destination "$native.old"
    Assert-Rejected { Invoke-Helper $findRuntime ([object[]]@($fixture.Plugins)) } ([IO.FileNotFoundException]) 'disabled or missing'
    Passed 'disabled native payload rejected'

    $fixture = New-Payload 'wrong-version'
    $managed = Join-Path $fixture.Directory 'DSPAAMod.dll'
    $parameters = [Mono.Cecil.ReaderParameters]::new()
    $parameters.InMemory = $true
    $definition = [Mono.Cecil.AssemblyDefinition]::ReadAssembly($managed, $parameters)
    try {
        $definition.Name.Version = [Version]::new(999, 0, 0, 0)
        Assert-True ($definition.Name.Version -ne $patcher.GetName().Version) 'The version mismatch fixture is not different.'
        $definition.Write("$managed.replacement")
    } finally { $definition.Dispose() }
    Move-Item -LiteralPath "$managed.replacement" -Destination $managed -Force
    Assert-Rejected { Invoke-Helper $findRuntime ([object[]]@($fixture.Plugins)) } ([InvalidOperationException]) 'versions do not match'
    Passed 'mismatched main assembly version rejected'

    Check-Config 'absent' $null 0 -Absent
    Check-Config 'sr-only' "# preserve this comment`r`n[Antialiasing]`r`nTechnique = Fsr`r`nResolution = Quality`r`n" 0
    foreach ($case in @(@('Off', 0), @('Fsr', 1), @('Dlss', 2), @('0', 0), @('1', 1), @('2', 2), @('fSr', 1), @('+2', 2))) {
        Check-Config ("valid-" + $script:preloaderCases + '-' + $case[0]) ("[FrameGeneration]`nBackend = " + $case[0] + "`n# untouched`n") ([int]$case[1])
    }
    foreach ($case in @(@('unknown', 'Unknown'), @('out-of-range', '3'), @('negative', '-1'), @('empty', ''), @('escaped-whitespace', 'Fsr\n'))) {
        Check-Config ("invalid-" + $case[0]) ("[FrameGeneration]`nBackend = " + $case[1] + "`n") 0 $true
    }
    Assert-True (@([AppDomain]::CurrentDomain.GetAssemblies() | Where-Object { $_.GetName().Name -eq 'DSPAAMod' }).Count -eq 0) 'The plugin assembly was loaded instead of inspected as metadata.'
    Assert-True (@([Diagnostics.Process]::GetCurrentProcess().Modules | Where-Object { $_.ModuleName -eq 'DSPAANative.dll' }).Count -eq 0) 'A helper test loaded the native DLL.'
    Write-Output "Preloader real helpers: $script:preloaderCases cases passed; no Initialize, native DLL, game, GPU, or production config mutation."
} finally {
    if ($null -ne $resolver) { [AppDomain]::CurrentDomain.remove_AssemblyResolve($resolver) }
    Remove-Item -LiteralPath $root -Recurse -Force
}
