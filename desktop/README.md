# Desktop application

This directory contains the C++ desktop application described in
`../ai-cli-mobile-architecture.md`. The current executable is a native Win32
shell with a terminal-style layout, project list, Codex/Claude switching,
group, binding and settings entry points, plus a real ConPTY session.

The `XamlIslandAdapter` in `src/app/main.cpp` initializes
`WindowsXamlManager` and attaches a `DesktopWindowXamlSource`. It is the
compatibility boundary reserved for the Windows Terminal WinUI 2
`TerminalControl` DLL. The current shell uses the verified ConPTY renderer
while the TerminalControl ABI bridge is kept isolated from the main binary;
this avoids linking TerminalCore's `/MTd` static graph into the `/MDd` app.
No browser, WebView, Electron, or independent `wt.exe` window is used.

## Build

Run from a VS 2022 x64 developer command prompt. The project and runnable
output stay in this workspace; dependencies and large caches stay on F:.
The executable is `desktop\bin\Debug\aicli.exe`.

```powershell
$cmake = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
& $cmake -S desktop -B desktop\build -G 'Visual Studio 17 2022' -A x64
& $cmake --build desktop\build --config Debug --target aicli
& .\desktop\tools\stage-runtime.ps1
```

Run the desktop application:

```powershell
desktop\bin\Debug\aicli.exe
```

Double-click a project to start `codex` or `claude` in a real ConPTY. Enter in
the command line sends input directly; the app restores a native window without
opening a browser or a separate Windows Terminal process.

## Build the fixed TerminalControl source

The fixed Windows Terminal source is kept outside the workspace on F:. Its
public `fmt` and GSL submodules are restored there as well. The wrapper script
generates feature flags, builds the proxy, TerminalCore dependencies and
`Microsoft.Terminal.Control.dll`, then copies the DLL, PRI and WinMD to the
desktop output directory:

```powershell
.\desktop\tools\build-terminal-control.ps1
```

The output also includes `TerminalConnection.dll`, its WinMD, and
`OpenConsoleProxy.dll`; these are required runtime companions for the control.

The current source uses Windows.UI.Xaml (WinUI 2) internally. The next desktop
step is a XAML Island adapter between this control and the WinUI 3 shell.

The current visual pass uses an owner-drawn modern dark theme: a custom
borderless title bar, status chips, rounded buttons, project cards, search
field, terminal header and consistent spacing. This pass intentionally leaves
behavior unchanged.
