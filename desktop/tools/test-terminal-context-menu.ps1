param(
    [string]$Output = (Join-Path $PSScriptRoot '..\build\terminal-menu-tests\Debug'),
    [switch]$Interactive
)
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSEdition -eq 'Core') {
    $parameters = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath, '-Output', $Output)
    if ($Interactive) { $parameters += '-Interactive' }
    & "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" @parameters
    exit $LASTEXITCODE
}
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..')).TrimEnd('\') + '\'
$outputPath = [IO.Path]::GetFullPath($Output)
if (!$outputPath.StartsWith($repo, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The isolated test output must be inside this repository.'
}
$manifest = Join-Path $outputPath 'AppxManifest.xml'
[xml]$xml = [IO.File]::ReadAllText($manifest)
$identity = [string]$xml.Package.Identity.Name
if ($identity -ne 'AiCli.Desktop.MenuProbe') { throw 'Refusing to register a production package.' }
$existing = Get-AppxPackage -Name $identity
if ($existing -and [IO.Path]::GetFullPath($existing.InstallLocation) -ne $outputPath) {
    throw 'A MenuProbe package exists at another path; inspect it first.'
}
$registered = $false
try {
    if (!$existing) {
        Add-AppxPackage -Register $manifest -ForceUpdateFromAnyVersion
        $registered = $true
    }
    $parameters = @()
    if ($Interactive) { $parameters += '--interactive' }
    & (Join-Path $outputPath 'aicli.exe') @parameters
    if ($LASTEXITCODE) { throw "Terminal context menu test failed: $LASTEXITCODE" }
} finally {
    if ($registered) {
        Get-AppxPackage -Name $identity | ForEach-Object { Remove-AppxPackage -Package $_.PackageFullName }
    }
}
