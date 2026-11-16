param([string]$ProbeExecutable = '')
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSEdition -eq 'Core') {
    & "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath -ProbeExecutable $ProbeExecutable
    exit $LASTEXITCODE
}

$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
. (Join-Path $repo 'tools\dependency-paths.ps1')
$dependencies = Get-AicliDependencyPaths
if (!$ProbeExecutable) { $ProbeExecutable = Join-Path $repo 'desktop\bin\Debug\aicli_installer_update_tests.exe' }
if (!(Test-Path -LiteralPath $ProbeExecutable)) { throw 'Build aicli_installer_update_tests first.' }
$nonce = [guid]::NewGuid().ToString('N')
$identity = 'AiCli.UpdateProbe' + $nonce
$root = [IO.Path]::GetFullPath((Join-Path $repo ('local-screenshots\installer-update-order-' + $nonce)))
$workspace = [IO.Path]::GetFullPath((Join-Path $repo 'local-screenshots')).TrimEnd('\') + '\'
if (!$root.StartsWith($workspace, [StringComparison]::OrdinalIgnoreCase)) { throw 'Probe root is outside the workspace.' }
$source = Join-Path $root 'source'
$installed = Join-Path $root 'installed'
$utf8 = [Text.UTF8Encoding]::new($false)
foreach ($directory in @($root, $source, $installed)) { New-Item -ItemType Directory -Path $directory -Force | Out-Null }
foreach ($directory in @($source, $installed)) {
    Copy-Item -LiteralPath $ProbeExecutable -Destination (Join-Path $directory 'aicli.exe')
    Copy-Item -LiteralPath (Join-Path $repo 'desktop\bin\Release\Assets') -Destination $directory -Recurse
}
$template = [IO.File]::ReadAllText((Join-Path $repo 'desktop\package\AppxManifest.xml'))
$template = $template.Replace('AiCli.Desktop.Dev', $identity)
$template = [regex]::Replace($template, '(?<=\s)Version="\d+\.\d+\.\d+\.\d+"', 'Version="9.0.0.1"')
$oldManifest = Join-Path $installed 'AppxManifest.xml'
$newManifest = Join-Path $source 'AppxManifest.xml'
[IO.File]::WriteAllText($oldManifest, $template, $utf8)
[IO.File]::WriteAllText($newManifest, $template.Replace('Version="9.0.0.1"', 'Version="9.0.0.2"'), $utf8)
$sentinel = Join-Path $installed 'retained-data.txt'
[IO.File]::WriteAllText($sentinel, 'user data must survive upgrade', $utf8)
$report = Join-Path $installed 'started-version.txt'
$stop = Join-Path $installed 'stop-probe.txt'

# Compile the actual installer lifecycle with only fixture identity/paths changed.
# Its registration helper performs a real Appx registration on the probe alone.
$script = [IO.File]::ReadAllText((Join-Path $repo 'desktop\installer\aicli.iss'))
$script = $script.Replace('AiCli.Desktop.Release', $identity)
$script = $script.Replace('AppId={{15A2F0B7-30F5-4CA4-92D7-C0B3DA678BA9}', 'AppId={{' + [guid]::NewGuid().ToString().ToUpperInvariant() + '}')
$script = $script.Replace('#define MyAppName "AiCli"', '#define MyAppName "AiCli update order probe"')
$script = $script.Replace('PrivilegesRequired=admin', 'PrivilegesRequired=lowest')
$script = $script.Replace('SetupIconFile=..\assets\aicli.ico', 'SetupIconFile=' + (Join-Path $repo 'desktop\assets\aicli.ico'))
[IO.File]::WriteAllText((Join-Path $root 'fixture.iss'), $script, $utf8)
Copy-Item -LiteralPath (Join-Path $repo 'desktop\installer\close-running-terminals.ps1') -Destination $root
Copy-Item -LiteralPath (Join-Path $repo 'desktop\installer\uninstall-package.ps1') -Destination $root
$registration = @'
param([string]$InstallDirectory)
$ErrorActionPreference = 'Stop'
Add-AppxPackage -Register (Join-Path $InstallDirectory 'AppxManifest.xml') -ForceUpdateFromAnyVersion -ErrorAction Stop
'@
[IO.File]::WriteAllText((Join-Path $root 'install-package.ps1'), $registration, $utf8)

try {
    Add-AppxPackage -Register $oldManifest -ForceUpdateFromAnyVersion
    $package = Get-AppxPackage -Name $identity
    & $ProbeExecutable --activate "$($package.PackageFamilyName)!App" | Out-Null
    if ($LASTEXITCODE) { throw 'Could not start the old probe package.' }
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    while (!(Test-Path -LiteralPath $report) -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 100 }
    if (!(Test-Path -LiteralPath $report) -or !(Get-Content -LiteralPath $report | Select-String '^9\.0\.0\.1 ')) {
        throw 'Old probe package did not become active.'
    }
    & (Join-Path $dependencies.innoSetup 'ISCC.exe') ('/O' + $root) '/DMyAppVersion=9.0.0.2' ('/DMyPackageIdentity=' + $identity) ('/DMySourceDir=' + $source) '/DMyInstallerBaseName=update-order-probe' (Join-Path $root 'fixture.iss') | Out-Null
    if ($LASTEXITCODE) { throw 'Could not compile the installer fixture.' }
    $log = Join-Path $root 'setup.log'
    $arguments = '/SILENT /NOCANCEL /NORESTART /SP- /SUPPRESSMSGBOXES /AICLIUPDATE /DIR="' + $installed + '" /LOG="' + $log + '"'
    $setup = Start-Process -FilePath (Join-Path $root 'update-order-probe.exe') -ArgumentList $arguments -WindowStyle Hidden -PassThru
    if (!$setup.WaitForExit(60000)) { throw 'Installer fixture did not exit in time.' }
    if ($setup.ExitCode -ne 0) { throw "Installer fixture failed: $($setup.ExitCode). See $log" }
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    while ([DateTime]::UtcNow -lt $deadline -and !(Get-Content -LiteralPath $report | Select-String '^9\.0\.0\.2 ')) { Start-Sleep -Milliseconds 100 }
    $starts = @(Get-Content -LiteralPath $report)
    if ($starts.Count -ne 2 -or $starts[1] -notmatch '^9\.0\.0\.2 ') { throw 'Expected one automatic restart using the registered new version.' }
    if ((Get-AppxPackage -Name $identity).Version -ne [version]'9.0.0.2') { throw 'Windows did not register the new package version.' }
    if ([IO.File]::ReadAllText($sentinel) -ne 'user data must survive upgrade') { throw 'Probe data changed during upgrade.' }
    $content = [IO.File]::ReadAllText($log)
    if ($content -notmatch 'Package registration succeeded; reopening AiCli after setup completed' -or $content -match 'raised an exception') { throw 'Unexpected installer lifecycle.' }
    Write-Host 'Verified real silent upgrade: close old package, register new version, then reopen once; probe data preserved.'

    # A failed registration must not trigger the ssDone automatic restart.
    [IO.File]::WriteAllText((Join-Path $root 'install-package.ps1'), 'exit 1', $utf8)
    & (Join-Path $dependencies.innoSetup 'ISCC.exe') ('/O' + $root) '/DMyAppVersion=9.0.0.2' ('/DMyPackageIdentity=' + $identity) ('/DMySourceDir=' + $source) '/DMyInstallerBaseName=update-order-failure-probe' (Join-Path $root 'fixture.iss') | Out-Null
    if ($LASTEXITCODE) { throw 'Could not compile the failure fixture.' }
    $failedLog = Join-Path $root 'failed-setup.log'
    $arguments = '/VERYSILENT /NOCANCEL /NORESTART /SP- /SUPPRESSMSGBOXES /AICLIUPDATE /DIR="' + $installed + '" /LOG="' + $failedLog + '"'
    $failedSetup = Start-Process -FilePath (Join-Path $root 'update-order-failure-probe.exe') -ArgumentList $arguments -WindowStyle Hidden -PassThru
    if (!$failedSetup.WaitForExit(60000)) { throw 'Failure fixture did not exit in time.' }
    $content = [IO.File]::ReadAllText($failedLog)
    if ($content -notmatch 'raised an exception' -or $content -match 'reopening AiCli after setup completed') { throw 'Failed registration did not prevent automatic restart.' }
    if (@(Get-Content -LiteralPath $report).Count -ne 2 -or (Get-AppxPackage -Name $identity).Version -ne [version]'9.0.0.2') { throw 'Failure changed package registration or reopened the probe.' }
    if ([IO.File]::ReadAllText($sentinel) -ne 'user data must survive upgrade') { throw 'Failure changed probe data.' }
    Write-Host 'Verified registration failure: no automatic restart, existing registration and probe data preserved.'
    Write-Host "Evidence: $root"
} finally {
    [IO.File]::WriteAllText($stop, 'stop test process', $utf8)
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    do {
        $live = @(Get-CimInstance Win32_Process -Filter "Name = 'aicli.exe'" | Where-Object { $_.ExecutablePath -eq (Join-Path $installed 'aicli.exe') })
        if (!$live.Count) { break }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    if (!$live.Count) {
        # Keep install logs for inspection, and unregister only this random probe.
        Get-AppxPackage -Name $identity | ForEach-Object { Remove-AppxPackage -Package $_.PackageFullName }
        $uninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall'
        Get-ChildItem -LiteralPath $uninstallKey -ErrorAction SilentlyContinue | Where-Object {
            ([string](Get-ItemProperty -LiteralPath $_.PSPath).InstallLocation).TrimEnd('\') -eq $installed
        } | ForEach-Object { Remove-Item -LiteralPath $_.PSPath }
        $shortcut = Join-Path ([Environment]::GetFolderPath('Programs')) 'AiCli update order probe.lnk'
        if (Test-Path -LiteralPath $shortcut) { Remove-Item -LiteralPath $shortcut }
    }
}
