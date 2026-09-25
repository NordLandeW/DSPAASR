[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$root = Split-Path $PSScriptRoot -Parent
$destination = Join-Path $root 'external/fsr-sdk'
$commit = '60f4ea81909200d8542eca14dccb2628b763a9a3'
$base = "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/$commit"
$files = @(
    @('Kits/FidelityFX/api/include/ffx_api.h', 'Kits/FidelityFX/api/include/ffx_api.h', '91F7F4A9111D18996E3BAE083BF82D47AC6497DE144B352ABFCA44F07D2871C4'),
    @('Kits/FidelityFX/api/include/ffx_api_types.h', 'Kits/FidelityFX/api/include/ffx_api_types.h', 'B54FBD96A0EED82662A49C00E28E5368AB69959F9856DAE5C12FED109D123D66'),
    @('Kits/FidelityFX/api/include/ffx_api_loader.h', 'Kits/FidelityFX/api/include/ffx_api_loader.h', '10795186FD2A8FF53BB13AA0C81079C7FFD4A0B1BB501C629ED0B874C492A20B'),
    @('Kits/FidelityFX/api/include/dx12/ffx_api_dx12.h', 'Kits/FidelityFX/api/include/dx12/ffx_api_dx12.h', '2082D6C2914E9C2FA9FAE6247D35F64C643CCB703311C83C8DBA9A35356BC711'),
    @('Kits/FidelityFX/upscalers/include/ffx_upscale.h', 'Kits/FidelityFX/upscalers/include/ffx_upscale.h', 'F13ABCDD4389E22AA50562A90255BEC9511E004722C7D2BF6F959D92FD78355C'),
    @('Kits/FidelityFX/framegeneration/include/ffx_framegeneration.h', 'Kits/FidelityFX/framegeneration/include/ffx_framegeneration.h', '83090E298D9D0DB4CDBA76A4E0846ACF80FE863233F7E9ECBFDD97DF5CC646BD'),
    @('Kits/FidelityFX/framegeneration/include/dx12/ffx_api_framegeneration_dx12.h', 'Kits/FidelityFX/framegeneration/include/dx12/ffx_api_framegeneration_dx12.h', '8C1DE87E4C6D68908724A8AD71124297A8F7092CDF21AA032D361DE1C747CF26'),
    @('Kits/FidelityFX/signedbin/amd_fidelityfx_framegeneration_dx12.dll', 'Kits/FidelityFX/signedbin/amd_fidelityfx_framegeneration_dx12.dll', '02297BEEDD285E822D3A64F314CF00FAF378DCEC0EDC47FF0C4DD71B3A8C2F18'),
    @('docs/license.md', 'docs/license.md', 'F0DA09D71AD5C82759A179E774535D4A829E5C96C49294167C1152402B2CB400'),
    @('Kits/FidelityFX/signedbin/amd_fidelityfx_loader_dx12.dll', 'Kits/FidelityFX/signedbin/amd_fidelityfx_loader_dx12.dll', 'E2D85AA05A9BD9ED8B38935FDF5199372CCA6F74C12015143BB6F945EE1608AA'),
    @('Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_dx12.dll', 'Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_dx12.dll', 'D0DCCCC74A43C44BA435B7A369B456E0970D8A4464E4BD683119B374F2C9FB46')
)
. (Join-Path $PSScriptRoot 'verified-download.ps1')
Invoke-PinnedDownload -BaseUri $base -Destination $destination -Files $files
foreach ($file in $files | Where-Object { $_[1] -like '*.dll' }) {
    $target = Join-Path $destination $file[1]
    $signature = Get-AuthenticodeSignature -LiteralPath $target
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'CN=Advanced Micro Devices(?:,|$)') {
        throw "Runtime signature is not valid AMD Authenticode: $target"
    }
}
Write-Output 'FSR SDK 2.3.0 verified. Analytical upscaler 3.1.5 / frame generation 3.1.6 and swapchain 3.1.7 are explicitly selected at runtime; ML is excluded. Read external/fsr-sdk/docs/license.md.'
