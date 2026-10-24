param(
    [string]$DeviceId = '607faa8d',
    [string]$QrScreenshot = '',
    [string]$Workspace = '',
    [string]$LocalAddressOverride = ''
)

$ErrorActionPreference = 'Stop'
if (!$Workspace) { $Workspace = Join-Path $PSScriptRoot '..' }
$Workspace = [IO.Path]::GetFullPath($Workspace)
. (Join-Path $Workspace 'tools\dependency-paths.ps1')
$dependencies = Set-AicliAndroidBuildEnvironment
$adb = Join-Path $dependencies.androidSdk 'platform-tools\adb.exe'
$java = Join-Path $dependencies.java17 'bin\java.exe'
$outputRoot = Join-Path (Get-AicliBuildCacheRoot) 'aicli\pairing-test'
if (!$QrScreenshot) { $QrScreenshot = Join-Path $Workspace 'local-screenshots\pairing-current.png' }
$mainApk = Join-Path $Workspace 'android-app\app\build\outputs\apk\debug\app-debug.apk'
$testApk = Join-Path $Workspace 'android-app\app\build\outputs\apk\androidTest\debug\app-debug-androidTest.apk'
$zxing = Get-ChildItem (Join-Path $dependencies.gradleArtifacts 'com.google.zxing\core\3.5.3') `
    -Recurse -Filter '*.jar' | Select-Object -First 1 -ExpandProperty FullName
if (!(Test-Path -LiteralPath $adb) -or !(Test-Path -LiteralPath $mainApk) -or
    !(Test-Path -LiteralPath $testApk) -or !(Test-Path -LiteralPath $QrScreenshot) -or !$zxing) {
    throw 'Android pairing test dependencies are incomplete.'
}

$qr = & $java -cp $zxing (Join-Path $Workspace 'tools\QrDecode.java') $QrScreenshot
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($qr)) {
    throw 'The desktop pairing QR could not be decoded.'
}
if ($LocalAddressOverride) {
    $qrObject = $qr | ConvertFrom-Json
    $qrObject.localAddress = $LocalAddressOverride
    $qr = $qrObject | ConvertTo-Json -Compress
}
$payload = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($qr))

& $adb -s $DeviceId install -r $mainApk | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'Failed to install the Android app.' }
& $adb -s $DeviceId install -r $testApk | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'Failed to install the Android test APK.' }

if (!('AicliPairingTestNative' -as [type])) {
    Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class AicliPairingTestNative {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string className, string title);
    [DllImport("user32.dll")]
    public static extern bool PostMessage(IntPtr window, uint message, IntPtr wparam, IntPtr lparam);
}
'@
}

New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null
$stdout = Join-Path $outputRoot 'android-pair-test.out'
$stderr = Join-Path $outputRoot 'android-pair-test.err'
foreach ($path in @($stdout, $stderr)) {
    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force }
}

$arguments = @(
    '-s', $DeviceId, 'shell', 'am', 'instrument', '-w', '-r',
    '-e', 'qrPayloadBase64', $payload,
    '-e', 'class', 'com.aitest.aicli.PairingInstrumentedTest#pairAuthenticateAndListConversations',
    'com.aitest.aicli.test/androidx.test.runner.AndroidJUnitRunner'
)
$test = Start-Process -FilePath $adb -ArgumentList $arguments `
    -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
$deadline = (Get-Date).AddSeconds(190)
while (!$test.HasExited -and (Get-Date) -lt $deadline) {
    $desktop = Get-Process aicli -ErrorAction SilentlyContinue |
        Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if ($desktop) {
        $dialog = [AicliPairingTestNative]::FindWindowEx(
            $desktop.MainWindowHandle, [IntPtr]::Zero, 'AicliBindingDialog', $null)
        if ($dialog -ne [IntPtr]::Zero) {
            # ID_BIND_APPROVE / BN_CLICKED
            [AicliPairingTestNative]::PostMessage(
                $dialog, 0x0111, [IntPtr]2103, [IntPtr]::Zero) | Out-Null
        }
    }
    Start-Sleep -Milliseconds 600
    $test.Refresh()
}
if (!$test.HasExited) {
    Stop-Process -Id $test.Id -Force
    throw 'Android pairing instrumentation timed out.'
}

$resultText = ''
$errorText = ''
if (Test-Path -LiteralPath $stdout) { $resultText = Get-Content -LiteralPath $stdout -Raw }
if (Test-Path -LiteralPath $stderr) { $errorText = Get-Content -LiteralPath $stderr -Raw }
if ($resultText) { $resultText | Out-Host }
if ($errorText) { $errorText | Out-Host }
# `adb shell am instrument -w` can report -1 through Start-Process even after
# the runner has emitted its normal successful result.  The streamed runner
# output is the authoritative status for this test; accept it when exactly one
# test completed and no crash/failure marker was produced.
$instrumentationOk = $resultText -match 'OK \(1 test\)' -and
    $resultText -notmatch 'INSTRUMENTATION_STATUS_CODE:\s*-2|FAILURES!!!|Process crashed'
if (!$instrumentationOk) {
    throw "Android pairing instrumentation failed: $($test.ExitCode)"
}
