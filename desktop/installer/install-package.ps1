param(
    [Parameter(Mandatory = $true)]
    [string]$InstallDirectory
)

$ErrorActionPreference = 'Stop'

if ($PSVersionTable.PSEdition -eq 'Core') {
    $windowsPowerShell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    & $windowsPowerShell -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath `
        -InstallDirectory $InstallDirectory
    exit $LASTEXITCODE
}

$installRoot = [IO.Path]::GetFullPath($InstallDirectory).TrimEnd('\')
$manifestPath = Join-Path $installRoot 'AppxManifest.xml'
$executablePath = Join-Path $installRoot 'aicli.exe'
if (!(Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw "AppxManifest.xml was not found: $manifestPath"
}
if (!(Test-Path -LiteralPath $executablePath -PathType Leaf)) {
    throw "aicli.exe was not found: $executablePath"
}

[xml]$manifest = [IO.File]::ReadAllText($manifestPath, [Text.Encoding]::UTF8)
$identityName = [string]$manifest.Package.Identity.Name
$applicationId = [string]$manifest.Package.Applications.Application.Id
if (!$identityName -or !$applicationId) {
    throw 'The installed AppxManifest.xml is missing its package identity.'
}

# Re-registering from a different install directory can otherwise leave the
# package identity pointing at stale files. AI CLI stores its conversations in
# %LOCALAPPDATA%\AI CLI, so replacing this sparse registration does not remove
# user projects or chat history.
Get-AppxPackage -Name $identityName -ErrorAction SilentlyContinue |
    ForEach-Object {
        Remove-AppxPackage -Package $_.PackageFullName -ErrorAction Stop
    }

Add-AppxPackage -Register $manifestPath -ForceUpdateFromAnyVersion -ErrorAction Stop
$registered = Get-AppxPackage -Name $identityName -ErrorAction Stop |
    Sort-Object Version -Descending |
    Select-Object -First 1
if (!$registered) {
    throw "Package registration completed but $identityName was not found."
}

$registeredRoot = [IO.Path]::GetFullPath([string]$registered.InstallLocation).TrimEnd('\')
if (!$registeredRoot.Equals($installRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Package registration points to an unexpected directory: $registeredRoot"
}

Write-Host "Registered $identityName"
Write-Host "AUMID: $($registered.PackageFamilyName)!$applicationId"
