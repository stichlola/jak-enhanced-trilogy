# Release gate: boots the game in a list of test scenarios (small windowed test instances, as
# multi_run.ps1 starts them) and checks each one from its log, its process and its screenshots.
#
#   & ([scriptblock]::Create([IO.File]::ReadAllText('<repo>\scripts\enhanced\release_gate.ps1'))) -Repo <repo>
#   ... -Only boot-desert,weather-desert      only these rows (comma list or array; * wildcards)
#   ... -List                                 print the rows and exit
#   ... -Instances 2                          rows run side by side (default 1; RAM: ~2.8 GB each)
#
#   -Games      games to run (default jak3; only jak3 has rows for now)
#   -FirstInstance  first JAK_ENHANCED_INSTANCE number tried (default 6: 1-5 are the dev loop's and
#               other test scripts'); a number whose ports are taken is skipped
#   -Repo       the jak-enhanced tree whose build is tested (out\build\Release\bin, out\<game>)
#   -ProjPath   the data tree gk runs on (default -Repo when it has out\<game>\iso, else the main tree)
#
# Every row: gk alive at the end (no exit code), no Application event 1000 for its PID, enough
# "[enhanced] average N fps ... in <level>" lines after the boot (N >= the row's floor, level not
# none/title), screenshots not black (mean luma > 8), plus the row's own log lines (in order) and
# checks. Output: one "GATE <row> PASS|FAIL <reason>" line per row, then "GATE RESULT pass=.. fail=..",
# and report.txt + report.html with downscaled screenshots in local\gate\<time>\. Exit code 1 on any FAIL.
# Only the processes started here are stopped; the user's %APPDATA%\OpenGOAL is never read or written.
param(
  [string[]]$Games = @('jak3'),
  [string[]]$Only = @(),
  [int]$Instances = 1,
  [int]$FirstInstance = 6,
  [string]$Repo = '',
  [string]$ProjPath = '',
  [string]$GkPath = '',
  [string]$TestCfg = '',
  [string]$OutDir = '',
  [int]$BootTimeout = 240,
  [int]$Width = 640,
  [int]$Height = 360,
  [switch]$List
)
$ErrorActionPreference = 'Stop'

if (-not $Repo) {
  $Repo = if ($PSScriptRoot) { Split-Path (Split-Path $PSScriptRoot) } else { (Get-Location).Path }
}
if (-not (Test-Path (Join-Path $Repo 'scripts\enhanced'))) { throw "-Repo: not a jak-enhanced tree: $Repo" }
$main = Split-Path (& git -C $Repo rev-parse --path-format=absolute --git-common-dir)
if ($Games.Count -eq 1 -and $Games[0] -match ',') { $Games = $Games[0] -split ',' }
if ($Only.Count -eq 1 -and $Only[0] -match ',') { $Only = $Only[0] -split ',' }

# --- the scenarios ---------------------------------------------------------------------------------
# Name, Game, Start (continue point), Seconds (after the first fps line), Env, Shots (seconds after the
# first fps line), Expect (log lines, each after the one before: Re, Label, Min), Levels (regexes the
# fps lines' levels must match in this order: first, then later), MinFps, Bright (min share of
# bright pixels in the last shot), Save (the saves dir must change), CfgFrom (a row whose config
# dir this one starts from), Lines (min fps lines, default Seconds/5 - 4)
function New-Row([hashtable]$h) {
  $d = @{ Game = 'jak3'; Start = 'desert-start'; Seconds = 45; Env = @{}; Shots = @(); Expect = @(); Levels = @();
    MinFps = 20; Bright = 0.0; Save = $false; CfgFrom = ''; Lines = -1; Note = '' }
  foreach ($k in $h.Keys) { $d[$k] = $h[$k] }
  if ($d.Lines -lt 0) { $d.Lines = [Math]::Max(2, [int][Math]::Floor($d.Seconds / 5) - 4) }
  if ($d.Shots.Count -eq 0) { $d.Shots = @([Math]::Max(5, $d.Seconds - 5)) }
  [pscustomobject]$d
}
function E([string]$re, [string]$label = '', [int]$min = 1) {
  @{ Re = $re; Label = $(if ($label) { $label } else { $re }); Min = $min }
}
$rows = @(
  # boot to a continue point and stay there
  New-Row @{ Name = 'boot-desert'; Start = 'desert-start'; Seconds = 60; Shots = @(55); Lines = 6; Levels = @('^des') }
  New-Row @{ Name = 'boot-spargus'; Start = 'wascitya-start'; Seconds = 60; Shots = @(55); Lines = 6; Levels = @('^was') }
  # the menu's hour picks right at boot (kind 1 queued at renderer init, polled in the first frames)
  New-Row @{ Name = 'hour-23-at-boot'; Seconds = 40; Env = @{ JAK_ENHANCED_MENU = '1:23' }; Shots = @(35)
    Expect = @((E '\[enhanced\] hour request 23' 'hour request 23'), (E '\[enhanced\] hour reached 2[23]\.' 'hour reached ~23')) }
  New-Row @{ Name = 'hour-6-at-boot'; Seconds = 40; Env = @{ JAK_ENHANCED_MENU = '1:6' }; Shots = @(35)
    Expect = @((E '\[enhanced\] hour request 6 ' 'hour request 6'), (E '\[enhanced\] hour reached [56]\.' 'hour reached ~6')) }
  New-Row @{ Name = 'hour-burst'; Seconds = 40; Env = @{ JAK_ENHANCED_MENU_SCRIPT = '0.5:1:23,1:1:6,2:1:12,6:1:18' }; Shots = @(35)
    Expect = @((E '\[enhanced\] menu script .* request 1 ' 'script picks' 4), (E '\[enhanced\] hour request 18' 'last pick taken'),
      (E '\[enhanced\] hour reached 1[78]\.' 'hour reached ~18')) }
  # a nap: asked 8 s into the first level, Jak lies down, the hours go by, he gets up
  New-Row @{ Name = 'nap-6h'; Seconds = 45; Env = @{ JAK_ENHANCED_NAP = '6' }; Shots = @(15, 40)
    Expect = @((E '\[enhanced\] nap request 6 h' 'nap request'), (E '\[enhanced\] nap start 6 h' 'nap start'), (E '\[enhanced\] nap end' 'nap end')) }
  # (the open window freezes the game: no fps lines after it)
  New-Row @{ Name = 'nap-window'; Seconds = 30; Env = @{ JAK_ENHANCED_NAP = 'window' }; Shots = @(20); Lines = 0
    Expect = @((E '\[enhanced\] nap window open' 'nap window open')) }
  # the parked Hellcat: in, climb, fire, land, take off, out in flight
  New-Row @{ Name = 'hellcat-fly'; Seconds = 70; Env = @{ JAK_ENHANCED_HELLCAT_TEST = '1' }; Shots = @(22, 65)
    Expect = @((E '\[hellcat\] parked:' 'hellcat parked'), (E '\[hellcat-test\] by' 'by the hellcat'),
      (E '\[hellcat-test\] "?(launch|climbing)"? #t' 'piloting'), (E '\[hellcat-test\] "?firing"? #t' 'firing'),
      (E '\[hellcat-test\] out #t' 'out in flight'), (E '\[hellcat-test\] end' 'test end')) }
  # level changes: desert -> Spargus on foot and in the Hellcat, Spargus -> desert by the menu
  New-Row @{ Name = 'desert-to-spargus-foot'; Seconds = 90; Env = @{ JAK_ENHANCED_HELLCAT_TEST = '3' }; Shots = @(85)
    Levels = @('^des', '^was'); Expect = @((E '\[spargus-test\] gate' 'gate swap'), (E '\[spargus-test\] t .*wascitya active' 'wascitya active')) }
  New-Row @{ Name = 'desert-to-spargus-hellcat'; Seconds = 90; Env = @{ JAK_ENHANCED_HELLCAT_TEST = '2' }; Shots = @(85)
    Levels = @('^des', '^was'); Expect = @((E '\[spargus-test\] gate' 'gate swap'), (E '\[spargus-test\] t .*wascitya active' 'wascitya active')) }
  # (the request 25 s after the first frame: one queued at boot is taken before the first fps line)
  New-Row @{ Name = 'spargus-to-desert'; Start = 'wascitya-start'; Seconds = 60; Env = @{ JAK_ENHANCED_MENU_SCRIPT = '25:2:desert-start' }; Shots = @(55)
    Levels = @('^was', '^des'); Expect = @((E '\[enhanced\] continue request "?desert-start"?: "?sent' 'continue sent')) }
  # the air train: its cutscene (which ends in Haven), and the train itself (triangle at it; the driver
  # closes city-destroy-grid-resolution first, which opens the train to Haven)
  New-Row @{ Name = 'air-train-scene'; Start = 'desert-warp'; Seconds = 90; Env = @{ JAK_ENHANCED_SCENE = 'desert-air-train-in' }; Shots = @(10, 85)
    Levels = @('^des', '^cty'); Expect = @((E '\[enhanced\] scene "?desert-air-train-in"? start' 'scene start'), (E '\[enhanced\] scene "?desert-air-train-in"? end' 'scene end')) }
  New-Row @{ Name = 'air-train-ride'; Start = 'desert-warp'; Seconds = 90; Env = @{ JAK_ENHANCED_AIR_TRAIN = '1' }; Shots = @(10, 85)
    Levels = @('^des', '^cty'); Expect = @((E '\[air-train\] found' 'train found'), (E '\[air-train\] gate "?air-train' 'train state'),
      (E '\[air-train\] use train .* to "?ctyport-warp' 'train used')) }
  # the arena's first training: the orbs (arena-token) up and drawn
  New-Row @{ Name = 'arena-training'; Start = 'game-start'; Seconds = 45; Env = @{ JAK_ENHANCED_CENSUS = '1' }; Shots = @(40)
    Levels = @('^wasstad'); Bright = 0.002; Expect = @((E '\[census\] .* tokens [1-9]' 'orbs alive')) }
  # the playable characters: run + jumps, then a gun (Jak: his own; Ashelin, Sig: their test scripts)
  New-Row @{ Name = 'player-jak-run'; Seconds = 35; Env = @{ JAK_ENHANCED_PLAYER = 'jak'; JAK_ENHANCED_PLAYER_TEST = 'run' }; Shots = @(14)
    Expect = @((E '\[player-test\] run$' 'run'), (E '\[player-test\] run-jump' 'run-jump'), (E '\[player-test\] jump' 'jump'), (E '\[player-test\] double-jump' 'double-jump')) }
  New-Row @{ Name = 'player-jak-gun'; Seconds = 35; Env = @{ JAK_ENHANCED_PLAYER = 'jak'; JAK_ENHANCED_PLAYER_TEST = 'gun'; JAK_ENHANCED_UNLOCK = '1' }; Shots = @(14)
    Expect = @((E '\[player-test\] \d+ gun-type \d+ using #t' 'gun out')) }
  New-Row @{ Name = 'player-ashelin-run'; Seconds = 35; Env = @{ JAK_ENHANCED_PLAYER = 'ashelin'; JAK_ENHANCED_PLAYER_TEST = 'run' }; Shots = @(14)
    Expect = @((E '\[enhanced-player\] "?ashelin"? #<' 'ashelin loaded'), (E '\[player-test\] run$' 'run'), (E '\[player-test\] run-jump' 'run-jump'), (E '\[player-test\] double-jump' 'double-jump')) }
  New-Row @{ Name = 'player-ashelin-gun'; Seconds = 45; Env = @{ JAK_ENHANCED_PLAYER = 'ashelin'; JAK_ENHANCED_ASHELIN_GUN_TEST = '1' }; Shots = @(18)
    Expect = @((E '\[enhanced-player\] "?ashelin"? #<' 'ashelin loaded'), (E '\[ashelin-gun\] test: fire standing' 'fire'), (E '\[ashelin-gun\] test: done' 'test done')) }
  New-Row @{ Name = 'player-sig-run'; Seconds = 35; Env = @{ JAK_ENHANCED_PLAYER = 'sig'; JAK_ENHANCED_PLAYER_TEST = 'run' }; Shots = @(14)
    Expect = @((E '\[enhanced-player\] "?sig"? #<' 'sig loaded'), (E '\[player-test\] run$' 'run'), (E '\[player-test\] run-jump' 'run-jump'), (E '\[player-test\] double-jump' 'double-jump')) }
  New-Row @{ Name = 'player-sig-gun'; Seconds = 45; Env = @{ JAK_ENHANCED_PLAYER = 'sig'; JAK_ENHANCED_SIG_GUN_TEST = '1' }; Shots = @(25)
    Expect = @((E '\[enhanced-player\] "?sig"? #<' 'sig loaded'), (E '\[sig-gun\] test: d-pad right' 'gun out'), (E '\[sig-gun\] shot fire' 'fire')) }
  # the weather script: clear, changing, thunderstorm, after the rain, clear
  New-Row @{ Name = 'weather-desert'; Seconds = 65; Env = @{ JAK_ENHANCED_WEATHER_SCRIPT = '0:0,10:1,20:4,35:5,50:0' }; Shots = @(25, 55); Lines = 9
    Expect = @((E '\[enhanced\] weather mode 1 ' 'mode 1'), (E '\[enhanced\] weather mode 4 ' 'mode 4'), (E '\[enhanced\] weather mode 5 ' 'mode 5'), (E '\[enhanced\] weather mode 0 ' 'back to 0')) }
  New-Row @{ Name = 'weather-spargus'; Start = 'wascitya-start'; Seconds = 65; Env = @{ JAK_ENHANCED_WEATHER_SCRIPT = '0:0,10:1,20:4,35:5,50:0' }; Shots = @(25, 55); Lines = 9
    Expect = @((E '\[enhanced\] weather mode 1 ' 'mode 1'), (E '\[enhanced\] weather mode 4 ' 'mode 4'), (E '\[enhanced\] weather mode 5 ' 'mode 5'), (E '\[enhanced\] weather mode 0 ' 'back to 0')) }
  # the Enhanced menu open from the start
  New-Row @{ Name = 'menu-open'; Seconds = 30; Env = @{ JAK_ENHANCED_MENU = '1' }; Shots = @(25); Expect = @((E '\[enhanced\] menu open' 'menu open')) }
  # saving to slot 2 in the instance's own config dir, then a boot elsewhere that loads it
  New-Row @{ Name = 'save-write'; Seconds = 30; Env = @{ JAK_ENHANCED_SAVE_TEST = '2' }; Save = $true
    Expect = @((E '\[enhanced\] save test: saving slot 2' 'save asked'), (E '\[enhanced\] auto-save save done .* slot 2' 'save done')) }
  New-Row @{ Name = 'save-load'; Start = 'wascitya-start'; Seconds = 45; Env = @{ JAK_ENHANCED_LOAD_TEST = '2' }; CfgFrom = 'save-write'
    Levels = @('^was', '^des'); Expect = @((E '\[enhanced\] load test: loading save 2' 'load asked'), (E '\[enhanced\] auto-save restore done .* slot 2' 'load done')) }
)
$rows = @($rows | Where-Object { $Games -contains $_.Game })
if ($Only.Count) {
  $rows = @($rows | Where-Object { $n = $_.Name; @($Only | Where-Object { $n -like $_.Trim() }).Count -gt 0 })
  # a row whose config dir comes from another one brings that one along
  foreach ($r in @($rows)) {
    if ($r.CfgFrom -and -not ($rows | Where-Object Name -eq $r.CfgFrom)) {
      throw "row $($r.Name) needs row $($r.CfgFrom) (its config dir): add it to -Only"
    }
  }
}
if ($List) {
  foreach ($r in $rows) {
    '{0,-26} {1} start={2} {3}s env={4}' -f $r.Name, $r.Game, $r.Start, $r.Seconds,
      (($r.Env.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ' ')
  }
  return
}
if ($rows.Count -eq 0) { throw 'no rows to run (see -List)' }

# --- paths -----------------------------------------------------------------------------------------
if (-not $ProjPath) {
  $ProjPath = if (Test-Path (Join-Path $Repo "out\$($rows[0].Game)\iso")) { $Repo } else { $main }
}
if (-not $TestCfg) { $TestCfg = Join-Path $main 'local\testcfg' }
if (-not $OutDir) { $OutDir = Join-Path $Repo ('local\gate\' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
New-Item -ItemType Directory -Force $OutDir | Out-Null
if (-not $GkPath) {
  # a copy of the build's gk and DLLs of its own (as dev_bin.ps1 makes them, but in the gate's folder: a
  # dev_bin prune by another script between two rows can't take it away), so the build can relink meanwhile
  $from = Join-Path $Repo 'out\build\Release\bin'
  if (-not (Test-Path (Join-Path $from 'gk.exe'))) { throw "no gk.exe in $from" }
  $bin = Join-Path $OutDir 'bin'
  New-Item -ItemType Directory -Force $bin | Out-Null
  Get-ChildItem $from -File | Where-Object { $_.Name -eq 'gk.exe' -or $_.Extension -eq '.dll' } |
    ForEach-Object { [IO.File]::Copy($_.FullName, (Join-Path $bin $_.Name), $true) }
  $GkPath = Join-Path $bin 'gk.exe'
}
foreach ($p in @($GkPath, $ProjPath, $TestCfg)) { if (-not (Test-Path $p)) { throw "missing: $p" } }

if (-not ('GateWin' -as [type])) {
  Add-Type -ReferencedAssemblies System.Drawing @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public static class GateWin {
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  // the window's client area (its own pixels, even under other windows), or null
  public static Bitmap Capture(IntPtr h) {
    RECT r; if (!GetClientRect(h, out r)) return null;
    int w = r.R - r.L, ht = r.B - r.T; if (w < 16 || ht < 16) return null;
    var bmp = new Bitmap(w, ht, PixelFormat.Format32bppArgb);
    using (var g = Graphics.FromImage(bmp)) { IntPtr dc = g.GetHdc(); PrintWindow(h, dc, 3); g.ReleaseHdc(dc); }
    return bmp;
  }
  // mean luma (0-255) and the share of bright pixels (luma > 220)
  public static double[] Stats(Bitmap bmp) {
    var d = bmp.LockBits(new Rectangle(0, 0, bmp.Width, bmp.Height), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
    var px = new byte[d.Stride * d.Height]; Marshal.Copy(d.Scan0, px, 0, px.Length); bmp.UnlockBits(d);
    double sum = 0; long bright = 0, n = 0;
    for (int y = 0; y < bmp.Height; y += 2) for (int x = 0; x < bmp.Width; x += 2) {
      int i = y * d.Stride + x * 4;
      double l = 0.0722 * px[i] + 0.7152 * px[i + 1] + 0.2126 * px[i + 2];
      sum += l; n++; if (l > 220) bright++;
    }
    return new double[] { sum / n, (double)bright / n };
  }
  public static void SaveSmall(Bitmap bmp, string path, int width) {
    int h = Math.Max(1, bmp.Height * width / bmp.Width);
    using (var s = new Bitmap(bmp, new Size(width, h))) s.Save(path, ImageFormat.Png);
  }
}
'@
}
[GateWin]::SetProcessDPIAware() | Out-Null
Add-Type -AssemblyName System.Windows.Forms

function PortFree([int]$port) {
  $l = New-Object Net.Sockets.TcpListener ([Net.IPAddress]::Loopback), $port
  try { $l.Start(); $l.Stop(); return $true } catch { return $false }
}
function Read-Lines([string]$log) {
  if (-not $log -or -not (Test-Path $log)) { return @() }
  # gk keeps the log open: read it shared
  try {
    $fs = [IO.File]::Open($log, 'Open', 'Read', 'ReadWrite')
    $sr = New-Object IO.StreamReader $fs
    $t = $sr.ReadToEnd(); $sr.Close()
    return @($t -split "`r?`n")
  } catch { return @() }
}
function Saves-Hash([string]$cfg, [string]$game) {
  $d = Join-Path $cfg "OpenGOAL\$game\saves"
  if (-not (Test-Path $d)) { return @{} }
  $h = @{}
  foreach ($f in Get-ChildItem $d -Recurse -File) { $h[$f.FullName.Substring($d.Length)] = (Get-FileHash $f.FullName -Algorithm MD5).Hash }
  $h
}

# --- one row: start ----------------------------------------------------------------------------------
$area = [System.Windows.Forms.Screen]::PrimaryScreen.WorkingArea
$script:nextInstance = $FirstInstance
function Start-Row($r, [int]$slot) {
  $gameNum = [int]$r.Game.Substring(3)
  $n = $script:nextInstance
  $tries = 0
  while (-not ((PortFree (8111 + $gameNum + 10 * $n)) -and (PortFree (8181 + 10 * $n)))) {
    $n++; $tries++
    if ($n -ge 60) { $n = $FirstInstance }
    if ($tries -gt 60) { throw 'no free instance ports' }
  }
  $script:nextInstance = $n + 1
  if ($script:nextInstance -ge 60) { $script:nextInstance = $FirstInstance }
  $cfg = Join-Path $OutDir "cfg-$($r.Name)"
  $dst = Join-Path $cfg "OpenGOAL\$($r.Game)"
  New-Item -ItemType Directory -Force $dst | Out-Null
  $from = Join-Path $TestCfg "OpenGOAL\$($r.Game)"
  if ($r.CfgFrom) { $from = Join-Path $OutDir "cfg-$($r.CfgFrom)\OpenGOAL\$($r.Game)" }
  Get-ChildItem $from | Where-Object { $_.Name -ne 'screenshots' } | Copy-Item -Destination $dst -Recurse -Force
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
  $x = $area.Left + 8 + $slot * ($Width + 16); $y = $area.Top + 32
  $disp = [ordered]@{ display_id = 0; display_mode = 0; version = '1.2'; window_xpos = $x; window_ypos = $y }
  [IO.File]::WriteAllText((Join-Path $sdir 'display-settings.json'), ($disp | ConvertTo-Json))

  $psi = New-Object Diagnostics.ProcessStartInfo $GkPath
  $psi.Arguments = "--game $($r.Game) --proj-path `"$ProjPath`" --config-path `"$cfg`" -- -boot -fakeiso -debug"
  $psi.WorkingDirectory = $ProjPath
  $psi.UseShellExecute = $false
  $psi.CreateNoWindow = $true
  $e = $psi.EnvironmentVariables
  # (nothing of the caller's test switches leaks in)
  foreach ($k in @($e.Keys)) { if ($k -like 'JAK_ENHANCED_*') { $e.Remove($k) } }
  $e['JAK_ENHANCED_INSTANCE'] = "$n"
  $e['JAK_ENHANCED_START_MENU'] = '0'
  $e['JAK_ENHANCED_PRESET'] = '3'
  if ($r.Start) { $e['JAK_ENHANCED_START'] = $r.Start }
  foreach ($k in $r.Env.Keys) { $e[$k] = "$($r.Env[$k])" }
  $t0 = Get-Date
  $savesBefore = Saves-Hash $cfg $r.Game
  $p = [Diagnostics.Process]::Start($psi)
  $null = $p.Handle
  [pscustomobject]@{ Row = $r; N = $n; Slot = $slot; X = $x; Y = $y; Cfg = $cfg; Proc = $p; Pid = $p.Id; T0 = $t0; Log = '';
    Hwnd = [IntPtr]::Zero; Ready = $null; End = $null; ShotsDone = @(); Shots = @(); Done = $false; SavesBefore = $savesBefore;
    Exit = ''; Fail = @(); Info = @() }
}

# --- one row: every poll -------------------------------------------------------------------------------
$SWP = 0x0001 -bor 0x0004 -bor 0x0010   # NOSIZE | NOZORDER | NOACTIVATE
$fg = [GateWin]::GetForegroundWindow()
function Poll-Row($it) {
  $r = $it.Row
  $now = Get-Date
  if ($it.Proc.HasExited) { $it.Done = $true; return }
  if ($it.Hwnd -eq [IntPtr]::Zero) {
    $it.Proc.Refresh()
    if ($it.Proc.MainWindowHandle -ne [IntPtr]::Zero) {
      $it.Hwnd = $it.Proc.MainWindowHandle
      [GateWin]::SetWindowPos($it.Hwnd, [IntPtr]::Zero, $it.X, $it.Y, 0, 0, $SWP) | Out-Null
      if ($fg -ne [IntPtr]::Zero) { [GateWin]::SetForegroundWindow($fg) | Out-Null }
    }
  }
  if (-not $it.Log) {
    $it.Log = Get-ChildItem (Join-Path $ProjPath 'log') -Filter "$($r.Game)_i$($it.N).*.log" -ErrorAction SilentlyContinue |
      Where-Object { $_.LastWriteTime -ge $it.T0.AddSeconds(-2) } | Sort-Object LastWriteTime | Select-Object -Last 1 -ExpandProperty FullName
  }
  if (-not $it.Ready) {
    if ($it.Log -and (Select-String -Path $it.Log -Pattern '\[enhanced\] average' -Quiet -ErrorAction SilentlyContinue)) {
      $it.Ready = $now; $it.End = $now.AddSeconds($r.Seconds)
    } elseif (($now - $it.T0).TotalSeconds -gt $BootTimeout) {
      $it.Fail += "no frames after $BootTimeout s"; $it.Done = $true; return
    }
    return
  }
  # screenshots when due
  foreach ($s in $r.Shots) {
    if ($it.ShotsDone -notcontains $s -and ($now - $it.Ready).TotalSeconds -ge $s) {
      $it.ShotsDone += $s
      $bmp = if ($it.Hwnd -ne [IntPtr]::Zero) { [GateWin]::Capture($it.Hwnd) } else { $null }
      if (-not $bmp) {
        # (the window handle first seen can be gone: ask the process again)
        $it.Proc.Refresh()
        if ($it.Proc.MainWindowHandle -ne [IntPtr]::Zero) { $it.Hwnd = $it.Proc.MainWindowHandle; $bmp = [GateWin]::Capture($it.Hwnd) }
      }
      if ($bmp) {
        $st = [GateWin]::Stats($bmp)
        $file = "$($r.Name)-$s.png"
        [GateWin]::SaveSmall($bmp, (Join-Path $OutDir $file), 320)
        $bmp.Dispose()
        $it.Shots += [pscustomobject]@{ T = $s; File = $file; Luma = $st[0]; Bright = $st[1] }
      } else {
        $it.Shots += [pscustomobject]@{ T = $s; File = ''; Luma = -1; Bright = 0 }
      }
    }
  }
  if ($now -ge $it.End) { $it.Done = $true }
}

# --- one row: the verdict ------------------------------------------------------------------------------
function Judge($it) {
  $r = $it.Row
  $p = $it.Proc
  $alive = -not $p.HasExited
  if (-not $alive) { $it.Exit = '0x{0:X8}' -f $p.ExitCode; $it.Fail += "exited $($it.Exit)" }
  $ev = Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = 1000; StartTime = $it.T0 } -ErrorAction SilentlyContinue |
    Where-Object { $_.Properties.Count -gt 8 -and $_.Properties[0].Value -eq 'gk.exe' -and
      [Convert]::ToInt32(($_.Properties[8].Value -replace '^0x', ''), 16) -eq $it.Pid } | Select-Object -First 1
  if ($ev) { $it.Fail += "event1000 module=$($ev.Properties[3].Value) code=$($ev.Properties[6].Value) offset=$($ev.Properties[7].Value)" }
  $lines = Read-Lines $it.Log
  if (-not $it.Log) { $it.Fail += 'no log' }
  # fps lines after the boot (the first one is the boot's)
  $fps = @()
  foreach ($l in $lines) {
    if ($l -match '\[enhanced\] average ([\d.]+) fps.* in (\S+)\s*$') { $fps += [pscustomobject]@{ Fps = [double]$Matches[1]; Level = $Matches[2] } }
  }
  $run = @($fps | Select-Object -Skip 1)
  $it.Info += 'fps n={0} avg={1:N0} min={2:N0}' -f $run.Count, $(if ($run.Count) { ($run.Fps | Measure-Object -Average).Average } else { 0 }),
    $(if ($run.Count) { ($run.Fps | Measure-Object -Minimum).Minimum } else { 0 })
  if ($run.Count -lt $r.Lines) { $it.Fail += "fps lines $($run.Count) < $($r.Lines)" }
  if ($run.Count) {
    $avg = ($run.Fps | Measure-Object -Average).Average
    if ($avg -lt $r.MinFps) { $it.Fail += ('fps avg {0:N1} < {1}' -f $avg, $r.MinFps) }
    $last = $run[-1].Level
    if ($last -in 'none', 'title') { $it.Fail += "last level $last" }
    $it.Info += 'levels ' + ((@($run | ForEach-Object Level) | Select-Object -Unique) -join '>')
  }
  # levels in order (the boot's line too: a level change can come before the first line of the run)
  $k = 0
  foreach ($re in $r.Levels) {
    $hit = $false
    while ($k -lt $fps.Count) { if ($fps[$k].Level -match $re) { $hit = $true; break }; $k++ }
    if (-not $hit) { $it.Fail += "no level $re$(if ($k -gt 0) { ' after the one before' })"; break }
  }
  # expected log lines in order
  $pos = 0
  foreach ($x in $r.Expect) {
    $found = 0; $first = -1
    for ($i = $pos; $i -lt $lines.Count; $i++) {
      if ($lines[$i] -match $x.Re) { $found++; if ($first -lt 0) { $first = $i } }
    }
    if ($found -lt $x.Min) { $it.Fail += "missing '$($x.Label)'$(if ($x.Min -gt 1) { " ($found/$($x.Min))" })"; continue }
    $pos = $first + 1
  }
  # screenshots: not black; the last one bright enough
  foreach ($s in $it.Shots) {
    if ($s.Luma -lt 0) { $it.Fail += "no screenshot at $($s.T)s" }
    elseif ($s.Luma -le 8) { $it.Fail += ('black screenshot at {0}s (luma {1:N1})' -f $s.T, $s.Luma) }
  }
  if ($it.Shots.Count -lt $r.Shots.Count -and $alive -and $it.Ready) { $it.Fail += "screenshots $($it.Shots.Count)/$($r.Shots.Count)" }
  if ($r.Bright -gt 0 -and $it.Shots.Count) {
    $b = $it.Shots[-1].Bright
    $it.Info += ('bright {0:P2}' -f $b)
    if ($b -lt $r.Bright) { $it.Fail += ('bright pixels {0:P2} < {1:P2}' -f $b, $r.Bright) }
  }
  if ($r.Save) {
    $after = Saves-Hash $it.Cfg $r.Game
    $changed = @($after.Keys | Where-Object { -not $it.SavesBefore.ContainsKey($_) -or $it.SavesBefore[$_] -ne $after[$_] })
    if ($changed.Count -eq 0) { $it.Fail += 'no save file written' } else { $it.Info += "saved $($changed -join ',')" }
  }
  if ($it.Shots.Count) { $it.Info += 'luma ' + (($it.Shots | ForEach-Object { '{0}s={1:N0}' -f $_.T, $_.Luma }) -join ' ') }
  # a failure's evidence: the log's last lines when the game died
  if (-not $alive -and $lines.Count) { $it.Info += 'tail: ' + ((@($lines | Where-Object { $_ }) | Select-Object -Last 3) -join ' | ') }
}

# --- run ----------------------------------------------------------------------------------------------
$before = @(Get-Process gk -ErrorAction SilentlyContinue | ForEach-Object Id)
$queue = New-Object System.Collections.ArrayList
$rows | ForEach-Object { [void]$queue.Add($_) }
$running = New-Object System.Collections.ArrayList
$finished = New-Object System.Collections.ArrayList
$tStart = Get-Date
"GATE start rows=$($rows.Count) instances=$Instances gk=$GkPath proj=$ProjPath out=$OutDir"
try {
  while ($queue.Count -or $running.Count) {
    # start rows into free slots (a row waits for the row its config comes from)
    while ($running.Count -lt $Instances -and $queue.Count) {
      $next = $null
      foreach ($q in $queue) {
        if (-not $q.CfgFrom -or ($finished | Where-Object { $_.Row.Name -eq $q.CfgFrom })) { $next = $q; break }
      }
      if (-not $next) { break }
      [void]$queue.Remove($next)
      $used = @($running | ForEach-Object Slot)
      $slot = 0; while ($used -contains $slot) { $slot++ }
      $it = Start-Row $next $slot
      [void]$running.Add($it)
      "GATE run {0} instance={1} pid={2}" -f $next.Name, $it.N, $it.Pid
    }
    Start-Sleep -Milliseconds 500
    foreach ($it in @($running)) {
      Poll-Row $it
      if ($it.Done) {
        Judge $it
        if (-not $it.Proc.HasExited -and $before -notcontains $it.Pid) {
          Stop-Process -Id $it.Pid -Force -ErrorAction SilentlyContinue
          $null = $it.Proc.WaitForExit(10000)
        }
        [void]$running.Remove($it)
        [void]$finished.Add($it)
        $v = if ($it.Fail.Count) { 'FAIL' } else { 'PASS' }
        'GATE {0} {1} {2}' -f $it.Row.Name, $v, $(if ($it.Fail.Count) { $it.Fail -join '; ' } else { $it.Info -join '; ' })
        # (the next launch on this instance can find its port still taken for a moment)
        Start-Sleep 2
      }
    }
  }
} finally {
  foreach ($it in $running) {
    if (-not $it.Proc.HasExited -and $before -notcontains $it.Pid) { Stop-Process -Id $it.Pid -Force -ErrorAction SilentlyContinue }
  }
}

# --- report ---------------------------------------------------------------------------------------------
$pass = @($finished | Where-Object { $_.Fail.Count -eq 0 }).Count
$fail = $finished.Count - $pass
$commit = (& git -C $Repo log -1 --format='%h %s' 2>$null)
$txt = @("GATE commit=$commit rows=$($finished.Count) pass=$pass fail=$fail minutes=$([int]((Get-Date) - $tStart).TotalMinutes)")
foreach ($it in $finished) {
  $txt += '{0,-26} {1} {2}' -f $it.Row.Name, $(if ($it.Fail.Count) { 'FAIL' } else { 'PASS' }),
    $(if ($it.Fail.Count) { ($it.Fail -join '; ') + ' | ' + ($it.Info -join '; ') } else { $it.Info -join '; ' })
  $txt += '    log=' + $(if ($it.Log) { $it.Log } else { '-' })
}
[IO.File]::WriteAllLines((Join-Path $OutDir 'report.txt'), [string[]]$txt)
$enc = { param($s) [Net.WebUtility]::HtmlEncode("$s") }
$h = New-Object Text.StringBuilder
[void]$h.Append(@"
<!doctype html><html><head><meta charset="utf-8"><title>Release gate</title><style>
body{font:14px system-ui,sans-serif;margin:16px;background:#16181c;color:#e6e6e6}
table{border-collapse:collapse;width:100%}td,th{border-bottom:1px solid #333;padding:6px;vertical-align:top;text-align:left}
.PASS{color:#6fcf7f;font-weight:600}.FAIL{color:#ff6b6b;font-weight:600}img{width:160px;margin-right:4px}
small{color:#9aa}</style></head><body>
<h1>Release gate</h1><p>$(& $enc $commit) &middot; $pass pass, $fail fail &middot; $(Get-Date -Format 'yyyy-MM-dd HH:mm')</p>
<table><tr><th>Row</th><th>Result</th><th>Why / details</th><th>Screenshots</th></tr>
"@)
foreach ($it in $finished) {
  $v = if ($it.Fail.Count) { 'FAIL' } else { 'PASS' }
  $imgs = ($it.Shots | Where-Object File | ForEach-Object { "<img src=`"$($_.File)`" title=`"$($_.T) s`">" }) -join ''
  [void]$h.Append("<tr><td>$(& $enc $it.Row.Name)<br><small>$(& $enc $it.Row.Start)</small></td><td class=`"$v`">$v</td><td>$(& $enc ($it.Fail -join '; '))<br><small>$(& $enc ($it.Info -join '; '))</small></td><td>$imgs</td></tr>`n")
}
[void]$h.Append('</table></body></html>')
[IO.File]::WriteAllText((Join-Path $OutDir 'report.html'), $h.ToString())
"GATE RESULT pass=$pass fail=$fail report=$(Join-Path $OutDir 'report.html')"
$global:LASTEXITCODE = if ($fail) { 1 } else { 0 }
if ($MyInvocation.MyCommand.CommandType -eq 'ExternalScript') { exit $global:LASTEXITCODE }
