# Run several game instances side by side for automated tests: small windowed games, each with its
# own REPL ports (JAK_ENHANCED_INSTANCE), log file and config dir, then a fixed-format summary.
#
#   & ([scriptblock]::Create([IO.File]::ReadAllText('<repo>\scripts\enhanced\multi_run.ps1'))) `
#       -Count 3 -Game jak3 -Start desert-start -Seconds 60
#
#   -Start      continue points, comma list; instance i takes item i (the last one repeats)
#   -Env        extra env for every instance, @{ JAK_ENHANCED_PLAYER = 'jak,ashelin,sig' }:
#               a comma list gives instance i item i, like -Start
#   -Minimized  minimize each window once it shows (rendering keeps going, nothing to see)
#   -Keep       leave the games running at the end (their PIDs are printed)
#
# gk.exe runs from a shadow copy of the bin folder (local\run\<stamp>-multi\bin, pruned by dev_bin.ps1),
# so a build can relink out\build\Release\bin while the games run; -GkPath overrides it.
# Each instance gets JAK_ENHANCED_INSTANCE=<n> (n = 1..): gk listens on 8112+game-1+10n, a goalc
# started with the same variable takes nREPL 8181+10n, og_repl.py --instance n reaches it.
# Config dirs come from local\testcfg (windowed 640x360, 640x360 internal, Performance preset);
# the user's %APPDATA%\OpenGOAL is never read or written. Only the PIDs started here are stopped.
param(
  [int]$Count = 2,
  [ValidateSet('jak1', 'jak2', 'jak3')][string]$Game = 'jak3',
  [string]$Start = '',
  [int]$Seconds = 60,
  [Alias('Env')][hashtable]$InstanceEnv = @{},
  [int]$FirstInstance = 1,
  [int]$Width = 640,
  [int]$Height = 360,
  [int]$BootTimeout = 180,
  # tested on the RTX 5070 Laptop / Ryzen AI 9 365 / 32 GB laptop, jak3 desert-start: 4 ran at
  # ~57 fps each but left 0.5 GB of RAM free; 3 leaves room for the user's own game and Blender
  [int]$MaxCount = 3,
  [switch]$Force,
  [switch]$Minimized,
  [switch]$Keep,
  [string]$Repo = '',
  [string]$GkPath = '',
  [string]$ProjPath = '',
  [string]$TestCfg = '',
  [string]$RunDir = ''
)
$ErrorActionPreference = 'Stop'

if (-not $Repo) {
  $Repo = if ($PSScriptRoot) { Split-Path (Split-Path $PSScriptRoot) } else { (Get-Location).Path }
}
if (-not (Test-Path (Join-Path $Repo 'scripts\enhanced'))) { throw "-Repo: not a jak-enhanced tree: $Repo" }
# the main worktree holds the built game data (out\jakN) and local\testcfg
$main = Split-Path (& git -C $Repo rev-parse --path-format=absolute --git-common-dir)
# gk runs from a shadow copy of the build's bin (dev_bin.ps1), so build.bat can relink it meanwhile
if (-not $GkPath) {
  $shadow = & ([scriptblock]::Create([IO.File]::ReadAllText((Join-Path $Repo 'scripts\enhanced\dev_bin.ps1')))) `
    -Tag multi -Repo $Repo -From (Join-Path $Repo 'out\build\Release\bin')
  $GkPath = Join-Path $shadow 'gk.exe'
}
if (-not $ProjPath) { $ProjPath = $main }
if (-not $TestCfg) { $TestCfg = Join-Path $main 'local\testcfg' }
if (-not $RunDir) { $RunDir = Join-Path $main ('local\multi\' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
foreach ($p in @($GkPath, $ProjPath, (Join-Path $TestCfg "OpenGOAL\$Game"))) {
  if (-not (Test-Path $p)) { throw "missing: $p" }
}
if ($Count -lt 1) { throw '-Count must be at least 1' }
if ($Count -gt $MaxCount -and -not $Force) {
  throw "-Count $Count is over this machine's tested limit ($MaxCount); pass -Force to try anyway"
}

if (-not ('MultiRunWin' -as [type])) {
  Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class MultiRunWin {
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
}
'@
}
Add-Type -AssemblyName System.Windows.Forms

$gameNum = [int]$Game.Substring(3)
$starts = @($Start -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
function Pick([string[]]$list, [int]$i) {
  if ($list.Count -eq 0) { return '' }
  return $list[[Math]::Min($i, $list.Count - 1)]
}
function FpsLines([string]$log) {
  if (-not $log) { return @() }
  @(Get-Content $log -ErrorAction SilentlyContinue | Select-String '\[enhanced\] average ([\d.]+) fps' |
    ForEach-Object { [double]$_.Matches[0].Groups[1].Value })
}
function PortFree([int]$port) {
  $l = New-Object Net.Sockets.TcpListener ([Net.IPAddress]::Loopback), $port
  try { $l.Start(); $l.Stop(); return $true } catch { return $false }
}

# --- instances: numbers whose ports are free -------------------------------------------------
$before = @(Get-Process gk -ErrorAction SilentlyContinue | ForEach-Object Id)
$area = [System.Windows.Forms.Screen]::PrimaryScreen.WorkingArea
$cols = [Math]::Max(1, [Math]::Floor(($area.Width - 8) / ($Width + 16)))
$list = @()
$n = $FirstInstance
for ($i = 0; $i -lt $Count; $i++) {
  # the gk port must be free; the goalc port too, unless it is this instance's dev goalc
  # (dev_goalc.py, control port 8185+10n), which then drives the game
  while ($n -lt 100 -and -not ((PortFree (8111 + $gameNum + 10 * $n)) -and
      ((PortFree (8181 + 10 * $n)) -or -not (PortFree (8185 + 10 * $n))))) { $n++ }
  if ($n -ge 100) { throw 'no free instance ports' }
  $list += [pscustomobject]@{ Index = $i; N = $n; Port = 8111 + $gameNum + 10 * $n;
    X = $area.Left + 8 + ($i % $cols) * ($Width + 16); Y = $area.Top + 32 + [Math]::Floor($i / $cols) * ($Height + 40)
    Start = (Pick $starts $i); Proc = $null; Cfg = ''; Log = ''; Ready = $null; Hwnd = [IntPtr]::Zero; FpsSkip = 0 }
  $n++
}

# --- one config dir per instance ---------------------------------------------------------
New-Item -ItemType Directory -Force $RunDir | Out-Null
foreach ($it in $list) {
  $cfg = Join-Path $RunDir "cfg$($it.N)"
  $dst = Join-Path $cfg "OpenGOAL\$Game"
  New-Item -ItemType Directory -Force $dst | Out-Null
  Get-ChildItem (Join-Path $TestCfg "OpenGOAL\$Game") | Where-Object { $_.Name -ne 'screenshots' } |
    Copy-Item -Destination $dst -Recurse -Force
  $sdir = Join-Path $dst 'settings'
  New-Item -ItemType Directory -Force $sdir | Out-Null
  $pc = Join-Path $sdir 'pc-settings.gc'
  if (Test-Path $pc) {
    $t = [IO.File]::ReadAllText($pc)
    $t = $t -replace '\(window-size [^)]*\)', "(window-size $Width $Height)"
    $t = $t -replace '\(msaa [^)]*\)', '(msaa 1)'
    $t = $t -replace '\(vsync [^)]*\)', '(vsync #f)'
    $t = $t -replace '\(discord-rpc\? [^)]*\)', '(discord-rpc? #f)'
    $t = $t -replace '\(ignore-controller-win-unfocused\? [^)]*\)', '(ignore-controller-win-unfocused? #t)'
    [IO.File]::WriteAllText($pc, $t)
  }
  # display_mode 0 = windowed (1 fullscreen, 2 borderless)
  $disp = [ordered]@{ display_id = 0; display_mode = 0; version = '1.2'; window_xpos = $it.X; window_ypos = $it.Y }
  [IO.File]::WriteAllText((Join-Path $sdir 'display-settings.json'), ($disp | ConvertTo-Json))
  $it.Cfg = $cfg
}

# --- start ---------------------------------------------------------------------------------
$fg = [MultiRunWin]::GetForegroundWindow()
$t0 = Get-Date
foreach ($it in $list) {
  $psi = New-Object Diagnostics.ProcessStartInfo $GkPath
  $psi.Arguments = "--game $Game --proj-path `"$ProjPath`" --config-path `"$($it.Cfg)`" -- -boot -fakeiso -debug"
  $psi.WorkingDirectory = $ProjPath
  $psi.UseShellExecute = $false
  $psi.CreateNoWindow = $true   # no console window; the game window still opens
  $e = $psi.EnvironmentVariables
  $e['JAK_ENHANCED_INSTANCE'] = "$($it.N)"
  $e['JAK_ENHANCED_START_MENU'] = '0'
  $e['JAK_ENHANCED_PRESET'] = '3'
  $e['JAK_ENHANCED_SHADER_RELOAD'] = '1'   # enh_* shaders reload when saved
  if ($it.Start) { $e['JAK_ENHANCED_START'] = $it.Start }
  foreach ($k in $InstanceEnv.Keys) { $e[$k] = (Pick @("$($InstanceEnv[$k])" -split ',') $it.Index) }
  $it.Proc = [Diagnostics.Process]::Start($psi)
  $null = $it.Proc.Handle   # keep the handle so ExitCode stays readable
  Start-Sleep -Milliseconds 700
}

# window placement and focus: put each window in its tile without activating it, give the
# focus back to whatever had it
$SWP = 0x0001 -bor 0x0004 -bor 0x0010   # NOSIZE | NOZORDER | NOACTIVATE
$deadline = $t0.AddSeconds($BootTimeout)
while ((Get-Date) -lt $deadline) {
  $pending = 0
  foreach ($it in $list) {
    if ($it.Proc.HasExited) { continue }
    if ($it.Hwnd -eq [IntPtr]::Zero) {
      $it.Proc.Refresh()
      if ($it.Proc.MainWindowHandle -ne [IntPtr]::Zero) {
        $it.Hwnd = $it.Proc.MainWindowHandle
        [MultiRunWin]::SetWindowPos($it.Hwnd, [IntPtr]::Zero, $it.X, $it.Y, 0, 0, $SWP) | Out-Null
        if ($Minimized) { [MultiRunWin]::ShowWindow($it.Hwnd, 7) | Out-Null }   # SW_SHOWMINNOACTIVE
        if ($fg -ne [IntPtr]::Zero) { [MultiRunWin]::SetForegroundWindow($fg) | Out-Null }
      }
    }
    if (-not $it.Log) {
      $it.Log = Get-ChildItem (Join-Path $ProjPath 'log') -Filter "$($Game)_i$($it.N).*.log" -ErrorAction SilentlyContinue |
        Where-Object { $_.LastWriteTime -ge $t0.AddSeconds(-2) } | Sort-Object LastWriteTime | Select-Object -Last 1 -ExpandProperty FullName
    }
    if (-not $it.Ready -and $it.Log -and
        (Select-String -Path $it.Log -Pattern '\[enhanced\] average' -Quiet -ErrorAction SilentlyContinue)) {
      $it.Ready = Get-Date
    }
    if (-not $it.Ready -or $it.Hwnd -eq [IntPtr]::Zero) { $pending++ }
  }
  if ($pending -eq 0) { break }
  Start-Sleep 2
}

# --- run ----------------------------------------------------------------------------------
foreach ($it in $list) { $it.FpsSkip = @(FpsLines $it.Log).Count + 1 }   # +1: the line straddling the start
$freeMin = [double]::MaxValue; $commitMax = 0; $commitLimit = 0
$runEnd = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $runEnd) {
  $mem = Get-CimInstance Win32_PerfFormattedData_PerfOS_Memory
  $freeMin = [Math]::Min($freeMin, $mem.AvailableMBytes / 1KB)
  $commitMax = [Math]::Max($commitMax, $mem.CommittedBytes / 1GB); $commitLimit = $mem.CommitLimit / 1GB
  if (@($list | Where-Object { -not $_.Proc.HasExited }).Count -eq 0) { break }
  Start-Sleep 5
}
$os = Get-CimInstance Win32_OperatingSystem
$totalGB = $os.TotalVisibleMemorySize / 1MB

# --- results ------------------------------------------------------------------------------
$rows = foreach ($it in $list) {
  $p = $it.Proc
  $alive = -not $p.HasExited
  $ramGB = 0
  $win = '-'
  if ($alive) {
    $p.Refresh(); $ramGB = $p.PeakWorkingSet64 / 1GB
    $rc = New-Object MultiRunWin+RECT
    if ($it.Hwnd -ne [IntPtr]::Zero -and [MultiRunWin]::GetClientRect($it.Hwnd, [ref]$rc)) { $win = "$($rc.R - $rc.L)x$($rc.B - $rc.T)" }
  }
  # fps: only the lines logged during the run phase (all instances booted)
  $fps = @(FpsLines $it.Log | Select-Object -Skip $it.FpsSkip)
  $state = if ($alive) { if ($it.Ready) { 'alive' } else { 'no-frames' } } else { 'exited' }
  $exit = ''; $event = ''; $tail = @()
  if (-not $alive) {
    $exit = '0x{0:X8}' -f $p.ExitCode
    if ($p.ExitCode -ne 0) { $state = 'crashed' }
    $ev = Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = 1000; StartTime = $t0 } -ErrorAction SilentlyContinue |
      Where-Object { $_.Properties.Count -gt 8 -and $_.Properties[0].Value -eq 'gk.exe' -and
                     [Convert]::ToInt32(($_.Properties[8].Value -replace '^0x', ''), 16) -eq $p.Id } | Select-Object -First 1
    if ($ev) { $state = 'crashed'; $event = "module=$($ev.Properties[3].Value) code=$($ev.Properties[6].Value) offset=$($ev.Properties[7].Value)" }
    if ($it.Log) { $tail = @(Get-Content $it.Log -Tail 8 -ErrorAction SilentlyContinue) }
  }
  [pscustomobject]@{ It = $it; State = $state; Fps = $fps; RamGB = $ramGB; Win = $win; Exit = $exit; Event = $event; Tail = $tail }
}

if (-not $Keep) {
  foreach ($it in $list) {
    if (-not $it.Proc.HasExited -and $before -notcontains $it.Proc.Id) {
      Stop-Process -Id $it.Proc.Id -Force -ErrorAction SilentlyContinue
      $null = $it.Proc.WaitForExit(10000)
    }
  }
}

'MULTI game={0} count={1} seconds={2} ram_total={3:N1}GB ram_avail_min={4:N1}GB commit_max={6:N1}/{7:N1}GB rundir={5}' -f `
  $Game, $Count, $Seconds, $totalGB, $(if ($freeMin -eq [double]::MaxValue) { 0 } else { $freeMin }), $RunDir,
  $commitMax, $commitLimit
foreach ($r in $rows) {
  $it = $r.It
  $f = if ($r.Fps.Count) { 'fps={0:N1} min={1:N1} n={2}' -f ($r.Fps | Measure-Object -Average).Average, ($r.Fps | Measure-Object -Minimum).Minimum, $r.Fps.Count } else { 'fps=-' }
  '#{0} pid={1} port={2} start={3} state={4} {5} ram_peak={6:N2}GB boot={7}s win={10}{8}{9}' -f `
    $it.N, $it.Proc.Id, $it.Port, $(if ($it.Start) { $it.Start } else { '-' }), $r.State, $f, $r.RamGB,
    $(if ($it.Ready) { [int]($it.Ready - $t0).TotalSeconds } else { '-' }),
    $(if ($r.Exit) { " exit=$($r.Exit)" } else { '' }), $(if ($r.Event) { " event1000: $($r.Event)" } else { '' }), $r.Win
  '   log={0}' -f $(if ($it.Log) { $it.Log } else { '-' })
  foreach ($l in $r.Tail) { '   | ' + $l }
}
'RESULT alive={0} crashed={1} other={2} kept={3}' -f @($rows | Where-Object State -eq 'alive').Count,
  @($rows | Where-Object State -eq 'crashed').Count, @($rows | Where-Object { $_.State -notin 'alive', 'crashed' }).Count,
  $(if ($Keep) { ($list | ForEach-Object { $_.Proc.Id }) -join ',' } else { 'none' })
