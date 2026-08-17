#ifndef MyAppVersion
  #define MyAppVersion "1.0.0.3"
#endif

#define MyAppName "AiCli"
#define MyAppPublisher "AI CLI"
#define MyAppExeName "aicli.exe"
#ifndef MyPackageIdentity
  #define MyPackageIdentity "AiCli.Desktop.Release"
#endif
#ifndef MySourceDir
  #define MySourceDir "..\bin\Release"
#endif
#ifndef MyInstallerBaseName
  #define MyInstallerBaseName "AI-CLI-Setup-" + MyAppVersion + "-x64"
#endif

[Setup]
AppId={{15A2F0B7-30F5-4CA4-92D7-C0B3DA678BA9}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
VersionInfoVersion={#MyAppVersion}
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription={#MyAppName} Windows Installer
DefaultDirName={autopf}\AiCli
DefaultGroupName={#MyAppName}
DisableDirPage=no
DisableProgramGroupPage=yes
OutputDir=output
OutputBaseFilename={#MyInstallerBaseName}
SetupIconFile=..\assets\aicli.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog commandline
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
CloseApplications=force
RestartApplications=no
MinVersion=10.0.19041
DisableWelcomePage=no
SetupLogging=yes

[Languages]
Name: "chinesesimp"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加快捷方式："; Flags: unchecked

[Files]
Source: "{#MySourceDir}\*"; DestDir: "{app}"; Excludes: "aicli.pdb,aicli_data_store_tests.exe,aicli_data_store_tests.pdb,aicli-test-window.png,component-load-error.txt,microsoft.system.package.metadata\*"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "install-package.ps1"; DestDir: "{app}\installer"; Flags: ignoreversion
Source: "uninstall-package.ps1"; DestDir: "{app}\installer"; Flags: ignoreversion
Source: "close-running-terminals.ps1"; Flags: dontcopy

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; IconFilename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; WorkingDir: "{app}"; IconFilename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
; Do not create AiCli directly from Setup's launch chain. NVM for Windows
; commonly exposes node.exe through a cross-drive symbolic link, and the
; first packaged-classic process created immediately after sparse package
; registration may not resolve that link even though its PATH is correct.
; Forward the launch request to the user's existing Explorer process so this
; is identical to opening the desktop/Start-menu shortcut.
Filename: "{win}\explorer.exe"; Parameters: """{app}\{#MyAppExeName}"""; Description: "启动 {#MyAppName}"; WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent runasoriginaluser

[UninstallRun]
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\installer\uninstall-package.ps1"" -IdentityName ""{#MyPackageIdentity}"""; Flags: runhidden waituntilterminated; RunOnceId: "UnregisterAiCliPackage"

[Code]
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  PowerShellPath: String;
  ScriptPath: String;
  Parameters: String;
  ResultCode: Integer;
begin
  Result := '';
  ExtractTemporaryFile('close-running-terminals.ps1');
  PowerShellPath := ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe');
  ScriptPath := ExpandConstant('{tmp}\close-running-terminals.ps1');
  Parameters := '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' +
    ScriptPath + '" -InstallDirectory "' + ExpandConstant('{app}') + '"';

  if (not Exec(PowerShellPath, Parameters, ExpandConstant('{tmp}'),
    SW_HIDE, ewWaitUntilTerminated, ResultCode)) or (ResultCode <> 0) then
  begin
    Result := '无法自动关闭 AiCli 内嵌终端，请稍后重试安装。';
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  PowerShellPath: String;
  ScriptPath: String;
  Parameters: String;
  ResultCode: Integer;
begin
  if CurStep = ssPostInstall then
  begin
    PowerShellPath := ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe');
    ScriptPath := ExpandConstant('{app}\installer\install-package.ps1');
    Parameters := '-NoProfile -ExecutionPolicy Bypass -File "' + ScriptPath +
      '" -InstallDirectory "' + ExpandConstant('{app}') + '"';

    if (not Exec(PowerShellPath, Parameters, ExpandConstant('{app}'),
      SW_HIDE, ewWaitUntilTerminated, ResultCode)) or (ResultCode <> 0) then
    begin
      RaiseException('AI CLI 运行环境注册失败，错误代码：' + IntToStr(ResultCode));
    end;
  end;
end;
