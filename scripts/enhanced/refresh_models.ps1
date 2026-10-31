# Jak Enhanced Trilogy: brings the replacement models (custom_assets\<game>\merc_replacements) into the
# game after an update. They are read when levels are extracted, into out\<game>\fr3, and an update
# only compiles the code, so here the levels that hold a model whose file changed are extracted again.
#   refresh_models.ps1 -Proj <data folder> -Bin <programs folder> -Game jak3 [-Iso <disc image>] [-MarkOnly]
# out\<game>\merc-stamp.json holds the hash of each file at the last extraction. -MarkOnly writes it
# and extracts nothing (a whole extraction just made them all). Which levels: the game's own lists
# (goal_src\<game>\dgos\*.gd name each level's art groups), for the models that changed or went away.
# The HD textures stay: extraction bakes custom_assets\<game>\texture_replacements in, as hd_setup.py's.
# Exit code: 0 up to date or done, 2 skipped (no disc data: asked for again at the next start), 3 failed
# (the game keeps its old models, and it is tried again at the next start).
param(
  [Parameter(Mandatory = $true)][string]$Proj,
  [Parameter(Mandatory = $true)][string]$Bin,
  [Parameter(Mandatory = $true)][string]$Game,
  [string]$Iso,
  [switch]$MarkOnly
)
$ErrorActionPreference = 'Stop'
$Proj = $Proj.TrimEnd('\')
$Bin = $Bin.TrimEnd('\')

$models = Join-Path $Proj "custom_assets\$Game\merc_replacements"
$outDir = Join-Path $Proj "out\$Game"
$stampFile = Join-Path $outDir 'merc-stamp.json'
$fr3 = Join-Path $outDir 'fr3'
$common = Join-Path $fr3 'GAME.fr3'
$commonSaved = "$common.saved"

# an extraction that was cut short: the common level as it was
if (Test-Path -LiteralPath $commonSaved) { Move-Item -LiteralPath $commonSaved -Destination $common -Force }

$now = @{}
if (Test-Path -LiteralPath $models) {
  foreach ($f in Get-ChildItem -LiteralPath $models -File) {
    if ($f.Extension -in '.glb', '.json') { $now[$f.Name] = (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash }
  }
}
$before = @{}
if (Test-Path -LiteralPath $stampFile) {
  $was = (Get-Content -LiteralPath $stampFile -Raw) | ConvertFrom-Json
  foreach ($p in $was.PSObject.Properties) { $before[$p.Name] = $p.Value }
}

function Save-Stamp {
  [void][IO.Directory]::CreateDirectory($outDir)
  $json = New-Object PSObject
  foreach ($k in ($now.Keys | Sort-Object)) { $json | Add-Member -NotePropertyName $k -NotePropertyValue $now[$k] }
  [IO.File]::WriteAllText($stampFile, ($json | ConvertTo-Json))
}

$changed = @{}
foreach ($k in @($now.Keys) + @($before.Keys)) {
  if ($now[$k] -ne $before[$k]) { $changed[[IO.Path]::GetFileNameWithoutExtension($k)] = $true }
}
if ($changed.Count -eq 0) { exit 0 }
if ($MarkOnly) { Save-Stamp; exit 0 }

# the levels holding them. A model of the common level needs all the levels' animated textures in
# GAME.fr3, which only a whole extraction writes
$wanted = @{}
$whole = $false
$lists = Join-Path $Proj "goal_src\$Game\dgos"
$artGroups = @{}
foreach ($model in $changed.Keys) { $artGroups[('"' + ($model -replace '-lod\d+$', '') + '-ag.go"')] = $model }
foreach ($list in Get-ChildItem -LiteralPath $lists -Filter *.gd) {
  $text = [IO.File]::ReadAllText($list.FullName)
  foreach ($ag in $artGroups.Keys) {
    if ($text.IndexOf($ag, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
      $m = [regex]::Match($text, '"([A-Za-z0-9_-]+\.[DC]GO)"')
      if ($m.Success) {
        if ($m.Groups[1].Value -eq 'GAME.CGO') { $whole = $true } else { $wanted[$m.Groups[1].Value] = $true }
      }
    }
  }
}
if ($wanted.Count -eq 0 -and -not $whole) {
  # (a model in no level of the game: nothing to do for it)
  Save-Stamp
  exit 0
}

$config = "decompiler/config/$Game/${Game}_config.jsonc"
$configText = [IO.File]::ReadAllText((Join-Path $Proj $config))
$inputsRel = [regex]::Match($configText, '"inputs_file"\s*:\s*"([^"]+)"').Groups[1].Value
$inputsText = [IO.File]::ReadAllText((Join-Path $Proj $inputsRel))
$known = [regex]::Match($inputsText, '"levels_to_extract"\s*:\s*\[.*?\]', 'Singleline').Value
$levels = @($wanted.Keys | Where-Object { $known.Contains('"' + $_ + '"') } | Sort-Object)
if ($levels.Count -eq 0 -and -not $whole) { Save-Stamp; exit 0 }

# the disc's data, kept from the first extraction (jak3_pal -> pal, jak3 -> ntsc_v1: as hd_setup.py)
$isoData = Join-Path $Proj 'iso_data'
function Find-DiscFolder {
  if (-not (Test-Path -LiteralPath $isoData)) { return $null }
  return Get-ChildItem -LiteralPath $isoData -Directory | Where-Object { $_.Name -match "^$Game(_[a-z0-9]+)?$" } | Sort-Object Name | Select-Object -First 1
}
$disc = Find-DiscFolder
if (-not $disc -and $Iso -and (Test-Path -LiteralPath $Iso)) {
  Write-Host "Reading your $Game disc again for the new models..."
  $ErrorActionPreference = 'Continue'
  & (Join-Path $Bin 'extractor.exe') --game $Game --proj-path $Proj -e $Iso 2>&1 | Out-Null
  $ErrorActionPreference = 'Stop'
  $disc = Find-DiscFolder
}
if (-not $disc) {
  Write-Host "The new models of $Game need your disc's data (iso_data), which isn't here: skipped, the game"
  Write-Host 'works with the old models. Put your disc image in the ISO folder: it is tried at the next start.'
  exit 2
}
$version = if ($disc.Name.Length -gt $Game.Length) { $disc.Name.Substring($Game.Length + 1) } else { 'ntsc_v1' }

$override = [ordered]@{
  decompile_code = $false; levels_extract = $true; allowed_objects = @()
  process_game_text = $false; process_game_count = $false
}
if (-not $whole) {
  $tmp = Join-Path $Proj 'out\enhanced-tmp'
  [void][IO.Directory]::CreateDirectory($tmp)
  $quoted = ($levels | ForEach-Object { '"' + $_ + '"' }) -join ', '
  $narrow = [regex]::Replace($inputsText, '"levels_to_extract"\s*:\s*\[.*?\]', ('"levels_to_extract": [' + $quoted + ']'), 'Singleline')
  [IO.File]::WriteAllText((Join-Path $tmp "$Game-inputs.jsonc"), $narrow)
  $override['inputs_file'] = "out/enhanced-tmp/$Game-inputs.jsonc"
  # (the common level gets rewritten by any extraction, and with the animated textures of only these
  # levels: ENHANCED.md; it holds none of these models, so it goes back as it was)
  if (Test-Path -LiteralPath $common) { Copy-Item -LiteralPath $common -Destination $commonSaved -Force }
  Write-Host "New models for $Game`: making the levels that hold them again ($($levels -join ', ')). About 2 minutes, only this once."
} else {
  Write-Host "New models for $Game`: making all its levels again. 10-30 minutes, only this once. You can leave it running."
}

$over = ($override | ConvertTo-Json -Compress).Replace('"', '\"')
$started = Get-Date
$lastShown = $started
$logFile = Join-Path $Proj "log\refresh-models-$Game.log"
[void][IO.Directory]::CreateDirectory((Split-Path $logFile))
Push-Location -LiteralPath $Proj
$ErrorActionPreference = 'Continue'
try {
  & (Join-Path $Bin 'decompiler.exe') $config iso_data decompiler_out --version $version --config-override $over 2>&1 | ForEach-Object {
    $line = "$_"
    Add-Content -LiteralPath $logFile -Value $line
    if ($line -match 'Processing level (\S+)') {
      Write-Host ("  [{0:mm\:ss}] level {1}" -f ((Get-Date) - $started), $Matches[1])
      $lastShown = Get-Date
    } elseif (((Get-Date) - $lastShown).TotalSeconds -ge 15) {
      Write-Host ("  [{0:mm\:ss}] working..." -f ((Get-Date) - $started))
      $lastShown = Get-Date
    }
  }
  $code = $LASTEXITCODE
} finally {
  $ErrorActionPreference = 'Stop'
  Pop-Location
}
if (-not $whole -and (Test-Path -LiteralPath $commonSaved)) { Move-Item -LiteralPath $commonSaved -Destination $common -Force }
if ($code -ne 0) {
  Write-Host "Making the new models failed (code $code, see $logFile). The game works with the old ones; it is tried again at the next start."
  exit 3
}
Save-Stamp
Write-Host 'New models ready.'
exit 0
