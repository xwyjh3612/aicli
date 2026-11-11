param(
    [string]$TerminalRoot = '',
    [string]$Configuration = 'Debug',
    [string]$Platform = 'x64'
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
. (Join-Path $repositoryRoot 'tools\dependency-paths.ps1')
$dependencies = Get-AicliDependencyPaths
if (!$TerminalRoot) { $TerminalRoot = $dependencies.terminalSource }
$buildRoot = Join-Path (Get-AicliBuildCacheRoot) 'third-party\windows-terminal\1.20.11381.0'
$workingSource = Join-Path $buildRoot 'source'
New-Item -ItemType Directory -Path $workingSource -Force | Out-Null
# Terminal writes generated files into its source tree. Use one shared,
# rebuildable working copy rather than writing into the dependency source tree.
& robocopy $TerminalRoot $workingSource /E /XD .git .vs bin obj /NFL /NDL /NJH /NJS /NP
if ($LASTEXITCODE -ge 8) { throw "Terminal source staging failed: $LASTEXITCODE" }
$TerminalRoot = $workingSource
$msbuild = 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe'
if (!(Test-Path -LiteralPath $msbuild)) { throw "MSBuild not found: $msbuild" }
if (!(Test-Path -LiteralPath (Join-Path $TerminalRoot 'src\dep\fmt\include\fmt'))) {
    throw 'fmt submodule is missing under the fixed Terminal source tree'
}
if (!(Test-Path -LiteralPath (Join-Path $TerminalRoot 'dep\gsl\include\gsl'))) {
    throw 'GSL submodule is missing under the fixed Terminal source tree'
}

$common = @(
    "/m",
    "/p:Configuration=$Configuration",
    "/p:Platform=$Platform",
    "/p:SolutionDir=$TerminalRoot\",
    "/p:OpenConsoleDir=$TerminalRoot\",
    '/p:PgoTarget=false',
    '/p:PGOBuildMode=',
    '/p:AppxSymbolPackageEnabled=false',
    '/v:minimal'
)

$featureArgs = @(
    "/p:Configuration=$Configuration",
    "/p:SolutionDir=$TerminalRoot\",
    '/p:WindowsTerminalBranding=Dev',
    '/v:minimal'
)
& $msbuild (Join-Path $TerminalRoot 'build\rules\GenerateFeatureFlags.proj') @featureArgs
if ($LASTEXITCODE) { throw "Feature flag generation failed: $LASTEXITCODE" }

& $msbuild (Join-Path $TerminalRoot 'src\host\proxy\Host.Proxy.vcxproj') @common
if ($LASTEXITCODE) { throw "Host proxy build failed: $LASTEXITCODE" }

& $msbuild (Join-Path $TerminalRoot 'src\cascadia\TerminalSettingsModel\dll\Microsoft.Terminal.Settings.Model.vcxproj') @common
if ($LASTEXITCODE) { throw "TerminalSettingsModel build failed: $LASTEXITCODE" }

& $msbuild (Join-Path $TerminalRoot 'src\cascadia\TerminalControl\dll\TerminalControl.vcxproj') @common
if ($LASTEXITCODE) { throw "TerminalControl build failed: $LASTEXITCODE" }

$artifact = Join-Path $TerminalRoot "bin\$Platform\$Configuration\Microsoft.Terminal.Control"
$publish = Join-Path $buildRoot "artifacts\$Platform\$Configuration"
New-Item -ItemType Directory -Force -Path $publish | Out-Null
Copy-Item (Join-Path $artifact 'Microsoft.Terminal.Control.dll'),
          (Join-Path $artifact 'Microsoft.Terminal.Control.winmd'),
          (Join-Path $artifact 'Microsoft.Terminal.Control.pri') $publish -Force
Copy-Item (Join-Path $artifact 'Microsoft.Terminal.Control') $publish -Recurse -Force
$bin = Join-Path $TerminalRoot "bin\$Platform\$Configuration"
$settings = Join-Path $bin 'Microsoft.Terminal.Settings.Model'
Copy-Item (Join-Path $settings 'Microsoft.Terminal.Settings.Model.dll'),
          (Join-Path $settings 'Microsoft.Terminal.Settings.Model.winmd'),
          (Join-Path $settings 'Microsoft.Terminal.Settings.Model.pri') $publish -Force
Copy-Item (Join-Path $bin 'TerminalConnection\TerminalConnection.dll'),
          (Join-Path $bin 'TerminalConnection\Microsoft.Terminal.TerminalConnection.winmd'),
          (Join-Path $bin 'TerminalConnection\Microsoft.Terminal.TerminalConnection.pri'),
          (Join-Path $bin 'OpenConsoleProxy.dll') $publish -Force
Write-Output "TerminalControl artifacts copied to $publish"
