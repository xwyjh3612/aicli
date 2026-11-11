# Dependency locations and optional disk policies come from local configuration.
function Get-AicliFileSha256 {
    param([Parameter(Mandatory = $true)][string]$LiteralPath)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    $stream = $null
    try {
        $stream = [IO.File]::OpenRead([IO.Path]::GetFullPath($LiteralPath))
        return [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-', '')
    } finally {
        if ($stream) { $stream.Dispose() }
        $algorithm.Dispose()
    }
}

function Get-AicliDependencyManifest {
    $manifestPath = Join-Path $PSScriptRoot '..\dependencies.json'
    if (!(Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw 'Local dependencies.json is missing. Copy dependencies.example.json and configure the dependency roots.'
    }
    return Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
}

function Get-AicliDependencyPaths {
    $manifest = Get-AicliDependencyManifest
    $resolved = [ordered]@{}
    foreach ($package in $manifest.packages.PSObject.Properties) {
        $rootName = $package.Value.root
        $overrideName = 'AICLI_' + $rootName.ToUpperInvariant() + '_ROOT'
        $rootPath = [Environment]::GetEnvironmentVariable($overrideName, 'Process')
        if (!$rootPath) { $rootPath = $manifest.roots.$rootName }
        $rootPath = [Environment]::ExpandEnvironmentVariables([string]$rootPath)
        if (!$rootPath -or ![IO.Path]::IsPathRooted($rootPath)) {
            throw "Configure roots.$rootName in local dependencies.json or set $overrideName to an absolute path."
        }
        $resolved[$package.Name] = [IO.Path]::GetFullPath(
            (Join-Path $rootPath $package.Value.path))
    }
    return [pscustomobject]$resolved
}

function Assert-AicliBuildPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    $manifest = Get-AicliDependencyManifest
    $drive = [IO.Path]::GetPathRoot([IO.Path]::GetFullPath($Path)).TrimEnd([char[]]'\/')
    foreach ($readOnlyDrive in $manifest.build.readOnlyDrives) {
        if ($drive -ieq ([string]$readOnlyDrive).TrimEnd([char[]]'\/')) {
            throw "The local dependency configuration reserves $drive for read-only dependencies. Choose another build location."
        }
    }
}

function Get-AicliBuildCacheRoot {
    $cachePath = $env:AICLI_BUILD_CACHE_ROOT
    if (!$cachePath) { $cachePath = (Get-AicliDependencyManifest).build.cacheRoot }
    if (!$cachePath) { $cachePath = Join-Path $env:LOCALAPPDATA 'BuildCache' }
    $cachePath = [Environment]::ExpandEnvironmentVariables($cachePath)
    $cachePath = [IO.Path]::GetFullPath($cachePath)
    Assert-AicliBuildPath -Path $cachePath
    return $cachePath
}

function Set-AicliAndroidBuildEnvironment {
    $dependencies = Get-AicliDependencyPaths
    $cacheRoot = Get-AicliBuildCacheRoot
    $env:JAVA_HOME = $dependencies.java17
    $env:ANDROID_HOME = $dependencies.androidSdk
    $env:ANDROID_SDK_ROOT = $dependencies.androidSdk
    $env:GRADLE_USER_HOME = Join-Path $cacheRoot 'gradle'
    $env:ANDROID_AVD_HOME = Join-Path $cacheRoot 'android\avd'
    $env:ANDROID_USER_HOME = Join-Path $cacheRoot 'android\user'
    foreach ($directory in @($env:GRADLE_USER_HOME, $env:ANDROID_AVD_HOME, $env:ANDROID_USER_HOME)) {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
    }
    # Downloaded JAR/AAR/POM files are reusable dependencies. Only Gradle's
    # metadata, compilation and transforms belong in the configured build cache.
    New-Item -ItemType Directory -Path $dependencies.gradleArtifacts -Force | Out-Null
    $artifactCache = Join-Path $env:GRADLE_USER_HOME 'caches\modules-2\files-2.1'
    if (!(Test-Path -LiteralPath $artifactCache)) {
        New-Item -ItemType Directory -Path (Split-Path -Parent $artifactCache) -Force | Out-Null
        New-Item -ItemType Junction -Path $artifactCache -Target $dependencies.gradleArtifacts | Out-Null
    } elseif (!((Get-Item -LiteralPath $artifactCache -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Move the existing downloaded dependency store to $($dependencies.gradleArtifacts) and create a junction at $artifactCache."
    } else {
        $artifactTarget = @((Get-Item -LiteralPath $artifactCache -Force).Target)[0]
        if (!$artifactTarget -or [IO.Path]::GetFullPath($artifactTarget).TrimEnd('\') -ne $dependencies.gradleArtifacts.TrimEnd('\')) {
            throw "The dependency junction at $artifactCache must point to $($dependencies.gradleArtifacts)."
        }
    }
    return $dependencies
}
