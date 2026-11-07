# One in-game check, asserted on log lines instead of screenshots: boots a test game straight into a
# scenario (dev_launch.ps1: shadow bin, instance >= 1, windowed or headless), waits until the game is
# ready, cycles variants inside the same run, checks regexes against the gk log (and against the
# answers of GOAL forms), prints PASS/FAIL per check and closes its game.
#
#   $s = [IO.File]::ReadAllText('<repo>\scripts\enhanced\dev_test.ps1')
#   & ([scriptblock]::Create($s)) -Scenario desert-boot              # scripts\enhanced\scenarios\<name>.json
#   & ([scriptblock]::Create($s)) -Start desert-start -Headless -Expect 'GAMEPLAY: enter desert'
#   & ([scriptblock]::Create($s)) -Start desert-start -Env @{ JAK_ENHANCED_HOUR = '18' } -Expect '\[enhanced\] hour 18'
#
# Scenario file (JSON; every key optional, command-line switches win over it):
#   { "start": "desert-start", "headless": true, "env": { "JAK_ENHANCED_HOUR": "18" },
#     "ready": "<regex>", "timeout": 40, "expect": ["<regex>", ...], "forbid": ["<regex>", ...],
#     "variants": [ { "name": "noon", "eval": "<GOAL form>", "answer": "<regex>",
#                     "tune": { "shadow": { "alpha_ref": 0.5 } }, "wait": 10, "settle": 1.5,
#                     "expect": ["<regex>"], "timeout": 15, "shot": true } ] }
#   ready    a gk log line that means "in the scenario" (default with a start: the first [camera] line
#            with the camera placed, which test boots log every 2 s, headless too: Jak and the level are
#            up; without one: "GAMEPLAY: enter")
#   expect   must each show in the gk log during the run (from launch to the end)
#   forbid   must never show (crashes, "[enhanced] ... failed", ...)
#   variants run in order in the same game: "eval" sends a GOAL form through the instance's dev goalc
#            (dev_goalc.ps1: started with the game, its first (mi) costs 30-50 s, then it stays warm for the
#            next runs) and "answer" checks its output; "tune" writes local\tuning_i<n>.json, which the
#            renderer re-reads within a second (windowed only; the file is removed afterwards). Order:
#            "wait" (seconds of game time to let pass BEFORE the variant acts), then "tune", then "eval",
#            then "settle" (seconds AFTER it acted, before its checks); "expect" must show in the log after
#            the wait, within "timeout" s; "shot" saves a screenshot (windowed, at most 4 a run)
# -Headless = gk --no-display: boots in ~5.5 s, game logic only (no renderer: no tuning, no shaders, no
# fps lines, no shots). Output: "PASS|FAIL <check> t=<s since launch> [why]", then
# "RESULT pass=<a> fail=<b> t=<s> log=<gk log>". $LASTEXITCODE 0 when all passed.
param(
  [string]$Scenario = '',
  [string]$Start = '',
  [hashtable]$Env = @{},
  [switch]$Headless,
  [switch]$Windowed,
  [string]$Ready = '',
  [string[]]$Expect = @(),
  [string[]]$Forbid = @(),
  [int]$Timeout = 0,
  [int]$Instance = 1,
  [string]$OutDir = '',
  [switch]$StopGoalc,
  [switch]$Keep,
  [string]$Repo = ''
)
$ErrorActionPreference = 'Stop'
$t0 = Get-Date
$inv = [Globalization.CultureInfo]::InvariantCulture   # "t=6.5" whatever the user's locale
if (-not $Repo) {
  $Repo = if ($PSScriptRoot) { Split-Path (Split-Path $PSScriptRoot) } else { (Get-Location).Path }
}
if (-not (Test-Path (Join-Path $Repo 'scripts\enhanced'))) { throw "-Repo: not a jak-enhanced tree: $Repo" }
if ($Instance -lt 1) { throw '-Instance must be 1 or more (0 is the user''s game)' }
$main = Split-Path (& git -C $Repo rev-parse --path-format=absolute --git-common-dir)
$enh = Join-Path $Repo 'scripts\enhanced'
function Run-Script([string]$name, [hashtable]$a) {
  & ([scriptblock]::Create([IO.File]::ReadAllText((Join-Path $enh $name)))) @a
}
function Set-Exit([int]$code) {
  $global:LASTEXITCODE = $code
  if ($MyInvocation.MyCommand.CommandType -eq 'ExternalScript') { exit $code }
}

# --- the scenario ------------------------------------------------------------------------------
$sc = $null
if ($Scenario) {
  $f = if (Test-Path $Scenario) { $Scenario } else { Join-Path $enh "scenarios\$Scenario.json" }
  if (-not (Test-Path $f)) { throw "no scenario $Scenario ($f)" }
  $sc = [IO.File]::ReadAllText($f) | ConvertFrom-Json
}
function Pick($name, $value, $default) {
  if ($value) { return $value }
  if ($sc -and $null -ne $sc.$name) { return $sc.$name }
  return $default
}
$Start = Pick 'start' $Start ''
$isHeadless = if ($Headless) { $true } elseif ($Windowed) { $false } else { [bool](Pick 'headless' $null $false) }
# (the [camera] line is logged on test boots only, those with a start point)
$Ready = Pick 'ready' $Ready $(if ($Start) { '^\[camera\] (?!0\.00 0\.00 0\.00 )' } else { '^GAMEPLAY: enter ' })
$Timeout = [int](Pick 'timeout' $Timeout 40)
$Expect = @(Pick 'expect' $Expect @())
$Forbid = @(Pick 'forbid' $Forbid @())
$variants = @(if ($sc -and $sc.variants) { $sc.variants })
$envAll = @{}
if ($sc -and $sc.env) { foreach ($p in $sc.env.PSObject.Properties) { $envAll[$p.Name] = "$($p.Value)" } }
foreach ($k in $Env.Keys) { $envAll[$k] = "$($Env[$k])" }
if (-not $OutDir) { $OutDir = Join-Path $main ("local\run\tests\{0}-i{1}" -f (Get-Date -Format 'yyyyMMdd-HHmmss'), $Instance) }
$needGoalc = [bool]($variants | Where-Object { $_.eval })
$tuneFile = Join-Path $main "local\tuning_i$Instance.json"
$tuneBackup = $null

# --- results -----------------------------------------------------------------------------------
$script:pass = 0; $script:fail = 0
function Report([bool]$ok, [string]$check, [string]$why = '') {
  $t = ((Get-Date) - $t0).TotalSeconds
  if ($ok) { $script:pass++ } else { $script:fail++ }
  $line = [string]::Format($inv, '{0} {1} t={2:0.0}', $(if ($ok) { 'PASS' } else { 'FAIL' }), $check, $t)
  if ($why) { $line += " ($why)" }
  $line
}

# --- the gk log, read as it grows ----------------------------------------------------------------
$script:log = ''; $script:pos = 0; $script:lines = New-Object Collections.Generic.List[string]; $script:partial = ''
function Read-Log {
  if (-not $script:log -or -not (Test-Path $script:log)) { return }
  $fs = [IO.File]::Open($script:log, 'Open', 'Read', 'ReadWrite,Delete')
  try {
    if ($fs.Length -le $script:pos) { return }
    $fs.Seek($script:pos, 'Begin') | Out-Null
    $buf = New-Object byte[] ($fs.Length - $script:pos)
    $n = $fs.Read($buf, 0, $buf.Length)
    $script:pos += $n
    $text = $script:partial + [Text.Encoding]::UTF8.GetString($buf, 0, $n)
    $parts = $text -split "`r?`n"
    $script:partial = $parts[-1]
    for ($i = 0; $i -lt $parts.Count - 1; $i++) { $script:lines.Add($parts[$i]) }
  } finally { $fs.Close() }
}
# first line index >= $from matching $re, or -1
function Find-Line([string]$re, [int]$from) {
  for ($i = $from; $i -lt $script:lines.Count; $i++) { if ($script:lines[$i] -match $re) { return $i } }
  return -1
}
function Wait-Line([string]$re, [int]$from, [double]$seconds) {
  $end = (Get-Date).AddSeconds($seconds)
  while ($true) {
    Read-Log
    $i = Find-Line $re $from
    if ($i -ge 0) { return $i }
    if ((Get-Date) -gt $end -or $script:gk.HasExited) { return -1 }
    Start-Sleep -Milliseconds 100
  }
}

# --- launch --------------------------------------------------------------------------------------
$la = @{ Instance = $Instance; Repo = $Repo; Minimized = $true; Env = $envAll }
if ($Start) { $la.Start = $Start }
if ($isHeadless) { $la.Headless = $true }
if (-not $needGoalc) { $la.NoGoalc = $true }
$out = @(Run-Script 'dev_launch.ps1' $la)
$launch = $out | Where-Object { $_ -match '^LAUNCH ' } | Select-Object -First 1
if (-not $launch) { throw "dev_launch gave no LAUNCH line: $($out -join ' | ')" }
$gkPid = [int][regex]::Match($launch, ' pid=(\d+)').Groups[1].Value
$script:log = [regex]::Match($launch, ' log=(.+)$').Groups[1].Value
if ($script:log -eq '-') { $script:log = '' }
$script:gk = Get-Process -Id $gkPid -ErrorAction SilentlyContinue
if (-not $script:gk) { throw "gk $gkPid isn't running: $launch" }
'TEST instance={0} pid={1} headless={2} start={3} log={4}' -f $Instance, $gkPid, $isHeadless, $Start, $script:log
$shots = 0

try {
  # the log appears a moment after launch
  $end = (Get-Date).AddSeconds(20)
  while (-not $script:log -and (Get-Date) -lt $end) {
    $pat = "jak3_i$Instance.*.log"
    $script:log = Get-ChildItem (Join-Path $main 'log') -Filter $pat -ErrorAction SilentlyContinue |
      Where-Object { $_.CreationTime -ge $t0.AddSeconds(-1) } | Sort-Object CreationTime |
      Select-Object -Last 1 -ExpandProperty FullName
    Start-Sleep -Milliseconds 200
  }
  $ri = Wait-Line $Ready 0 $Timeout
  Report ($ri -ge 0) 'ready' $(if ($ri -lt 0) { if ($script:gk.HasExited) { 'gk exited' } else { "no /$Ready/ in $Timeout s" } })
  if ($ri -lt 0) { return }

  if ($needGoalc) {
    $g = Run-Script 'dev_goalc.ps1' @{ Instance = $Instance; Repo = $Repo; Wait = $true; Timeout = 300 }
    $lt = Run-Script 'dev_goalc.ps1' @{ Instance = $Instance; Repo = $Repo; Eval = '(lt)'; Timeout = 30 }
    $ok = [bool]($lt -match 'Got version|already connected')
    Report $ok 'goalc' $(if (-not $ok) { "$g / $(@($lt)[-1])" })
  }

  $vi = 0
  foreach ($v in $variants) {
    $vi++
    $name = if ($v.name) { $v.name } else { "variant$vi" }
    if ($v.wait) { Start-Sleep -Milliseconds ([int]([double]$v.wait * 1000)) }   # before the variant acts
    Read-Log
    $from = $script:lines.Count
    if ($v.tune) {
      if ($isHeadless) { Report $false "$name tune" 'headless: no renderer reads tuning' }
      else {
        if ($null -eq $tuneBackup) {
          $tuneBackup = if (Test-Path $tuneFile) { [IO.File]::ReadAllText($tuneFile) } else { '' }
        }
        [IO.File]::WriteAllText($tuneFile, ($v.tune | ConvertTo-Json -Depth 8))
      }
    }
    if ($v.eval) {
      $r = @(Run-Script 'dev_goalc.ps1' @{ Instance = $Instance; Repo = $Repo; Eval = "$($v.eval)"; Timeout = 60 })
      $text = ($r | Select-Object -SkipLast 1) -join "`n"
      $ok = $r[-1] -match '^EVAL OK'
      if ($v.answer) {
        $ok = $ok -and ($text -match $v.answer)
        Report $ok "$name answer" $(if (-not $ok) { "got: $(($text -replace '\s+', ' ').Trim() | ForEach-Object { $_.Substring(0, [Math]::Min(160, $_.Length)) })" })
      } elseif (-not $ok) {
        Report $false "$name eval" ($text -replace '\s+', ' ')
      }
    }
    if ($v.settle) { Start-Sleep -Milliseconds ([int]([double]$v.settle * 1000)) }   # after it acted
    $vt = if ($v.timeout) { [double]$v.timeout } else { 15 }
    foreach ($re in @($v.expect)) {
      if (-not $re) { continue }
      $i = Wait-Line $re $from $vt
      Report ($i -ge 0) "$name /$re/" $(if ($i -lt 0) { "not in $vt s" })
    }
    if ($v.shot) {
      if ($isHeadless -or $shots -ge 4) { "SKIP $name shot ($(if ($isHeadless) { 'headless' } else { 'max 4' }))" }
      else {
        New-Item -ItemType Directory -Force $OutDir | Out-Null
        $png = Join-Path $OutDir ('{0:D2}-{1}.png' -f $vi, ($name -replace '[^\w-]', '_'))
        $null = Run-Script 'capture_window.ps1' @{ Out = $png; ProcessId = $gkPid }
        $shots++
        "SHOT $png"
      }
    }
  }

  # the run's expectations: anywhere from launch on, waiting what is left of the timeout
  foreach ($re in $Expect) {
    $i = Wait-Line $re 0 ([Math]::Max(1, $Timeout - ((Get-Date) - $t0).TotalSeconds))
    Report ($i -ge 0) "/$re/" $(if ($i -lt 0) { 'not seen' })
  }
  Read-Log
  foreach ($re in $Forbid) {
    $i = Find-Line $re 0
    Report ($i -lt 0) "no /$re/" $(if ($i -ge 0) { $script:lines[$i].Substring(0, [Math]::Min(160, $script:lines[$i].Length)) })
  }
  Report (-not $script:gk.HasExited) 'alive' $(if ($script:gk.HasExited) { 'exit 0x{0:X8}' -f $script:gk.ExitCode })
} finally {
  if ($null -ne $tuneBackup) {
    if ($tuneBackup) { [IO.File]::WriteAllText($tuneFile, $tuneBackup) } else { Remove-Item $tuneFile -ErrorAction SilentlyContinue }
  }
  if (-not $Keep -and -not $script:gk.HasExited) { Stop-Process -Id $gkPid -ErrorAction SilentlyContinue }
  if ($StopGoalc -and $needGoalc) { $null = Run-Script 'dev_goalc.ps1' @{ Instance = $Instance; Repo = $Repo; Stop = $true } }
  [string]::Format($inv, 'RESULT pass={0} fail={1} t={2:0.0} log={3}{4}', $script:pass, $script:fail,
    ((Get-Date) - $t0).TotalSeconds, $script:log, $(if ($Keep -and -not $script:gk.HasExited) { " kept=$gkPid" } else { '' }))
  Set-Exit $(if ($script:fail -eq 0 -and $script:pass -gt 0) { 0 } else { 1 })
}
