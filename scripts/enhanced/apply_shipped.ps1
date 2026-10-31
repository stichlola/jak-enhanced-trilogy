# Jak Enhanced Trilogy: puts the files a player can edit (the main world's objects, lights and painted
# grass, the terrain pictures) in place from data\shipped, without losing the player's own edits.
#   apply_shipped.ps1 -Proj <the release's data folder>
# The launcher runs it once per version (make_release.py keeps these files out of their place in the
# release, so no update can overwrite them, not even an older release's update.ps1 or the installer).
# File by file, with applied.json (what was last put here) and manifest.json (this release's hash and
# the hashes every earlier version of the file had):
#   not here, never put here        -> installed
#   not here, but it had been put   -> the player removed it: stays removed
#   same as what was put, or as any -> not edited: replaced by the new one
#   earlier version
#   anything else                   -> the player's: kept (the new one stays in data\shipped)
# Text files are compared with their line ends made alike (the release has CRLF, the game writes LF).
param([Parameter(Mandatory = $true)][string]$Proj)
$ErrorActionPreference = 'Stop'
$Proj = $Proj.TrimEnd('\')
$shipped = Join-Path $Proj 'shipped'
$sha = [Security.Cryptography.SHA256]::Create()
$latin = [Text.Encoding]::GetEncoding(28591)

function Get-FileKey([string]$file) {
  $bytes = [IO.File]::ReadAllBytes($file)
  if ($file.EndsWith('.json', [StringComparison]::OrdinalIgnoreCase)) {
    $bytes = $latin.GetBytes($latin.GetString($bytes).Replace("`r`n", "`n"))
  }
  return ([BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant()
}

# (Windows PowerShell's ConvertFrom-Json gives an object, and it is short of depth for nothing here)
$manifest = (Get-Content -LiteralPath (Join-Path $shipped 'manifest.json') -Raw) | ConvertFrom-Json
$appliedFile = Join-Path $shipped 'applied.json'
$applied = @{}
if (Test-Path -LiteralPath $appliedFile) {
  $was = (Get-Content -LiteralPath $appliedFile -Raw) | ConvertFrom-Json
  foreach ($p in $was.PSObject.Properties) { $applied[$p.Name] = $p.Value }
}

$installed = 0; $kept = @()
foreach ($entry in $manifest.files.PSObject.Properties) {
  $rel = $entry.Name
  $key = $entry.Value.sha256
  $from = Join-Path $shipped ($rel.Replace('/', '\'))
  $to = Join-Path $Proj ($rel.Replace('/', '\'))
  $replace = $false
  if (-not (Test-Path -LiteralPath $to)) {
    $replace = -not $applied.ContainsKey($rel)
  } else {
    $have = Get-FileKey $to
    if ($have -eq $key) {
      $applied[$rel] = $key
      continue
    }
    if (($applied.ContainsKey($rel) -and $applied[$rel] -eq $have) -or (@($entry.Value.previous) -contains $have)) {
      $replace = $true
    } elseif (-not $applied.ContainsKey($rel) -and $rel -like '*/placed.json' -and
              [IO.File]::ReadAllText($to) -notmatch '"model"') {
      # (the editor saved an empty list: no object of the player's is lost)
      $replace = $true
    } elseif ($applied[$rel] -ne $key) {
      # edited, and the release has a different one than the player started from
      $kept += $rel
    }
  }
  if ($replace) {
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($to))
    Copy-Item -LiteralPath $from -Destination $to -Force
    $applied[$rel] = $key
    $installed++
  }
}

$json = New-Object PSObject
foreach ($k in ($applied.Keys | Sort-Object)) { $json | Add-Member -NotePropertyName $k -NotePropertyValue $applied[$k] }
[IO.File]::WriteAllText($appliedFile, ($json | ConvertTo-Json))
[IO.File]::WriteAllText((Join-Path $shipped 'applied-version.txt'), [string]$manifest.version)
if ($installed) { Write-Host "Main world: $installed files brought up to date." }
foreach ($rel in $kept) {
  Write-Host "Kept your edited $rel (the new release's copy is in data\shipped)."
}
