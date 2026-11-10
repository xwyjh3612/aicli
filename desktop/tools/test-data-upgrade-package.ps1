param([string]$ProbeExecutable = '')
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSEdition -eq 'Core') {
    & "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath -ProbeExecutable $ProbeExecutable
    exit $LASTEXITCODE
}
Import-Module "$env:SystemRoot\System32\WindowsPowerShell\v1.0\Modules\Appx\Appx.psd1"
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
if (!$ProbeExecutable) { $ProbeExecutable = Join-Path $repo 'desktop\bin\Debug\aicli_data_upgrade_tests.exe' }
$identity = 'AiCli.DataUpgrade.Probe1006'
$family = 'AiCli.DataUpgrade.Probe1006_jw3zr00rmb0ym'
$root = Join-Path $repo 'local-screenshots\package-upgrade-probe'
$data = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'AI CLI Upgrade Probe 1006'
if ((Get-AppxPackage -Name $identity) -or (Test-Path -LiteralPath $data)) { throw 'An earlier probe exists; inspect it before retrying.' }
New-Item -ItemType Directory -Path $root -Force | Out-Null
$old = Join-Path $root 'old'
$new = Join-Path $root 'new'
foreach ($directory in @($old, $new)) {
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    Copy-Item -LiteralPath $ProbeExecutable -Destination (Join-Path $directory 'aicli.exe') -Force
    Copy-Item -LiteralPath (Join-Path $repo 'desktop\bin\Debug\Assets') -Destination $directory -Recurse -Force
}
$template = [IO.File]::ReadAllText((Join-Path $repo 'desktop\package\AppxManifest.xml'))
$oldText = $template.Replace('AiCli.Desktop.Dev', $identity).Replace('1.0.0.6', '1.0.0.5')
$oldManifest = Join-Path $old 'AppxManifest.xml'
$newManifest = Join-Path $new 'AppxManifest.xml'
[IO.File]::WriteAllText($oldManifest, $oldText)
$newText = $template.Replace('AiCli.Desktop.Dev', $identity).
    Replace('IgnorableNamespaces="', 'xmlns:desktop6="http://schemas.microsoft.com/appx/manifest/desktop/windows10/6" IgnorableNamespaces="desktop6 ').
    Replace('</Properties>', '<desktop6:FileSystemWriteVirtualization>disabled</desktop6:FileSystemWriteVirtualization></Properties>').
    Replace('</Capabilities>', '<rescap:Capability Name="unvirtualizedResources" /></Capabilities>')
[IO.File]::WriteAllText($newManifest, $newText)
$report = Join-Path $root 'paths.txt'
$childReport = Join-Path $root 'child.txt'
$migrationReport = Join-Path $root 'migration.txt'
foreach ($file in @($report, $childReport, $migrationReport)) {
    if (Test-Path -LiteralPath $file) { Remove-Item -LiteralPath $file }
}
try {
    Add-AppxPackage -Register $oldManifest -ForceUpdateFromAnyVersion
    & $ProbeExecutable --activate "$family!App" "--probe-write `"$report`""
    if ($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath $report)) { throw 'Old packaged writer failed.' }
    $paths = [IO.File]::ReadAllLines($report)
    if (Test-Path -LiteralPath $paths[0]) { throw 'Expected the logical download path to be invisible outside the old package.' }
    if (!(Test-Path -LiteralPath $paths[1])) { throw 'Physical download path is not accessible.' }
    Write-Host 'Reproduced: logical AppData download is invisible to an unpackaged installer.'
    $child = Start-Process -FilePath $paths[1] -ArgumentList @('--probe-child', "`"$childReport`"") -WindowStyle Hidden -Wait -PassThru
    if ($child.ExitCode -ne 0 -or !(Test-Path -LiteralPath $childReport)) { throw 'Physical installer launch failed.' }
    Write-Host 'Verified: physical download path launches successfully.'
    # Also move the package directory: in-place registration must update the
    # path and preserve the old package data, without Remove-AppxPackage.
    Add-AppxPackage -Register $newManifest -ForceUpdateFromAnyVersion
    if ((Get-AppxPackage -Name $identity).InstallLocation -ne $new) { throw 'Package still points at old install directory.' }
    & $ProbeExecutable --activate "$family!App" "--probe-migrate `"$migrationReport`""
    if ($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath $migrationReport)) { throw 'New packaged data migration failed.' }
    if (!(Test-Path -LiteralPath (Join-Path $data 'aicli.db'))) { throw 'Migrated data is still virtualized.' }
    Write-Host 'Verified: changed install path, retained package data, migrated database and certificate to real AppData.'
} finally {
    Get-AppxPackage -Name $identity | ForEach-Object { Remove-AppxPackage -Package $_.PackageFullName }
    # Only the explicitly named probe data directory may be recursively removed.
    $expected = [IO.Path]::GetFullPath((Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'AI CLI Upgrade Probe 1006'))
    if ((Test-Path -LiteralPath $data) -and (Resolve-Path -LiteralPath $data).Path -eq $expected) {
        Remove-Item -LiteralPath $expected -Recurse -Force
    }
}
