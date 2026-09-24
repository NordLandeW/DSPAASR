[CmdletBinding()]
param([Parameter(Mandatory)][string]$PluginDirectory)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$target = [IO.Path]::GetFullPath($PluginDirectory)
if (Get-Process DSPGAME -ErrorAction SilentlyContinue) { throw 'Close DSPGAME before replacing plugin DLLs.' }
$manifest = Get-Content -LiteralPath (Join-Path $target 'manifest.json') -Raw | ConvertFrom-Json
if ($manifest.name -ne 'DSPAAMod') { throw 'Target is not an existing DSPAAMod installation.' }
$inputs = [ordered]@{
    'DSPAAMod.dll' = Join-Path $root 'managed/bin/Release/net48/DSPAAMod.dll'
    'DSPAANative.dll' = Join-Path $root 'build/native/Release/DSPAANative.dll'
}
foreach ($name in $inputs.Keys) {
    if (!(Test-Path -LiteralPath $inputs[$name] -PathType Leaf) -or !(Test-Path -LiteralPath (Join-Path $target $name) -PathType Leaf)) {
        throw "Missing source or installed DLL: $name"
    }
}
$backup = Join-Path $root ('artifacts/deployments/' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $backup
$before = @()
foreach ($name in $inputs.Keys) {
    $installed = Join-Path $target $name
    Copy-Item -LiteralPath $installed -Destination (Join-Path $backup $name)
    $hash = (Get-FileHash -LiteralPath $installed).Hash
    if ($hash -ne (Get-FileHash -LiteralPath (Join-Path $backup $name)).Hash) { throw 'Backup hash mismatch.' }
    $before += [ordered]@{ File = $name; SHA256 = $hash }
}
[ordered]@{Target=$target;Files=$before} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $backup 'before.json') -Encoding utf8NoBOM
if (Get-Process DSPGAME -ErrorAction SilentlyContinue) { throw 'DSPGAME started during backup; nothing deployed.' }
try {
    foreach ($name in $inputs.Keys) {
        $installed = Join-Path $target $name
        Copy-Item -LiteralPath $inputs[$name] -Destination $installed -Force
        if ((Get-FileHash -LiteralPath $installed).Hash -ne (Get-FileHash -LiteralPath $inputs[$name]).Hash) { throw "Deployment hash mismatch: $name" }
    }
} catch {
    foreach ($name in $inputs.Keys) { Copy-Item -LiteralPath (Join-Path $backup $name) -Destination (Join-Path $target $name) -Force }
    throw
}
$result = [ordered]@{
    Target = $target
    Backup = $backup
    Version = [Reflection.AssemblyName]::GetAssemblyName((Join-Path $target 'DSPAAMod.dll')).Version.ToString()
    Files = @($inputs.Keys | ForEach-Object { [ordered]@{File=$_; SHA256=(Get-FileHash -LiteralPath (Join-Path $target $_)).Hash} })
}
$result | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $backup 'result.json') -Encoding utf8NoBOM
$result | ConvertTo-Json -Depth 4
Write-Output 'Replaced only the two plugin DLLs. Runtime, other package files, other mods and saves were not modified. Game was not launched.'
