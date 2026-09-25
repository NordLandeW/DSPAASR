[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$root = Split-Path $PSScriptRoot -Parent
$pins = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'streamline-pins.json') -Raw | ConvertFrom-Json -AsHashtable
. (Join-Path $PSScriptRoot 'verified-download.ps1')
$files = @(,@('LICENSE.txt','LICENSE.txt',$pins.licenseSHA256))
foreach ($header in $pins.headers.GetEnumerator()) { $files += ,@("include/$($header.Key)","include/$($header.Key)",$header.Value) }
Invoke-PinnedDownload -BaseUri "https://raw.githubusercontent.com/NVIDIA-RTX/Streamline/$($pins.commit)" -Destination (Join-Path $root 'external/streamline') -Files $files
$runtime = Join-Path $root 'external/streamline-runtime/rel'
$missing = @()
foreach ($entry in $pins.runtime.GetEnumerator()) {
    $target = Join-Path $runtime $entry.Key
    if (Test-Path -LiteralPath $target) {
        if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $entry.Value) { throw "Existing Streamline dependency differs; not overwriting: $target" }
    } else { $missing += $entry.Key }
}
if ($missing.Count) {
    Invoke-PinnedDownload -BaseUri "https://github.com/NVIDIA-RTX/Streamline/releases/download/v$($pins.version)" -Destination (Join-Path $root 'external') -Files @(,@($pins.archive,$pins.archive,$pins.archiveSHA256))
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead((Join-Path $root "external/$($pins.archive)"))
    try {
        $null = New-Item -ItemType Directory -Path $runtime -Force
        foreach ($name in $missing) {
            # Production files are bin/x64; bin/x64/development is NEVER selected.
            $entry = $archive.GetEntry("bin/x64/$name")
            if (!$entry) { throw "Pinned production archive is missing $name" }
            $target = Join-Path $runtime $name
            $stage = $target + '.stage-' + [guid]::NewGuid().ToString('N')
            try {
                [IO.Compression.ZipFileExtensions]::ExtractToFile($entry,$stage,$false)
                if ((Get-FileHash -LiteralPath $stage -Algorithm SHA256).Hash -ne $pins.runtime[$name]) { throw "Pinned runtime extraction differs: $name" }
                [IO.File]::Move($stage,$target) # Do not overwrite a file which appeared concurrently.
            } finally { if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage } }
        }
    } finally { $archive.Dispose() }
}
foreach ($entry in $pins.runtime.GetEnumerator()) {
    $target = Join-Path $runtime $entry.Key
    if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $entry.Value) { throw "Streamline runtime hash mismatch: $target" }
    if ($entry.Key.EndsWith('.dll')) {
        $signature = Get-AuthenticodeSignature -LiteralPath $target
        if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'CN=NVIDIA Corporation(?:,|$)') {
            throw "Runtime signature is not valid NVIDIA Authenticode: $target"
        }
    }
    Write-Output "Verified production Streamline $($entry.Key)"
}
Write-Output 'Streamline 2.14.1 production subset verified. Preserve the distinct Streamline, DLSS-G and Reflex licenses. No development plugins, OTA or driver configuration changes.'
