# AI CLI desktop

The desktop application is the primary AI CLI workspace described in
[`../ai-cli-mobile-architecture.md`](../ai-cli-mobile-architecture.md).

It is a native C++ Windows application with:

- an embedded Windows Terminal `TermControl` hosted through a XAML Island;
- an in-process `ITerminalConnection` implementation backed by the matching
  Microsoft ConPTY 1.24 side-by-side runtime;
- PowerShell as the hidden terminal host for Codex, Claude, and custom CLIs;
- SQLite-backed projects, dynamic project types, groups, devices, settings, and events;
- an HTTPS/WSS Agent used by the Java Android companion app;
- QR pairing, certificate pinning, signed requests, replay protection, input leases,
  and immediate device revocation.

No browser, WebView, Electron, independent `wt.exe`, `TerminalConnection.dll`,
or `OpenConsoleProxy.dll` is used. The app intentionally ships the official
`conpty.dll` and architecture-specific `x64/OpenConsole.exe` from Microsoft
ConPTY `1.24.260710001`.

## Build

Stop any running `aicli.exe` before linking, then run:

```powershell
$cmake = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
& $cmake -S desktop -B desktop\build -G 'Visual Studio 17 2022' -A x64
& $cmake --build desktop\build --config Debug --target aicli -- /m:2
& .\desktop\tools\stage-runtime.ps1
```

The executable is:

```text
desktop\bin\Debug\aicli.exe
```

Large dependencies and generated Windows Terminal metadata remain on `F:`.
The runnable application stays inside this workspace.

## Data-store tests

```powershell
& $cmake --build desktop\build --config Debug --target aicli_data_store_tests -- /m:2
& .\desktop\bin\Debug\aicli_data_store_tests.exe F:\codex_tool-target\desktop\tests\aicli-data-store-test.db
```

The test covers built-in project-type/group protection, dynamic type and group
renames, project migration, deletion guards, device revocation, request replay
protection, and event persistence.

## Terminal runtime

`stage-runtime.ps1` stages the fixed Windows Terminal 1.24 control, settings
model, WinUI 2 runtime, PRI, XBF, WinMD, matching `conpty.dll`, and
`x64/OpenConsole.exe`. It also removes obsolete private connection binaries so
they cannot accidentally re-enter the startup path.

`src/terminal/conpty_connection_bridge.cpp` implements `ITerminalConnection`,
uses a full-duplex overlapped pipe with the side-by-side ConPTY/OpenConsole host,
and launches `powershell.exe -NoLogo -NoExit`. After the PTY connects, it sends
UTF-8 setup, changes to the configured project directory, and submits the
configured startup command through the same native terminal input channel.

Do not replace this runtime with the system `CreatePseudoConsole`/`conhost.exe`.
The system host handles ordinary PowerShell output but collapses Codex inline
history insertion into viewport repainting, leaving TermControl with no native
scrollback. The matching 1.24 OpenConsole runtime preserves Codex scrollback.

## Mobile Agent

The Agent listens on HTTPS/WSS, default port `4280`. Before LAN access, use the
desktop Settings dialog to configure the advertised HTTPS address and run the
firewall helper. Public access currently uses direct HTTPS, VPN, port mapping,
or a user-managed reverse tunnel; this repository does not deploy a hosted
Relay service.

The packaged desktop app currently runs at `mediumIL`. Codex `--yolo` and
Claude `--dangerously-skip-permissions` disable those CLIs' own confirmations,
but they do not create a Windows Administrator token. A future elevated broker
must remain separate from the public Agent endpoint.
