# build.ps1 -- one-click build for CRPC (reference implementation, validated).
# Location in the final repo: <repo>/scripts/build.ps1
#
# Usage:
#   .\scripts\build.ps1 [-Config Debug|Release|Both] [-Arch x86|x64]
#                       [-Sanitizer none|asan|ubsan|tsan] [-Jobs N] [-Clean]
#                       [-Shared] [-Examples] [-NoTests] [-Bench]
#                       [-SourceDir <path>] [-BuildRoot <path>]
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

    # Platform boundary: 'asan'/'ubsan'/'tsan' are Linux/GCC/Clang workflows.
    # MSVC only implements /fsanitize=address, so the Windows script rejects
    # every non-'none' sanitizer explicitly at runtime (see the guard below).
    # The set itself is kept intact (dynamic config not crippled); the gate is
    # driven by the detected compiler, not by a trimmed parameter list.
    [ValidateSet('none', 'asan', 'ubsan', 'tsan')]
    [string]$Sanitizer = 'none',

    # Language standard. 17 is the default and what the normal gate builds.
    # 20 exists because the library must also compile under C++20 (the
    # co_await support in response<Resp> needs it), and because without a
    # build that passes this, tests/test_rpc_response_cpp20.cpp is a no-op in
    # every configuration that exists -- which is exactly what had happened:
    # its cases had never run anywhere until this switch was added.
    [ValidateSet('17', '20')]
    [string]$CxxStandard = '17',

    [int]$Jobs = 0,

    [switch]$Clean,

    [switch]$Shared,

    # Also build the examples. They are OFF in CMake by default, so without this
    # switch the quickstarts are never compiled and can silently rot -- a
    # documentation example that no longer builds is worse than none.
    [switch]$Examples,

    # Configure with CRPC_BUILD_TESTS=OFF. The unit suite is ON by default because
    # that is what the normal gate wants; an examples-only configure line needs it
    # off, and not for build time: enable_testing() used to be called only inside
    # the CRPC_BUILD_TESTS branch, so EXAMPLES=ON with TESTS=OFF produced a tree
    # where `ctest -N` listed zero tests. Testing only the TESTS=ON combination
    # would never have caught that, so CI needs a way to ask for this shape.
    [switch]$NoTests,

    # Also build the microbenchmarks. They are OFF in CMake by default and are
    # never run by the test gate, so without this switch bench/ is never compiled
    # at all and its soft thresholds can rot into code that does not build.
    [switch]$Bench,

    # Resolved below, not here: with [CmdletBinding()] PowerShell evaluates
    # parameter defaults before $PSScriptRoot is populated, so a default of
    # `(Resolve-Path (Join-Path $PSScriptRoot '..'))` throws "Cannot bind
    # argument to parameter 'Path' because it is an empty string" on 5.1.
    [string]$SourceDir = '',

    [string]$BuildRoot = ''
)

$ErrorActionPreference = 'Stop'
if (-not $SourceDir) {
    $SourceDir = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
}
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
    # The language standard changes what compiles, so it gets its own tree for
    # the same reason shared does: reusing one would reconfigure it in place
    # and leave objects from the other standard behind.
    $std = if ($script:CxxStandard -eq '17') { '' } else { "-cxx$script:CxxStandard" }
    return "build/$Arch-$Cfg$suffix$kind$std"
}

$sw = [System.Diagnostics.Stopwatch]::StartNew()
$script:SharedBuild = [bool]$Shared
$script:CxxStandard = $CxxStandard

if (-not $BuildRoot) { $BuildRoot = Join-Path $SourceDir '.opencode/tmp/out' }
$logDir = Join-Path $BuildRoot 'logs'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$script:LogPath = Join-Path $logDir "build-$Arch-$Config-$Sanitizer-$stamp.log"

Write-Log "source=$SourceDir arch=$Arch config=$Config sanitizer=$Sanitizer shared=$($script:SharedBuild) cxx=$CxxStandard"
Write-Log "log=$script:LogPath"

$cmake = Find-CMake
if (-not $cmake) { Write-Log 'cmake not found (PATH, Program Files, VS bundle)' 'ERROR'; exit 2 }
$ninja = Find-Ninja
$vcvars = Find-VcVars -TargetArch $Arch

Write-Log "cmake=$cmake"
Write-Log "ninja=$(if ($ninja) { $ninja } else { 'n/a' })"
Write-Log "vcvars=$(if ($vcvars) { $vcvars } else { 'n/a' })"

if ($Jobs -le 0) { $Jobs = [Environment]::ProcessorCount }

# Dependency checkouts are CMake cache variables (CRPC_RTTI_DIR,
# CRPC_COROCC_DIR). The top-level CMakeLists.txt also falls back to the
# environment, but forwarding them as -D arguments keeps the configure line
# self-documenting and independent of how the cmake process is started.
# A missing RTTI checkout is reported here rather than left to the compiler:
# without it the tests that use RTTI_DEFINE_TYPE/RTTI_DEFINE_ENUM cannot build,
# and the failure surfaces as a wall of unrelated C2065 errors in test_mvc.cpp.
# The dependencies are git submodules under third_party/, so an unset variable is
# the normal case and not an error: CMakeLists.txt picks the submodule up. A set
# variable still wins and is forwarded, so a checkout that lives elsewhere keeps
# working. Only fail when neither exists, which is the case the error message is
# actually for.
$depArgs = @()
foreach ($dep in @('CRPC_RTTI_DIR', 'CRPC_COROCC_DIR')) {
    $value = [Environment]::GetEnvironmentVariable($dep)
    if ($value) {
        $depArgs += "-D${dep}=$value"
        Write-Log "$dep=$value (explicit override)"
    } else {
        Write-Log "$dep not set: using the third_party submodule"
    }
}
$submoduleRtti = Join-Path $SourceDir 'third_party\rtti\CMakeLists.txt'
if (-not [Environment]::GetEnvironmentVariable('CRPC_RTTI_DIR') -and
    -not (Test-Path -LiteralPath $submoduleRtti)) {
    Write-Log ('No RTTI checkout: neither CRPC_RTTI_DIR nor third_party/rtti exists. ' +
               'Run "git submodule update --init --recursive", or set ' +
               '$env:CRPC_RTTI_DIR to a checkout. Without it the tests that use ' +
               'RTTI_DEFINE_TYPE/RTTI_DEFINE_ENUM cannot be compiled.') 'ERROR'
    exit 3
}

$configs = if ($Config -eq 'Both') { @('Debug', 'Release') } else { @($Config) }

$sanFlags = @{
    none  = @()
    asan  = @('-DCRPC_SANITIZER=address')
    ubsan = @('-DCRPC_SANITIZER=undefined')
    tsan  = @('-DCRPC_SANITIZER=thread')
}

# Windows/MSVC sanitizer gate (W11 adjudication (a)+(e)). MSVC only implements
# /fsanitize=address; it has no undefined/thread sanitizer, and a partial
# /fsanitize=address instrumentation of crpc vs. the EXCLUDE_FROM_ALL
# third-party _deps makes the STL detect_mismatch ABI contract disagree
# (33x LNK2038 + 2x LNK1319). 'undefined'/'thread' are not real gates here:
# they only produce 455x D9002 warnings promoted by /WX (a fake red). The
# Windows sanitizer gate is therefore unavailable; coverage is provided by
# the Linux CI three-gate job (ci.yml linux-sanitizers / test.sh --sanitizer all).
# Reject explicitly (non-zero exit, not a silent no-op) so the failure is
# legible instead of surfacing as a linker error. `-Sanitizer` and the
# CRPC_SANITIZER cache variable stay intact; the gate follows the runtime
# compiler detection ($vcvars is MSVC).
if ($Sanitizer -ne 'none' -and $vcvars) {
    $reason = ("MSVC does not support sanitizer '" + $Sanitizer + "': MSVC only implements /fsanitize=address; " +
               "the Windows sanitizer gate is unavailable and sanitizer coverage is provided by " +
               "the Linux CI three-gate job (asan/ubsan/tsan). Use the POSIX scripts for sanitizer builds.")
    Write-Log $reason 'ERROR'
    Write-Log "refusing -Sanitizer $Sanitizer on MSVC; re-run with -Sanitizer none" 'ERROR'
    exit 4
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
        "-DCMAKE_CXX_STANDARD=$script:CxxStandard",
        $(if ($NoTests) { '-DCRPC_BUILD_TESTS=OFF' } else { '-DCRPC_BUILD_TESTS=ON' })
    )
    $configureArgs += $sanFlags[$Sanitizer]
    $configureArgs += $depArgs
    if ($script:SharedBuild) {
        $configureArgs += '-DCRPC_BUILD_SHARED=ON'
    } else {
        $configureArgs += '-DCRPC_BUILD_SHARED=OFF'
    }
    if ($Examples) {
        $configureArgs += '-DCRPC_BUILD_EXAMPLES=ON'
    }
    if ($Bench) {
        $configureArgs += '-DCRPC_BUILD_BENCH=ON'
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

