# Pure path-resolution tests. No MSBuild, game, native DLL or deployment runs.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'local-paths.ps1')
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('DSPAASR-paths-' + [Guid]::NewGuid().ToString('N'))
$names = @('BepInExPath','DspLibsPath','GameDirectory')
$environmentBefore = @{}
foreach ($name in $names) { $environmentBefore[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
$script:passed = 0
function Require($Condition, [string]$Message) { if (!$Condition) { throw $Message }; $script:passed++ }
function Reject([scriptblock]$Action, [string]$Pattern) {
    $caught = ''
    try { & $Action | Out-Null } catch { $caught = $_.Exception.Message }
    Require ($caught -and $caught -like $Pattern) "Expected rejection '$Pattern', received '$caught'."
}
try {
    $null = New-Item -ItemType Directory -Path $temporary
    $file = Join-Path $temporary 'Directory.Build.props'
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $null, 'Process') }
    Reject { Resolve-DspLocalPath -Name GameDirectory -RepositoryRoot $temporary } '*Set -GameDirectory*'
    Require (!(Test-Path -LiteralPath $file)) 'Missing configuration was created.'

    $explicit = Join-Path $temporary 'explicit path'
    $environment = Join-Path $temporary 'environment path'
    $local = Join-Path $temporary 'local path with spaces'
    [IO.File]::WriteAllText($file, '<broken')
    [Environment]::SetEnvironmentVariable('GameDirectory', $environment, 'Process')
    Require ((Resolve-DspLocalPath -Name GameDirectory -Value $explicit -RepositoryRoot $temporary) -ceq $explicit) 'Explicit value did not override environment and malformed local configuration.'
    Require ((Resolve-DspLocalPath -Name GameDirectory -RepositoryRoot $temporary) -ceq $environment) 'Environment did not override malformed local configuration.'
    [Environment]::SetEnvironmentVariable('GameDirectory', $null, 'Process')
    Reject { Resolve-DspLocalPath -Name GameDirectory -RepositoryRoot $temporary } '*'

    $escaped = [Security.SecurityElement]::Escape($local)
    $properties = ($names | ForEach-Object { '<' + $_ + ' Condition="''$(' + $_ + ')'' == ''''">' + $escaped + '</' + $_ + '>' }) -join ''
    $xml = '<Project><PropertyGroup>' + $properties + '</PropertyGroup></Project>'
    [IO.File]::WriteAllText($file, $xml)
    $hash = (Get-FileHash -LiteralPath $file).Hash
    $written = (Get-Item -LiteralPath $file).LastWriteTimeUtc.Ticks
    foreach ($name in $names) {
        Require ((Resolve-DspLocalPath -Name $name -RepositoryRoot $temporary) -ceq $local) "Conditional local default failed for $name."
    }
    Require ((Get-FileHash -LiteralPath $file).Hash -ceq $hash) 'Reading changed configuration bytes.'
    Require ((Get-Item -LiteralPath $file).LastWriteTimeUtc.Ticks -eq $written) 'Reading changed configuration modification time.'

    [IO.File]::WriteAllText($file, ('<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003"><PropertyGroup><GameDirectory>' + $escaped + '</GameDirectory></PropertyGroup></Project>'))
    Require ((Resolve-DspLocalPath -Name GameDirectory -RepositoryRoot $temporary) -ceq $local) 'Namespaced literal property was not read.'
    foreach ($body in @('<PropertyGroup/>','<PropertyGroup><GameDirectory> </GameDirectory></PropertyGroup>')) {
        [IO.File]::WriteAllText($file, ('<Project>' + $body + '</Project>'))
        Reject { Resolve-DspLocalPath -Name GameDirectory -RepositoryRoot $temporary } '*Set -GameDirectory*'
    }
    [IO.File]::WriteAllText($file, ('<Project><PropertyGroup><GameDirectory>' + $escaped + '</GameDirectory><GameDirectory>' + $escaped + '</GameDirectory></PropertyGroup></Project>'))
    Reject { Resolve-DspLocalPath -Name GameDirectory -RepositoryRoot $temporary } '*duplicate GameDirectory*'
    foreach ($body in @(
        ('<PropertyGroup Condition="false"><GameDirectory>' + $escaped + '</GameDirectory></PropertyGroup>'),
        ('<PropertyGroup><GameDirectory Condition="false">' + $escaped + '</GameDirectory></PropertyGroup>'),
        '<PropertyGroup><GameDirectory><Nested/></GameDirectory></PropertyGroup>'
    )) {
        [IO.File]::WriteAllText($file, ('<Project>' + $body + '</Project>'))
        Reject { Resolve-DspLocalPath -Name GameDirectory -RepositoryRoot $temporary } '*literal default*'
    }
    foreach ($literal in @('$(MSBuildThisFileDirectory)','@(SomeItems)','%(SomeMetadata)','%24private')) {
        [IO.File]::WriteAllText($file, ('<Project><PropertyGroup><GameDirectory>' + $literal + '</GameDirectory></PropertyGroup></Project>'))
        Reject { Resolve-DspLocalPath -Name GameDirectory -RepositoryRoot $temporary } '*MSBuild expressions or escapes*'
    }
    [IO.File]::WriteAllText($file, '<Project><PropertyGroup><GameDirectory>relative/game</GameDirectory></PropertyGroup></Project>')
    Reject { Resolve-DspLocalPath -Name GameDirectory -RepositoryRoot $temporary } '*absolute literal path*'
    [IO.File]::WriteAllText($file, '<NotProject/>')
    Reject { Resolve-DspLocalPath -Name GameDirectory -RepositoryRoot $temporary } '*Project root*'
    [IO.File]::WriteAllText($file, '<!DOCTYPE Project [<!ENTITY path "not-expanded">]><Project><PropertyGroup><GameDirectory>&path;</GameDirectory></PropertyGroup></Project>')
    Reject { Resolve-DspLocalPath -Name GameDirectory -RepositoryRoot $temporary } '*DTD*'
    Write-Output "Local path resolution: $script:passed checks passed; no build/game/deployment executed."
} finally {
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $environmentBefore[$name], 'Process') }
    if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Recurse -Force }
}
