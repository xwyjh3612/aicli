param(
    [string]$Output = 'C:\project\aitest\codex_tool\desktop\bin\Debug',
    [string]$TerminalPackage = 'C:\Program Files\WindowsApps\Microsoft.WindowsTerminal_1.24.11911.0_x64__8wekyb3d8bbwe',
    [string]$WorkingRoot = 'F:\codex_tool-target\desktop\runtime-stage',
    [string]$Configuration = 'Debug',
    [string]$Platform = 'x64'
)
$ErrorActionPreference = 'Stop'

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

if (!(Test-Path -LiteralPath $TerminalPackage)) {
    throw "Windows Terminal package not found: $TerminalPackage"
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
    Copy-Item (Join-Path $TerminalPackage $file) $Output -Force
}

# The application supplies its own ITerminalConnection implementation backed
# by ConPTY.  Never leave the private connection/proxy binaries from the
# installed Windows Terminal package beside the app: loading one of them can
# silently select an incompatible ABI path (or recreate the historical
# 0xc0000142 startup crash).
foreach ($legacy in @('TerminalConnection.dll', 'OpenConsoleProxy.dll', 'OpenConsole.exe')) {
    Remove-Item -LiteralPath (Join-Path $Output $legacy) -Force -ErrorAction SilentlyContinue
}

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
$allowedWorkingRoot = [IO.Path]::GetFullPath('F:\codex_tool-target\desktop').TrimEnd('\')
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

[xml]$terminalResources = Get-Content -Raw -LiteralPath $terminalPriXml
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

$priConfig = Join-Path $workingFull 'priconfig.xml'
& $makePri createconfig /cf $priConfig /dq en-US /pv 10.0.0 /o | Out-Null
if ($LASTEXITCODE) { throw "Failed to create Terminal Control PRI configuration: $LASTEXITCODE" }
& $makePri new /pr $layoutRoot /cf $priConfig /in Microsoft.Terminal.Control /of (Join-Path $outputFull 'Microsoft.Terminal.Control.pri') /o | Out-Null
if ($LASTEXITCODE) { throw "Failed to create Terminal Control PRI: $LASTEXITCODE" }

$xaml = 'F:\deps\Microsoft.UI.Xaml.2.8'
Copy-Item (Join-Path $xaml 'Microsoft.UI.Xaml.dll'), (Join-Path $xaml 'Microsoft.UI.Xaml.winmd'), (Join-Path $xaml 'resources.pri') $Output -Force
$xamlResources = Join-Path $xaml 'Microsoft.UI.Xaml'
if (Test-Path -LiteralPath $xamlResources) {
    Copy-Item $xamlResources $Output -Recurse -Force
}
$packageRoot = Join-Path $PSScriptRoot '..\package'
$assets = Join-Path $Output 'Assets'
New-Item -ItemType Directory -Force -Path $assets | Out-Null
Copy-Item (Join-Path $packageRoot 'AppxManifest.xml') $Output -Force
Copy-Item (Join-Path $PSScriptRoot 'configure-agent-firewall.ps1') $Output -Force
Copy-Item (Join-Path $xaml 'logo.png') (Join-Path $assets 'Logo.png') -Force
$openssl = 'F:\deps\aicli\openssl'
Copy-Item (Join-Path $openssl 'libssl-4-x64.dll'), (Join-Path $openssl 'libcrypto-4-x64.dll') $Output -Force
$controlHashes = Get-FileHash (Join-Path $controlResourceFull '*.xbf') -Algorithm SHA256
Write-Host "Windows Terminal 1.24.11911.0 runtime staged to $Output"
$controlHashes | ForEach-Object { Write-Host "  $([IO.Path]::GetFileName($_.Path))  $($_.Hash)" }
