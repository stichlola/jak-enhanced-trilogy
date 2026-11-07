# Hot-load changed GOAL files into a running game: (ml "<file>") through the instance's dev goalc
# (scripts\enhanced\dev_goalc.py, kept running, so no 30-50 s cold (mi) per change).
#
#   $s = [IO.File]::ReadAllText('<repo>\scripts\enhanced\dev_reload.ps1')
#   & ([scriptblock]::Create($s)) -Instance 1                       # files changed since the last load
#   & ([scriptblock]::Create($s)) -Instance 1 -Files goal_src\jak3\engine\target\target2.gc
#
#   -Files   .gc files (absolute or relative to the repo). Default: goal_src/jak3 files that differ from
#            HEAD (plus untracked ones) and changed since the last (mi)/(ml) of this dev goalc
#   -All     every file that differs from HEAD, even if already loaded
#   -Force   also load headers (*-h.gc), .gp/.gs and files whose deftype/defenum changed
#   -Mi      afterwards run (mi) in the background, so the next boot's CGO/DGO have the change too
#            (an engine file cascades to every later file: 25-70 s)
#   -Lt      (lt) first (the game was restarted: dev_goalc reconnects by itself when it isn't connected)
#
# Output, one line per file, then the summary:
#   OK <file> <ms>ms [loaded|compiled-only]       ERROR <file>:<line> <message>
#   NEEDS <file> mi+restart (<why>)               SKIP <file> <why>
#   RESULT ok=<a> error=<b> needs=<c> ms=<total>
# $LASTEXITCODE is 0 when every file loaded, else 1 (exit code too when run with -File).
#
# Caveats: a hot load runs the file's top-level forms again (its globals are re-created); a process
# already inside a state keeps the old code until it enters the state again; type layouts can't change
# in a running game (NEEDS). Starting the dev goalc the first time costs one cold (mi), 30-50 s.
param(
  [int]$Instance = 1,
  [string[]]$Files = @(),
  [switch]$All,
  [switch]$Force,
  [switch]$Mi,
  [switch]$Lt,
  [int]$Timeout = 120,
  [string]$Repo = ''
)
$ErrorActionPreference = 'Stop'
$t0 = Get-Date
# exit only when run as a .ps1 file: in a scriptblock it would close the caller's PowerShell
$asFile = $MyInvocation.MyCommand.CommandType -eq 'ExternalScript'
if (-not $Repo) {
  $Repo = if ($PSScriptRoot) { Split-Path (Split-Path $PSScriptRoot) } else { (Get-Location).Path }
}
if (-not (Test-Path (Join-Path $Repo 'scripts\enhanced'))) { throw "-Repo: not a jak-enhanced tree: $Repo" }
$port = 8185 + $(if ($Instance -gt 0 -and $Instance -lt 100) { 10 * $Instance } else { 0 })

function Send-Dev([hashtable]$req, [int]$timeoutMs = 600000) {
  $c = New-Object Net.Sockets.TcpClient
  try {
    if (-not $c.ConnectAsync('127.0.0.1', $port).Wait(300)) { throw "port $port closed" }
    $c.ReceiveTimeout = $timeoutMs
    $s = $c.GetStream()
    $w = New-Object IO.StreamWriter($s, (New-Object Text.UTF8Encoding $false)); $w.AutoFlush = $true
    $r = New-Object IO.StreamReader($s, [Text.Encoding]::UTF8)
    $w.WriteLine(($req | ConvertTo-Json -Compress -Depth 5))
    return ($r.ReadLine() | ConvertFrom-Json)
  } finally { $c.Close() }
}

$up = $true
try { $st = Send-Dev @{ op = 'status' } 3000 } catch { $up = $false }
if (-not $up) {
  'NOTE starting the dev goalc for instance {0} (one cold (mi), 30-50 s)' -f $Instance
  $null = & ([scriptblock]::Create([IO.File]::ReadAllText((Join-Path $Repo 'scripts\enhanced\dev_goalc.ps1')))) `
    -Instance $Instance -Wait -Repo $Repo
} elseif ($st.mi -eq 'running') {
  'NOTE (mi) is running in the dev goalc: the loads wait for it'
}

if ($Files.Count) {
  $st = Send-Dev @{ op = 'status' } 3000
  $main = if ($st.proj) { $st.proj } else { Split-Path (& git -C $Repo rev-parse --path-format=absolute --git-common-dir) }
}
$rel = @(foreach ($f in $Files) {
    $p = if ([IO.Path]::IsPathRooted($f)) { $f } else { Join-Path $main $f }
    $p = [IO.Path]::GetFullPath($p)
    if ($p.StartsWith($main, [StringComparison]::OrdinalIgnoreCase)) { $p.Substring($main.Length).TrimStart('\', '/') -replace '\\', '/' }
    else { $f -replace '\\', '/' }
  })
$req = @{ op = 'reload'; files = $rel; all = [bool]$All; force = [bool]$Force; mi = [bool]$Mi; lt = [bool]$Lt; timeout = $Timeout }
$r = Send-Dev $req (($Timeout + 900) * 1000)
if ($r.error) { "ERROR dev_goalc: $($r.error)"; $global:LASTEXITCODE = 1; if ($asFile) { exit 1 }; return }
$n = @{ OK = 0; ERROR = 0; NEEDS = 0 }
foreach ($x in @($r.results)) {
  switch ($x.status) {
    'OK' { $n.OK++; 'OK {0} {1}ms {2}{3}' -f $x.file, $x.ms, $(if ($x.loaded) { 'loaded' } else { 'compiled-only' }), $(if ($x.msg) { " NOTE $($x.msg)" } else { '' }) }
    'ERROR' { $n.ERROR++; 'ERROR {0} {1}' -f $x.loc, $x.msg }
    'NEEDS' { $n.NEEDS++; 'NEEDS {0} {1}' -f $x.file, $x.msg }
    default { 'SKIP {0} {1}' -f $x.file, $x.msg }
  }
}
foreach ($x in @($r.notes)) { if ($x) { "NOTE $x" } }
if (@($r.results).Count -eq 0) { 'NOTE nothing to load (no goal_src/jak3 change since the last load; -All for every changed file)' }
'RESULT ok={0} error={1} needs={2} ms={3}' -f $n.OK, $n.ERROR, $n.NEEDS, [int]((Get-Date) - $t0).TotalMilliseconds
$code = if ($r.ok) { 0 } else { 1 }
$global:LASTEXITCODE = $code
if ($asFile) { exit $code }
