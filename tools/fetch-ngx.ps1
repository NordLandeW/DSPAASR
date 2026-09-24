[CmdletBinding()]
param([switch]$IncludeDevelopment)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$root = Split-Path $PSScriptRoot -Parent
$destination = Join-Path $root 'external/ngx'
$commit = '374959484e79a640feaba44c93ac8cfb0a03f5b5'
$base = "https://raw.githubusercontent.com/NVIDIA/DLSS/$commit"
$files = @(
    @('include/nvsdk_ngx.h', 'include/nvsdk_ngx.h', 'DC38E7467CF415379C9D12AE1B6E4A494C453ED92720FB53E92AECB523E7B848'),
    @('include/nvsdk_ngx_defs.h', 'include/nvsdk_ngx_defs.h', 'EA23F33497CD274860D1C25A97644FCE807DCB0037C594547203343103FAD03E'),
    @('include/nvsdk_ngx_params.h', 'include/nvsdk_ngx_params.h', '943BC8CC5CDAE03B6303016FBAD3183636F2335AE27A2D18776798C3B4EFABBC'),
    @('include/nvsdk_ngx_helpers_d3d.h', 'include/nvsdk_ngx_helpers_d3d.h', 'EC75224F36ED6580BAAF250FA83620405AD81A1D98C2C436654A0C9CF6A6B8BA'),
    @('include/nvsdk_ngx_helpers.h', 'include/nvsdk_ngx_helpers.h', '5BCBADFE7478B802CF6D3ACA4DC5DDD7D0889B99726E69C63F9E9BD555F44471'),
    @('include/nvsdk_ngx_helpers_cuda.h', 'include/nvsdk_ngx_helpers_cuda.h', 'F2851A76107BDF4FBCB7B261F3C35573C312D8ECB4BC18FB02D9E2CEC7705042'),
    @('lib/Windows_x86_64/x64/nvsdk_ngx_d.lib', 'lib/nvsdk_ngx_d.lib', '4B6CECAD7F1906571C94010241F650E4A5457E64FAD49DDACCB82DE79F6C2999'),
    @('LICENSE.txt', 'LICENSE.txt', 'D4216E39EBEF5F9B50A6712EBB37BEEB5379862A67733A9999C651F21592AAF0'),
    @('lib/Windows_x86_64/rel/nvngx_dlss.dll', 'runtime/rel/nvngx_dlss.dll', '3975567B8943C53ACCE397F2B72380092F84F162D00B0D2C7D08A1025C563983')
)
if ($IncludeDevelopment) {
    $files += ,@('lib/Windows_x86_64/dev/nvngx_dlss.dll', 'runtime/dev/nvngx_dlss.dll', '378262F4BA429E6199CE25D6F9275734F354C3B017F3F096D8CEA1AF468312FF')
}
. (Join-Path $PSScriptRoot 'verified-download.ps1')
Invoke-PinnedDownload -BaseUri $base -Destination $destination -Files $files -VerifyNvidiaRuntime
Write-Output 'NGX SDK 310.9.1 verified. Read external/ngx/LICENSE.txt; development DLLs must not be distributed.'
