function Invoke-PinnedDownload {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$BaseUri,
        [Parameter(Mandatory)][string]$Destination,
        [Parameter(Mandatory)][array]$Files,
        [switch]$VerifyNvidiaRuntime
    )
    foreach ($entry in $Files) {
        $target = Join-Path $Destination $entry[1]
        if (Test-Path -LiteralPath $target) {
            if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $entry[2]) {
                throw "Existing dependency has unexpected bytes; not overwriting: $target"
            }
        } else {
            $null = New-Item -ItemType Directory -Force -Path (Split-Path $target -Parent)
            $temporary = "$target.download-$([guid]::NewGuid().ToString('N'))"
            try {
                Invoke-WebRequest "$BaseUri/$($entry[0])" -OutFile $temporary -TimeoutSec 120
                if ((Get-FileHash -LiteralPath $temporary -Algorithm SHA256).Hash -ne $entry[2]) {
                    throw "Downloaded dependency does not match pinned SHA256: $($entry[0])"
                }
                Move-Item -LiteralPath $temporary -Destination $target
            } finally {
                if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary }
            }
        }
        if ($VerifyNvidiaRuntime -and [IO.Path]::GetExtension($target) -eq '.dll') {
            $signature = Get-AuthenticodeSignature -LiteralPath $target
            if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'CN=NVIDIA Corporation(?:,|$)') {
                throw "Runtime signature is not valid NVIDIA Authenticode: $target"
            }
        }
        Write-Output "Verified $($entry[1])"
    }
}
