# test.ps1 -- one-click test for CRPC (reference implementation).
# Location in the final repo: <repo>/scripts/test.ps1
#
# Usage:
#   .\scripts\test.ps1 [-Config Debug|Release|Both] [-Arch x86|x64]
#                      [-Sanitizer none|asan|ubsan|tsan|all] [-Filter <regex>]
#                      [-Repeat N] [-SourceDir <path>] [-BuildRoot <path>]
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
    [string]$SourceDir = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path,
    [string]$BuildRoot = ''
)

$ErrorActionPreference = 'Stop'
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

if (-not $BuildRoot) { $BuildRoot = Join-Path $SourceDir 'out' }
$logDir = Join-Path $BuildRoot 'logs'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$script:LogPath = Join-Path $logDir "test-$Arch-$Config-$Sanitizer-$stamp.log"
Write-Log "source=$SourceDir arch=$Arch config=$Config sanitizer=$Sanitizer repeat=$Repeat"

$sanList = if ($Sanitizer -eq 'all') { @('asan', 'ubsan', 'tsan') } else { @($Sanitizer) }
$cfgList = if ($Config -eq 'Both') { @('Debug', 'Release') } else { @($Config) }

$buildScript = Join-Path $PSScriptRoot 'build.ps1'

foreach ($san in $sanList) {
    foreach ($cfg in $cfgList) {
        Write-Log "=== build $Arch/$cfg/$san ==="
        & $buildScript -Config $cfg -Arch $Arch -Sanitizer $san -SourceDir $SourceDir -BuildRoot $BuildRoot
        if ($LASTEXITCODE -ne 0) {
            Write-Log "build failed for $Arch/$cfg/$san" 'ERROR'
            $script:Failures++
            continue
        }

        $dirName = if ($san -eq 'none') { "build/$Arch-$cfg" } else { "build/$Arch-$cfg-$san" }
        $buildDir = Join-Path $BuildRoot $dirName
        if (-not (Test-Path -LiteralPath $buildDir)) {
            Write-Log "build dir missing: $buildDir" 'ERROR'
            $script:Failures++
            continue
        }

        for ($r = 1; $r -le $Repeat; ++$r) {
            Write-Log "--- ctest $Arch/$cfg/$san run $r/$Repeat ---"
            $ctestArgs = @('--test-dir', $buildDir, '--output-on-failure')
            if ($Filter) { $ctestArgs += @('-R', $Filter) }
            $out = & $ctest @ctestArgs 2>&1
            $out | ForEach-Object { [System.IO.File]::AppendAllText($script:LogPath, "$_`r`n", [System.Text.UTF8Encoding]::new($false)) }
            $out | Where-Object { $_ -match 'Test\s+#|tests passed|tests failed|Failed|\*\*\*' } | ForEach-Object { Write-Host $_ }

            if ($LASTEXITCODE -ne 0) {
                Write-Log "ctest FAILED $Arch/$cfg/$san run $r" 'ERROR'
                $script:Failures++
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
