param(
    [string]$IdentityName = 'AiCli.Desktop.Release'
)

$ErrorActionPreference = 'Stop'

if ($PSVersionTable.PSEdition -eq 'Core') {
    $windowsPowerShell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    & $windowsPowerShell -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath `
        -IdentityName $IdentityName
    exit $LASTEXITCODE
}

Get-AppxPackage -Name $IdentityName -ErrorAction SilentlyContinue |
    ForEach-Object {
        Remove-AppxPackage -Package $_.PackageFullName -ErrorAction Stop
    }

if (Get-Command Get-NetFirewallRule -ErrorAction SilentlyContinue) {
    Get-NetFirewallRule -DisplayName 'AI CLI Remote Agent' -ErrorAction SilentlyContinue |
        Remove-NetFirewallRule -ErrorAction SilentlyContinue
}

Write-Host "Unregistered $IdentityName"
