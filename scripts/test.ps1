# test.ps1 -- one-click test for CRPC (reference implementation).
# Location in the final repo: <repo>/scripts/test.ps1
#
# Usage:
#   .\scripts\test.ps1 [-Config Debug|Release|Both] [-Arch x86|x64]
#                      [-Sanitizer none|asan|ubsan|tsan|all] [-Filter <regex>]
#                      [-Repeat N] [-Shared] [-SourceDir <path>] [-BuildRoot <path>]
#
# -Shared builds and tests CRPC_BUILD_SHARED=ON. In that mode the run also
# executes crpc_test_shared_link directly: it is the consumer link gate, and
# running it as a ctest entry as well keeps a filtered ctest invocation from
# silently skipping it.
#
# Behavior:
#   * runs CTest per build dir, then every test executable directly for per-case output
#   * -Sanitizer all builds+runs asan, ubsan, tsan in separate build dirs
#   * detects ASan/LSan/UBSan/TSan reports in output -> failure
#   * UTF-8 logs, non-zero exit on any failure

[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'Both')] [string]$Config = 'Debug',
    [ValidateSet('x86', 'x64')] [string]$Arch = 'x64',
    [ValidateSet('none', 'asan', 'ubsan', 'tsan', 'all')] [string]$Sanitizer = 'none',
    [string]$Filter = '',
    [int]$Repeat = 1,
    [switch]$Shared,
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
$script:LogPath = $null

function Write-Log {
    param([string]$Message, [string]$Level = 'INFO')
    $line = ('[{0}] [{1}] {2}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $Level, $Message)
    Write-Host $line
    if ($script:LogPath) {
        [System.IO.File]::AppendAllText($script:LogPath, $line + "`r`n", [System.Text.UTF8Encoding]::new($false))
    }
}

function Find-CMake {
    $cmd = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    foreach ($c in @(
        (Join-Path ${env:ProgramFiles} 'CMake\bin\cmake.exe'),
        (Join-Path ${env:ProgramFiles(x86)} 'CMake\bin\cmake.exe'),
        (Join-Path ${env:ProgramFiles} 'Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'))) {
        if (Test-Path -LiteralPath $c) { return $c }
    }
    return $null
}

function Find-CTest {
    $cmake = Find-CMake
    if (-not $cmake) { return $null }
    $c = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
    if (Test-Path -LiteralPath $c) { return $c }
    $cmd = Get-Command ctest -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $null
}

$cmake = Find-CMake
$ctest = Find-CTest
if (-not $cmake) { Write-Host 'cmake not found'; exit 2 }

if (-not $BuildRoot) { $BuildRoot = Join-Path $SourceDir '.opencode/tmp/out' }
$logDir = Join-Path $BuildRoot 'logs'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$script:LogPath = Join-Path $logDir "test-$Arch-$Config-$Sanitizer-$stamp.log"
Write-Log "source=$SourceDir arch=$Arch config=$Config sanitizer=$Sanitizer repeat=$Repeat shared=$([bool]$Shared)"

# --- Unattended-run guards ---------------------------------------------------
#
# A Debug build that hits a CRT assertion, an invalid parameter, a pure virtual
# call or an access violation will normally raise a modal dialog. In an
# unattended run that dialog blocks the whole desktop and the test never
# returns: ctest hangs instead of failing. Two independent guards are applied so
# that a test binary which does not link the CRPC test harness is still safe:
#
#   1. Windows Error Reporting is told not to show its UI for this user.
#   2. Every ctest entry gets a hard timeout, so a process that is blocked on
#      something else cannot wedge the run either.
#
# The in-process guard (SetErrorMode + report-mode redirection + minidump) lives
# in tests/test_framework.hpp and covers the normal case; these are the backstop.

$werKey = 'HKCU:\Software\Microsoft\Windows\Windows Error Reporting'
try {
    if (-not (Test-Path -LiteralPath $werKey)) {
        New-Item -Path $werKey -Force | Out-Null
    }
    New-ItemProperty -Path $werKey -Name 'DontShowUI' -Value 1 -PropertyType DWord -Force | Out-Null
    New-ItemProperty -Path $werKey -Name 'Disabled' -Value 1 -PropertyType DWord -Force | Out-Null
} catch {
    Write-Log "could not disable WER UI: $($_.Exception.Message)" 'WARN'
}

$dumpDir = Join-Path $BuildRoot 'dumps'
New-Item -ItemType Directory -Force -Path $dumpDir | Out-Null
$env:CRPC_TEST_MINIDUMP_DIR = $dumpDir

# Per-test ceiling. A test that needs longer must raise it explicitly rather than
# relying on the runner to hang forever.
$testTimeoutSeconds = 30


$sanList = if ($Sanitizer -eq 'all') { @('asan', 'ubsan', 'tsan') } else { @($Sanitizer) }
$cfgList = if ($Config -eq 'Both') { @('Debug', 'Release') } else { @($Config) }

$buildScript = Join-Path $PSScriptRoot 'build.ps1'

foreach ($san in $sanList) {
    foreach ($cfg in $cfgList) {
        Write-Log "=== build $Arch/$cfg/$san ==="
        $buildArgs = @{ Config = $cfg; Arch = $Arch; Sanitizer = $san
                        SourceDir = $SourceDir; BuildRoot = $BuildRoot }
        if ($Shared) { $buildArgs['Shared'] = $true }
        & $buildScript @buildArgs
        if ($LASTEXITCODE -ne 0) {
            Write-Log "build failed for $Arch/$cfg/$san" 'ERROR'
            $script:Failures++
            continue
        }

        $dirName = if ($san -eq 'none') { "build/$Arch-$cfg" } else { "build/$Arch-$cfg-$san" }
        if ($Shared) { $dirName = "$dirName-shared" }
        $buildDir = Join-Path $BuildRoot $dirName
        if (-not (Test-Path -LiteralPath $buildDir)) {
            Write-Log "build dir missing: $buildDir" 'ERROR'
            $script:Failures++
            continue
        }

        for ($r = 1; $r -le $Repeat; ++$r) {
            Write-Log "--- ctest $Arch/$cfg/$san run $r/$Repeat ---"
            $ctestArgs = @('--test-dir', $buildDir, '--output-on-failure',
                           '--timeout', "$testTimeoutSeconds",
                           '--no-tests=error')
            if ($Filter) { $ctestArgs += @('-R', $Filter) }
            $out = & $ctest @ctestArgs 2>&1
            $out | ForEach-Object { [System.IO.File]::AppendAllText($script:LogPath, "$_`r`n", [System.Text.UTF8Encoding]::new($false)) }
            $out | Where-Object { $_ -match 'Test\s+#|tests passed|tests failed|Failed|Timeout|\*\*\*' } | ForEach-Object { Write-Host $_ }

            if ($LASTEXITCODE -ne 0) {
                Write-Log "ctest FAILED $Arch/$cfg/$san run $r" 'ERROR'
                $script:Failures++
            }

            # A timed-out ctest entry can leave the child process alive. Kill any
            # leftover test binary so the next run starts clean instead of
            # inheriting a wedged process.
            Get-Process -Name 'crpc_test_*' -ErrorAction SilentlyContinue |
                ForEach-Object {
                    Write-Log "killing leftover test process $($_.ProcessName) (pid $($_.Id))" 'WARN'
                    Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue
                }

            # The shared-build consumer gate is part of the ctest set, but a
            # -Filter run can exclude it. Execute it unconditionally in shared
            # mode so a filtered invocation cannot skip the export check.
            if ($Shared) {
                $probe = Join-Path $buildDir "tests\crpc_test_shared_link.exe"
                if (-not (Test-Path -LiteralPath $probe)) {
                    Write-Log "shared link gate missing: $probe" 'ERROR'
                    $script:Failures++
                } else {
                    # The probe imports crpc.dll from the build root, so run it
                    # there: the loader searches the working directory after the
                    # executable's own directory. ctest gets the same directory
                    # from the WORKING_DIRECTORY property on the test.
                    Push-Location -LiteralPath $buildDir
                    try {
                        $probeOut = & $probe 2>&1
                    } finally {
                        Pop-Location
                    }
                    $probeOut | ForEach-Object { [System.IO.File]::AppendAllText($script:LogPath, "$_`r`n", [System.Text.UTF8Encoding]::new($false)) }
                    if ($LASTEXITCODE -ne 0) {
                        Write-Log "shared link gate FAILED" 'ERROR'
                        $probeOut | Select-Object -Last 20 | ForEach-Object { Write-Host $_ }
                        $script:Failures++
                    } else {
                        Write-Log "shared link gate OK"
                    }
                }
            }

            $sanPattern = 'ERROR: (AddressSanitizer|LeakSanitizer|UndefinedBehaviorSanitizer|ThreadSanitizer)|runtime error:|SUMMARY: (AddressSanitizer|ThreadSanitizer|LeakSanitizer)'
            $sanHits = @($out | Select-String -Pattern $sanPattern)
            if ($sanHits.Count -gt 0) {
                Write-Log "sanitizer report detected ($($sanHits.Count) lines)" 'ERROR'
                $sanHits | Select-Object -First 20 | ForEach-Object { Write-Host $_ }
                $script:Failures++
            }
        }
    }
}

Write-Log ("done: failures={0}, log={1}" -f $script:Failures, $script:LogPath)
if ($script:Failures -gt 0) { exit 1 }
exit 0

