# build.ps1 -- one-click build for CRPC (reference implementation, validated).
# Location in the final repo: <repo>/scripts/build.ps1
#
# Usage:
#   .\scripts\build.ps1 [-Config Debug|Release|Both] [-Arch x86|x64]
#                       [-Sanitizer none|asan|ubsan|tsan] [-Jobs N] [-Clean]
#                       [-Shared] [-SourceDir <path>] [-BuildRoot <path>]
#
# -Shared selects CRPC_BUILD_SHARED=ON (a crpc.dll/crpc.so plus the
# crpc_test_shared_link consumer gate). Without it the library is a static
# archive, which is the default and needs no runtime deployment.
#
# Guarantees:
#   * toolchain auto-detected (vswhere/vcvars for MSVC, PATH for clang/gcc)
#   * zero-warning gate: /W4 /WX (MSVC), -Wall -Wextra -Werror (clang/gcc)
#   * idempotent; safe to re-run; -Clean removes only the chosen build dir
#   * UTF-8 logs (no Tee-Object: it writes UTF-16 on PowerShell 5.1)
#   * non-zero exit code on any failure

[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'Both')]
    [string]$Config = 'Debug',

    [ValidateSet('x86', 'x64')]
    [string]$Arch = 'x64',

    [ValidateSet('none', 'asan', 'ubsan', 'tsan')]
    [string]$Sanitizer = 'none',

    [int]$Jobs = 0,

    [switch]$Clean,

    [switch]$Shared,

    [string]$SourceDir = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path,

    [string]$BuildRoot = ''
)

$ErrorActionPreference = 'Stop'
$script:Failures = 0
$script:Warnings = 0

function Write-Log {
    param([string]$Message, [string]$Level = 'INFO')
    $line = ('[{0}] [{1}] {2}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $Level, $Message)
    Write-Host $line
    if ($script:LogPath) {
        [System.IO.File]::AppendAllText($script:LogPath, $line + "`r`n", [System.Text.UTF8Encoding]::new($false))
    }
}

function Find-VcVars {
    param([string]$TargetArch)
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) {
        $vswhere = Join-Path $env:ProgramFiles 'Microsoft Visual Studio\Installer\vswhere.exe'
    }
    if (-not (Test-Path -LiteralPath $vswhere)) { return $null }

    $install = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
    if (-not $install) { return $null }
    $install = ($install | Select-Object -First 1).Trim()
    $name = if ($TargetArch -eq 'x86') { 'vcvars32.bat' } else { 'vcvars64.bat' }
    $bat = Join-Path $install "VC\Auxiliary\Build\$name"
    if (Test-Path -LiteralPath $bat) { return $bat }
    return $null
}

function Find-CMake {
    $cmd = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $candidates = @(
        (Join-Path ${env:ProgramFiles} 'CMake\bin\cmake.exe'),
        (Join-Path ${env:ProgramFiles(x86)} 'CMake\bin\cmake.exe')
    )
    foreach ($c in $candidates) { if (Test-Path -LiteralPath $c) { return $c } }
    $vs = Join-Path ${env:ProgramFiles} 'Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    if (Test-Path -LiteralPath $vs) { return $vs }
    return $null
}

function Find-Ninja {
    $cmd = Get-Command ninja -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $vs = Join-Path ${env:ProgramFiles} 'Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
    if (Test-Path -LiteralPath $vs) { return $vs }
    return $null
}

function Get-BuildDirName {
    param([string]$Cfg, [string]$San)
    $suffix = if ($San -eq 'none') { '' } else { "-$San" }
    # A shared build is a different configuration of the same sources, so it
    # gets its own directory: reusing the static one would leave a stale
    # crpc.lib in place and silently link the wrong library type.
    $kind = if ($script:SharedBuild) { '-shared' } else { '' }
    return "build/$Arch-$Cfg$suffix$kind"
}

$sw = [System.Diagnostics.Stopwatch]::StartNew()
$script:SharedBuild = [bool]$Shared

if (-not $BuildRoot) { $BuildRoot = Join-Path $SourceDir '.opencode/tmp/out' }
$logDir = Join-Path $BuildRoot 'logs'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$script:LogPath = Join-Path $logDir "build-$Arch-$Config-$Sanitizer-$stamp.log"

Write-Log "source=$SourceDir arch=$Arch config=$Config sanitizer=$Sanitizer shared=$($script:SharedBuild)"
Write-Log "log=$script:LogPath"

$cmake = Find-CMake
if (-not $cmake) { Write-Log 'cmake not found (PATH, Program Files, VS bundle)' 'ERROR'; exit 2 }
$ninja = Find-Ninja
$vcvars = Find-VcVars -TargetArch $Arch

Write-Log "cmake=$cmake"
Write-Log "ninja=$(if ($ninja) { $ninja } else { 'n/a' })"
Write-Log "vcvars=$(if ($vcvars) { $vcvars } else { 'n/a' })"

if ($Jobs -le 0) { $Jobs = [Environment]::ProcessorCount }

$configs = if ($Config -eq 'Both') { @('Debug', 'Release') } else { @($Config) }

$sanFlags = @{
    none  = @()
    asan  = @('-DCRPC_SANITIZER=address')
    ubsan = @('-DCRPC_SANITIZER=undefined')
    tsan  = @('-DCRPC_SANITIZER=thread')
}

foreach ($cfg in $configs) {
    $dirName = Get-BuildDirName -Cfg $cfg -San $Sanitizer
    $buildDir = Join-Path $BuildRoot $dirName

    if ($Clean -and (Test-Path -LiteralPath $buildDir)) {
        Write-Log "clean: removing $buildDir"
        Remove-Item -LiteralPath $buildDir -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

    $generator = if ($ninja) { 'Ninja' } else { 'NMake Makefiles' }
    $configureArgs = @(
        '-S', $SourceDir,
        '-B', $buildDir,
        '-G', $generator,
        "-DCMAKE_BUILD_TYPE=$cfg",
        '-DCRPC_WARNINGS_AS_ERRORS=ON',
        '-DCRPC_BUILD_TESTS=ON'
    )
    $configureArgs += $sanFlags[$Sanitizer]
    if ($script:SharedBuild) {
        $configureArgs += '-DCRPC_BUILD_SHARED=ON'
    } else {
        $configureArgs += '-DCRPC_BUILD_SHARED=OFF'
    }

    if ($vcvars) {
        # Run cmake inside a vcvars environment so the compiler is on PATH.
        $argLine = ($configureArgs | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' '
        $cmd = "call `"$vcvars`" >nul && `"$cmake`" $argLine"
        Write-Log "configure: $cmd"
        $out = cmd /c $cmd 2>&1
    } else {
        Write-Log "configure: $cmake $($configureArgs -join ' ')"
        $out = & $cmake @configureArgs 2>&1
    }
    $out | ForEach-Object { [System.IO.File]::AppendAllText($script:LogPath, "$_`r`n", [System.Text.UTF8Encoding]::new($false)) }
    if ($LASTEXITCODE -ne 0) {
        Write-Log "configure FAILED ($cfg)" 'ERROR'
        $out | Select-Object -Last 25 | ForEach-Object { Write-Host $_ }
        $script:Failures++
        continue
    }

    if ($vcvars) {
        $buildArgs = @('--build', $buildDir, '--parallel', "$Jobs")
        $argLine = ($buildArgs | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' '
        $cmd = "call `"$vcvars`" >nul && `"$cmake`" $argLine"
        Write-Log "build: $cmd"
        $out = cmd /c $cmd 2>&1
    } else {
        Write-Log "build: $cmake --build $buildDir --parallel $Jobs"
        $out = & $cmake --build $buildDir --parallel $Jobs 2>&1
    }
    $out | ForEach-Object { [System.IO.File]::AppendAllText($script:LogPath, "$_`r`n", [System.Text.UTF8Encoding]::new($false)) }

    $warnCount = ($out | Select-String -Pattern 'warning[: ]' -SimpleMatch:$false).Count
    $errCount = ($out | Select-String -Pattern 'error[: ]' -SimpleMatch:$false).Count
    $script:Warnings += $warnCount

    if ($LASTEXITCODE -ne 0 -or $errCount -gt 0) {
        Write-Log "build FAILED ($cfg): errors=$errCount warnings=$warnCount" 'ERROR'
        $out | Select-Object -Last 30 | ForEach-Object { Write-Host $_ }
        $script:Failures++
        continue
    }
    if ($warnCount -gt 0) {
        Write-Log "build FAILED ($cfg): $warnCount warning(s) with -WX enabled" 'ERROR'
        $out | Select-String -Pattern 'warning[: ]' | Select-Object -First 20 | ForEach-Object { Write-Host $_ }
        $script:Failures++
        continue
    }

    Write-Log "build OK ($cfg) -> $buildDir"
}

$sw.Stop()
Write-Log ("done in {0:N1}s, failures={1}, warnings={2}" -f $sw.Elapsed.TotalSeconds, $script:Failures, $script:Warnings)
if ($script:Failures -gt 0) { exit 1 }
exit 0

