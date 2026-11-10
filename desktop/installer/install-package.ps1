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

if ($identityName -eq 'AiCli.Desktop.Release') {
    # Keep the old registration and its private AppData. The unpackaged helper
    # snapshots SQLite (including WAL), certs and uploads before registration.
    # The new app migrates the private overlay on first startup. Failure stops
    # registration, leaving the original data and package available.
    $dataUpgrade = Join-Path $installRoot 'aicli-data-upgrade.exe'
    & $dataUpgrade 'AiCli.Desktop.Release_jw3zr00rmb0ym'
    if ($LASTEXITCODE -ne 0) { throw 'User data backup/migration failed. Existing package registration was preserved.' }
} elseif ($identityName -like 'AiCli.Desktop.Qt*') {
    # Retain the existing Qt installer behavior; this release is Win32 only.
    Get-AppxPackage -Name $identityName -ErrorAction SilentlyContinue |
        ForEach-Object { Remove-AppxPackage -Package $_.PackageFullName -ErrorAction Stop }
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
