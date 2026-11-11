param(
    [string]$Output = (Join-Path $PSScriptRoot '..\bin\Debug'),
    [switch]$Launch
)
$ErrorActionPreference = 'Stop'

if ($PSVersionTable.PSEdition -eq 'Core') {
    $windowsPowerShell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $arguments = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath,
        '-Output', $Output
    )
    if ($Launch) { $arguments += '-Launch' }
    & $windowsPowerShell @arguments
    exit $LASTEXITCODE
}

$outputFull = [IO.Path]::GetFullPath($Output)
$manifest = Join-Path $outputFull 'AppxManifest.xml'
$executable = Join-Path $outputFull 'aicli.exe'
if (!(Test-Path -LiteralPath $manifest)) {
    throw "AppxManifest.xml was not found: $manifest"
}
if (!(Test-Path -LiteralPath $executable)) {
    throw "aicli.exe was not found: $executable"
}

[xml]$manifestXml = [IO.File]::ReadAllText($manifest)
$identityName = [string]$manifestXml.Package.Identity.Name
$applicationId = [string]$manifestXml.Package.Applications.Application.Id
if (!$identityName -or !$applicationId) {
    throw 'The staged package manifest is missing Identity Name or Application Id.'
}

Add-AppxPackage -Register $manifest -ForceUpdateFromAnyVersion
$package = Get-AppxPackage -Name $identityName | Sort-Object Version -Descending | Select-Object -First 1
if (!$package) {
    throw "Package registration completed but $identityName was not found."
}
$aumid = "$($package.PackageFamilyName)!$applicationId"
Write-Host "Registered $identityName"
Write-Host "AUMID: $aumid"
Write-Host "Executable: $executable"

if ($Launch) {
    Start-Process -FilePath $executable
}
