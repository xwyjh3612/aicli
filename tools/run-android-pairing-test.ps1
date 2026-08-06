param(
    [string]$DeviceId = '607faa8d',
    [string]$QrScreenshot = 'F:\codex_tool-target\desktop\pairing-current.png',
    [string]$Workspace = 'C:\project\aitest\codex_tool',
    [string]$LocalAddressOverride = ''
)

$ErrorActionPreference = 'Stop'
$adb = 'F:\android\sdk\platform-tools\adb.exe'
$mainApk = Join-Path $Workspace 'android-app\app\build\outputs\apk\debug\app-debug.apk'
$testApk = Join-Path $Workspace 'android-app\app\build\outputs\apk\androidTest\debug\app-debug-androidTest.apk'
$zxing = Get-ChildItem 'F:\android\.gradle\caches\modules-2\files-2.1\com.google.zxing\core\3.5.3' `
    -Recurse -Filter '*.jar' | Select-Object -First 1 -ExpandProperty FullName
if (!(Test-Path -LiteralPath $adb) -or !(Test-Path -LiteralPath $mainApk) -or
    !(Test-Path -LiteralPath $testApk) -or !(Test-Path -LiteralPath $QrScreenshot) -or !$zxing) {
    throw 'Android pairing test dependencies are incomplete.'
}

$qr = & java -cp ((Join-Path $Workspace 'tools') + ';' + $zxing) QrDecode $QrScreenshot
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

$outputRoot = 'F:\codex_tool-target\desktop'
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
    -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
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
if ($test.ExitCode -ne 0 -or $resultText -notmatch 'OK \(1 test\)' -or
    $resultText -match 'INSTRUMENTATION_STATUS_CODE:\s*-2|FAILURES!!!|Process crashed') {
    throw "Android pairing instrumentation failed: $($test.ExitCode)"
}
