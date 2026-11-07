# Open the game for development: gk from a shadow copy of the bin folder (so build.bat can always
# relink out\build\Release\bin), shader hot reload on, and the instance's dev goalc started alongside
# (dev_goalc.ps1: (mi) + (lt) in the background, then dev_reload.ps1 hot-loads GOAL changes).
#
#   $s = [IO.File]::ReadAllText('<repo>\scripts\enhanced\dev_launch.ps1')
#   & ([scriptblock]::Create($s))                         # the user's game: instance 0, their own config
#   & ([scriptblock]::Create($s)) -Instance 1 -TestCfg -Start desert-start -Wait     # an agent's test game
#   & ([scriptblock]::Create($s)) -Instance 1 -TestCfg -Headless -Env @{ JAK_ENHANCED_HOUR = '18' }
#
#   -Instance  0 = stock ports and the user's %APPDATA%\OpenGOAL config; agents use 1+ (ports +10n)
#   -TestCfg   own config dir copied from local\testcfg (windowed -Width x -Height, Performance preset,
#              no start menu); default for instances 1+ unless -Config names a dir
#   -Config    config dir (holds OpenGOAL\<game>\settings) to use as it is
#   -Headless  gk --no-display (logic only: no renderer, no fps lines; boots in ~5.5 s)
#   -Start     continue point (JAK_ENHANCED_START)
#   -Env       extra environment, @{ JAK_ENHANCED_PLAYER = 'sig' }
#   -NoGoalc   don't start the dev goalc        -NoShaderReload  leave JAK_ENHANCED_SHADER_RELOAD unset
#   -Wait      wait until frames come ("[enhanced] average" in the log; headless: the log's first lines)
#   -Minimized start the window minimized (test runs that only read logs)
# Output: "LAUNCH instance=<n> pid=<gk> port=<gk listener> bin=<shadow bin> cfg=<dir> log=<gk log|->",
# then the dev goalc's GOALC line. Stop the game with Stop-Process -Id <pid> (only the one you started).
param(
  [int]$Instance = 0,
  [ValidateSet('jak1', 'jak2', 'jak3')][string]$Game = 'jak3',
  [switch]$TestCfg,
  [string]$Config = '',
  [switch]$Headless,
  [string]$Start = '',
  [hashtable]$Env = @{},
  [switch]$NoGoalc,
  [switch]$NoShaderReload,
  [switch]$Wait,
  [switch]$Minimized,
  [int]$Width = 640,
  [int]$Height = 360,
  [int]$BootTimeout = 120,
  [string]$Repo = ''
)
$ErrorActionPreference = 'Stop'
if (-not $Repo) {
  $Repo = if ($PSScriptRoot) { Split-Path (Split-Path $PSScriptRoot) } else { (Get-Location).Path }
}
if (-not (Test-Path (Join-Path $Repo 'scripts\enhanced'))) { throw "-Repo: not a jak-enhanced tree: $Repo" }
$main = Split-Path (& git -C $Repo rev-parse --path-format=absolute --git-common-dir)
$enh = Join-Path $Repo 'scripts\enhanced'
$gameNum = [int]$Game.Substring(3)
$off = if ($Instance -gt 0 -and $Instance -lt 100) { 10 * $Instance } else { 0 }
$port = 8111 + $gameNum + $off

# a game that just closed can hold its port for a few more seconds: wait for it before giving up
$portEnd = (Get-Date).AddSeconds(15)
while ($true) {
  $l = New-Object Net.Sockets.TcpListener ([Net.IPAddress]::Loopback), $port
  try { $l.Start(); $l.Stop(); break } catch {
    if ((Get-Date) -gt $portEnd) { throw "gk port $port is taken: instance $Instance already runs a game" }
    Start-Sleep -Milliseconds 500
  }
}

# the shadow bin's run dir also holds the test config, so dev_bin.ps1 prunes both once gk is gone
$bin = & ([scriptblock]::Create([IO.File]::ReadAllText((Join-Path $enh 'dev_bin.ps1')))) -Tag "gk$Instance" -Repo $Repo
if ($Instance -gt 0 -and -not $Config) { $TestCfg = $true }
$cfg = ''
if ($TestCfg) {
  $src = Join-Path $main "local\testcfg\OpenGOAL\$Game"
  if (-not (Test-Path $src)) { throw "missing test config: $src" }
  $cfg = Join-Path (Split-Path $bin) 'cfg'
  $dst = Join-Path $cfg "OpenGOAL\$Game"
  New-Item -ItemType Directory -Force $dst | Out-Null
  Get-ChildItem $src | Where-Object { $_.Name -ne 'screenshots' } | Copy-Item -Destination $dst -Recurse -Force
  $sdir = Join-Path $dst 'settings'
  New-Item -ItemType Directory -Force $sdir | Out-Null
  $pc = Join-Path $sdir 'pc-settings.gc'
  if (Test-Path $pc) {
    $t = [IO.File]::ReadAllText($pc)
    $t = $t -replace '\(window-size [^)]*\)', "(window-size $Width $Height)"
    $t = $t -replace '\(discord-rpc\? [^)]*\)', '(discord-rpc? #f)'
    $t = $t -replace '\(ignore-controller-win-unfocused\? [^)]*\)', '(ignore-controller-win-unfocused? #t)'
    [IO.File]::WriteAllText($pc, $t)
  }
  # display_mode 0 = windowed; tiles right of the multi_run grid's first slot
  $x = 60 + 40 * ($Instance % 8); $y = 60 + 30 * ($Instance % 8)
  $disp = [ordered]@{ display_id = 0; display_mode = 0; version = '1.2'; window_xpos = $x; window_ypos = $y }
  [IO.File]::WriteAllText((Join-Path $sdir 'display-settings.json'), ($disp | ConvertTo-Json))
} elseif ($Config) {
  $cfg = $Config
}

$psi = New-Object Diagnostics.ProcessStartInfo (Join-Path $bin 'gk.exe')
$a = "--game $Game --proj-path `"$main`""
if ($cfg) { $a += " --config-path `"$cfg`"" }
if ($Headless) { $a += ' --no-display' }
$psi.Arguments = "$a -- -boot -fakeiso -debug"
$psi.WorkingDirectory = $main
$psi.UseShellExecute = $false
$psi.CreateNoWindow = $true
$e = $psi.EnvironmentVariables
if ($off) { $e['JAK_ENHANCED_INSTANCE'] = "$Instance" } else { $e.Remove('JAK_ENHANCED_INSTANCE') }
if ($TestCfg) { $e['JAK_ENHANCED_START_MENU'] = '0'; $e['JAK_ENHANCED_PRESET'] = '3' }
if (-not $NoShaderReload) { $e['JAK_ENHANCED_SHADER_RELOAD'] = '1' }
if ($Start) { $e['JAK_ENHANCED_START'] = $Start }
foreach ($k in $Env.Keys) { $e[$k] = "$($Env[$k])" }
$t0 = Get-Date
$p = [Diagnostics.Process]::Start($psi)

$goalcLine = ''
if (-not $NoGoalc) {
  try {
    $goalcLine = & ([scriptblock]::Create([IO.File]::ReadAllText((Join-Path $enh 'dev_goalc.ps1')))) -Instance $Instance -Repo $Repo
  } catch { $goalcLine = "GOALC instance=$Instance failed: $($_.Exception.Message)" }
}

# window: minimized on request; the gk log of this run
$logPat = if ($off) { "$($Game)_i$Instance.*.log" } else { "$Game.*.log" }
$log = ''
$end = $t0.AddSeconds($BootTimeout)
$hidden = (-not $Minimized) -or $Headless   # headless: no window to minimize
while ((Get-Date) -lt $end -and -not $p.HasExited) {
  if (-not $log) {
    $log = Get-ChildItem (Join-Path $main 'log') -Filter $logPat -ErrorAction SilentlyContinue |
      Where-Object { $_.CreationTime -ge $t0.AddSeconds(-1) -and $_.Name -match "^$Game(_i$Instance)?\.\d" } |
      Sort-Object CreationTime | Select-Object -Last 1 -ExpandProperty FullName
  }
  if (-not $hidden -and -not $Headless) {
    $p.Refresh()
    if ($p.MainWindowHandle -ne [IntPtr]::Zero) {
      if (-not ('DevLaunchWin' -as [type])) {
        Add-Type 'using System; using System.Runtime.InteropServices; public static class DevLaunchWin { [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c); }'
      }
      [DevLaunchWin]::ShowWindow($p.MainWindowHandle, 7) | Out-Null   # SW_SHOWMINNOACTIVE
      $hidden = $true
    }
  }
  $done = $log -and $hidden
  if ($done -and $Wait) {
    $pat = if ($Headless) { '\[enhanced\]|target' } else { '\[enhanced\] average' }
    $done = Select-String -Path $log -Pattern $pat -Quiet -ErrorAction SilentlyContinue
  }
  if ($done) { break }
  Start-Sleep -Milliseconds 250
}
$state = if ($p.HasExited) { "exited=0x{0:X8}" -f $p.ExitCode } else { 'running' }
'LAUNCH instance={0} pid={1} port={2} state={3} boot={4:N1}s bin={5} cfg={6} log={7}' -f $Instance, $p.Id, $port, $state,
  ((Get-Date) - $t0).TotalSeconds, $bin, $(if ($cfg) { $cfg } else { 'user' }), $(if ($log) { $log } else { '-' })
if ($goalcLine) { $goalcLine }
