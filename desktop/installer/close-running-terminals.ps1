param(
    [Parameter(Mandatory = $true)]
    [string]$InstallDirectory
)

$ErrorActionPreference = 'Stop'

$targetPath = [IO.Path]::GetFullPath(
    (Join-Path $InstallDirectory 'x64\OpenConsole.exe'))

function Get-MatchingOpenConsoleProcesses {
    @(
        Get-CimInstance Win32_Process -Filter "Name = 'OpenConsole.exe'" `
            -ErrorAction SilentlyContinue |
            Where-Object {
                if ([string]::IsNullOrWhiteSpace($_.ExecutablePath)) {
                    return $false
                }

                try {
                    $processPath = [IO.Path]::GetFullPath($_.ExecutablePath)
                    return [string]::Equals(
                        $processPath,
                        $targetPath,
                        [StringComparison]::OrdinalIgnoreCase)
                } catch {
                    return $false
                }
            }
    )
}

foreach ($process in (Get-MatchingOpenConsoleProcesses)) {
    Stop-Process -Id $process.ProcessId -Force -ErrorAction SilentlyContinue
}

$deadline = [DateTime]::UtcNow.AddSeconds(5)
do {
    $remaining = @(Get-MatchingOpenConsoleProcesses)
    if ($remaining.Count -eq 0) {
        exit 0
    }
    Start-Sleep -Milliseconds 100
} while ([DateTime]::UtcNow -lt $deadline)

exit 1
