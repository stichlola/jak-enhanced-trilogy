# Fast C++ build for the dev loop: ninja run directly with a cached vcvars64 environment (calling
# vcvars64.bat costs ~1.6 s per build; cmake --build adds the rest of build.bat's 2.5 s no-op), only
# the targets the change needs, and each step's time from .ninja_log.
#
#   $s = [IO.File]::ReadAllText('<repo>\scripts\enhanced\dev_build.ps1')
#   & ([scriptblock]::Create($s))                    # auto targets: gk, plus goalc when its sources changed
#   & ([scriptblock]::Create($s)) -Targets gk,goalc
#   & ([scriptblock]::Create($s)) -Targets all       # ninja's default target (everything)
#
#   -Targets  auto (default) | all | target names. auto = gk; goalc too when the working tree differs
#             from HEAD under goalc/, common/ or decompiler/, or goalc.exe is older than a goalc/ file
#   -Jobs     ninja -j (default: the core count)
#   -Top      how many of the slowest steps to print (default 6)
#   -Refresh  rebuild the cached vcvars environment (local\run\vcvars.env)
# Games and goalc of the dev loop run from shadow copies of the bin folder (dev_bin.ps1), so they never
# lock the link; a gk started straight from out\build\Release\bin does: FAILED on bin/gk.exe says so.
# Output: "STEP <ms> <output>" for the slowest steps, then "BUILD OK|FAIL targets=<..> ms=<wall>
# steps=<n> compile=<ms>"; on failure the compiler's error lines first. $LASTEXITCODE 0/1.
param(
  [string[]]$Targets = @('auto'),
  [int]$Jobs = 0,
  [int]$Top = 6,
  [switch]$Refresh,
  [string]$BuildDir = '',
  [string]$Repo = ''
)
$ErrorActionPreference = 'Stop'
$t0 = Get-Date
if (-not $Repo) {
  $Repo = if ($PSScriptRoot) { Split-Path (Split-Path $PSScriptRoot) } else { (Get-Location).Path }
}
if (-not (Test-Path (Join-Path $Repo 'scripts\enhanced'))) { throw "-Repo: not a jak-enhanced tree: $Repo" }
$main = Split-Path (& git -C $Repo rev-parse --path-format=absolute --git-common-dir)
if (-not $BuildDir) { $BuildDir = Join-Path $Repo 'out\build\Release' }
$cache = Join-Path $BuildDir 'CMakeCache.txt'
if (-not (Test-Path $cache)) { throw "no CMakeCache.txt in $BuildDir (configure it first)" }

function Set-Exit([int]$code) {
  $global:LASTEXITCODE = $code
  if ($MyInvocation.MyCommand.CommandType -eq 'ExternalScript') { exit $code }
}

# --- vcvars64 environment, cached --------------------------------------------------------------
# Keyed on vcvars64.bat's path and time: a VS update rewrites it and the cache is rebuilt.
$vs = 'C:\Program Files\Microsoft Visual Studio\2022'
$vcvars = @('Community', 'Professional', 'Enterprise', 'BuildTools') |
  ForEach-Object { Join-Path $vs "$_\VC\Auxiliary\Build\vcvars64.bat" } | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $vcvars) { throw "vcvars64.bat not found under $vs" }
$envFile = Join-Path $main 'local\run\vcvars.env'
$key = "$vcvars|$((Get-Item $vcvars).LastWriteTimeUtc.Ticks)"
$vars = $null
if (-not $Refresh -and (Test-Path $envFile)) {
  $lines = [IO.File]::ReadAllLines($envFile)
  if ($lines.Count -gt 1 -and $lines[0] -eq $key) { $vars = $lines | Select-Object -Skip 1 }
}
if (-not $vars) {
  $vars = @(cmd /c "`"$vcvars`" >nul 2>nul && set") | Where-Object { $_ -match '^[^=]+=' }
  if (-not ($vars | Where-Object { $_ -match '^INCLUDE=' })) { throw "vcvars64.bat gave no INCLUDE" }
  New-Item -ItemType Directory -Force (Split-Path $envFile) | Out-Null
  [IO.File]::WriteAllLines($envFile, [string[]](@($key) + $vars))
}

# --- targets -----------------------------------------------------------------------------------
if ($Targets.Count -eq 1 -and $Targets[0] -match ',') { $Targets = $Targets[0] -split ',' }
$why = ''
if ($Targets -contains 'auto') {
  $Targets = @('gk')
  $dirty = @(& git -C $Repo status --porcelain --untracked-files=normal -- goalc common decompiler 2>$null)
  if ($dirty.Count) { $Targets += 'goalc'; $why = 'goalc/common/decompiler differ from HEAD' }
  else {
    # a goalc change already committed but never built: goalc.exe older than its newest source
    $exe = Join-Path $BuildDir 'bin\goalc.exe'
    if (Test-Path $exe) {
      $built = (Get-Item $exe).LastWriteTimeUtc
      $newer = Get-ChildItem (Join-Path $Repo 'goalc') -Recurse -File -Include *.cpp, *.h -ErrorAction SilentlyContinue |
        Where-Object { $_.LastWriteTimeUtc -gt $built } | Select-Object -First 1
      if ($newer) { $Targets += 'goalc'; $why = "$($newer.Name) is newer than goalc.exe" }
    } else { $Targets += 'goalc'; $why = 'no goalc.exe' }
  }
} elseif ($Targets -contains 'all') { $Targets = @() }

# --- ninja -------------------------------------------------------------------------------------
$ninja = (Select-String -Path $cache -Pattern '^CMAKE_MAKE_PROGRAM:FILEPATH=(.*)$').Matches[0].Groups[1].Value
$log = Join-Path $BuildDir '.ninja_log'
# Steps of this build = log entries whose output time (3rd field) is newer than any before it: ninja
# recompacts the log now and then (always after a CMake re-run), so line counts can't tell.
function Log-Entries {
  if (-not (Test-Path $log)) { return @() }
  foreach ($l in [IO.File]::ReadAllLines($log)) {
    $f = $l -split "`t"
    if ($f.Count -ge 4 -and $f[0] -match '^\d+$') {
      [pscustomobject]@{ ms = [int]$f[1] - [int]$f[0]; mt = [int64]$f[2]; path = $f[3] }
    }
  }
}
$mt0 = [int64](Log-Entries | Measure-Object mt -Maximum).Maximum
$psi = New-Object Diagnostics.ProcessStartInfo $ninja
$a = @()
if ($Jobs -gt 0) { $a += "-j $Jobs" }
$a += $Targets
$psi.Arguments = ($a -join ' ')
$psi.WorkingDirectory = $BuildDir
$psi.UseShellExecute = $false
$psi.CreateNoWindow = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true   # a CMake re-run's warnings: shown only when the build fails
$e = $psi.EnvironmentVariables
foreach ($v in $vars) { $i = $v.IndexOf('='); $e[$v.Substring(0, $i)] = $v.Substring($i + 1) }
$p = [Diagnostics.Process]::Start($psi)
$errTask = $p.StandardError.ReadToEndAsync()
$out = $p.StandardOutput.ReadToEnd()
$p.WaitForExit()
$out += "`n" + $errTask.Result
$code = $p.ExitCode

# --- report ------------------------------------------------------------------------------------
$steps = @(Log-Entries | Where-Object { $_.mt -gt $mt0 } | ForEach-Object {
    $o = $_.path -replace '^.*/CMakeFiles/', '' -replace '\.dir/', ':'
    if ($_.path -match '(\.cmake|build\.ninja)$') { $o = 'cmake re-run' }   # one regen step, ~200 outputs
    [pscustomobject]@{ ms = $_.ms; out = $o; obj = $_.path -match '\.obj$' } } |
  Sort-Object out, ms -Unique)
if ($out -match 'Re-running CMake') { 'NOTE CMake re-ran (a CMakeLists or a CONFIGURE_DEPENDS glob changed)' }
$steps | Sort-Object ms -Descending | Select-Object -First $Top | ForEach-Object { 'STEP {0} {1}' -f $_.ms, $_.out }
$compile = [int]($steps | Where-Object obj | Measure-Object ms -Sum).Sum
$tlist = if ($Targets.Count) { $Targets -join ',' } else { 'all' }
if ($why) { $tlist += " ($why)" }
$ms = [int]((Get-Date) - $t0).TotalMilliseconds
if ($code -ne 0) {
  # the error lines (file:line: error: ...), each with the next two lines of context, and FAILED steps
  $lines = $out -split "`r?`n" | ForEach-Object { $_ -replace '\x1b\[[0-9;]*m', '' }
  $shown = 0
  for ($i = 0; $i -lt $lines.Count -and $shown -lt 40; $i++) {
    if ($lines[$i] -match '^FAILED: (.*)') { "FAILED $($Matches[1])"; $shown++ }
    elseif ($lines[$i] -match ': (fatal )?error' -or $lines[$i] -match '^lld-link: error') {
      $lines[$i]; $shown++
      for ($j = 1; $j -le 2 -and $i + $j -lt $lines.Count; $j++) { "  $($lines[$i + $j])" }
    }
  }
  if ($out -match 'FAILED: bin/gk\.exe') {
    $holders = @(Get-Process gk -ErrorAction SilentlyContinue | Where-Object {
        try { $_.Path -eq (Join-Path $BuildDir 'bin\gk.exe') } catch { $false } } | ForEach-Object Id)
    if ($holders.Count) { "HINT bin\gk.exe is in use by pid $($holders -join ','): start games with dev_launch.ps1 (shadow bin)" }
  }
  'BUILD FAIL targets={0} ms={1} steps={2} compile={3}' -f $tlist, $ms, $steps.Count, $compile
  Set-Exit 1
} else {
  'BUILD OK targets={0} ms={1} steps={2} compile={3}' -f $tlist, $ms, $steps.Count, $compile
  Set-Exit 0
}
