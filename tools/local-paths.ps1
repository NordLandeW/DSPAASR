# Shared, read-only path defaults. This does not evaluate MSBuild code.
function Resolve-DspLocalPath {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][ValidateSet('BepInExPath','DspLibsPath','GameDirectory')][string]$Name,
        [string]$Value = '',
        [Parameter(Mandatory)][string]$RepositoryRoot
    )
    if (![string]::IsNullOrWhiteSpace($Value)) { return $Value }
    $environmentValue = [Environment]::GetEnvironmentVariable($Name, 'Process')
    if (![string]::IsNullOrWhiteSpace($environmentValue)) { return $environmentValue }

    $file = Join-Path $RepositoryRoot 'Directory.Build.props'
    if (Test-Path -LiteralPath $file -PathType Leaf) {
        $settings = [Xml.XmlReaderSettings]::new()
        $settings.DtdProcessing = [Xml.DtdProcessing]::Prohibit
        $settings.XmlResolver = $null
        $reader = [Xml.XmlReader]::Create($file, $settings)
        try {
            $document = [Xml.XmlDocument]::new()
            $document.XmlResolver = $null
            $document.Load($reader)
        } finally { $reader.Dispose() }
        if ($document.DocumentElement.LocalName -ne 'Project') { throw 'Local path configuration must have a Project root.' }
        $nodes = @($document.SelectNodes("/*[local-name()='Project']/*[local-name()='PropertyGroup']/*[local-name()='$Name']"))
        if ($nodes.Count -gt 1) { throw "Directory.Build.props contains duplicate $Name values; use one literal default." }
        if ($nodes.Count -eq 1) {
            $node = $nodes[0]
            # The shared subset accepts unconditional literals or the standard
            # MSBuild empty-property guard. Other conditions cannot be ignored.
            $guard = "'`$($Name)' == ''"
            $condition = $node.GetAttribute('Condition')
            if ($node.ParentNode.HasAttribute('Condition') -or
                ($condition -and $condition -cne $guard) -or
                @($node.SelectNodes('*')).Count) {
                throw "Directory.Build.props $Name must be a literal default with no custom condition."
            }
            $literal = $node.InnerText.Trim()
            if ($literal -match '[$@%]\(|%[0-9a-fA-F]{2}') { throw "Directory.Build.props $Name must not contain MSBuild expressions or escapes." }
            if ($literal -and ![IO.Path]::IsPathFullyQualified($literal)) {
                throw "Directory.Build.props $Name must be an absolute literal path."
            }
            if ($literal) { return $literal }
        }
    }
    throw "Set -$Name, the $Name environment variable, or a literal $Name in the repository's ignored Directory.Build.props."
}
