# Jak Enhanced Trilogy: adds the launcher to Steam as a non-Steam game, for every Steam account on this
# computer (the setup's "Add to Steam" box).
#   add_to_steam.ps1 -Root <the install folder>
# Steam keeps its list in userdata\<account>\config\shortcuts.vdf (binary VDF) and writes it back from
# memory when it quits: so a running Steam is closed first, or the new entry would be lost. Exit codes:
# 0 added (or already there), 1 no Steam found.
param([Parameter(Mandatory = $true)][string]$Root, [string]$SteamPath)
$ErrorActionPreference = 'Stop'
$Root = $Root.TrimEnd('\')
$name = 'Jak Enhanced Trilogy'
$exe = '"' + $Root + '\Jak Enhanced Trilogy.bat"'
$startDir = '"' + $Root + '\"'
$icon = $Root + '\data\game\assets\enhanced\art\jak-enhanced.ico'

$steam = if ($SteamPath) { $SteamPath } else {
  (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -Name SteamPath -ErrorAction SilentlyContinue).SteamPath
}
if (-not $steam -or -not (Test-Path -LiteralPath (Join-Path $steam 'userdata'))) {
  Write-Host 'Steam not found: nothing added.'
  exit 1
}
$steam = $steam.Replace('/', '\')

$reopen = $false
if (-not $SteamPath -and (Get-Process steam -ErrorAction SilentlyContinue)) {
  Write-Host 'Closing Steam (it opens again by itself, with the game in the library)...'
  Start-Process -FilePath (Join-Path $steam 'steam.exe') -ArgumentList '-shutdown'
  $until = (Get-Date).AddSeconds(30)
  while ((Get-Process steam -ErrorAction SilentlyContinue) -and (Get-Date) -lt $until) { Start-Sleep -Milliseconds 500 }
  $reopen = $true
}

# the id Steam gives a non-Steam game: CRC32 of exe + name, top bit set
function Get-Crc32([byte[]]$bytes) {
  $crc = [uint32]::MaxValue
  foreach ($b in $bytes) {
    $crc = $crc -bxor $b
    for ($i = 0; $i -lt 8; $i++) {
      if ($crc -band 1) { $crc = ($crc -shr 1) -bxor [uint32]3988292384 } else { $crc = $crc -shr 1 }
    }
  }
  return $crc -bxor [uint32]::MaxValue
}
$utf8 = New-Object Text.UTF8Encoding $false
$appId = [uint32]((Get-Crc32 $utf8.GetBytes($exe + $name)) -bor [uint32]2147483648)

# one entry, numbered n, in binary VDF: 0x01 string, 0x02 int32, 0x00 map, 0x08 end of a map
function New-Entry([int]$n) {
  $s = New-Object IO.MemoryStream
  function Put([byte]$type, [string]$key) { $s.WriteByte($type); $k = $utf8.GetBytes($key); $s.Write($k, 0, $k.Length); $s.WriteByte(0) }
  function Str([string]$key, [string]$value) { Put 1 $key; $v = $utf8.GetBytes($value); $s.Write($v, 0, $v.Length); $s.WriteByte(0) }
  function Int([string]$key, [uint32]$value) { Put 2 $key; $s.Write([BitConverter]::GetBytes($value), 0, 4) }
  Put 0 ([string]$n)
  Int 'appid' $appId
  Str 'AppName' $name
  Str 'Exe' $exe
  Str 'StartDir' $startDir
  Str 'icon' $icon
  Str 'ShortcutPath' ''
  Str 'LaunchOptions' ''
  Int 'IsHidden' 0
  Int 'AllowDesktopConfig' 1
  Int 'AllowOverlay' 1
  Int 'OpenVR' 0
  Int 'Devkit' 0
  Str 'DevkitGameID' ''
  Int 'DevkitOverrideAppID' 0
  Int 'LastPlayTime' 0
  Str 'FlatpakAppID' ''
  Put 0 'tags'
  $s.WriteByte(8)
  $s.WriteByte(8)
  return $s.ToArray()
}

$added = 0
foreach ($account in Get-ChildItem -LiteralPath (Join-Path $steam 'userdata') -Directory) {
  if ($account.Name -eq '0' -or $account.Name -eq 'ac') { continue }
  $config = Join-Path $account.FullName 'config'
  [void][IO.Directory]::CreateDirectory($config)
  $file = Join-Path $config 'shortcuts.vdf'
  $head = [byte[]](@(0) + $utf8.GetBytes('shortcuts') + @(0))
  if (Test-Path -LiteralPath $file) {
    $bytes = [IO.File]::ReadAllBytes($file)
    $text = [Text.Encoding]::GetEncoding(28591).GetString($bytes)
    if ($text.Contains($exe)) { Write-Host "Steam account $($account.Name): already there."; continue }
    # (the list ends with 0x08 0x08: the entries' map and the root closing)
    if ($bytes.Length -lt $head.Length + 2 -or $bytes[-1] -ne 8 -or $bytes[-2] -ne 8) {
      Write-Host "Steam account $($account.Name): shortcuts.vdf not as expected, left alone."
      continue
    }
    # the next number: one past the entries already there (at least their count)
    $n = ([regex]::Matches($text, "\x00(\d+)\x00\x02appid", 'IgnoreCase') | ForEach-Object { [int]$_.Groups[1].Value } |
          Measure-Object -Maximum).Maximum
    $n = if ($null -eq $n) { 0 } else { $n + 1 }
    Copy-Item -LiteralPath $file -Destination "$file.bak" -Force
    $body = $bytes[0..($bytes.Length - 3)]
  } else {
    $n = 0
    $body = $head
  }
  $out = New-Object IO.MemoryStream
  $out.Write([byte[]]$body, 0, $body.Length)
  $entry = New-Entry $n
  $out.Write($entry, 0, $entry.Length)
  $out.WriteByte(8)
  $out.WriteByte(8)
  [IO.File]::WriteAllBytes($file, $out.ToArray())
  Write-Host "Steam account $($account.Name): added."
  $added++
}
if ($reopen) { Start-Process -FilePath (Join-Path $steam 'steam.exe') }
exit 0
