[CmdletBinding()]
param([string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Push-Location $root
try {
    dotnet build tests/managed/DSPAAMod.Tests.csproj -c $Configuration --nologo
    if ($LASTEXITCODE -ne 0) { throw 'Managed test build failed.' }
    $output = & dotnet "tests/managed/bin/$Configuration/net8.0/DSPAAMod.Tests.dll" (Join-Path $root "build/native/$Configuration/DSPAANative.dll") 2>&1
    $exitCode = $LASTEXITCODE
    $output | Write-Output
    foreach ($line in $output) {
        if ("$line" -match '^interop_artifact=(.+)$') {
            $path = [IO.Path]::GetFullPath($Matches[1])
            if ([IO.Path]::GetDirectoryName($path).TrimEnd('\', '/') -ne [IO.Path]::GetTempPath().TrimEnd('\', '/') -or
                [IO.Path]::GetFileName($path) -notmatch '^DSPAAMod-managed-[0-9a-f]{32}$') { throw 'Unsafe test cleanup path.' }
            Remove-Item -LiteralPath $path -Recurse -Force
        }
    }
    if ($exitCode -ne 0) { throw "Managed tests failed: $exitCode" }
} finally { Pop-Location }
