$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'verified-download.ps1')
$root = Split-Path $PSScriptRoot -Parent
$directory = Join-Path $root ('artifacts/download-test-' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $directory
try {
    $file = Join-Path $directory 'existing.data'
    $bytes = [byte[]](13, 10, 32, 0, 255, 123)
    [IO.File]::WriteAllBytes($file, $bytes)
    $expected = (Get-FileHash -LiteralPath $file).Hash
    $entries = ,@('not-requested', 'existing.data', $expected)
    # Deliberately unreachable: a valid cached file must not cause any network access.
    Invoke-PinnedDownload -BaseUri 'http://127.0.0.1:1' -Destination $directory -Files $entries
    $rejected = $false
    try {
        Invoke-PinnedDownload -BaseUri 'http://127.0.0.1:1' -Destination $directory -Files (, @('unused', 'existing.data', ('0' * 64)))
    } catch {
        if ($_.Exception.Message -notlike 'Existing dependency has unexpected bytes*') { throw }
        $rejected = $true
    }
    if (!$rejected) { throw 'A modified dependency was not rejected.' }
    if ((Get-FileHash -LiteralPath $file).Hash -ne $expected) { throw 'The original dependency was overwritten.' }
    if ((Get-ChildItem -LiteralPath $directory -File).Count -ne 1) { throw 'Unexpected download side effect.' }
    Write-Output 'Cached dependency and no-clobber behavior passed.'
} finally { Remove-Item -LiteralPath $directory -Recurse -Force }
