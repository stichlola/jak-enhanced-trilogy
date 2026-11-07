# Jak Enhanced: collect gk crashes (Jak 3 for now). Read-only: it never starts or stops a process.
# Sources: Windows Application events 1000 (faulting module + offset, PID, process start, exe path)
# and 1001 (WER report folder), the gk logs <repo>\log\jak3*.log (matched to the crash by process
# start time = the log's name), and minidumps (%LOCALAPPDATA%\CrashDumps\gk.exe.<pid>.dmp).
#   crash_collect.ps1                    crashes of the last 24 h, printed
#   crash_collect.ps1 -Hours 2 -Out d    also d\crashes.md and d\crashes-<stamp>.zip (logs, dumps)
#   crash_collect.ps1 -Watch [-ProcessId n]    wait for the next gk crash (of that PID), then collect it
# Run as: & ([scriptblock]::Create([IO.File]::ReadAllText('<path>\crash_collect.ps1'))) -Hours 2
param(
  [double]$Hours = 24,
  [datetime]$Since,
  [string]$Repo = '',        # tree whose log\ to read (default: this script's repo and its main tree)
  [string]$Game = 'jak3',
  [string]$Out = '',
  [switch]$Watch,
  [int]$ProcessId = 0,             # -Watch: only this gk process; stop when it exits
  [int]$Tail = 40,
  [switch]$NoDumps           # -Out: leave the minidumps out of the zip
)
$ErrorActionPreference = 'Stop'
if (-not $PSBoundParameters.ContainsKey('Since')) { $Since = (Get-Date).AddHours(-$Hours) }

# ---- log folders: -Repo, the script's tree, the main tree of its worktree, the crashed exe's tree
$logDirs = New-Object System.Collections.Generic.List[string]
function Add-LogDir([string]$root) {
  if (-not $root -or -not (Test-Path $root)) { return }
  # jak-project is often a junction to the real tree: follow it so the logs aren't listed twice
  $it = Get-Item $root
  if ($it.LinkType -eq 'Junction' -and $it.Target) { $root = @($it.Target)[0] }
  $d = Join-Path $root 'log'
  if (-not (Test-Path $d)) { return }
  $d = (Resolve-Path $d).Path.TrimEnd('\')
  if (-not ($logDirs | Where-Object { $_ -eq $d })) { $logDirs.Add($d) }
}
function Add-Tree([string]$dir) {
  if (-not $dir -or -not (Test-Path $dir)) { return }
  Add-LogDir $dir
  try {
    $common = (& git -C $dir rev-parse --path-format=absolute --git-common-dir 2>$null)
    if ($LASTEXITCODE -eq 0 -and $common) { Add-LogDir (Split-Path $common -Parent) }
  } catch {}
}
if ($Repo) { Add-Tree $Repo }
elseif ($PSScriptRoot) { Add-Tree (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) }
else { Add-Tree (Get-Location).Path }
function Add-ExeTree([string]$exe) {
  # gk.exe in <root>\out\build\Release\bin: walk up until a folder has log\
  $d = Split-Path $exe -Parent
  for ($i = 0; $i -lt 6 -and $d; $i++) {
    if (Test-Path (Join-Path $d 'log')) { Add-LogDir $d; return }
    $d = Split-Path $d -Parent
  }
}

$dumpDir = Join-Path $env:LOCALAPPDATA 'CrashDumps'

function Read-Shared([string]$path) {
  # the log may still be open in gk: share read/write
  $fs = [IO.File]::Open($path, 'Open', 'Read', 'ReadWrite, Delete')
  try { $sr = New-Object IO.StreamReader($fs); return $sr.ReadToEnd() -split "`r?`n" } finally { $fs.Close() }
}

function Get-LogFiles {
  $re = '^' + [regex]::Escape($Game) + '(_i(\d+))?\.(\d{4}-\d\d-\d\dT\d\d-\d\d-\d\d)\.log$'
  foreach ($d in $logDirs) {
    foreach ($f in Get-ChildItem $d -Filter ($Game + '*.log')) {
      $m = [regex]::Match($f.Name, $re)
      if (-not $m.Success) { continue }
      [pscustomobject]@{
        File = $f; Instance = $(if ($m.Groups[2].Success) { [int]$m.Groups[2].Value } else { 0 })
        Start = [datetime]::ParseExact($m.Groups[3].Value, 'yyyy-MM-ddTHH-mm-ss', $null)
      }
    }
  }
}

function Get-CrashEvents([datetime]$after) {
  $ev = @(Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = 1000, 1001; StartTime = $after } -ErrorAction SilentlyContinue)
  $wer = @($ev | Where-Object { $_.Id -eq 1001 -and (($_.Properties | ForEach-Object { "$($_.Value)" }) -contains 'gk.exe') })
  foreach ($e in ($ev | Where-Object { $_.Id -eq 1000 -and "$($_.Properties[0].Value)" -eq 'gk.exe' } | Sort-Object TimeCreated)) {
    $p = $e.Properties
    $offset = "$($p[7].Value)"
    $report = $wer | Where-Object { $_.TimeCreated -ge $e.TimeCreated.AddSeconds(-2) -and $_.TimeCreated -le $e.TimeCreated.AddSeconds(60) -and
      (($_.Properties | ForEach-Object { "$($_.Value)" }) -contains $offset) } | Sort-Object TimeCreated -Descending | Select-Object -First 1
    $reportDir = ''
    if ($report) { $reportDir = ($report.Properties | ForEach-Object { "$($_.Value)" } | Where-Object { $_ -match 'Report(Archive|Queue)' } | Select-Object -First 1) -replace '^\\\\\?\\', '' }
    $start = $null
    try { $start = [datetime]::FromFileTime([int64]$p[9].Value) } catch {}
    [pscustomobject]@{
      Time = $e.TimeCreated; ProcId = [int]$p[8].Value; Start = $start; Exe = "$($p[10].Value)"
      Module = "$($p[3].Value)"; Offset = $offset; Code = "$($p[6].Value)"; WerReport = $reportDir
    }
  }
}

function Find-Log($crash, $logs) {
  # the log is named after the process start (same second, a few s of slack); among instances
  # started together, the one last written closest to the crash
  $c = @($logs | Where-Object { $crash.Start -and [math]::Abs(($_.Start - $crash.Start).TotalSeconds) -le 8 -and
      $_.File.LastWriteTime -le $crash.Time.AddSeconds(30) })
  if (-not $c) {
    # no start time: the newest log started before the crash and written near it
    $c = @($logs | Where-Object { $_.Start -le $crash.Time -and [math]::Abs(($_.File.LastWriteTime - $crash.Time).TotalSeconds) -le 20 })
  }
  $c | Sort-Object { [math]::Abs(($_.File.LastWriteTime - $crash.Time).TotalSeconds) } | Select-Object -First 1
}

function Summarize($crash, $log) {
  $lines = @(); if ($log) { $lines = @(Read-Shared $log.File.FullName | Where-Object { $_.Trim() }) }
  $levelRe = 'Displaying level|lwide-activate|level loading|load-level|Elapsed time for level|continue-point|\[enhanced\].*(start|continue|teleport|level|state)|^[^\[]*\btarget-[a-z-]+'
  $assertRe = '(?i)assert|fatal|exception|abort|segfault|access violation|stack overflow|unhandled|\bpanic\b'
  $s = [ordered]@{
    Time = $crash.Time
    Instance = $(if ($log) { $log.Instance } else { '?' })
    Pid = $crash.ProcId
    Fault = $(if ($crash.Module) { '{0}+0x{1} ({2})' -f $crash.Module, ($crash.Offset -replace '^0x', '').TrimStart('0'), $crash.Code } else { '' })
    Exe = $crash.Exe
    Log = $(if ($log) { $log.File.FullName } else { '' })
    Dump = ''
    WerReport = $crash.WerReport
    Level = @($lines | Where-Object { $_ -match $levelRe -and $_ -notmatch "minimap" } | Select-Object -Last 6)
    Enhanced = @($lines | Where-Object { $_ -match '\[enhanced' } | Select-Object -Last 6)
    Asserts = @($lines | Where-Object { $_ -match $assertRe } | Select-Object -Last 6)
    Tail = @($lines | Select-Object -Last $Tail)
  }
  if ($crash.Module -eq 'unknown') { $s.Fault += ' [unknown module = GOAL code; the offset is the absolute address]' }
  if ($crash.ProcId) {
    $d = Join-Path $dumpDir ('gk.exe.{0}.dmp' -f $crash.ProcId)
    if (Test-Path $d) { $s.Dump = $d }
  }
  [pscustomobject]$s
}

function Format-Crash($s, [int]$n) {
  $o = New-Object System.Collections.Generic.List[string]
  $o.Add(('## Crash {0}: {1:yyyy-MM-dd HH:mm:ss}, instance {2}, pid {3}' -f $n, $s.Time, $s.Instance, $s.Pid))
  $o.Add('- fault: ' + $s.Fault)
  foreach ($k in 'Exe', 'Log', 'Dump', 'WerReport') { if ($s.$k) { $o.Add(('- {0}: {1}' -f $k.ToLower(), $s.$k)) } }
  if (-not $s.Log) {
    # gk keeps only the newest ~10 logs per name (jak3_i<n>): older ones are rotated away
    $oldest = Get-LogFiles | Sort-Object Start | Select-Object -First 1
    $why = if ($oldest -and $s.Time -lt $oldest.Start) { 'probably rotated away: gk keeps ~10 per name, collect sooner or use -Watch' } else { 'no log started with this process' }
    $o.Add('- log: not found (' + $why + '; looked in ' + ($logDirs -join ', ') + ')')
  }
  foreach ($k in 'Level', 'Asserts', 'Enhanced') {
    if ($s.$k.Count) { $o.Add(''); $o.Add("$($k.ToLower()):"); $o.Add('```'); $s.$k | ForEach-Object { $o.Add($_) }; $o.Add('```') }
  }
  if ($s.Tail.Count) { $o.Add(''); $o.Add("log tail ($($s.Tail.Count) lines):"); $o.Add('```'); $s.Tail | ForEach-Object { $o.Add($_) }; $o.Add('```') }
  $o.Add('')
  $o
}

function Collect([object[]]$crashes) {
  $logs = @(Get-LogFiles)
  $sums = @()
  foreach ($c in $crashes) { Add-ExeTree $c.Exe }
  if ($crashes.Count) { $logs = @(Get-LogFiles) }
  foreach ($c in $crashes) { $sums += Summarize $c (Find-Log $c $logs) }
  # dumps with no event (WER off or event log cleared)
  $known = @($sums | ForEach-Object { $_.Dump })
  foreach ($d in (Get-ChildItem $dumpDir -Filter 'gk.exe.*.dmp' -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -ge $Since -and $known -notcontains $_.FullName })) {
    $id = 0; [void][int]::TryParse(($d.Name -split '\.')[2], [ref]$id)
    $sums += Summarize ([pscustomobject]@{ Time = $d.LastWriteTime; ProcId = $id; Start = $null; Exe = ''; Module = ''; Offset = ''; Code = ''; WerReport = '' }) (Find-Log ([pscustomobject]@{ Time = $d.LastWriteTime; Start = $null }) $logs)
  }
  $sums = @($sums | Sort-Object Time)
  $text = New-Object System.Collections.Generic.List[string]
  $text.Add(('# gk crashes since {0:yyyy-MM-dd HH:mm}: {1}' -f $Since, $sums.Count))
  $text.Add('')
  $i = 0
  foreach ($s in $sums) { $i++; foreach ($l in (Format-Crash $s $i)) { $text.Add($l) } }
  $text | ForEach-Object { Write-Output $_ }
  if ($Out) {
    New-Item -ItemType Directory -Force $Out | Out-Null
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $md = Join-Path $Out ('crashes-{0}.md' -f $stamp)
    [IO.File]::WriteAllLines($md, [string[]]$text, (New-Object Text.UTF8Encoding($false)))
    $stage = Join-Path $Out ('crashes-{0}' -f $stamp)
    New-Item -ItemType Directory -Force $stage | Out-Null
    Copy-Item $md (Join-Path $stage 'crashes.md')
    foreach ($s in $sums) {
      if ($s.Log -and -not (Test-Path (Join-Path $stage (Split-Path $s.Log -Leaf)))) {
        [IO.File]::WriteAllLines((Join-Path $stage (Split-Path $s.Log -Leaf)), [string[]](Read-Shared $s.Log))
      }
      if ($s.Dump -and -not $NoDumps) { Copy-Item $s.Dump $stage }
    }
    $zip = Join-Path $Out ('crashes-{0}.zip' -f $stamp)
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -Force
    Remove-Item -Recurse -Force $stage
    Write-Output "report: $md"
    Write-Output "zip:    $zip"
  }
}

if ($Watch) {
  $from = Get-Date
  if ($ProcessId) {
    $p = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
    if (-not $p) { Write-Output "no process $ProcessId"; return }
    Write-Output "watching gk pid $ProcessId for a crash (Ctrl+C to stop)"
  } else { Write-Output 'watching for the next gk crash (Ctrl+C to stop)' }
  while ($true) {
    Start-Sleep -Seconds 2
    $hit = @(Get-CrashEvents $from.AddSeconds(-2) | Where-Object { -not $ProcessId -or $_.ProcId -eq $ProcessId })
    if ($hit) {
      Start-Sleep -Seconds 5   # let WER finish the dump and the 1001 event
      $Since = $from
      Collect @(Get-CrashEvents $from.AddSeconds(-2) | Where-Object { -not $ProcessId -or $_.ProcId -eq $ProcessId })
      return
    }
    if ($ProcessId -and -not (Get-Process -Id $ProcessId -ErrorAction SilentlyContinue)) {
      Start-Sleep -Seconds 8   # the crash event can come a few seconds after the exit
      $hit = @(Get-CrashEvents $from.AddSeconds(-2) | Where-Object { $_.ProcId -eq $ProcessId })
      if ($hit) { $Since = $from; Collect $hit; return }
      Write-Output "gk $ProcessId exited without a crash event"; return
    }
  }
}

Collect @(Get-CrashEvents $Since)
