$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$projectDirectory = Split-Path -Parent $PSScriptRoot
$repositoryRoot = Split-Path -Parent $projectDirectory
. (Join-Path $repositoryRoot 'tools\dependency-paths.ps1')
$dependencies = Set-AicliAndroidBuildEnvironment
$javaHome = $dependencies.java17
$androidSdk = $dependencies.androidSdk
$gradleHome = $dependencies.gradle
$gradle = Join-Path $gradleHome "bin\gradle.bat"
$keytool = Join-Path $javaHome "bin\keytool.exe"
$buildTools = Join-Path $androidSdk "build-tools\35.0.0"
$apksigner = Join-Path $buildTools "apksigner.bat"
$aapt = Join-Path $buildTools "aapt.exe"

foreach ($required in @($gradle, $keytool, $apksigner, $aapt)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Missing Android release tool: $required"
    }
}

$signingDirectory = Join-Path $env:LOCALAPPDATA 'AiCli\signing'
$keystore = Join-Path $signingDirectory "ai-cli-release.p12"
$protectedPasswordFile = Join-Path $signingDirectory "release-password.dpapi"
$keyAlias = "aicli-release"
New-Item -ItemType Directory -Path $signingDirectory -Force | Out-Null

if (Test-Path -LiteralPath $protectedPasswordFile) {
    $securePassword = Get-Content -LiteralPath $protectedPasswordFile -Raw |
        ConvertTo-SecureString
    $credential = [System.Management.Automation.PSCredential]::new(
        "aicli-release", $securePassword)
    $password = $credential.GetNetworkCredential().Password
} else {
    if (Test-Path -LiteralPath $keystore) {
        throw "The existing release key has no DPAPI password file: $protectedPasswordFile"
    }
    $randomBytes = New-Object byte[] 36
    $randomGenerator = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    try {
        $randomGenerator.GetBytes($randomBytes)
    } finally {
        $randomGenerator.Dispose()
    }
    $password = [Convert]::ToBase64String($randomBytes).Replace("+", "-").Replace("/", "_").TrimEnd([char]'=')
    $securePassword = ConvertTo-SecureString $password -AsPlainText -Force
    $protectedPassword = ConvertFrom-SecureString $securePassword
    [System.IO.File]::WriteAllText(
        $protectedPasswordFile,
        $protectedPassword,
        [System.Text.UTF8Encoding]::new($false))
}

if (-not (Test-Path -LiteralPath $keystore)) {
    & $keytool -genkeypair -v `
        -storetype PKCS12 `
        -keystore $keystore `
        -storepass $password `
        -keypass $password `
        -alias $keyAlias `
        -keyalg RSA `
        -keysize 4096 `
        -validity 10000 `
        -dname "CN=AI CLI, OU=Mobile Client, O=AI CLI, L=Taipei, ST=Taiwan, C=TW"
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to generate the Android release key."
    }
}

$env:AICLI_RELEASE_KEYSTORE = $keystore
$env:AICLI_RELEASE_STORE_PASSWORD = $password
$env:AICLI_RELEASE_KEY_ALIAS = $keyAlias
$env:AICLI_RELEASE_KEY_PASSWORD = $password

Push-Location $projectDirectory
try {
    & $gradle --no-daemon --stacktrace :app:assembleRelease
    if ($LASTEXITCODE -ne 0) {
        throw "Gradle release build failed."
    }
} finally {
    Pop-Location
    Remove-Item Env:AICLI_RELEASE_STORE_PASSWORD -ErrorAction SilentlyContinue
    Remove-Item Env:AICLI_RELEASE_KEY_PASSWORD -ErrorAction SilentlyContinue
    $password = $null
}

$releaseDirectory = Join-Path $projectDirectory "app\build\outputs\apk\release"
$builtApk = Join-Path $releaseDirectory "app-release.apk"
if (-not (Test-Path -LiteralPath $builtApk)) {
    throw "Gradle release APK was not found: $builtApk"
}

& $apksigner verify --verbose --print-certs $builtApk
if ($LASTEXITCODE -ne 0) {
    throw "APK signature verification failed."
}

$badging = & $aapt dump badging $builtApk
if ($LASTEXITCODE -ne 0) {
    throw "APK metadata verification failed."
}
$packageLine = $badging | Select-String -Pattern "^package:"
if (-not $packageLine) {
    throw "APK package and version metadata were not found."
}
$versionMatch = [regex]::Match($packageLine.Line, "versionName='([^']+)'" )
if (-not $versionMatch.Success) {
    throw "APK versionName was not found."
}
$versionName = $versionMatch.Groups[1].Value
$publishedApk = Join-Path $releaseDirectory "AI-CLI-Android-$versionName-release.apk"
Copy-Item -LiteralPath $builtApk -Destination $publishedApk -Force
$hash = Get-AicliFileSha256 -LiteralPath $publishedApk
$repositoryRoot = Split-Path -Parent $projectDirectory
$publishDirectory = Join-Path $repositoryRoot "release"
New-Item -ItemType Directory -Path $publishDirectory -Force | Out-Null
$rootPublishedApk = Join-Path $publishDirectory ([IO.Path]::GetFileName($publishedApk))
$rootHashFile = "$rootPublishedApk.sha256"
Copy-Item -LiteralPath $publishedApk -Destination $rootPublishedApk -Force
[IO.File]::WriteAllText(
    $rootHashFile,
    "$hash  $([IO.Path]::GetFileName($rootPublishedApk))`r`n",
    [Text.UTF8Encoding]::new($false))

Write-Output ""
Write-Output "Release APK: $publishedApk"
Write-Output "Published APK: $rootPublishedApk"
Write-Output $packageLine.Line
Write-Output "SHA256: $hash"
Write-Output "Signing key: $keystore"
Write-Output "Signing password: protected with Windows DPAPI in $protectedPasswordFile"
