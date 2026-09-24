[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$root = Split-Path $PSScriptRoot -Parent
$destination = Join-Path $root 'external/nvapi'
$base = 'https://raw.githubusercontent.com/NVIDIA/nvapi/70d337db9186e968eab622f7e786de7e437faf3d'
$files = @(
    @('License.txt', 'License.txt', '84726B60C17FE0F71F92A6C074C3D4087370886EBA5D21CCC4EC55DB37D6C8BA'),
    @('nvapi.h', 'nvapi.h', '76BBB71107C1388134D41712BED56BB6D80EC426F9BB2C168E2D6941B90F16CA'),
    @('NvApiDriverSettings.h', 'NvApiDriverSettings.h', '95FFDA903832840343A81189E052A0756774BD92E4B37F2DC49598D493F951D4'),
    @('nvapi_lite_common.h', 'nvapi_lite_common.h', '3904BAAB8E8501A53746DDF4E5B13A2E1D5ACD6EE50F8C3C2EADD8E4D62C55F2'),
    @('nvapi_lite_d3dext.h', 'nvapi_lite_d3dext.h', '2322DB28A2F7384FF6DF6FD1110B48E99660FA29956A2CBCD28A9D7157017E1A'),
    @('nvapi_lite_salend.h', 'nvapi_lite_salend.h', '92017F8E26F585601F4EAAC475FA9E542EAA2F99A80E376DDE52F2F73AB18E67'),
    @('nvapi_lite_salstart.h', 'nvapi_lite_salstart.h', '9DB770E52947B33D68B0C1F17BC21EDD9D736B824CCECEE2C5C527D48A89BE93'),
    @('nvapi_lite_sli.h', 'nvapi_lite_sli.h', '2A9C75FAECB32BC7A5237A4B83CC28A6A488C75493E44BF28688ABF3FEC01A34'),
    @('nvapi_lite_stereo.h', 'nvapi_lite_stereo.h', 'BD6FE69136CFF36C73A9967A7CEF37186791D9B7C3EB3AE7AC61B4EFA628325B'),
    @('nvapi_lite_surround.h', 'nvapi_lite_surround.h', '197AD1E4762B2243904B0A13418747436AE70E4824099BF56BE4804C4C29956F'),
    @('amd64/nvapi64.lib', 'nvapi64.lib', '901E479E548D41C30688C7481F0C6353C0BE7FDEE978796D27D87C32E19CE172')
)
. (Join-Path $PSScriptRoot 'verified-download.ps1')
Invoke-PinnedDownload -BaseUri $base -Destination $destination -Files $files
Write-Output 'Optional NVAPI diagnostics SDK verified; this script does not modify driver settings.'
