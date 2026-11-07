# Start, query or stop the dev goalc of one instance (scripts\enhanced\dev_goalc.py): a goalc kept
# running from a shadow bin, with a reply channel on 127.0.0.1:8185+10n.
#
#   $s = [IO.File]::ReadAllText('<repo>\scripts\enhanced\dev_goalc.ps1')
#   & ([scriptblock]::Create($s)) -Instance 1            # start (no-op if running), (mi) + (lt) in background
#   & ([scriptblock]::Create($s)) -Instance 1 -Wait      # ... and wait until (mi) is done
#   & ([scriptblock]::Create($s)) -Instance 1 -Status
#   & ([scriptblock]::Create($s)) -Instance 1 -Eval '(lt)'
#   & ([scriptblock]::Create($s)) -Instance 1 -Stop
#
# Instance 0 is the stock ports (the user's own game: gk 8114, nREPL 8181). Agents use n >= 1.
# Output: one line, "GOALC instance=<n> pid=<daemon> goalc=<pid> mi=<state> connected=<bool> log=<file>".
param(
  [int]$Instance = 1,
  [switch]$Status,
  [switch]$Stop,
  [switch]$Wait,
  [switch]$NoMi,
  [string]$Eval = '',
  [int]$Timeout = 900,
  [string]$Repo = ''
)
$ErrorActionPreference = 'Stop'
if (-not $Repo) {
  $Repo = if ($PSScriptRoot) { Split-Path (Split-Path $PSScriptRoot) } else { (Get-Location).Path }
}
if (-not (Test-Path (Join-Path $Repo 'scripts\enhanced'))) { throw "-Repo: not a jak-enhanced tree: $Repo" }
$main = Split-Path (& git -C $Repo rev-parse --path-format=absolute --git-common-dir)
$port = 8185 + $(if ($Instance -gt 0 -and $Instance -lt 100) { 10 * $Instance } else { 0 })

function Send-Dev([hashtable]$req, [int]$timeoutMs = 600000) {
  $c = New-Object Net.Sockets.TcpClient
  try {
    # a refused localhost connect takes ~2 s on Windows: give up after 300 ms instead
    if (-not $c.ConnectAsync('127.0.0.1', $port).Wait(300)) { throw "port $port closed" }
    $c.ReceiveTimeout = $timeoutMs
    $s = $c.GetStream()
    $w = New-Object IO.StreamWriter($s, (New-Object Text.UTF8Encoding $false)); $w.AutoFlush = $true
    $r = New-Object IO.StreamReader($s, [Text.Encoding]::UTF8)
    $w.WriteLine(($req | ConvertTo-Json -Compress -Depth 5))
    return ($r.ReadLine() | ConvertFrom-Json)
  } finally { $c.Close() }
}
function Up { try { $null = Send-Dev @{ op = 'status' } 3000; return $true } catch { return $false } }
function Line($st) {
  'GOALC instance={0} pid={1} goalc={2} mi={3} connected={4} log={5}' -f $st.instance, $st.pid, $st.goalc_pid,
    ($st.mi -replace ' ', ''), $st.connected, $st.log
}

if ($Stop) {
  if (Up) { $null = Send-Dev @{ op = 'stop' }; "GOALC instance=$Instance stopped" } else { "GOALC instance=$Instance not running" }
  return
}
if ($Status) {
  if (Up) { Line (Send-Dev @{ op = 'status' }) } else { "GOALC instance=$Instance not running" }
  return
}

if (-not (Up)) {
  $py = Join-Path $env:LOCALAPPDATA 'Python\pythoncore-3.14-64\python.exe'
  if (-not (Test-Path $py)) { $py = 'python' }
  $bin = & ([scriptblock]::Create([IO.File]::ReadAllText((Join-Path $Repo 'scripts\enhanced\dev_bin.ps1')))) -Tag "goalc$Instance" -Repo $Repo
  $log = Join-Path $main "local\run\goalc_i$Instance.log"
  $psi = New-Object Diagnostics.ProcessStartInfo $py
  $a = "-u `"$(Join-Path $Repo 'scripts\enhanced\dev_goalc.py')`" --instance $Instance --bin `"$bin`" --proj `"$main`" --log `"$log`" --lt"
  if (-not $NoMi) { $a += ' --mi' }
  $psi.Arguments = $a
  $psi.WorkingDirectory = $main
  $psi.UseShellExecute = $false
  $psi.CreateNoWindow = $true
  $psi.EnvironmentVariables.Remove('JAK_ENHANCED_INSTANCE')
  $p = [Diagnostics.Process]::Start($psi)
  $end = (Get-Date).AddSeconds(30)
  while (-not (Up)) {
    if ($p.HasExited) { throw "dev_goalc.py exited ($($p.ExitCode)); see $log" }
    if ((Get-Date) -gt $end) { throw "dev_goalc.py didn't open port $port; see $log" }
    Start-Sleep -Milliseconds 200
  }
}
if ($Wait) {
  $end = (Get-Date).AddSeconds($Timeout)
  while ($true) {
    $st = Send-Dev @{ op = 'status' }
    if ($st.mi -ne 'running' -and ($st.mi -ne 'none' -or $st.uptime -gt 10)) { break }
    if ((Get-Date) -gt $end) { break }
    Start-Sleep -Milliseconds 500
  }
}
if ($Eval) {
  $r = Send-Dev @{ op = 'eval'; form = $Eval; timeout = $Timeout }
  if ($r.out) { $r.out }
  'EVAL {0} ms={1} prompt={2}' -f $(if ($r.ok) { 'OK' } else { 'ERROR' }), $r.ms, $r.prompt
  return
}
Line (Send-Dev @{ op = 'status' })
