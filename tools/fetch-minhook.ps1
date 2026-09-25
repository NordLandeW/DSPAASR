[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$destination = Join-Path $root 'external/minhook'
$commit = 'c3fcafdc10146beb5919319d0683e44e3c30d537'
function Assert-Pinned([string]$Directory) {
    $head = & git -C $Directory rev-parse HEAD
    if ($LASTEXITCODE -or $head -ne $commit) { throw 'Existing MinHook checkout is not the fixed v1.3.4 commit; refusing to replace it.' }
    $dirty = & git -C $Directory status --porcelain
    if ($LASTEXITCODE -or $dirty) { throw 'MinHook has local changes; refusing to overwrite them.' }
}
if (Test-Path -LiteralPath $destination) { Assert-Pinned $destination }
else {
    $null = New-Item -ItemType Directory -Force -Path (Split-Path $destination -Parent)
    $temporary = $destination + '.download-' + [Guid]::NewGuid().ToString('N')
    try {
        & git clone --filter=blob:none --no-checkout https://github.com/TsudaKageyu/minhook.git $temporary
        if ($LASTEXITCODE) { throw 'MinHook clone failed.' }
        & git -C $temporary checkout --detach $commit
        if ($LASTEXITCODE) { throw 'MinHook fixed checkout failed.' }
        Assert-Pinned $temporary
        Move-Item -LiteralPath $temporary -Destination $destination
    } finally { if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Recurse -Force } }
}
Write-Output "MinHook v1.3.4 verified at $commit. Preserve the complete external/minhook/LICENSE.txt."
