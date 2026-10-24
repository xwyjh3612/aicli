param(
    [string]$BuildDirectory = '',
    [string]$InnoCompiler = '',
    [switch]$SkipBuild,
    [switch]$Qt
)

$ErrorActionPreference = 'Stop'

$desktopRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $desktopRoot '..'))
. (Join-Path $repositoryRoot 'tools\dependency-paths.ps1')
$dependencies = Get-AicliDependencyPaths
$useQtUi = $Qt.IsPresent
$releaseDirectory = Join-Path $desktopRoot $(if ($useQtUi) { 'bin\Qt\Release' } else { 'bin\Release' })
$packageIdentity = if ($useQtUi) { 'AiCli.Desktop.QtRelease' } else { 'AiCli.Desktop.Release' }
$applicationUserModelId = if ($useQtUi) {
    'AiCli.Desktop.QtRelease_jw3zr00rmb0ym!App'
} else {
    'AiCli.Desktop.Release_jw3zr00rmb0ym!App'
}
$installerPrefix = if ($useQtUi) { 'AI-CLI-Qt-Setup' } else { 'AI-CLI-Setup' }
$installerScript = Join-Path $desktopRoot 'installer\aicli.iss'
$installerOutput = Join-Path $desktopRoot 'installer\output'
$publishDirectory = Join-Path $repositoryRoot 'release'
$stageRuntime = Join-Path $desktopRoot 'tools\stage-runtime.ps1'

if (!$BuildDirectory) {
    $BuildDirectory = Join-Path $desktopRoot $(if ($useQtUi) {
        'build-qt-release'
    } else {
        'build-nonqt-release'
    })
}
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
if ([IO.Path]::GetPathRoot($BuildDirectory) -eq 'F:\') {
    throw 'F: is reserved for shared dependencies. Choose a build directory on C:.'
}
$isolatedRuntimeRoot = Join-Path $BuildDirectory 'runtime'

$cmakeCandidates = @(
    'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe',
    'C:\Program Files\CMake\bin\cmake.exe'
)
$cmake = $cmakeCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (!$cmake) { throw 'cmake.exe was not found.' }

if (!$InnoCompiler) {
    $innoCandidates = @(
        (Join-Path $dependencies.innoSetup 'ISCC.exe'),
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
        "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$($isolatedRuntimeRoot.Replace('\', '/'))" `
        "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE=$($releaseDirectory.Replace('\', '/'))" `
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

# A multi-configuration CMake tree can also build Debug.  Historically the
# installer tree shared desktop\bin\Qt with the development tree, so an
# accidental Debug build silently replaced QtDev's executable with a binary
# compiled for QtRelease.  Direct launch then activated a different package
# and WindowsXamlManager intermittently ran without the intended package
# manifest.  Keep non-Release outputs isolated above and reject any stale or
# cross-flavour executable before producing an installer.
$executableUnicode = [Text.Encoding]::Unicode.GetString(
    [IO.File]::ReadAllBytes($executable))
if (!$executableUnicode.Contains($applicationUserModelId)) {
    throw "Release executable has the wrong application identity: expected $applicationUserModelId"
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

$hash = Get-AicliFileSha256 -LiteralPath $installerPath
$hashPath = "$installerPath.sha256"
[IO.File]::WriteAllText(
    $hashPath,
    "$hash  $([IO.Path]::GetFileName($installerPath))`r`n",
    [Text.UTF8Encoding]::new($false))

New-Item -ItemType Directory -Force -Path $publishDirectory | Out-Null
$publishedInstaller = Join-Path $publishDirectory ([IO.Path]::GetFileName($installerPath))
$publishedHash = "$publishedInstaller.sha256"
Copy-Item -LiteralPath $installerPath -Destination $publishedInstaller -Force
Copy-Item -LiteralPath $hashPath -Destination $publishedHash -Force

Write-Host "Installer: $installerPath"
Write-Host "Published: $publishedInstaller"
Write-Host "SHA256:   $hash"
