param(
    [string]$BuildDirectory = '',
    [string]$InnoCompiler = '',
    [switch]$SkipBuild,
    [switch]$Qt
)

$ErrorActionPreference = 'Stop'

$desktopRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $desktopRoot '..'))
$useQtUi = $Qt.IsPresent
$releaseDirectory = Join-Path $desktopRoot $(if ($useQtUi) { 'bin\Qt\Release' } else { 'bin\Release' })
$runtimeOutputRoot = Join-Path $desktopRoot $(if ($useQtUi) { 'bin\Qt' } else { 'bin' })
$packageIdentity = if ($useQtUi) { 'AiCli.Desktop.QtRelease' } else { 'AiCli.Desktop.Release' }
$applicationUserModelId = if ($useQtUi) {
    'AiCli.Desktop.QtRelease_jw3zr00rmb0ym!App'
} else {
    'AiCli.Desktop.Release_jw3zr00rmb0ym!App'
}
$installerPrefix = if ($useQtUi) { 'AI-CLI-Qt-Setup' } else { 'AI-CLI-Setup' }
$installerScript = Join-Path $desktopRoot 'installer\aicli.iss'
$installerOutput = Join-Path $desktopRoot 'installer\output'
$stageRuntime = Join-Path $desktopRoot 'tools\stage-runtime.ps1'

if (!$BuildDirectory) {
    if (Test-Path -LiteralPath 'F:\') {
        $BuildDirectory = if ($useQtUi) {
            'F:\codex_tool-target\desktop\build-qt-release'
        } else {
            'F:\codex_tool-target\desktop\build-nonqt-release'
        }
    } else {
        $BuildDirectory = Join-Path $env:LOCALAPPDATA $(if ($useQtUi) {
            'AI CLI\build-qt-release'
        } else {
            'AI CLI\build-nonqt-release'
        })
    }
}
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)

$cmakeCandidates = @(
    'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe',
    'C:\Program Files\CMake\bin\cmake.exe'
)
$cmake = $cmakeCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (!$cmake) { throw 'cmake.exe was not found.' }

if (!$InnoCompiler) {
    $innoCandidates = @(
        'F:\tools\InnoSetup7\ISCC.exe',
        'C:\Program Files\Inno Setup 7\ISCC.exe',
        'C:\Program Files (x86)\Inno Setup 6\ISCC.exe'
    )
    $InnoCompiler = $innoCandidates |
        Where-Object { Test-Path -LiteralPath $_ } |
        Select-Object -First 1
}
if (!(Test-Path -LiteralPath $InnoCompiler -PathType Leaf)) {
    throw "Inno Setup compiler was not found: $InnoCompiler"
}

if (!$SkipBuild) {
    $qtOption = if ($useQtUi) { 'ON' } else { 'OFF' }
    & $cmake -S $desktopRoot -B $BuildDirectory -G 'Visual Studio 17 2022' -A x64 `
        "-DAICLI_USE_QT_UI=$qtOption" `
        "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$($runtimeOutputRoot.Replace('\', '/'))" `
        "-DAICLI_PACKAGE_IDENTITY_NAME=$packageIdentity" `
        "-DAICLI_APPLICATION_USER_MODEL_ID=$applicationUserModelId"
    if ($LASTEXITCODE) { throw "CMake configuration failed: $LASTEXITCODE" }

    & $cmake --build $BuildDirectory --config Release --target aicli --parallel 6
    if ($LASTEXITCODE) { throw "Release build failed: $LASTEXITCODE" }
}

# POST_BUILD commands do not run when MSBuild considers aicli.exe up to date.
# Stage explicitly so every installer contains the current Terminal/WinUI PRI
# files and the Release package identity even on an incremental build.
$windowsPowerShell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
& $windowsPowerShell -NoProfile -ExecutionPolicy Bypass -File $stageRuntime `
    -Output $releaseDirectory `
    -Configuration Release `
    -Platform x64 `
    -PackageIdentityName $packageIdentity `
    -ApplicationLogo (Join-Path $desktopRoot 'assets\aicli-logo.png')
if ($LASTEXITCODE) { throw "Release runtime staging failed: $LASTEXITCODE" }

$executable = Join-Path $releaseDirectory 'aicli.exe'
$manifestPath = Join-Path $releaseDirectory 'AppxManifest.xml'
$mainPri = Join-Path $releaseDirectory 'resources.pri'
$controlPri = Join-Path $releaseDirectory 'Microsoft.Terminal.Control.pri'
foreach ($required in @($executable, $manifestPath, $mainPri, $controlPri)) {
    if (!(Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required Release file is missing: $required"
    }
}

[xml]$manifest = [IO.File]::ReadAllText($manifestPath, [Text.Encoding]::UTF8)
$identityName = [string]$manifest.Package.Identity.Name
$version = [string]$manifest.Package.Identity.Version
if ($identityName -ne $packageIdentity) {
    throw "Release manifest has the wrong identity: $identityName"
}
if (!$version) { throw 'Release manifest has no version.' }
if ((Get-Item -LiteralPath $mainPri).Length -lt 10MB) {
    throw 'Release resources.pri is incomplete.'
}
if ((Get-Item -LiteralPath $controlPri).Length -lt 10KB) {
    throw 'Release Microsoft.Terminal.Control.pri is incomplete.'
}

New-Item -ItemType Directory -Force -Path $installerOutput | Out-Null
$installerBaseName = "$installerPrefix-$version-x64"
& $InnoCompiler `
    "/DMyAppVersion=$version" `
    "/DMyPackageIdentity=$packageIdentity" `
    "/DMySourceDir=$releaseDirectory" `
    "/DMyInstallerBaseName=$installerBaseName" `
    $installerScript
if ($LASTEXITCODE) { throw "Inno Setup compilation failed: $LASTEXITCODE" }

$installerPath = Join-Path $installerOutput "$installerBaseName.exe"
if (!(Test-Path -LiteralPath $installerPath -PathType Leaf)) {
    throw "Installer was not generated: $installerPath"
}

$hash = (Get-FileHash -LiteralPath $installerPath -Algorithm SHA256).Hash
$hashPath = "$installerPath.sha256"
[IO.File]::WriteAllText(
    $hashPath,
    "$hash  $([IO.Path]::GetFileName($installerPath))`r`n",
    [Text.UTF8Encoding]::new($false))

Write-Host "Installer: $installerPath"
Write-Host "SHA256:   $hash"
