[CmdletBinding()]
param(
    [string]$GameManagedPath = $env:DSP_GAME_MANAGED_PATH,
    [string]$BepInExPath = '',
    [string]$Configuration = 'Release'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (!$GameManagedPath) { throw 'Specify -GameManagedPath (or DSP_GAME_MANAGED_PATH) pointing to the unmodified DSPGAME_Data/Managed directory.' }
. (Join-Path $PSScriptRoot 'local-paths.ps1')
$BepInExPath = Resolve-DspLocalPath -Name BepInExPath -Value $BepInExPath -RepositoryRoot $root
$game = (Resolve-Path -LiteralPath $GameManagedPath).Path
$bepInEx = (Resolve-Path -LiteralPath $BepInExPath).Path
# Reject the publicized development references that concealed the 1.0.0 regression.
# Cecil is already supplied by BepInEx; read metadata without loading game code.
Add-Type -Path (Join-Path $bepInEx 'Mono.Cecil.dll')
$module = [Mono.Cecil.ModuleDefinition]::ReadModule((Join-Path $game 'Assembly-CSharp.dll'))
try {
    $combo = $module.Types | Where-Object FullName -eq 'UIComboBox'
    $buttons = $combo.Fields | Where-Object Name -eq 'ItemButtons'
    if (!$buttons -or !$buttons.IsPrivate) { throw 'Expected the original private UIComboBox.ItemButtons field; do not use publicized game references.' }
} finally { $module.Dispose() }
# Recompile every production source file, including the Unity UI adapter. Keep
# this check separate from the ordinary development build and its intermediates.
$output = Join-Path $root "build/game-reference-check/$Configuration"
Push-Location $root
try {
    dotnet build managed/DSPAAMod.csproj -c $Configuration --nologo -t:Rebuild `
        "-p:DspLibsPath=$game" "-p:BepInExPath=$bepInEx" `
        "-p:OutputPath=$output/bin/" "-p:IntermediateOutputPath=$output/obj/" `
        -p:AppendTargetFrameworkToOutputPath=false
    if ($LASTEXITCODE -ne 0) { throw 'Production plugin build against unmodified game references failed.' }
    Write-Output "Game-reference check passed: $output/bin/DSPAAMod.dll"
    dotnet build preloader/DSPAASR.Preloader.csproj -c $Configuration --nologo -t:Rebuild `
        "-p:BepInExPath=$bepInEx" "-p:OutputPath=$output/preloader/" `
        "-p:IntermediateOutputPath=$output/preloader-obj/" -p:AppendTargetFrameworkToOutputPath=false
    if ($LASTEXITCODE -ne 0) { throw 'Production preloader build against the selected BepInEx core failed.' }
    Write-Output "Preloader check passed: $output/preloader/DSPAASR.Preloader.dll"
} finally { Pop-Location }
