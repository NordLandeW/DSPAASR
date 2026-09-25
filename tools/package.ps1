[CmdletBinding()]
param(
    [string]$OutputDirectory = '',
    [string]$GameManagedPath = $env:DSP_GAME_MANAGED_PATH,
    [string]$BepInExPath = 'C:/Game Modding/BepInEx/BepInEx/core'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$metadata = Get-Content -LiteralPath (Join-Path $root 'pack/manifest.json') -Raw | ConvertFrom-Json
if (!$OutputDirectory) { $OutputDirectory = Join-Path $root ("dist/$($metadata.name)-$($metadata.version_number)-" + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
$output = [IO.Path]::GetFullPath($OutputDirectory, $root)
$zip = "$output.zip"
if ((Test-Path -LiteralPath $output) -or (Test-Path -LiteralPath $zip)) { throw 'Refusing to overwrite an existing package.' }
# Package the exact production DLL checked against unmodified runtime references.
& (Join-Path $PSScriptRoot 'test-game-references.ps1') -GameManagedPath $GameManagedPath -BepInExPath $BepInExPath
# Validate tools and built inputs before creating an output directory.
$inkscape = (Get-Command inkscape -ErrorAction Stop).Source
$python = (Get-Command python -ErrorAction Stop).Source
$inkscapeHelp = (& $inkscape --help | Out-String)
$modernInkscape = $inkscapeHelp.Contains('--export-type')
if (!$modernInkscape -and !$inkscapeHelp.Contains('--export-png')) { throw 'Unsupported Inkscape command-line interface.' }
$files = [ordered]@{
    'build/game-reference-check/Release/bin/DSPAAMod.dll' = 'DSPAAMod.dll'
    'build/native/Release/DSPAANative.dll' = 'DSPAANative.dll'
    'external/ngx/runtime/rel/nvngx_dlss.dll' = 'nvngx_dlss.dll'
    'external/ngx/LICENSE.txt' = 'NVIDIA-RTX-SDK-LICENSE.txt'
    'external/fsr-sdk/Kits/FidelityFX/signedbin/amd_fidelityfx_loader_dx12.dll' = 'amd_fidelityfx_loader_dx12.dll'
    'external/fsr-sdk/Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_dx12.dll' = 'amd_fidelityfx_upscaler_dx12.dll'
    'external/fsr-sdk/docs/license.md' = 'AMD-FSR-SDK-LICENSE.md'
    'external/minhook/LICENSE.txt' = 'MINHOOK-LICENSE.txt'
    'external/fsr-sdk/Kits/FidelityFX/signedbin/amd_fidelityfx_framegeneration_dx12.dll' = 'amd_fidelityfx_framegeneration_dx12.dll'
    'external/streamline/LICENSE.txt' = 'STREAMLINE-LICENSE.txt'
    'external/streamline-runtime/rel/sl.interposer.dll' = 'sl.interposer.dll'
    'external/streamline-runtime/rel/sl.common.dll' = 'sl.common.dll'
    'external/streamline-runtime/rel/sl.dlss_g.dll' = 'sl.dlss_g.dll'
    'external/streamline-runtime/rel/sl.reflex.dll' = 'sl.reflex.dll'
    'external/streamline-runtime/rel/sl.pcl.dll' = 'sl.pcl.dll'
    'external/streamline-runtime/rel/nvngx_dlssg.dll' = 'nvngx_dlssg.dll'
    'external/streamline-runtime/rel/nvngx_dlss.license.txt' = 'nvngx_dlss.license.txt'
    'external/streamline-runtime/rel/reflex.license.txt' = 'reflex.license.txt'
    'README.md' = 'README.md'
    'LICENSE' = 'LICENSE'
    'docs/third-party.md' = 'third-party.md'
    'docs/nvidia-dlss-notices.txt' = 'NVIDIA-DLSS-NOTICES.txt'
    'pack/manifest.json' = 'manifest.json'
    'pack/CHANGELOG.md' = 'CHANGELOG.md'
}
foreach ($source in $files.Keys) {
    if (!(Test-Path -LiteralPath (Join-Path $root $source) -PathType Leaf)) { throw "Missing build input: $source" }
}
$assembly = [Reflection.AssemblyName]::GetAssemblyName((Join-Path $root 'build/game-reference-check/Release/bin/DSPAAMod.dll'))
if ($assembly.Version.ToString(3) -ne $metadata.version_number) { throw 'Assembly/package versions differ.' }
# Revalidate the pinned release runtime and its unmodified NVIDIA signature.
& (Join-Path $PSScriptRoot 'fetch-ngx.ps1')
& (Join-Path $PSScriptRoot 'fetch-fsr.ps1')
& (Join-Path $PSScriptRoot 'fetch-minhook.ps1')
& (Join-Path $PSScriptRoot 'fetch-streamline.ps1')
$null = New-Item -ItemType Directory -Path $output
foreach ($source in $files.Keys) { Copy-Item -LiteralPath (Join-Path $root $source) -Destination (Join-Path $output $files[$source]) }
$icon = Join-Path $output 'icon.png'
if ($modernInkscape) {
    & $inkscape (Join-Path $root 'pack/icon.svg') --export-type=png --export-area-page --export-width=256 --export-height=256 "--export-filename=$icon"
} else {
    & $inkscape --without-gui "--file=$(Join-Path $root 'pack/icon.svg')" "--export-png=$icon" --export-area-page --export-width=256 --export-height=256
}
# Some older Inkscape releases return zero even for an unknown option.
if ($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath $icon -PathType Leaf)) { throw 'Icon rendering failed.' }
$checksums = @(Get-ChildItem -LiteralPath $output -File | Sort-Object Name | ForEach-Object {
    [ordered]@{ File = $_.Name; Bytes = $_.Length; SHA256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
})
$checksums | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $output 'SHA256SUMS.json') -Encoding utf8NoBOM
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($output, $zip, [IO.Compression.CompressionLevel]::Optimal, $false)
& $python (Join-Path $PSScriptRoot 'validate-package.py') $zip
if ($LASTEXITCODE -ne 0) { throw 'Package validation failed; do not install this archive.' }
Write-Output "Gale local-import ZIP: $zip"
Write-Output ('SHA256: ' + (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash)
Write-Output 'No deployment, game launch or public upload was performed.'
