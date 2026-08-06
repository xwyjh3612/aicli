param(
    [string]$Exe = 'F:\codex_tool-target\desktop\Debug\aicli-terminal-probe.exe'
)

Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class AicliPtyNative {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr child, string cls, string title);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    public static extern IntPtr SendMessage(IntPtr hwnd, uint message, IntPtr wParam, StringBuilder lParam);
    [DllImport("user32.dll")]
    public static extern IntPtr SendMessage(IntPtr hwnd, uint message, IntPtr wParam, IntPtr lParam);
}
'@

if (!(Test-Path -LiteralPath $Exe)) { throw "Probe executable not found: $Exe" }
$process = Start-Process -FilePath $Exe -ArgumentList @('chcp 65001') -PassThru
try {
    Start-Sleep -Seconds 1
    $edit = [AicliPtyNative]::FindWindowEx($process.MainWindowHandle, [IntPtr]::Zero, 'Edit', $null)
    if ($edit -eq [IntPtr]::Zero) { throw 'Terminal edit host was not created' }
    foreach ($character in 'echo PTY_SMOKE_OK'.ToCharArray()) {
        [AicliPtyNative]::SendMessage($edit, 0x0102, [IntPtr][int][char]$character, [IntPtr]::Zero) | Out-Null
    }
    [AicliPtyNative]::SendMessage($edit, 0x0102, [IntPtr]13, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Seconds 1
    $buffer = New-Object Text.StringBuilder 20000
    [AicliPtyNative]::SendMessage($edit, 0x000D, [IntPtr]$buffer.Capacity, $buffer) | Out-Null
    $output = $buffer.ToString()
    if (!$output.Contains('PTY_SMOKE_OK')) { throw 'PTY output did not contain the command result' }
    'PTY smoke test passed'
}
finally {
    if (!$process.HasExited) {
        [AicliPtyNative]::SendMessage($process.MainWindowHandle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
        $process.WaitForExit(5000)
    }
    if (!$process.HasExited) { Stop-Process -Id $process.Id -Force }
}
