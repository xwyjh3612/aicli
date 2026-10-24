param(
    [string]$Output = '',
    [string]$TerminalPackage = '',
    [string]$ConptyPackage = '',
    [string]$WorkingRoot = '',
    [string]$Configuration = 'Debug',
    [string]$Platform = 'x64',
    [string]$PackageIdentityName = 'AiCli.Desktop.Dev',
    [string]$ApplicationLogo = ''
)
$ErrorActionPreference = 'Stop'
Import-Module Microsoft.PowerShell.Utility -ErrorAction Stop
Add-Type -AssemblyName System.Drawing
$desktopRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
. (Join-Path $desktopRoot '..\tools\dependency-paths.ps1')
$dependencies = Get-AicliDependencyPaths
$runtimeCacheRoot = Join-Path (Get-AicliBuildCacheRoot) 'aicli\runtime-stage'
if (!$Output) { $Output = Join-Path $desktopRoot 'bin\Debug' }
if (!$TerminalPackage) { $TerminalPackage = $dependencies.terminalRuntime }
if (!$ConptyPackage) { $ConptyPackage = $dependencies.conpty }
if (!$WorkingRoot) {
    $WorkingRoot = Join-Path $runtimeCacheRoot "$PackageIdentityName\$Configuration"
}
if ([IO.Path]::GetPathRoot([IO.Path]::GetFullPath($Output)) -eq 'F:\') {
    throw 'F: is reserved for shared dependencies. Choose an output directory on C:.'
}

if ($PackageIdentityName -notmatch '^[A-Za-z0-9.-]+$') {
    throw "Invalid package identity name: $PackageIdentityName"
}

function Get-MakePriPath {
    $kitsRoot = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots' -ErrorAction Stop).KitsRoot10
    $candidates = Get-ChildItem (Join-Path $kitsRoot 'bin') -Directory -ErrorAction Stop |
        Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' } |
        Sort-Object { [version]$_.Name } -Descending |
        ForEach-Object { Join-Path $_.FullName 'x64\makepri.exe' } |
        Where-Object { Test-Path -LiteralPath $_ }
    $result = $candidates | Select-Object -First 1
    if (!$result) { throw 'makepri.exe was not found in the installed Windows SDK.' }
    return $result
}

function Copy-FileIfDifferent {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination
    )

    if (Test-Path -LiteralPath $Destination) {
        $sourceInfo = Get-Item -LiteralPath $Source
        $destinationInfo = Get-Item -LiteralPath $Destination
        if ($sourceInfo.Length -eq $destinationInfo.Length) {
            $sourceHash = Get-AicliFileSha256 -LiteralPath $Source
            $destinationHash = Get-AicliFileSha256 -LiteralPath $Destination
            if ($sourceHash -eq $destinationHash) { return }
        }
    }
    Copy-Item -LiteralPath $Source -Destination $Destination -Force
}

function Export-ScaledLogo {
    param(
        [Parameter(Mandatory = $true)][System.Drawing.Image]$Source,
        [Parameter(Mandatory = $true)][int]$Size,
        [Parameter(Mandatory = $true)][string]$Destination
    )

    $bitmap = [System.Drawing.Bitmap]::new(
        $Size,
        $Size,
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.Clear([System.Drawing.Color]::Transparent)
        $graphics.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
        $graphics.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
        $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
        $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $graphics.DrawImage(
            $Source,
            [System.Drawing.Rectangle]::new(0, 0, $Size, $Size))
        $bitmap.Save($Destination, [System.Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $graphics.Dispose()
        $bitmap.Dispose()
    }
}

function Export-PackageLogoAssets {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$DestinationDirectory
    )

    # The taskbar uses target-size package assets when buttons are combined.
    # Supplying the unplated variants prevents Windows from placing the app
    # icon on an opaque system-accent (#0078d7) square.
    $sourceImage = [System.Drawing.Image]::FromFile($Source)
    try {
        Export-ScaledLogo -Source $sourceImage -Size 44 `
            -Destination (Join-Path $DestinationDirectory 'Square44x44Logo.png')
        Export-ScaledLogo -Source $sourceImage -Size 150 `
            -Destination (Join-Path $DestinationDirectory 'Square150x150Logo.png')
        Export-ScaledLogo -Source $sourceImage -Size 50 `
            -Destination (Join-Path $DestinationDirectory 'StoreLogo.png')

        $scaleSizes = [ordered]@{
            100 = 44
            125 = 55
            150 = 66
            200 = 88
            400 = 176
        }
        foreach ($entry in $scaleSizes.GetEnumerator()) {
            Export-ScaledLogo -Source $sourceImage -Size $entry.Value `
                -Destination (Join-Path $DestinationDirectory `
                    "Square44x44Logo.scale-$($entry.Key).png")
        }

        $tileScaleSizes = [ordered]@{
            100 = 150
            125 = 188
            150 = 225
            200 = 300
            400 = 600
        }
        foreach ($entry in $tileScaleSizes.GetEnumerator()) {
            Export-ScaledLogo -Source $sourceImage -Size $entry.Value `
                -Destination (Join-Path $DestinationDirectory `
                    "Square150x150Logo.scale-$($entry.Key).png")
        }

        foreach ($size in @(16, 20, 24, 30, 32, 36, 40, 48, 60, 64, 72, 80, 96, 256)) {
            $plated = Join-Path $DestinationDirectory `
                "Square44x44Logo.targetsize-$size.png"
            $unplated = Join-Path $DestinationDirectory `
                "Square44x44Logo.targetsize-$($size)_altform-unplated.png"
            $lightUnplated = Join-Path $DestinationDirectory `
                "Square44x44Logo.targetsize-$($size)_altform-lightunplated.png"
            Export-ScaledLogo -Source $sourceImage -Size $size -Destination $plated
            Copy-Item -LiteralPath $plated -Destination $unplated -Force
            # Windows requests lightunplated on light taskbars and in several
            # shell surfaces such as Task Manager. Without it, Explorer can
            # fall back to the plated asset and add the #0078d7 accent square.
            Copy-Item -LiteralPath $plated -Destination $lightUnplated -Force
        }
    } finally {
        $sourceImage.Dispose()
    }
}

function Save-XmlUtf8 {
    param(
        [Parameter(Mandatory = $true)][xml]$Document,
        [Parameter(Mandatory = $true)][string]$Path
    )

    $settings = [Xml.XmlWriterSettings]::new()
    $settings.Encoding = [Text.UTF8Encoding]::new($false)
    $settings.Indent = $true
    $settings.NewLineChars = "`r`n"
    $settings.NewLineHandling = [Xml.NewLineHandling]::Replace
    $writer = [Xml.XmlWriter]::Create($Path, $settings)
    try {
        $Document.Save($writer)
    } finally {
        $writer.Dispose()
    }
}

function Configure-PriIndex {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$ReswInitialPath
    )

    [xml]$config = [IO.File]::ReadAllText($Path, [Text.Encoding]::UTF8)
    $packaging = $config.resources.packaging
    if ($packaging) {
        [void]$config.resources.RemoveChild($packaging)
    }

    $reswIndexer = @($config.resources.index.'indexer-config') |
        Where-Object { [string]$_.type -ieq 'resw' } |
        Select-Object -First 1
    if (!$reswIndexer) {
        throw "PRI configuration is missing its RESW indexer: $Path"
    }
    $reswIndexer.SetAttribute('initialPath', $ReswInitialPath)
    Save-XmlUtf8 -Document $config -Path $Path
}

function Export-TerminalControlStrings {
    param(
        [Parameter(Mandatory = $true)][xml]$TerminalResources,
        [Parameter(Mandatory = $true)][string]$OutputRoot
    )

    $resourcePrefix = '/Microsoft.Terminal.Control/Resources/'
    $nodes = $TerminalResources.SelectNodes(
        "//NamedResource[contains(@uri, '$resourcePrefix') and Candidate[@type='String']]")
    if (!$nodes -or !$nodes.Count) {
        throw 'Windows Terminal PRI does not contain Terminal Control string resources.'
    }

    $stringsByLanguage = @{}
    foreach ($node in $nodes) {
        $uri = [string]$node.uri
        $prefixIndex = $uri.IndexOf($resourcePrefix, [StringComparison]::Ordinal)
        if ($prefixIndex -lt 0) { continue }
        $resourceKey = $uri.Substring($prefixIndex + $resourcePrefix.Length).Replace('/', '.')

        foreach ($candidate in @($node.Candidate)) {
            if ([string]$candidate.type -ne 'String') { continue }
            $languageQualifier = @($candidate.QualifierSet.Qualifier) |
                Where-Object { [string]$_.name -eq 'Language' } |
                Select-Object -First 1
            if (!$languageQualifier) { continue }

            $language = [string]$languageQualifier.value
            $valueNode = $candidate.SelectSingleNode('Value')
            if (!$language -or !$valueNode) { continue }
            $value = $valueNode.InnerText

            if (!$stringsByLanguage.ContainsKey($language)) {
                $stringsByLanguage[$language] = @{}
            }
            $languageStrings = $stringsByLanguage[$language]
            if ($languageStrings.ContainsKey($resourceKey) -and
                $languageStrings[$resourceKey] -ne $value) {
                throw "Conflicting Terminal Control resource: $language/$resourceKey"
            }
            $languageStrings[$resourceKey] = $value
        }
    }

    if (!$stringsByLanguage.ContainsKey('EN-US') -or
        !$stringsByLanguage['EN-US'].ContainsKey('InvalidUri')) {
        throw 'Windows Terminal PRI is missing the required EN-US InvalidUri resource.'
    }

    foreach ($language in ($stringsByLanguage.Keys | Sort-Object)) {
        $languageDir = Join-Path $OutputRoot $language
        New-Item -ItemType Directory -Force -Path $languageDir | Out-Null
        $reswPath = Join-Path $languageDir 'Resources.resw'

        $settings = [Xml.XmlWriterSettings]::new()
        $settings.Encoding = [Text.UTF8Encoding]::new($false)
        $settings.Indent = $true
        $settings.NewLineChars = "`r`n"
        $settings.NewLineHandling = [Xml.NewLineHandling]::Replace
        $writer = [Xml.XmlWriter]::Create($reswPath, $settings)
        try {
            $writer.WriteStartDocument()
            $writer.WriteStartElement('root')
            foreach ($entry in ($stringsByLanguage[$language].GetEnumerator() | Sort-Object Name)) {
                $writer.WriteStartElement('data')
                $writer.WriteAttributeString('name', [string]$entry.Key)
                $writer.WriteAttributeString(
                    'xml', 'space', 'http://www.w3.org/XML/1998/namespace', 'preserve')
                $writer.WriteElementString('value', [string]$entry.Value)
                $writer.WriteEndElement()
            }
            $writer.WriteEndElement()
            $writer.WriteEndDocument()
        } finally {
            $writer.Dispose()
        }
    }

    return [pscustomobject]@{
        Languages = $stringsByLanguage.Count
        Resources = $nodes.Count
    }
}

if (!(Test-Path -LiteralPath $TerminalPackage)) {
    throw "Windows Terminal package not found: $TerminalPackage"
}
if (!(Test-Path -LiteralPath $ConptyPackage)) {
    throw "Microsoft ConPTY package not found: $ConptyPackage"
}

New-Item -ItemType Directory -Force -Path $Output | Out-Null
$terminalFiles = @(
    'Microsoft.Terminal.Control.dll',
    'Microsoft.Terminal.Control.winmd',
    'Microsoft.Terminal.Settings.Model.dll',
    'Microsoft.Terminal.Settings.Model.winmd',
    'Microsoft.Terminal.UI.dll',
    'Microsoft.Terminal.UI.winmd',
    'Microsoft.Terminal.TerminalConnection.winmd'
)
foreach ($file in $terminalFiles) {
    $sourceFile = Join-Path $TerminalPackage $file
    # Microsoft's portable runtime ZIP omits the development WinMD files.
    # Keep the matching metadata in its own shared dependency directory.
    if ($file.EndsWith('.winmd') -and !(Test-Path -LiteralPath $sourceFile)) {
        $sourceFile = Join-Path $dependencies.terminalMetadata $file
    }
    Copy-FileIfDifferent -Source $sourceFile -Destination (Join-Path $Output $file)
}

# The application supplies its own ITerminalConnection implementation. Never
# leave Windows Terminal's private WinRT connection/proxy binaries beside the
# app: loading one can recreate the historical startup crash. A root-level
# OpenConsole.exe is legacy as well; the supported side-by-side host is staged
# below in the architecture folder expected by Microsoft's conpty.dll.
foreach ($legacy in @('TerminalConnection.dll', 'OpenConsoleProxy.dll', 'OpenConsole.exe')) {
    Remove-Item -LiteralPath (Join-Path $Output $legacy) -Force -ErrorAction SilentlyContinue
}

$conptyDll = Join-Path $ConptyPackage 'runtimes\win-x64\native\conpty.dll'
$openConsole = Join-Path $ConptyPackage 'build\native\runtimes\x64\OpenConsole.exe'
$openConsoleDir = Join-Path $Output 'x64'
New-Item -ItemType Directory -Force -Path $openConsoleDir | Out-Null
Copy-FileIfDifferent -Source $conptyDll -Destination (Join-Path $Output 'conpty.dll')
Copy-FileIfDifferent -Source $openConsole -Destination (Join-Path $openConsoleDir 'OpenConsole.exe')

# Terminal's DLL and compiled XAML must be from the exact same build. The
# installed package keeps its 1.24 XBF files embedded in the package PRI, so
# extract those files and build a component PRI for the loose WinRT DLL.
$outputFull = [IO.Path]::GetFullPath($Output).TrimEnd('\')
$controlResourceDir = Join-Path $outputFull 'Microsoft.Terminal.Control'
$controlResourceFull = [IO.Path]::GetFullPath($controlResourceDir)
if (!$controlResourceFull.StartsWith($outputFull + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to replace a control resource directory outside the output: $controlResourceFull"
}
if (Test-Path -LiteralPath $controlResourceFull) {
    Remove-Item -LiteralPath $controlResourceFull -Recurse -Force
}
Remove-Item -LiteralPath (Join-Path $outputFull 'Microsoft.Terminal.Control.pri') -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath (Join-Path $outputFull 'Microsoft.Terminal.Settings.Model.pri') -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $controlResourceFull | Out-Null

$workingFull = [IO.Path]::GetFullPath($WorkingRoot).TrimEnd('\')
$allowedWorkingRoot = [IO.Path]::GetFullPath($runtimeCacheRoot).TrimEnd('\')
if (!$workingFull.StartsWith($allowedWorkingRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to use a runtime staging directory outside $allowedWorkingRoot`: $workingFull"
}
if (Test-Path -LiteralPath $workingFull) {
    Remove-Item -LiteralPath $workingFull -Recurse -Force
}
$layoutRoot = Join-Path $workingFull 'layout'
$layoutControl = Join-Path $layoutRoot 'Microsoft.Terminal.Control'
New-Item -ItemType Directory -Force -Path $layoutControl | Out-Null

$makePri = Get-MakePriPath
$terminalPri = Join-Path $TerminalPackage 'resources.pri'
$terminalPriXml = Join-Path $workingFull 'terminal-resources.xml'
& $makePri dump /if $terminalPri /of $terminalPriXml /dt Detailed /o | Out-Null
if ($LASTEXITCODE) { throw "Failed to dump Windows Terminal resources.pri: $LASTEXITCODE" }

$utf8NoBom = [Text.UTF8Encoding]::new($false, $true)
[xml]$terminalResources = [IO.File]::ReadAllText($terminalPriXml, $utf8NoBom)
$terminalStringSource = Join-Path $workingFull 'terminal-control-strings'
$terminalStringSummary = Export-TerminalControlStrings -TerminalResources $terminalResources -OutputRoot $terminalStringSource
$requiredXbf = @('SearchBoxControl.xbf', 'TermControl.xbf')
foreach ($name in $requiredXbf) {
    $node = $terminalResources.SelectSingleNode("//NamedResource[@name='$name' and contains(@uri, '/Files/Microsoft.Terminal.Control/')]")
    if (!$node -or !$node.Candidate.Base64Value) {
        throw "Windows Terminal 1.24 resource is missing: $name"
    }
    $bytes = [Convert]::FromBase64String([string]$node.Candidate.Base64Value)
    if ($bytes.Length -lt 4 -or $bytes[0] -ne 0x58 -or $bytes[1] -ne 0x42 -or $bytes[2] -ne 0x46) {
        throw "Extracted resource is not an XBF file: $name"
    }
    [IO.File]::WriteAllBytes((Join-Path $controlResourceFull $name), $bytes)
    [IO.File]::WriteAllBytes((Join-Path $layoutControl $name), $bytes)
}
Copy-Item -LiteralPath $terminalStringSource -Destination (Join-Path $layoutRoot 'Resources') -Recurse -Force

$priConfig = Join-Path $workingFull 'priconfig.xml'
& $makePri createconfig /cf $priConfig /dq en-US /pv 10.0.0 /o | Out-Null
if ($LASTEXITCODE) { throw "Failed to create Terminal Control PRI configuration: $LASTEXITCODE" }
Configure-PriIndex -Path $priConfig -ReswInitialPath ''
& $makePri new /pr $layoutRoot /cf $priConfig /in Microsoft.Terminal.Control /of (Join-Path $outputFull 'Microsoft.Terminal.Control.pri') /o | Out-Null
if ($LASTEXITCODE) { throw "Failed to create Terminal Control PRI: $LASTEXITCODE" }

$xaml = $dependencies.winui
$xamlPri = Join-Path $xaml 'resources.pri'
$xamlPriXml = Join-Path $workingFull 'xaml-resources.xml'
& $makePri dump /if $xamlPri /of $xamlPriXml /dt Basic /o | Out-Null
if ($LASTEXITCODE) { throw "Failed to dump Microsoft.UI.Xaml resources.pri: $LASTEXITCODE" }
[xml]$xamlPriResources = [IO.File]::ReadAllText($xamlPriXml, $utf8NoBom)
$xamlResourceMap = $xamlPriResources.SelectSingleNode('//ResourceMap[@primary="true"]')
if (!$xamlResourceMap -or ![string]$xamlResourceMap.name) {
    throw 'Microsoft.UI.Xaml resources.pri does not contain a primary resource map.'
}

# Terminal's ScopedResourceLoader reads ResourceManager::Current().MainResourceMap(),
# not Microsoft.Terminal.Control.pri directly. Import the complete WinUI PRI and
# graft Terminal Control's exact 1.24 strings into that main map. Without this,
# hovering an invalid auto-detected URI dereferences a null ResourceMap inside
# Microsoft.Terminal.Control.dll and crashes the process.
$mainLayoutRoot = Join-Path $workingFull 'main-layout'
New-Item -ItemType Directory -Force -Path $mainLayoutRoot | Out-Null
Copy-Item -LiteralPath $xamlPri -Destination (Join-Path $mainLayoutRoot 'Microsoft.UI.Xaml.pri') -Force
Copy-Item -LiteralPath $terminalStringSource -Destination (Join-Path $mainLayoutRoot 'Resources') -Recurse -Force
$logoSource = if ($ApplicationLogo) { $ApplicationLogo } else { Join-Path $xaml 'logo.png' }
if (!(Test-Path -LiteralPath $logoSource)) {
    throw "Application logo was not found: $logoSource"
}
$mainLayoutAssets = Join-Path $mainLayoutRoot 'Assets'
New-Item -ItemType Directory -Force -Path $mainLayoutAssets | Out-Null
Copy-Item -LiteralPath $logoSource -Destination (Join-Path $mainLayoutAssets 'Logo.png') -Force
Export-PackageLogoAssets -Source $logoSource -DestinationDirectory $mainLayoutAssets
$mainPriConfig = Join-Path $workingFull 'main-priconfig.xml'
& $makePri createconfig /cf $mainPriConfig /dq en-US /pv 10.0.0 /o | Out-Null
if ($LASTEXITCODE) { throw "Failed to create main PRI configuration: $LASTEXITCODE" }
Configure-PriIndex -Path $mainPriConfig -ReswInitialPath 'Microsoft.Terminal.Control'
& $makePri new /pr $mainLayoutRoot /cf $mainPriConfig /in ([string]$xamlResourceMap.name) /of (Join-Path $outputFull 'resources.pri') /o | Out-Null
if ($LASTEXITCODE) { throw "Failed to create merged application PRI: $LASTEXITCODE" }

$mergedPriXml = Join-Path $workingFull 'merged-resources.xml'
& $makePri dump /if (Join-Path $outputFull 'resources.pri') /of $mergedPriXml /dt Basic /o | Out-Null
if ($LASTEXITCODE) { throw "Failed to verify merged application PRI: $LASTEXITCODE" }
[xml]$mergedResources = [IO.File]::ReadAllText($mergedPriXml, $utf8NoBom)
$invalidUriResource = $mergedResources.SelectSingleNode(
    "//NamedResource[contains(@uri, '/Microsoft.Terminal.Control/Resources/InvalidUri')]")
if (!$invalidUriResource) {
    throw 'Merged application PRI is missing Microsoft.Terminal.Control/Resources/InvalidUri.'
}

Copy-FileIfDifferent -Source (Join-Path $xaml 'Microsoft.UI.Xaml.dll') -Destination (Join-Path $Output 'Microsoft.UI.Xaml.dll')
Copy-FileIfDifferent -Source (Join-Path $xaml 'Microsoft.UI.Xaml.winmd') -Destination (Join-Path $Output 'Microsoft.UI.Xaml.winmd')
$xamlResourceFiles = Join-Path $xaml 'Microsoft.UI.Xaml'
if (Test-Path -LiteralPath $xamlResourceFiles) {
    Copy-Item $xamlResourceFiles $Output -Recurse -Force
}
$packageRoot = Join-Path $PSScriptRoot '..\package'
$assets = Join-Path $Output 'Assets'
New-Item -ItemType Directory -Force -Path $assets | Out-Null
$manifestTemplate = Join-Path $packageRoot 'AppxManifest.xml'
$manifestOutput = Join-Path $Output 'AppxManifest.xml'
$manifestText = [IO.File]::ReadAllText($manifestTemplate)
$manifestText = [Text.RegularExpressions.Regex]::Replace(
    $manifestText,
    '(<Identity\s+Name=")[^"]+("[^>]*>)',
    { param($match) $match.Groups[1].Value + $PackageIdentityName + $match.Groups[2].Value },
    [Text.RegularExpressions.RegexOptions]::IgnoreCase)
[IO.File]::WriteAllText($manifestOutput, $manifestText, [Text.UTF8Encoding]::new($false))
Copy-Item (Join-Path $PSScriptRoot 'configure-agent-firewall.ps1') $Output -Force
Copy-Item -Path (Join-Path $mainLayoutAssets '*') -Destination $assets -Force
$openssl = $dependencies.openssl
Copy-FileIfDifferent -Source (Join-Path $openssl 'libssl-4-x64.dll') -Destination (Join-Path $Output 'libssl-4-x64.dll')
Copy-FileIfDifferent -Source (Join-Path $openssl 'libcrypto-4-x64.dll') -Destination (Join-Path $Output 'libcrypto-4-x64.dll')
$sha256 = [Security.Cryptography.SHA256]::Create()
$controlHashes = Get-ChildItem -LiteralPath $controlResourceFull -Filter '*.xbf' |
    ForEach-Object {
        $stream = [IO.File]::OpenRead($_.FullName)
        try {
            $hash = [BitConverter]::ToString($sha256.ComputeHash($stream)).Replace('-', '')
            [pscustomobject]@{ Path = $_.FullName; Hash = $hash }
        } finally {
            $stream.Dispose()
        }
    }
$sha256.Dispose()
Write-Host "Windows Terminal 1.24.11911.0 runtime staged to $Output"
Write-Host "Microsoft ConPTY 1.24.260710001 runtime staged with x64 OpenConsole"
Write-Host "Terminal Control strings merged: $($terminalStringSummary.Resources) resources, $($terminalStringSummary.Languages) languages"
$controlHashes | ForEach-Object { Write-Host "  $([IO.Path]::GetFileName($_.Path))  $($_.Hash)" }
