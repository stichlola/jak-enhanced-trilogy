# Release gate: boots the game in a list of test scenarios and checks each one from its log, its process
# and (windowed rows) its screenshots. Rows that only need log lines run headless (gk --no-display,
# ~240 MB), several at once; rows that need the renderer (fps, weather, the menu, pictures, streamed
# cutscenes) run in small windowed test instances, one at a time by default.
#
#   & ([scriptblock]::Create([IO.File]::ReadAllText('<repo>\scripts\enhanced\release_gate.ps1'))) -Repo <repo>
#   ... -Only boot-desert,weather-desert      only these rows (comma list or array; * wildcards)
#   ... -List                                 print the rows and exit
#   ... -Instances 1                          windowed rows side by side (default 1; RAM: ~2.8 GB each)
#   ... -Parallel 4                           headless rows side by side (default 4; ~240 MB each)
#   ... -MakeBaseline                         add this run's screenshots to the baseline of -Version, as one
#                                             more rerun sample (the newest -BaselineKeep are kept; rows
#                                             that FAIL are not added)
#   ... -Baseline <dir|version|none>          compare the screenshots with a baseline (default: the
#                                             newest under local\gate\baseline other than -Version)
#
#   -Games      games to run (default jak3; only jak3 has rows for now)
#   -FirstInstance/-LastInstance  the JAK_ENHANCED_INSTANCE numbers used (default 6-12: 1-5 are the dev
#               loop's and other test scripts', 0 the user's game); a number whose ports are taken is skipped
#   -Repo       the jak-enhanced tree whose build is tested (out\<game>, and its bin, see -Bin)
#   -Bin        the folder whose gk.exe (and DLLs) is tested (default $env:JAK_RELEASE_BIN, which
#               release.ps1 sets to the static build it ships, else <Repo>\out\build\Release\bin)
#   -ProjPath   the data tree gk runs on (default -Repo when it has out\<game>\iso, else the main tree)
#   -Version    the baseline's name (default $env:JAK_RELEASE_VERSION, else git describe)
#   -NoEarly    every row runs its full Seconds (by default a row ends once all its checks are met and
#               it has run Settle more seconds)
#
# Every row: gk alive at the end (no exit code), no Application event 1000 for its PID, the level it
# should be in (the row's Levels in order, else the level of its start point), frames coming (windowed:
# "[enhanced] average N fps ... in <level>" lines, N >= MinFps; headless: the test boot's "[camera] ...
# in <level>" lines every 2 s), screenshots not black (mean luma > 8), and the row's Expect lines in order,
# each with its value checked when the row gives a range. Every verdict line lists the checks that matched
# and their values, so a PASS can be trusted without opening the log. Screenshots that differ a lot from
# the baseline's make the row WARN (it still passes): each shot is compared with every rerun sample of
# the baseline and the best match counts; its limits come from how much those reruns differ from each
# other (per row and shot), else from the baseline it was made beside, else the row's MinSim/MaxDiff.
# Output: "GATE <row> PASS|WARN|FAIL ..." per row,
# "GATE RESULT pass=.. warn=.. fail=..", report.txt + report.html (downscaled screenshots, baseline side by
# side) in local\gate\<time>\. Exit code 1 on any FAIL. Only the processes started here are stopped; the
# user's %APPDATA%\OpenGOAL is never read or written. Numbers are written with dot decimals.
param(
  [string[]]$Games = @('jak3'),
  [string[]]$Only = @(),
  [int]$Instances = 1,
  [int]$Parallel = 4,
  [int]$FirstInstance = 6,
  [int]$LastInstance = 12,
  [string]$Repo = '',
  [string]$ProjPath = '',
  [string]$GkPath = '',
  [string]$TestCfg = '',
  [string]$OutDir = '',
  [string]$Bin = '',
  [string]$Baseline = '',
  [switch]$MakeBaseline,
  [int]$BaselineKeep = 4,
  [string]$Version = '',
  [int]$BootTimeout = 240,
  [int]$Settle = 8,
  [int]$Width = 640,
  [int]$Height = 360,
  [switch]$NoEarly,
  [switch]$List
)
$ErrorActionPreference = 'Stop'
$inv = [Globalization.CultureInfo]::InvariantCulture
$prevCulture = [Threading.Thread]::CurrentThread.CurrentCulture
[Threading.Thread]::CurrentThread.CurrentCulture = $inv
# (the caller's culture comes back however the gate ends: the try's finally at the end of the file)
try {

if (-not $Repo) {
  $Repo = if ($PSScriptRoot) { Split-Path (Split-Path $PSScriptRoot) } else { (Get-Location).Path }
}
if (-not (Test-Path (Join-Path $Repo 'scripts\enhanced'))) { throw "-Repo: not a jak-enhanced tree: $Repo" }
$main = Split-Path (& git -C $Repo rev-parse --path-format=absolute --git-common-dir)
if ($Games.Count -eq 1 -and $Games[0] -match ',') { $Games = $Games[0] -split ',' }
if ($Only.Count -eq 1 -and $Only[0] -match ',') { $Only = $Only[0] -split ',' }
if ($LastInstance -lt $FirstInstance) { throw '-LastInstance < -FirstInstance' }

# --- the scenarios ---------------------------------------------------------------------------------
# Name, Game, Start (continue point), Headless (gk --no-display: log lines only), Seconds (at most, after
# the first frames line), Env, Shots (windowed: seconds after the first fps line), Expect (log lines, each
# after the one before), Levels (regexes the frames lines' levels must match in this order; default: the
# start point's level), MinFps, Bright (min share of bright pixels in the last shot), Save (the saves dir
# must change), CfgFrom (a row whose config dir this one starts from), Lines (min frames lines; default
# from the time run), Check (a script block on the Expect values: returns "!why" for a failure, else
# info), MaxDiff/MinSim (baseline tolerance when the baseline has no limits from reruns: mean luma
# difference 0-255, SSIM-like 0-1), Note
# (default 6 / 0.85, from reruns of one build at 64x36: diff 0.2-5.8, sim 0.81-1.00, most rows >= 0.94; a
# magenta box over 20 % of the frame gave diff 7.4-9.4 / sim 0.76-0.79, the desert from another camera
# sim 0.50, another level 0.15)
function New-Row([hashtable]$h) {
  $d = @{ Game = 'jak3'; Start = 'desert-start'; Headless = $false; Seconds = 45; Env = @{}; Shots = @(); Expect = @(); Levels = @();
    MinFps = 20; Bright = 0.0; Save = $false; CfgFrom = ''; Lines = -1; Check = $null; MaxDiff = 6.0; MinSim = 0.85; Note = '' }
  foreach ($k in $h.Keys) { $d[$k] = $h[$k] }
  if ($d.Headless) { $d.Shots = @() } elseif ($d.Shots.Count -eq 0) { $d.Shots = @([Math]::Max(5, $d.Seconds - 5)) }
  if ($d.Levels.Count -eq 0) { $d.Levels = @(Start-Level $d.Start) }
  [pscustomobject]$d
}
# the level a continue point is in (rows without Levels assert it)
function Start-Level([string]$start) {
  switch -regex ($start) {
    '^desert' { '^des'; break }
    '^wascity' { '^was'; break }
    '^game-start' { '^wasstad'; break }
    '^cty' { '^cty'; break }
    default { '^(?!none$|title$)' }
  }
}
# an expected log line: regex (a named group v is its value), label, how many at least, the value's range
# (MinV/MaxV) and which match gives it (Agg first|min|max)
function E([string]$re, [string]$label = '', [int]$min = 1, $MinV = $null, $MaxV = $null, [string]$Agg = 'first') {
  @{ Re = $re; Label = $(if ($label) { $label } else { $re }); Min = $min; MinV = $MinV; MaxV = $MaxV; Agg = $Agg }
}
$napCheck = {
  param($v)
  if ($null -eq $v['nap start'] -or $null -eq $v['nap end']) { return '!nap hours: no start/end time' }
  $h = (($v['nap end'] - $v['nap start']) + 24) % 24
  if ([Math]::Abs($h - 6) -gt 1.0) { return ('!nap hours {0:0.00} (start {1:0.00}, end {2:0.00}), want 6' -f $h, $v['nap start'], $v['nap end']) }
  ('nap hours {0:0.00} ({1:0.00} > {2:0.00})' -f $h, $v['nap start'], $v['nap end'])
}
# the run test (enhanced-player.gc "run"): run, jumps, square, circle, the body-jump metric per phase (the
# drawn body's main joint against its root, most per frame; for Jak himself it is data only: his main joint
# leaves his root by up to 3 m in the walk/run blends while he is drawn centred, so no limit)
function Run-Expect([string]$who, $bodyMax) {
  $x = @()
  if ($who -ne 'jak') { $x += E "\[enhanced-player\] `"?$who`"? #<" "$who loaded" }
  $x += @((E '\[player-test\] run$' 'run'), (E '\[player-test\] state target-(walk|run)' 'running'),
    (E '\[player-test\] run-jump' 'run-jump'), (E '\[player-test\] state target-jump' 'jumping'),
    (E '\[player-test\] body-jump "?run"? max (?<v>[\d.]+)' 'body-jump run m' -MaxV $bodyMax),
    (E '\[player-test\] double-jump' 'double-jump'), (E '\[player-test\] state target-double-jump' 'double jump state'),
    (E '\[player-test\] body-jump "?jumps"? max (?<v>[\d.]+)' 'body-jump jumps m' -MaxV $bodyMax),
    (E '\[player-test\] square' 'square'))
  $x += E '\[player-test\] state target-(attack|running-attack)' 'punch state'
  # the characters' own moves over Jak's punch and spin kick (enhanced-moves.gc)
  if ($who -eq 'ashelin') { $x += E '\[enhanced-move\] ashelin-front-kick' 'her front kick' }
  if ($who -eq 'sig') { $x += E '\[enhanced-move\] sig-(elbow|boxer)' 'his elbow' }
  $x += @((E '\[player-test\] circle' 'circle'), (E '\[player-test\] state target-attack' 'spin kick state'))
  if ($who -eq 'sig') { $x += E '\[enhanced-move\] sig-spin-punch' 'his spinning backfist' }
  $x += E '\[player-test\] body-jump "?attacks"? max (?<v>[\d.]+)' 'body-jump attacks m' -MaxV $bodyMax
  $x
}
$weather = '0:0,6:1,12:4,20:5,28:0'
$weatherExpect = @((E '\[enhanced\] weather mode 1 ' 'mode 1'), (E '\[enhanced\] weather mode 4 ' 'mode 4'),
  (E '\[enhanced\] weather mode 5 ' 'mode 5'), (E '\[enhanced\] weather mode 0 ' 'back to 0'))
$rows = @(
  # boot to a continue point and stay there
  New-Row @{ Name = 'boot-desert'; Start = 'desert-start'; Seconds = 30; Shots = @(25) }
  New-Row @{ Name = 'boot-spargus'; Start = 'wascitya-start'; Seconds = 30; Shots = @(25) }
  # the menu's hour picks right at boot (kind 1 queued in the first frames)
  New-Row @{ Name = 'hour-23-at-boot'; Headless = $true; Seconds = 30; Env = @{ JAK_ENHANCED_MENU = '1:23' }
    Expect = @((E '\[enhanced\] hour request 23 from (?<v>[\d.]+)' 'hour request 23 (from)'), (E '\[enhanced\] hour reached (?<v>[\d.]+)' 'hour reached' -MinV 22 -MaxV 24)) }
  New-Row @{ Name = 'hour-6-at-boot'; Headless = $true; Seconds = 30; Env = @{ JAK_ENHANCED_MENU = '1:6' }
    Expect = @((E '\[enhanced\] hour request 6 from (?<v>[\d.]+)' 'hour request 6 (from)'), (E '\[enhanced\] hour reached (?<v>[\d.]+)' 'hour reached' -MinV 5 -MaxV 7)) }
  New-Row @{ Name = 'hour-burst'; Headless = $true; Seconds = 30; Env = @{ JAK_ENHANCED_MENU_SCRIPT = '0.5:1:23,1:1:6,2:1:12,6:1:18' }
    Expect = @((E '\[enhanced\] menu script .* request 1 ' 'script picks' 4), (E '\[enhanced\] hour request 18 from (?<v>[\d.]+)' 'last pick taken (from)'),
      (E '\[enhanced\] hour reached (?<v>[\d.]+)' 'hour reached' -MinV 17 -MaxV 19)) }
  # a nap: asked 8 s into the first level, Jak lies down, the hours go by (checked), he gets up
  New-Row @{ Name = 'nap-6h'; Headless = $true; Seconds = 45; Env = @{ JAK_ENHANCED_NAP = '6' }; Check = $napCheck
    Expect = @((E '\[enhanced\] nap request 6 h' 'nap request'), (E '\[enhanced\] nap start 6 h at (?<v>[\d.]+)' 'nap start'), (E '\[enhanced\] nap end at (?<v>[\d.]+)' 'nap end')) }
  # (the open window freezes the game: no frames lines after it)
  New-Row @{ Name = 'nap-window'; Headless = $true; Seconds = 25; Env = @{ JAK_ENHANCED_NAP = 'window' }; Lines = 0
    Expect = @((E '\[enhanced\] nap window open' 'nap window open')) }
  # the parked Hellcat: in, climb, fire, land, take off, out in flight
  New-Row @{ Name = 'hellcat-fly'; Headless = $true; Seconds = 70; Env = @{ JAK_ENHANCED_HELLCAT_TEST = '1' }
    Expect = @((E '\[hellcat\] parked:' 'hellcat parked'), (E '\[hellcat-test\] by' 'by the hellcat'),
      (E '\[hellcat-test\] "?(launch|climbing)"? #t' 'piloting'), (E '\[hellcat-test\] "?firing"? #t' 'firing'),
      (E '\[hellcat-test\] out #t' 'out in flight'), (E '\[hellcat-test\] end' 'test end')) }
  # level changes: desert -> Spargus on foot and in the Hellcat, Spargus -> desert by the menu (windowed,
  # as every row that loads a level after the boot: a headless gk logs "Adding level X" and never
  # finishes loading it; the air train's actors aren't found either)
  # (after the level change Jak and the camera stop in a different spot each run: reruns sim 0.59-0.99,
  # diff 1.4-16; fallback limits wide, the baseline's own samples set the real ones)
  New-Row @{ Name = 'desert-to-spargus-foot'; Seconds = 90; Shots = @(45); Env = @{ JAK_ENHANCED_HELLCAT_TEST = '3' }; MinSim = 0.5; MaxDiff = 20.0
    Levels = @('^des', '^was'); Expect = @((E '\[spargus-test\] gate' 'gate swap'), (E '\[spargus-test\] t .*wascitya active' 'wascitya active')) }
  New-Row @{ Name = 'desert-to-spargus-hellcat'; Seconds = 90; Shots = @(45); Env = @{ JAK_ENHANCED_HELLCAT_TEST = '2' }; MinSim = 0.5; MaxDiff = 20.0
    Levels = @('^des', '^was'); Expect = @((E '\[spargus-test\] gate' 'gate swap'), (E '\[spargus-test\] t .*wascitya active' 'wascitya active')) }
  # (the request 25 s after the first frame: one queued at boot is taken before the level is up)
  New-Row @{ Name = 'spargus-to-desert'; Shots = @(50); Start = 'wascitya-start'; Seconds = 60; Env = @{ JAK_ENHANCED_MENU_SCRIPT = '25:2:desert-start' }
    Levels = @('^was', '^des'); Expect = @((E '\[enhanced\] continue request "?desert-start"?: "?sent' 'continue sent')) }
  # the air train: its cutscene (streamed: windowed, headless gk stalls STR streaming; it ends in Haven),
  # and the train itself (triangle at it; the driver closes city-destroy-grid-resolution first, which opens
  # the train to Haven)
  # (the scene at 10 s has two looks on reruns, sim 0.65-0.99: fallback limits wide, the baseline's own
  # samples set the real ones; the ride at 60 s 0.81-0.88)
  New-Row @{ Name = 'air-train-scene'; Start = 'desert-warp'; Seconds = 90; Env = @{ JAK_ENHANCED_SCENE = 'desert-air-train-in' }; Shots = @(10, 60)
    MinSim = 0.6; MaxDiff = 12.0
    Levels = @('^des', '^cty'); Expect = @((E '\[enhanced\] scene "?desert-air-train-in"? start' 'scene start'), (E '\[enhanced\] scene "?desert-air-train-in"? end' 'scene end')) }
  New-Row @{ Name = 'air-train-ride'; Start = 'desert-warp'; Seconds = 90; Shots = @(10, 60); Env = @{ JAK_ENHANCED_AIR_TRAIN = '1' }; MinSim = 0.75; MaxDiff = 7.0
    Levels = @('^des', '^cty'); Expect = @((E '\[air-train\] found' 'train found'), (E '\[air-train\] gate "?air-train' 'train state'),
      (E '\[air-train\] use train .* to "?ctyport-warp' 'train used')) }
  # the arena's first training: the orbs (arena-token) up and drawn (bright floor: 1.18 % measured,
  # 0.48 % since the composite's highlight shoulder (gfx-polish) rolls off near-white pixels)
  New-Row @{ Name = 'arena-training'; Start = 'game-start'; Seconds = 40; Env = @{ JAK_ENHANCED_CENSUS = '1' }; Shots = @(35)
    Levels = @('^wasstad'); Bright = 0.003; Expect = @((E '\[census\] .* tokens (?<v>\d+)' 'orbs alive' -MinV 1)) }
  # the playable characters: run, jumps, punch (Ashelin: her front kick), spin kick, the body-jump metric
  # (windowed: a picture of each body), then their guns (headless: the test scripts' lines)
  New-Row @{ Name = 'player-jak'; Seconds = 45; Env = @{ JAK_ENHANCED_PLAYER = 'jak'; JAK_ENHANCED_PLAYER_TEST = 'run' }; Shots = @(14)
    Expect = (Run-Expect 'jak' $null); Note = 'body-jump for Jak himself is data only (no limit)' }
  # (skins measured 0.07-0.16 m; a skin left at Jak's root and snapping back was 1-3 m)
  # (the shot can't tell the bodies apart: reruns of one build 0.89-0.99 (minimap shown or not at 14 s,
  # ground decals, pose), Jak against Sig in the same spot 0.90; the log lines check who is played)
  New-Row @{ Name = 'player-ashelin'; Seconds = 45; Env = @{ JAK_ENHANCED_PLAYER = 'ashelin'; JAK_ENHANCED_PLAYER_TEST = 'run' }; Shots = @(14)
    Expect = (Run-Expect 'ashelin' 0.4) }
  New-Row @{ Name = 'player-sig'; Seconds = 45; Env = @{ JAK_ENHANCED_PLAYER = 'sig'; JAK_ENHANCED_PLAYER_TEST = 'run' }; Shots = @(14)
    Expect = (Run-Expect 'sig' 0.4) }
  New-Row @{ Name = 'player-jak-gun'; Headless = $true; Seconds = 35; Env = @{ JAK_ENHANCED_PLAYER = 'jak'; JAK_ENHANCED_PLAYER_TEST = 'gun'; JAK_ENHANCED_UNLOCK = '1' }
    Expect = @((E '\[player-test\] \d+ gun-type \d+ using #t' 'gun out')) }
  New-Row @{ Name = 'player-ashelin-gun'; Headless = $true; Seconds = 45; Env = @{ JAK_ENHANCED_PLAYER = 'ashelin'; JAK_ENHANCED_ASHELIN_GUN_TEST = '1' }
    Expect = @((E '\[enhanced-player\] "?ashelin"? #<' 'ashelin loaded'), (E '\[ashelin-gun\] test: d-pad up \(take out\)' 'take out'),
      (E '\[ashelin-gun\] "?out"?$' 'gun out'), (E '\[ashelin-gun\] test: fire standing' 'fire standing'),
      (E '\[ashelin-gun\] fire .* arm-along-shot (?<v>[\d.]+)' 'shots (min arm-along-shot)' 20 -MinV 0.9 -Agg 'min'),
      (E '\[ashelin-gun\] reload$' 'magazine empty: reload'), (E '\[ashelin-gun\] reloaded (?<v>\d+)' 'reloaded rounds' -MinV 20 -MaxV 20),
      (E '\[ashelin-gun\] test: done' 'test done')) }
  New-Row @{ Name = 'player-sig-gun'; Headless = $true; Seconds = 45; Env = @{ JAK_ENHANCED_PLAYER = 'sig'; JAK_ENHANCED_SIG_GUN_TEST = '1' }
    Expect = @((E '\[enhanced-player\] "?sig"? #<' 'sig loaded'), (E '\[sig-gun\] test: d-pad right \(out\)' 'd-pad right (out)'),
      (E '\[sig-gun\] test: fire standing' 'fire standing'), (E '\[sig-gun\] aim [\d.]+ .*gun-along-shot (?<v>[\d.]+)' 'aim (min gun-along-shot)' 3 -MinV 0.9 -Agg 'min'),
      (E '\[sig-gun\] shot fire' 'firing'), (E '\[sig-gun\] test: walk firing' 'walk firing'), (E '\[sig-gun\] test: done' 'test done')) }
  # the weather script: clear, changing, thunderstorm, after the rain, clear
  New-Row @{ Name = 'weather-desert'; Seconds = 40; Env = @{ JAK_ENHANCED_WEATHER_SCRIPT = $weather }; Shots = @(16, 33); Expect = $weatherExpect }
  New-Row @{ Name = 'weather-spargus'; Start = 'wascitya-start'; Seconds = 40; Env = @{ JAK_ENHANCED_WEATHER_SCRIPT = $weather }; Shots = @(16, 33); Expect = $weatherExpect }
  # the Enhanced menu open from the start
  New-Row @{ Name = 'menu-open'; Seconds = 25; Env = @{ JAK_ENHANCED_MENU = '1' }; Shots = @(20); Expect = @((E '\[enhanced\] menu open' 'menu open'))
    Note = 'panel checked by its log line only (the baseline compare WARNs when the picture changes)' }
  # saving to slot 2 in the instance's own config dir, then a boot elsewhere that loads it
  New-Row @{ Name = 'save-write'; Headless = $true; Seconds = 30; Env = @{ JAK_ENHANCED_SAVE_TEST = '2' }; Save = $true
    Expect = @((E '\[enhanced\] save test: saving slot 2' 'save asked'), (E '\[enhanced\] auto-save save done .* slot 2' 'save done')) }
  New-Row @{ Name = 'save-load'; Shots = @(30); Start = 'wascitya-start'; Seconds = 45; Env = @{ JAK_ENHANCED_LOAD_TEST = '2' }; CfgFrom = 'save-write'
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
    '{0,-26} {1} {2} start={3} {4}s levels={5} env={6}' -f $r.Name, $r.Game, $(if ($r.Headless) { 'headless' } else { 'windowed' }), $r.Start, $r.Seconds,
      ($r.Levels -join '>'), (($r.Env.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ' ')
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
if (-not $Version) { $Version = if ($env:JAK_RELEASE_VERSION) { $env:JAK_RELEASE_VERSION } else { "$(& git -C $Repo describe --tags --always 2>$null)" } }
$Version = $Version -replace '[^\w.\-]', '_'
$baseRoot = Join-Path $Repo 'local\gate\baseline'
$baseDir = ''
if ($Baseline -ne 'none') {
  if ($Baseline -and (Test-Path (Join-Path $Baseline 'baseline.json'))) { $baseDir = $Baseline }
  elseif ($Baseline -and (Test-Path (Join-Path $baseRoot "$Baseline\baseline.json"))) { $baseDir = Join-Path $baseRoot $Baseline }
  elseif ($Baseline) { throw "-Baseline: no baseline.json in $Baseline or $baseRoot\$Baseline" }
  elseif (Test-Path $baseRoot) {
    $baseDir = Get-ChildItem $baseRoot -Directory | Where-Object { $_.Name -ne $Version -and (Test-Path (Join-Path $_.FullName 'baseline.json')) } |
      Sort-Object { (Get-Item (Join-Path $_.FullName 'baseline.json')).LastWriteTime } | Select-Object -Last 1 -ExpandProperty FullName
  }
}
$base = $null
if ($baseDir) { $base = [IO.File]::ReadAllText((Join-Path $baseDir 'baseline.json')) | ConvertFrom-Json }
# baseline.json: { version, commit, date, rows: [{ name, shots: [{ t, files: [one per rerun, newest last],
# luma, minSim, maxDiff, limits: "reruns n" | "from <version>" }] }] } (a first-format shot has one "file")
function Shot-Files($bs) { @(if ($bs.files) { $bs.files } elseif ($bs.file) { $bs.file }) }
# limits from 2+ rerun samples of a shot: each sample's best match among the others, the worst of those,
# less a margin that shrinks with more samples (sim 0.10 for 2, 0.07 for 3, 0.06 for 4; diff +4, +3, +2.7),
# clamped to sim 0.5-0.97, diff 3-30 (reruns of a still shot: worst sim 0.93-0.98; a 20 % box: 0.80)
function Sample-Limits([string]$dir, $files) {
  $worstSim = 1.0; $worstDiff = 0.0
  foreach ($a in $files) {
    $bSim = -1.0; $bDiff = 1e9
    foreach ($b in $files) {
      if ($a -eq $b) { continue }
      $c = [GateWin2]::Compare((Join-Path $dir $a), (Join-Path $dir $b))
      if ($c[1] -gt $bSim) { $bSim = $c[1] }; if ($c[0] -lt $bDiff) { $bDiff = $c[0] }
    }
    $worstSim = [Math]::Min($worstSim, $bSim); $worstDiff = [Math]::Max($worstDiff, $bDiff)
  }
  $k = $files.Count - 1
  @([Math]::Round([Math]::Min(30.0, [Math]::Max(3.0, $worstDiff + 2.0 + 2.0 / $k)), 1),
    [Math]::Round([Math]::Min(0.97, [Math]::Max(0.5, $worstSim - 0.04 - 0.06 / $k)), 3),
    ('reruns {0} (worst best-match sim {1:0.00}, diff {2:0.0})' -f $files.Count, $worstSim, $worstDiff))
}
# a shot's limits: from its baseline's rerun samples (worked out now, so older baselines follow the rule),
# else carried over from another baseline, else the row's
function Shot-Limits($r, $bs, [string]$dir) {
  $f = @(Shot-Files $bs | Where-Object { Test-Path (Join-Path $dir $_) })
  if ($f.Count -ge 2) { return (Sample-Limits $dir $f) }
  if ($bs -and $null -ne $bs.minSim -and $null -ne $bs.maxDiff) { return @([double]$bs.maxDiff, [double]$bs.minSim, "$($bs.limits)") }
  @([double]$r.MaxDiff, [double]$r.MinSim, 'row')
}
function Base-Shot($b, [string]$name, $t) {
  if (-not $b) { return $null }
  $br = $b.rows | Where-Object name -eq $name | Select-Object -First 1
  if ($br) { $br.shots | Where-Object { [double]$_.t -eq [double]$t } | Select-Object -First 1 }
}
if (-not $GkPath) {
  # a copy of the build's gk and DLLs of its own (as dev_bin.ps1 makes them, but in the gate's folder: a
  # dev_bin prune by another script between two rows can't take it away), so the build can relink meanwhile;
  # in a release the static build that ships (release.ps1 sets JAK_RELEASE_BIN), not the dev build
  $from = if ($Bin) { $Bin } elseif ($env:JAK_RELEASE_BIN) { $env:JAK_RELEASE_BIN } else { Join-Path $Repo 'out\build\Release\bin' }
  if (-not (Test-Path (Join-Path $from 'gk.exe'))) { throw "no gk.exe in $from (-Bin / JAK_RELEASE_BIN)" }
  $Bin = (Resolve-Path $from).Path
  $binCopy = Join-Path $OutDir 'bin'
  New-Item -ItemType Directory -Force $binCopy | Out-Null
  Get-ChildItem $from -File | Where-Object { $_.Name -eq 'gk.exe' -or $_.Extension -eq '.dll' } |
    ForEach-Object { [IO.File]::Copy($_.FullName, (Join-Path $binCopy $_.Name), $true) }
  $GkPath = Join-Path $binCopy 'gk.exe'
} else { $Bin = '' }
foreach ($p in @($GkPath, $ProjPath, $TestCfg)) { if (-not (Test-Path $p)) { throw "missing: $p" } }

if (-not ('GateWin2' -as [type])) {
  Add-Type -ReferencedAssemblies System.Drawing @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public static class GateWin2 {
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
  // luma of a picture scaled to w x h
  static double[] Gray(string path, int w, int h) {
    using (var src = new Bitmap(path)) using (var s = new Bitmap(w, h, PixelFormat.Format32bppArgb)) {
      using (var g = Graphics.FromImage(s)) {
        g.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBilinear;
        g.DrawImage(src, 0, 0, w, h);
      }
      var o = new double[w * h];
      for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        Color c = s.GetPixel(x, y); o[y * w + x] = 0.0722 * c.B + 0.7152 * c.G + 0.2126 * c.R;
      }
      return o;
    }
  }
  // two pictures at 64x36: mean absolute luma difference (0-255) and a mean SSIM over 8x8 windows (0-1)
  public static double[] Compare(string a, string b) {
    const int w = 64, h = 36; const double C1 = 6.5025, C2 = 58.5225;
    var A = Gray(a, w, h); var B = Gray(b, w, h);
    double diff = 0; for (int i = 0; i < A.Length; i++) diff += Math.Abs(A[i] - B[i]);
    diff /= A.Length;
    double sum = 0; int cnt = 0;
    for (int y = 0; y + 8 <= h; y += 4) for (int x = 0; x + 8 <= w; x += 4) {
      double ma = 0, mb = 0;
      for (int j = 0; j < 8; j++) for (int i = 0; i < 8; i++) { ma += A[(y + j) * w + x + i]; mb += B[(y + j) * w + x + i]; }
      ma /= 64; mb /= 64;
      double va = 0, vb = 0, cv = 0;
      for (int j = 0; j < 8; j++) for (int i = 0; i < 8; i++) {
        double da = A[(y + j) * w + x + i] - ma, db = B[(y + j) * w + x + i] - mb;
        va += da * da; vb += db * db; cv += da * db;
      }
      va /= 63; vb /= 63; cv /= 63;
      sum += ((2 * ma * mb + C1) * (2 * cv + C2)) / ((ma * ma + mb * mb + C1) * (va + vb + C2)); cnt++;
    }
    return new double[] { diff, sum / cnt };
  }
}
'@
}
[GateWin2]::SetProcessDPIAware() | Out-Null
Add-Type -AssemblyName System.Windows.Forms

function PortFree([int]$port) {
  $l = New-Object Net.Sockets.TcpListener ([Net.IPAddress]::Loopback), $port
  try { $l.Start(); $l.Stop(); return $true } catch { return $false }
}
# new lines of a row's log since the last read (gk keeps it open: read it shared)
function Update-Lines($it) {
  if (-not $it.Log -or -not (Test-Path $it.Log)) { return }
  try {
    $fs = [IO.File]::Open($it.Log, 'Open', 'Read', 'ReadWrite')
    try {
      if ($fs.Length -le $it.Off) { return }
      [void]$fs.Seek($it.Off, 'Begin')
      $buf = New-Object byte[] ($fs.Length - $it.Off)
      $n = $fs.Read($buf, 0, $buf.Length)
      $it.Off += $n
    } finally { $fs.Close() }
    $t = $it.Rest + [Text.Encoding]::UTF8.GetString($buf, 0, $n)
    $parts = $t -split "`r?`n"
    $it.Rest = $parts[-1]
    for ($i = 0; $i -lt $parts.Count - 1; $i++) { [void]$it.Lines.Add($parts[$i]) }
  } catch { }
}
function Saves-Hash([string]$cfg, [string]$game) {
  $d = Join-Path $cfg "OpenGOAL\$game\saves"
  if (-not (Test-Path $d)) { return @{} }
  $h = @{}
  foreach ($f in Get-ChildItem $d -Recurse -File) { $h[$f.FullName.Substring($d.Length)] = (Get-FileHash $f.FullName -Algorithm MD5).Hash }
  $h
}
function Short([string]$l) { ($l -replace '^\[\d\d:\d\d\] \[\w+\] ', '').Trim() | ForEach-Object { if ($_.Length -gt 110) { $_.Substring(0, 110) + '...' } else { $_ } } }

# frames lines: windowed "[enhanced] average N fps ... in <level>", headless "[camera] ... in <level>"
function Frame-Lines($it) {
  $out = New-Object System.Collections.ArrayList
  foreach ($l in $it.Lines) {
    if ($it.Row.Headless) {
      if ($l -match '^\[camera\] .* in (\S+)\s*$') { [void]$out.Add([pscustomobject]@{ Fps = 0.0; Level = $Matches[1] }) }
    } elseif ($l -match '\[enhanced\] average ([\d.]+) fps.* in (\S+)\s*$') {
      [void]$out.Add([pscustomobject]@{ Fps = [double]::Parse($Matches[1], $inv); Level = $Matches[2] })
    }
  }
  , $out
}
# the Expect lines, each after the one before: found, the line, the value
function Match-Expect($r, $lines) {
  $res = @(); $pos = 0
  foreach ($x in $r.Expect) {
    $found = 0; $first = -1; $val = $null; $text = ''
    $rx = New-Object Text.RegularExpressions.Regex $x.Re
    for ($i = $pos; $i -lt $lines.Count; $i++) {
      $m = $rx.Match($lines[$i])
      if (-not $m.Success) { continue }
      $found++
      $v = $null
      if ($m.Groups['v'].Success) { $v = [double]::Parse($m.Groups['v'].Value, $inv) }
      if ($first -lt 0) { $first = $i; $text = $lines[$i]; $val = $v }
      elseif ($null -ne $v -and (($x.Agg -eq 'min' -and $v -lt $val) -or ($x.Agg -eq 'max' -and $v -gt $val))) { $val = $v; $text = $lines[$i] }
    }
    $why = ''
    if ($found -lt $x.Min) { $why = "missing '$($x.Label)'$(if ($x.Min -gt 1) { " ($found/$($x.Min))" })" }
    elseif ($null -ne $x.MinV -and ($null -eq $val -or $val -lt $x.MinV)) { $why = "'$($x.Label)' = $val < $($x.MinV)" }
    elseif ($null -ne $x.MaxV -and ($null -eq $val -or $val -gt $x.MaxV)) { $why = "'$($x.Label)' = $val > $($x.MaxV)" }
    $res += [pscustomobject]@{ Label = $x.Label; Found = $found; Index = $first; Text = $text; Value = $val; Ok = (-not $why); Why = $why;
      Range = $(if ($null -ne $x.MinV -or $null -ne $x.MaxV) { "[$($x.MinV)..$($x.MaxV)]" } else { '' }) }
    if ($found -ge $x.Min) { $pos = $first + 1 }
  }
  $res
}
# the levels in order: the index in the frames lines where each one was seen, -1 when not
function Match-Levels($r, $frames) {
  $k = 0; $res = @()
  foreach ($re in $r.Levels) {
    $hit = -1
    while ($k -lt $frames.Count) { if ($frames[$k].Level -match $re) { $hit = $k; break }; $k++ }
    $res += [pscustomobject]@{ Re = $re; At = $hit; Level = $(if ($hit -ge 0) { $frames[$hit].Level } else { '' }) }
    if ($hit -lt 0) { break }
  }
  $res
}
# frames lines a row needs after its first one: explicit, else from the time it ran (every 5 s windowed,
# every 2 s headless)
function Lines-Needed($it, [double]$ran) {
  if ($it.Row.Lines -ge 0) { return $it.Row.Lines }
  if ($it.Row.Headless) { [Math]::Max(1, [int][Math]::Floor($ran / 2) - 4) } else { [Math]::Max(1, [int][Math]::Floor($ran / 5) - 3) }
}

# --- one row: start ----------------------------------------------------------------------------------
$area = [System.Windows.Forms.Screen]::PrimaryScreen.WorkingArea
$script:nextInstance = $FirstInstance
$script:usedInstances = @{}
function Start-Row($r, [int]$slot) {
  $gameNum = [int]$r.Game.Substring(3)
  $n = $script:nextInstance
  $tries = 0
  while ($script:usedInstances.ContainsKey($n) -or -not ((PortFree (8111 + $gameNum + 10 * $n)) -and (PortFree (8181 + 10 * $n)))) {
    $n++; $tries++
    if ($n -gt $LastInstance) { $n = $FirstInstance }
    if ($tries -gt 3 * ($LastInstance - $FirstInstance + 1)) { return $null }
  }
  $script:nextInstance = if ($n + 1 -gt $LastInstance) { $FirstInstance } else { $n + 1 }
  $script:usedInstances[$n] = $true
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
  $psi.Arguments = "--game $($r.Game) --proj-path `"$ProjPath`" --config-path `"$cfg`"$(if ($r.Headless) { ' --no-display' }) -- -boot -fakeiso -debug"
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
    Lines = (New-Object System.Collections.ArrayList); Off = [int64]0; Rest = ''; NextCheck = $t0; Early = $false;
    Hwnd = [IntPtr]::Zero; Ready = $null; End = $null; ShotsDone = @(); Shots = @(); Tries = @{}; Done = $false; SavesBefore = $savesBefore;
    Exit = ''; Fail = @(); Info = @(); Ok = @(); Warn = @(); Exp = @(); Lv = @(); Ran = 0.0; MetAt = $null; FreeAt = $null }
}

# --- one row: every poll -------------------------------------------------------------------------------
$SWP = 0x0001 -bor 0x0004 -bor 0x0010   # NOSIZE | NOZORDER | NOACTIVATE
$fg = [GateWin2]::GetForegroundWindow()
$readyRe = '\[enhanced\] average'
function Poll-Row($it) {
  $r = $it.Row
  $now = Get-Date
  if ($it.Proc.HasExited) { $it.Done = $true; return }
  if (-not $r.Headless -and $it.Hwnd -eq [IntPtr]::Zero) {
    $it.Proc.Refresh()
    if ($it.Proc.MainWindowHandle -ne [IntPtr]::Zero) {
      $it.Hwnd = $it.Proc.MainWindowHandle
      [GateWin2]::SetWindowPos($it.Hwnd, [IntPtr]::Zero, $it.X, $it.Y, 0, 0, $SWP) | Out-Null
      if ($fg -ne [IntPtr]::Zero) { [GateWin2]::SetForegroundWindow($fg) | Out-Null }
    }
  }
  if (-not $it.Log) {
    $it.Log = Get-ChildItem (Join-Path $ProjPath 'log') -Filter "$($r.Game)_i$($it.N).*.log" -ErrorAction SilentlyContinue |
      Where-Object { $_.LastWriteTime -ge $it.T0.AddSeconds(-2) } | Sort-Object LastWriteTime | Select-Object -Last 1 -ExpandProperty FullName
  }
  if (-not $it.Ready) {
    Update-Lines $it
    $hit = $false
    foreach ($l in $it.Lines) {
      if ($r.Headless) { if ($l -match '^\[camera\] .* in (\S+)\s*$' -and $Matches[1] -ne 'none') { $hit = $true; break } }
      elseif ($l -match $readyRe) { $hit = $true; break }
    }
    if ($hit) {
      $it.Ready = $now; $it.End = $now.AddSeconds($r.Seconds)
    } elseif (($now - $it.T0).TotalSeconds -gt $BootTimeout) {
      $it.Fail += "no frames after $BootTimeout s"; $it.Done = $true
    }
    return
  }
  # screenshots when due
  foreach ($s in $r.Shots) {
    if ($it.ShotsDone -notcontains $s -and ($now - $it.Ready).TotalSeconds -ge $s) {
      $it.ShotsDone += $s
      $bmp = if ($it.Hwnd -ne [IntPtr]::Zero) { [GateWin2]::Capture($it.Hwnd) } else { $null }
      if (-not $bmp) {
        # (the window handle first seen can be gone: ask the process again)
        $it.Proc.Refresh()
        if ($it.Proc.MainWindowHandle -ne [IntPtr]::Zero) { $it.Hwnd = $it.Proc.MainWindowHandle; $bmp = [GateWin2]::Capture($it.Hwnd) }
      }
      if ($bmp) {
        $st = [GateWin2]::Stats($bmp)
        $file = "$($r.Name)-$s.png"
        [GateWin2]::SaveSmall($bmp, (Join-Path $OutDir $file), 320)
        $bmp.Dispose()
        $it.Shots += [pscustomobject]@{ T = $s; File = $file; Luma = $st[0]; Bright = $st[1]; Diff = -1.0; Sim = -1.0; Base = '' }
      } else {
        # (no picture this time - the window busy or without a client area for a while, as in a level
        # load: up to 16 tries, half a second apart, before it counts as missing)
        $it.Tries[$s] = 1 + [int]$it.Tries[$s]
        if ($it.Tries[$s] -lt 16 -and $now.AddSeconds(1) -lt $it.End) {
          $it.ShotsDone = @($it.ShotsDone | Where-Object { $_ -ne $s })
        } else {
          $rc = New-Object GateWin2+RECT
          $ok = $it.Hwnd -ne [IntPtr]::Zero -and [GateWin2]::GetClientRect($it.Hwnd, [ref]$rc)
          $it.Info += "shot $s s failed: hwnd $($it.Hwnd) client $(if ($ok) { "$($rc.R - $rc.L)x$($rc.B - $rc.T)" } else { 'none' })"
          $it.Shots += [pscustomobject]@{ T = $s; File = ''; Luma = -1; Bright = 0; Diff = -1.0; Sim = -1.0; Base = '' }
        }
      }
    }
  }
  if ($now -ge $it.End) { $it.Done = $true; return }
  # done early: every check met (the expected lines and levels, the shots, enough frames) for Settle s
  if (-not $NoEarly -and $now -ge $it.NextCheck) {
    $it.NextCheck = $now.AddSeconds(2)
    if ($it.ShotsDone.Count -lt $r.Shots.Count) { return }
    Update-Lines $it
    $ex = @(Match-Expect $r $it.Lines)
    if (@($ex | Where-Object { -not $_.Ok }).Count) { $it.MetAt = $null; return }
    $fr = Frame-Lines $it
    $lv = @(Match-Levels $r $fr)
    if ($lv.Count -lt $r.Levels.Count -or @($lv | Where-Object { $_.At -lt 0 }).Count) { $it.MetAt = $null; return }
    $ran = ($now - $it.Ready).TotalSeconds
    if ($fr.Count - 1 -lt (Lines-Needed $it $ran)) { return }
    if (-not $it.MetAt) { $it.MetAt = $now; return }
    if (($now - $it.MetAt).TotalSeconds -ge $Settle) { $it.Early = $true; $it.Done = $true }
  }
}

# --- one row: the verdict ------------------------------------------------------------------------------
function Judge($it) {
  $r = $it.Row
  $p = $it.Proc
  $alive = -not $p.HasExited
  if (-not $alive) { $it.Exit = '0x{0:X8}' -f $p.ExitCode; $it.Fail += "exited $($it.Exit)" }
  # (event 1000's PID is a UInt32: decimal; only its Message text shows it in hex)
  $ev = Get-WinEvent -FilterHashtable @{ LogName = 'Application'; Id = 1000; StartTime = $it.T0 } -ErrorAction SilentlyContinue |
    Where-Object { $_.Properties.Count -gt 8 -and "$($_.Properties[0].Value)" -eq 'gk.exe' -and
      $(try { $v = "$($_.Properties[8].Value)"; $(if ($v -match '^0x') { [Convert]::ToInt64($v.Substring(2), 16) } else { [int64]$v }) -eq $it.Pid } catch { $false }) } |
    Select-Object -First 1
  if ($ev) { $it.Fail += "event1000 module=$($ev.Properties[3].Value) code=$($ev.Properties[6].Value) offset=$($ev.Properties[7].Value)" }
  Update-Lines $it
  if (-not $it.Log) { $it.Fail += 'no log' }
  $it.Ran = if ($it.Ready) { ((Get-Date) - $it.Ready).TotalSeconds } else { 0 }
  # frames lines after the boot (the first one is the boot's)
  $fr = Frame-Lines $it
  $run = @($fr | Select-Object -Skip 1)
  $need = Lines-Needed $it $it.Ran
  if ($r.Headless) { $it.Info += "frames n=$($run.Count)" }
  else {
    $it.Info += 'fps n={0} avg={1:0} min={2:0}' -f $run.Count, $(if ($run.Count) { ($run.Fps | Measure-Object -Average).Average } else { 0 }),
      $(if ($run.Count) { ($run.Fps | Measure-Object -Minimum).Minimum } else { 0 })
  }
  if ($run.Count -lt $need) { $it.Fail += "frames lines $($run.Count) < $need" }
  if ($run.Count) {
    if (-not $r.Headless) {
      $avg = ($run.Fps | Measure-Object -Average).Average
      if ($avg -lt $r.MinFps) { $it.Fail += ('fps avg {0:0.0} < {1}' -f $avg, $r.MinFps) }
    }
    $last = $run[-1].Level
    if ($last -in 'none', 'title') { $it.Fail += "last level $last" }
  }
  # levels in order (the boot's line too: a level change can come before the first line of the run)
  $it.Lv = @(Match-Levels $r $fr)
  $lvBad = $false
  for ($i = 0; $i -lt $r.Levels.Count; $i++) {
    if ($i -ge $it.Lv.Count -or $it.Lv[$i].At -lt 0) { $it.Fail += "no level $($r.Levels[$i])$(if ($i -gt 0) { ' after the one before' })"; $lvBad = $true; break }
  }
  if (-not $lvBad -and $it.Lv.Count) { $it.Ok += 'levels ' + (($it.Lv | ForEach-Object { "$($_.Level)~/$($_.Re)/" }) -join ' > ') }
  if ($fr.Count) { $it.Info += 'seen ' + ((@($fr | ForEach-Object Level) | Select-Object -Unique) -join '>') }
  # expected log lines in order, with their values
  $it.Exp = @(Match-Expect $r $it.Lines)
  $vals = @{}
  foreach ($x in $it.Exp) {
    $vals[$x.Label] = $x.Value
    if (-not $x.Ok) { $it.Fail += $x.Why; continue }
    $it.Ok += "$($x.Label)$(if ($x.Found -gt 1) { " x$($x.Found)" })$(if ($null -ne $x.Value) { "=$($x.Value)$($x.Range)" })"
  }
  if ($r.Check) {
    foreach ($c in @(& $r.Check $vals)) { if ("$c".StartsWith('!')) { $it.Fail += "$c".Substring(1) } else { $it.Ok += "$c" } }
  }
  # screenshots: not black; the last one bright enough
  foreach ($s in $it.Shots) {
    if ($s.Luma -lt 0) { $it.Fail += "no screenshot at $($s.T)s" }
    elseif ($s.Luma -le 8) { $it.Fail += ('black screenshot at {0}s (luma {1:0.0})' -f $s.T, $s.Luma) }
  }
  if ($it.Shots.Count -lt $r.Shots.Count -and $alive -and $it.Ready) { $it.Fail += "screenshots $($it.Shots.Count)/$($r.Shots.Count)" }
  if ($r.Bright -gt 0 -and $it.Shots.Count) {
    $b = $it.Shots[-1].Bright
    if ($b -lt $r.Bright) { $it.Fail += ('bright pixels {0:0.00}% < {1:0.00}%' -f (100 * $b), (100 * $r.Bright)) }
    else { $it.Ok += ('bright {0:0.00}% >= {1:0.00}%' -f (100 * $b), (100 * $r.Bright)) }
  }
  if ($r.Save) {
    $after = Saves-Hash $it.Cfg $r.Game
    $changed = @($after.Keys | Where-Object { -not $it.SavesBefore.ContainsKey($_) -or $it.SavesBefore[$_] -ne $after[$_] })
    if ($changed.Count -eq 0) { $it.Fail += 'no save file written' } else { $it.Ok += "saved $($changed -join ',')" }
  }
  if ($it.Shots.Count) { $it.Info += 'luma ' + (($it.Shots | ForEach-Object { '{0}s={1:0}' -f $_.T, $_.Luma }) -join ' ') }
  # the screenshots against the baseline's (a big change: WARN)
  # (against each rerun sample of the shot; the best match counts, so a minimap shown in one rerun and
  # not in another doesn't warn)
  if ($base) {
    foreach ($s in $it.Shots | Where-Object File) {
      $bs = Base-Shot $base $r.Name $s.T
      if (-not $bs) { $it.Info += "no baseline shot at $($s.T)s in $($base.version)"; continue }
      $files = @(Shot-Files $bs | ForEach-Object { Join-Path $baseDir $_ } | Where-Object { Test-Path $_ })
      if (-not $files.Count) { $it.Info += "baseline shot files at $($s.T)s missing in $baseDir"; continue }
      $best = $null
      foreach ($bf in $files) {
        $c = [GateWin2]::Compare($bf, (Join-Path $OutDir $s.File))
        if (-not $best -or $c[1] -gt $best[1] -or ($c[1] -eq $best[1] -and $c[0] -lt $best[0])) { $best = @($c[0], $c[1], $bf) }
      }
      $s.Diff = $best[0]; $s.Sim = $best[1]; $s.Base = $best[2]
      $lim = Shot-Limits $r $bs $baseDir
      $txt = '{0}s diff {1:0.0} sim {2:0.00} vs {3} (best of {4}; limits diff {5:0.0} sim {6:0.00} {7})' -f $s.T, $best[0], $best[1], $base.version, $files.Count, $lim[0], $lim[1], $lim[2]
      if ($best[0] -gt $lim[0] -or $best[1] -lt $lim[1]) { $it.Warn += $txt } else { $it.Info += $txt }
    }
  }
  if ($it.Early) { $it.Info += ('done early at {0:0} s of {1}' -f $it.Ran, $r.Seconds) }
  # a failure's evidence: the log's last lines when the game died
  if (-not $alive -and $it.Lines.Count) { $it.Info += 'tail: ' + ((@($it.Lines | Where-Object { $_ }) | Select-Object -Last 3 | ForEach-Object { Short $_ }) -join ' | ') }
}
function Verdict($it) { if ($it.Fail.Count) { 'FAIL' } elseif ($it.Warn.Count) { 'WARN' } else { 'PASS' } }

# --- run ----------------------------------------------------------------------------------------------
$before = @(Get-Process gk -ErrorAction SilentlyContinue | ForEach-Object Id)
$queue = New-Object System.Collections.ArrayList
# (long rows first in each kind, so the parallel headless slots end together)
$rows | Sort-Object @{ e = { $_.Headless } }, @{ e = { $_.Seconds }; Descending = $true } | ForEach-Object { [void]$queue.Add($_) }
$running = New-Object System.Collections.ArrayList
$finished = New-Object System.Collections.ArrayList
$tStart = Get-Date
"GATE start rows=$($rows.Count) windowed=$(@($rows | Where-Object { -not $_.Headless }).Count)x$Instances headless=$(@($rows | Where-Object Headless).Count)x$Parallel instances=$FirstInstance-$LastInstance version=$Version baseline=$(if ($base) { $base.version } else { '-' }) gk=$GkPath$(if ($Bin) { " (from $Bin, $((Get-Item (Join-Path $Bin 'gk.exe')).LastWriteTime.ToString('yyyy-MM-dd HH:mm')))" }) proj=$ProjPath out=$OutDir"
try {
  while ($queue.Count -or $running.Count) {
    # start rows into free slots (a row waits for the row its config comes from)
    foreach ($kind in @($false, $true)) {
      $cap = if ($kind) { $Parallel } else { $Instances }
      while (@($running | Where-Object { $_.Row.Headless -eq $kind }).Count -lt $cap) {
        $next = $null
        foreach ($q in $queue) {
          if ($q.Headless -ne $kind) { continue }
          if (-not $q.CfgFrom -or ($finished | Where-Object { $_.Row.Name -eq $q.CfgFrom })) { $next = $q; break }
        }
        if (-not $next) { break }
        $slot = 0
        if (-not $kind) { $used = @($running | Where-Object { -not $_.Row.Headless } | ForEach-Object Slot); while ($used -contains $slot) { $slot++ } }
        $it = Start-Row $next $slot
        if (-not $it) {
          # (no free instance number now: try again after a row ends)
          if ($running.Count -eq 0) { throw "no free instance ports in $FirstInstance-$LastInstance" }
          break
        }
        [void]$queue.Remove($next)
        [void]$running.Add($it)
        "GATE run {0} {1} instance={2} pid={3}" -f $next.Name, $(if ($kind) { 'headless' } else { 'windowed' }), $it.N, $it.Pid
      }
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
        $v = Verdict $it
        'GATE {0} {1} {2}' -f $it.Row.Name, $v, ((@($it.Fail) + @($it.Warn) + @($it.Ok) + @($it.Info)) -join '; ')
        # (the next launch on this instance number can find its port still taken for a moment: it is
        # given back 3 s later)
        $it.FreeAt = (Get-Date).AddSeconds(3)
      }
    }
    foreach ($it in $finished) {
      if ($it.FreeAt -and (Get-Date) -ge $it.FreeAt) { $script:usedInstances.Remove($it.N); $it.FreeAt = $null }
    }
  }
} finally {
  foreach ($it in $running) {
    if (-not $it.Proc.HasExited -and $before -notcontains $it.Pid) { Stop-Process -Id $it.Pid -Force -ErrorAction SilentlyContinue }
  }
}

# --- baseline ------------------------------------------------------------------------------------------
# Each run adds its shots to the version's baseline as one more rerun sample (newest -BaselineKeep kept);
# with 2+ samples a shot's limits come from how much they differ (Sample-Limits); with one sample the
# limits of the baseline compared against (same row and shot) are carried over, else the row's apply.
if ($MakeBaseline) {
  $bd = Join-Path $baseRoot $Version
  New-Item -ItemType Directory -Force $bd | Out-Null
  $stamp = Split-Path $OutDir -Leaf
  $old = Join-Path $bd 'baseline.json'
  $prev = if (Test-Path $old) { [IO.File]::ReadAllText($old) | ConvertFrom-Json } else { $null }
  # (a run of some rows only (-Only) keeps the other rows of that version's baseline, a FAILed row its old shots)
  $saved = @($finished | Where-Object { (Verdict $_) -ne 'FAIL' -and @($_.Shots | Where-Object File).Count })
  $brows = @(if ($prev) { $prev.rows | Where-Object { $n = $_.name; -not ($saved | Where-Object { $_.Row.Name -eq $n }) } })
  foreach ($it in @($finished | Where-Object { (Verdict $_) -eq 'FAIL' -and @($_.Shots | Where-Object File).Count })) {
    "GATE baseline: $($it.Row.Name) FAILed, its shots not added$(if (Base-Shot $prev $it.Row.Name $it.Shots[0].T) { ' (the old ones kept)' })"
  }
  foreach ($it in $saved) {
    $shots = @(foreach ($s in @($it.Shots | Where-Object File)) {
      $ps = Base-Shot $prev $it.Row.Name $s.T
      $nf = '{0}-{1}-{2}.png' -f $it.Row.Name, $s.T, $stamp
      Copy-Item (Join-Path $OutDir $s.File) (Join-Path $bd $nf) -Force
      $files = @(@(Shot-Files $ps | Where-Object { Test-Path (Join-Path $bd $_) }) + $nf)
      if ($files.Count -gt $BaselineKeep) {
        $files[0..($files.Count - $BaselineKeep - 1)] | ForEach-Object { Remove-Item (Join-Path $bd $_) -ErrorAction SilentlyContinue }
        $files = @($files | Select-Object -Last $BaselineKeep)
      }
      $e = [ordered]@{ t = $s.T; files = $files; luma = [Math]::Round($s.Luma, 1) }
      if ($files.Count -ge 2) {
        $sl = Sample-Limits $bd $files
        $e.maxDiff = $sl[0]; $e.minSim = $sl[1]; $e.limits = $sl[2]
      } else {
        $cs = Base-Shot $base $it.Row.Name $s.T
        if ($cs -and $null -ne $cs.minSim) {
          $e.minSim = $cs.minSim; $e.maxDiff = $cs.maxDiff
          $e.limits = if ("$($cs.limits)" -like 'from *') { "$($cs.limits)" } else { "from $($base.version)" }
        }
      }
      $e
    })
    $brows += [ordered]@{ name = $it.Row.Name; verdict = (Verdict $it); shots = $shots }
    $ls = @($shots | ForEach-Object { if ($null -ne $_.minSim) { '{0}s n={1} sim>={2:0.00} diff<={3:0.0}' -f $_.t, $_.files.Count, $_.minSim, $_.maxDiff } else { "$($_.t)s n=$($_.files.Count) row limits" } })
    "GATE baseline $($it.Row.Name): $($ls -join ', ')"
  }
  $bj = [ordered]@{ version = $Version; commit = "$(& git -C $Repo rev-parse --short HEAD 2>$null)"; date = (Get-Date -Format 'yyyy-MM-dd HH:mm'); rows = $brows }
  [IO.File]::WriteAllText((Join-Path $bd 'baseline.json'), ($bj | ConvertTo-Json -Depth 6))
  "GATE baseline $Version saved: $($brows.Count) rows with shots in $bd"
}

# --- report ---------------------------------------------------------------------------------------------
$pass = @($finished | Where-Object { (Verdict $_) -eq 'PASS' }).Count
$warn = @($finished | Where-Object { (Verdict $_) -eq 'WARN' }).Count
$fail = @($finished | Where-Object { (Verdict $_) -eq 'FAIL' }).Count
$minutes = ((Get-Date) - $tStart).TotalMinutes
$commit = (& git -C $Repo log -1 --format='%h %s' 2>$null)
$txt = @(('GATE commit={0} rows={1} pass={2} warn={3} fail={4} minutes={5:0.0} baseline={6} gk={7}' -f $commit, $finished.Count, $pass, $warn, $fail, $minutes, $(if ($base) { $base.version } else { '-' }), $(if ($Bin) { $Bin } else { $GkPath })))
foreach ($it in $finished) {
  $txt += '{0,-26} {1} {2} {3:0}s{4}' -f $it.Row.Name, (Verdict $it), $(if ($it.Row.Headless) { 'headless' } else { 'windowed' }), $it.Ran, $(if ($it.Early) { ' (early)' })
  foreach ($f in $it.Fail) { $txt += "    FAIL $f" }
  foreach ($w in $it.Warn) { $txt += "    WARN $w" }
  foreach ($x in $it.Exp) { $txt += '    {0} {1}{2}{3}: {4}' -f $(if ($x.Ok) { 'ok  ' } else { 'FAIL' }), $x.Label, $(if ($x.Found -gt 1) { " x$($x.Found)" }), $(if ($null -ne $x.Value) { " = $($x.Value) $($x.Range)" }), $(if ($x.Text) { Short $x.Text } else { '-' }) }
  foreach ($o in @($it.Ok | Where-Object { $_ -like 'levels *' -or $_ -like 'bright *' -or $_ -like 'saved *' -or $_ -like 'nap hours *' })) { $txt += "    ok   $o" }
  $txt += '    ' + ($it.Info -join '; ')
  if ($it.Row.Note) { $txt += "    note: $($it.Row.Note)" }
  $txt += '    log=' + $(if ($it.Log) { $it.Log } else { '-' })
}
[IO.File]::WriteAllLines((Join-Path $OutDir 'report.txt'), [string[]]$txt)
$enc = { param($s) [Net.WebUtility]::HtmlEncode("$s") }
$uri = { param($p) ([Uri]$p).AbsoluteUri }
$h = New-Object Text.StringBuilder
[void]$h.Append(@"
<!doctype html><html><head><meta charset="utf-8"><title>Release gate</title><style>
body{font:14px system-ui,sans-serif;margin:16px;background:#16181c;color:#e6e6e6}
table{border-collapse:collapse;width:100%}td,th{border-bottom:1px solid #333;padding:6px;vertical-align:top;text-align:left}
.PASS{color:#6fcf7f;font-weight:600}.FAIL{color:#ff6b6b;font-weight:600}.WARN{color:#f2c94c;font-weight:600}
img{width:160px;margin-right:4px}figure{display:inline-block;margin:0 8px 6px 0}figcaption{font-size:12px;color:#9aa}
small{color:#9aa}ul{margin:0;padding-left:16px}li.bad{color:#ff6b6b}code{color:#cfd6e0;font-size:12px}</style></head><body>
<h1>Release gate</h1><p>$(& $enc $commit) &middot; $pass pass, $warn warn, $fail fail &middot; $('{0:0.0}' -f $minutes) min &middot; baseline $(& $enc $(if ($base) { $base.version } else { 'none' })) &middot; gk $(& $enc $(if ($Bin) { $Bin } else { $GkPath })) &middot; $(Get-Date -Format 'yyyy-MM-dd HH:mm')</p>
<table><tr><th>Row</th><th>Result</th><th>Checks (matched lines and values)</th><th>Screenshots (baseline | now)</th></tr>
"@)
foreach ($it in $finished) {
  $v = Verdict $it
  $li = New-Object Text.StringBuilder
  foreach ($f in $it.Fail) { [void]$li.Append("<li class=`"bad`">$(& $enc $f)</li>") }
  foreach ($w in $it.Warn) { [void]$li.Append("<li class=`"WARN`">$(& $enc $w)</li>") }
  foreach ($x in $it.Exp | Where-Object Ok) {
    [void]$li.Append("<li>$(& $enc $x.Label)$(if ($x.Found -gt 1) { " x$($x.Found)" })$(if ($null -ne $x.Value) { " = <b>$(& $enc $x.Value)</b> $(& $enc $x.Range)" })<br><code>$(& $enc (Short $x.Text))</code></li>")
  }
  foreach ($o in @($it.Ok | Where-Object { $_ -like 'levels *' -or $_ -like 'bright *' -or $_ -like 'saved *' -or $_ -like 'nap hours *' })) { [void]$li.Append("<li>$(& $enc $o)</li>") }
  $imgs = New-Object Text.StringBuilder
  foreach ($s in $it.Shots | Where-Object File) {
    $cap = "$($s.T) s"
    if ($s.Base) { $cap += (' &middot; diff {0:0.0} sim {1:0.00}' -f $s.Diff, $s.Sim) }
    $bimg = ''
    if ($s.Base) { $bimg = '<img src="{0}" title="baseline">' -f (& $uri $s.Base) }
    [void]$imgs.Append(('<figure>{0}<img src="{1}" title="now"><figcaption>{2}</figcaption></figure>' -f $bimg, $s.File, $cap))
  }
  $kind = if ($it.Row.Headless) { 'headless' } else { 'windowed' }
  [void]$h.Append("<tr><td>$(& $enc $it.Row.Name)<br><small>$(& $enc $it.Row.Start) &middot; $kind &middot; $('{0:0}' -f $it.Ran) s$(if ($it.Early) { ' (early)' })</small></td><td class=`"$v`">$v</td><td><ul>$li</ul><small>$(& $enc ($it.Info -join '; '))$(if ($it.Row.Note) { '<br>note: ' + (& $enc $it.Row.Note) })</small></td><td>$imgs</td></tr>`n")
}
[void]$h.Append('</table></body></html>')
[IO.File]::WriteAllText((Join-Path $OutDir 'report.html'), $h.ToString())
'GATE RESULT pass={0} warn={1} fail={2} minutes={3:0.0} report={4}' -f $pass, $warn, $fail, $minutes, (Join-Path $OutDir 'report.html')
$global:LASTEXITCODE = if ($fail) { 1 } else { 0 }
} finally {
  [Threading.Thread]::CurrentThread.CurrentCulture = $prevCulture
}
if ($MyInvocation.MyCommand.CommandType -eq 'ExternalScript') { exit $global:LASTEXITCODE }
