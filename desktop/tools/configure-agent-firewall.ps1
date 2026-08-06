param(
    [Parameter(Mandatory = $true)]
    [ValidateRange(1, 65535)]
    [int]$Port,

    [Parameter(Mandatory = $true)]
    [string]$Program
)

$ErrorActionPreference = 'Stop'
$ruleName = 'AI CLI Remote Agent'
$resolvedProgram = [IO.Path]::GetFullPath($Program)
if (!(Test-Path -LiteralPath $resolvedProgram -PathType Leaf)) {
    throw "AI CLI executable not found: $resolvedProgram"
}

Get-NetFirewallRule -DisplayName $ruleName -ErrorAction SilentlyContinue |
    Remove-NetFirewallRule -ErrorAction Stop
New-NetFirewallRule -DisplayName $ruleName -Description 'HTTPS/WSS access for paired AI CLI phones' `
    -Direction Inbound -Action Allow -Protocol TCP -LocalPort $Port `
    -Program $resolvedProgram -Profile Any -Enabled True | Out-Null

