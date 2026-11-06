# Jak Enhanced Trilogy: installs a newer release over this one, then starts the launcher again.
#   update.ps1 -Root <the release folder> -Tag v0.6.2 [-Gui]
#   update.ps1 -Root <the release folder> -CheckOnly    exit code 10 if the game is open from there, else 0
#   update.ps1 -Root <the release folder> -CloseOnly    closes it without asking; 0 closed, 11 couldn't
# The launcher runs it (and quits) when the player says yes to a new version, -Gui in a progress window
# (no console: its log in data\log\update.log); the installer uses the last two (it asks in its own window). The release ZIP holds only the programs and data\: the
# extracted discs (data\out), the ISO folder and the HD textures are not in it, so copying it over the
# folder keeps them, and what the player can edit (the main world) comes in data\shipped, for the
# launcher to apply file by file (apply_shipped.ps1); saves and settings live in %APPDATA%\OpenGOAL.
param([Parameter(Mandatory = $true)][string]$Root, [string]$Tag, [switch]$CheckOnly, [switch]$CloseOnly,
      [switch]$Gui)
$ErrorActionPreference = 'Stop'
$Root = $Root.TrimEnd('\')

# what runs from this install: every program inside the folder, and the launcher's windows and the
# hidden PowerShell that waits for the game to open the report window, which name it on their command line
function Get-InstallProcesses {
  $inside = $Root + '\'
  $programs = @(); $helpers = @()
  foreach ($p in Get-CimInstance Win32_Process) {
    if ($p.ProcessId -eq $PID) { continue }
    if ($p.ExecutablePath -and $p.ExecutablePath.StartsWith($inside, [StringComparison]::OrdinalIgnoreCase)) {
      $programs += $p
    } elseif ($p.Name -in 'cmd.exe', 'powershell.exe' -and $p.CommandLine) {
      # (the launcher writes an apostrophe of the path doubled)
      $line = $p.CommandLine.Replace("''", "'")
      if ($line.IndexOf($inside, [StringComparison]::OrdinalIgnoreCase) -ge 0 -and
          $line -match 'Jak Enhanced Trilogy\.bat|Report a problem\.bat|report\.ps1|launcher\.ps1|ask\.ps1|gk\.exe') {
        $helpers += $p
      }
    }
  }
  return @{ Programs = $programs; Helpers = $helpers }
}

function Wait-Gone([int[]]$ids, [int]$seconds) {
  $until = (Get-Date).AddSeconds($seconds)
  while ((Get-Date) -lt $until) {
    if (-not (Get-Process -Id $ids -ErrorAction SilentlyContinue)) { return $true }
    Start-Sleep -Milliseconds 300
  }
  return -not (Get-Process -Id $ids -ErrorAction SilentlyContinue)
}

# closes all of it: true when nothing is left. First what would answer the game's exit with a window of
# its own, then the game by asking its window to close, and by force only if it doesn't
function Close-Install {
  $found = Get-InstallProcesses
  foreach ($h in $found.Helpers) { Stop-Process -Id $h.ProcessId -Force -ErrorAction SilentlyContinue }
  $ids = @($found.Programs | ForEach-Object { [int]$_.ProcessId })
  if ($ids.Count) {
    foreach ($id in $ids) {
      $proc = Get-Process -Id $id -ErrorAction SilentlyContinue
      if ($proc -and $proc.MainWindowHandle -ne [IntPtr]::Zero) { [void]$proc.CloseMainWindow() }
    }
    if (-not (Wait-Gone $ids 15)) {
      Stop-Process -Id $ids -Force -ErrorAction SilentlyContinue
      [void](Wait-Gone $ids 10)
    }
  }
  $left = Get-InstallProcesses
  return ($left.Programs.Count + $left.Helpers.Count) -eq 0
}

if ($CheckOnly) {
  $found = Get-InstallProcesses
  if ($found.Programs.Count + $found.Helpers.Count) { exit 10 }
  exit 0
}
if ($CloseOnly) {
  if (Close-Install) { exit 0 }
  exit 11
}
if (-not $Tag) { throw 'update.ps1: -Tag is needed' }

# -Gui: a window with what it does, the progress line under it and a bar (the console's lines too, for the log)
$ui = $null
if ($Gui) {
  Add-Type -AssemblyName System.Windows.Forms, System.Drawing
  [Windows.Forms.Application]::EnableVisualStyles()
  $ui = New-Object Windows.Forms.Form
  $ui.Text = 'Jak Enhanced Trilogy'
  try { $ui.Icon = New-Object Drawing.Icon (Join-Path $Root 'data\game\assets\enhanced\art\jak-enhanced.ico') } catch { }
  $ui.FormBorderStyle = 'FixedDialog'
  $ui.ControlBox = $false
  $ui.StartPosition = 'CenterScreen'
  $ui.ClientSize = New-Object Drawing.Size 480, 118
  $ui.Font = [Drawing.SystemFonts]::MessageBoxFont
  $uiStatus = New-Object Windows.Forms.Label
  $uiStatus.SetBounds(14, 12, 452, 40)
  $uiBar = New-Object Windows.Forms.ProgressBar
  $uiBar.SetBounds(14, 56, 452, 18)
  $uiBar.Style = 'Marquee'
  $uiBar.MarqueeAnimationSpeed = 30
  $uiLine = New-Object Windows.Forms.Label
  $uiLine.SetBounds(14, 82, 452, 30)
  $uiLine.ForeColor = [Drawing.SystemColors]::GrayText
  $ui.Controls.AddRange(@($uiStatus, $uiBar, $uiLine))
  try {
    [void][IO.Directory]::CreateDirectory((Join-Path $Root 'data\log'))
    Start-Transcript -Path (Join-Path $Root 'data\log\update.log') -Force | Out-Null
  } catch { }
}

function Update-Ui { if ($ui) { [Windows.Forms.Application]::DoEvents() } }

# what it does now: a line in the console, the window's headline
function Write-Step([string]$text) {
  Write-Host $text
  if ($ui) {
    $uiStatus.Text = $text
    $uiLine.Text = ''
    $uiBar.Style = 'Marquee'
    if (-not $ui.Visible) { $ui.Show(); $ui.Activate() }
    Update-Ui
  }
}

# its progress, rewritten in place; with a percentage, the bar
function Write-Line([string]$text, [int]$percent = -1) {
  Write-Host ("`r" + $text.PadRight(70)) -NoNewline
  if ($ui) {
    $uiLine.Text = $text.Trim()
    if ($percent -ge 0) {
      $uiBar.Style = 'Continuous'
      $uiBar.Value = [Math]::Max(0, [Math]::Min(100, $percent))
    }
    Update-Ui
  }
}

function Test-Room([string]$path, [long]$bytes, [string]$what) {
  try { $free = (New-Object IO.DriveInfo ([IO.Path]::GetPathRoot($path))).AvailableFreeSpace } catch { return }
  if ($free -lt $bytes) {
    throw ("not enough room for {0} on {1}: it takes {2} MB, there are {3} MB free" -f
           $what, [IO.Path]::GetPathRoot($path), [int]($bytes / 1MB), [int]($free / 1MB))
  }
}

# with its progress; a connection that ends early is an error here, not a broken ZIP later
function Get-Download([string]$url, [string]$file) {
  $request = [Net.HttpWebRequest]::Create($url)
  $request.UserAgent = 'jak-enhanced-update'
  try {
    $response = $request.GetResponse()
  } catch {
    $web = $_.Exception.InnerException
    # (HTTP 404, no such file: a patch the release doesn't have, the whole ZIP then)
    if ($web -and $web.Response) { throw "the server doesn't have this version's file (HTTP $([int]$web.Response.StatusCode))" }
    throw "couldn't reach the server: check your connection"
  }
  try {
    $total = $response.ContentLength
    if ($total -gt 0) {
      Test-Room $file ($total * 3) 'the download and its unpacked files'
      Test-Room $Root ($total * 2) 'the new files'
      Write-Step ("Downloading Jak Enhanced Trilogy {0} ({1} MB)..." -f $Tag, [int]($total / 1e6))
    } else {
      Write-Step "Downloading Jak Enhanced Trilogy $Tag..."
    }
    $in = $response.GetResponseStream()
    $out = [IO.File]::Create($file)
    try {
      $buffer = New-Object byte[] (1MB)
      $done = [long]0
      $clock = [Diagnostics.Stopwatch]::StartNew()
      $shown = 0
      while (($n = $in.Read($buffer, 0, $buffer.Length)) -gt 0) {
        $out.Write($buffer, 0, $n)
        $done += $n
        if ($clock.ElapsedMilliseconds - $shown -ge 500) {
          $shown = $clock.ElapsedMilliseconds
          $speed = $done / 1e6 / [Math]::Max($clock.Elapsed.TotalSeconds, 0.1)
          if ($total -gt 0) {
            $part = [int](100 * $done / $total)
            Write-Line ("  {0} of {1} MB ({2}%), {3:0.0} MB/s" -f [int]($done / 1e6), [int]($total / 1e6), $part, $speed) $part
          } else {
            Write-Line ("  {0} MB, {1:0.0} MB/s" -f [int]($done / 1e6), $speed)
          }
        }
      }
    } finally {
      $out.Close()
      $in.Close()
    }
    Write-Host ''
    if ($total -gt 0 -and $done -ne $total) {
      throw ("the download stopped early: {0} of {1} MB" -f [int]($done / 1e6), [int]($total / 1e6))
    }
  } finally {
    $response.Close()
  }
}

function Expand-WithProgress([string]$zipFile, [string]$destination) {
  Add-Type -AssemblyName System.IO.Compression.FileSystem
  try {
    $zip = [IO.Compression.ZipFile]::OpenRead($zipFile)
  } catch {
    throw "the downloaded file is damaged"
  }
  try {
    $bytes = [long]0
    foreach ($e in $zip.Entries) { $bytes += $e.Length }
    Test-Room $destination $bytes 'the unpacked files'
    $base = [IO.Path]::GetFullPath($destination) + '\'
    $count = $zip.Entries.Count
    $i = 0
    foreach ($e in $zip.Entries) {
      $i++
      $target = [IO.Path]::GetFullPath((Join-Path $base $e.FullName))
      if (-not $target.StartsWith($base, [StringComparison]::OrdinalIgnoreCase)) { throw "a bad path in the ZIP: $($e.FullName)" }
      if ($e.FullName.EndsWith('/')) {
        [void][IO.Directory]::CreateDirectory($target)
      } else {
        [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))
        [IO.Compression.ZipFileExtensions]::ExtractToFile($e, $target, $true)
      }
      if ($i % 100 -eq 0 -or $i -eq $count) { Write-Line ("  {0} of {1} files" -f $i, $count) ([int](100 * $i / $count)) }
    }
    Write-Host ''
  } finally {
    $zip.Dispose()
  }
}

# robocopy's codes below 8 are success
function Invoke-Copy([string]$from, [string]$to) {
  $count = 0
  # /E every folder, files replaced, nothing of the player's deleted; data\VERSION comes last, below
  & robocopy $from $to /E /XF VERSION /R:3 /W:2 /NDL /NJH /NJS /NP | ForEach-Object {
    $count++
    if ($count % 50 -eq 0) { Write-Line ("  {0} files" -f $count) }
  }
  $code = $LASTEXITCODE
  Write-Line ("  {0} files" -f $count)
  Write-Host ''
  if ($code -ge 8) { throw "copying the new files failed (robocopy $code)" }
}

$copying = $false
$cancelled = $false
$backup = $null
$old = ''
$work = Join-Path $env:TEMP 'jak-enhanced-update'
$name = 'Jak-Enhanced-Trilogy'
$launcher = Join-Path $Root 'Jak Enhanced Trilogy.bat'
try {
  try { $Host.UI.RawUI.WindowTitle = "Jak Enhanced Trilogy: updating to $Tag" } catch { }
  Write-Step "Updating Jak Enhanced Trilogy to $Tag..."
  # GitHub only answers over TLS 1.2 or newer
  [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
  # the launcher has to be gone before its own file is replaced (cmd reads a .bat as it runs it)
  Start-Sleep -Seconds 2
  if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
  [void](New-Item -ItemType Directory $work)
  # the version installed here: from it, only what changed (make_patch in make_release.py), when the
  # release has that patch; the whole ZIP otherwise
  $versionFile = Join-Path $Root 'data\VERSION'
  $old = if (Test-Path -LiteralPath $versionFile) { ((Get-Content -LiteralPath $versionFile -Raw).Trim() -split '-')[0] } else { '' }
  $base = "https://github.com/stichlola/jak-enhanced-trilogy/releases/download/$Tag"
  $patch = $false
  if ($old) {
    $zip = Join-Path $work "$name-$Tag-patch-$old.zip"
    try {
      Get-Download "$base/$name-$Tag-patch-$old.zip" $zip
      $patch = $true
    } catch {
      # only when the release has no patch from it: no connection stays an error (20 MB don't become 400)
      if ($_.Exception.Message -notmatch 'HTTP 404') { throw }
      Write-Host "(no update from $old alone: the whole new version)"
    }
  }
  if (-not $patch) {
    $zip = Join-Path $work "$name-$Tag.zip"
    Get-Download "$base/$name-$Tag.zip" $zip
  }
  Write-Step 'Unpacking...'
  $unpacked = Join-Path $work 'new'
  Expand-WithProgress $zip $unpacked
  $new = Join-Path $unpacked $name
  if (-not $patch -and -not (Test-Path -LiteralPath (Join-Path $new 'gk.exe'))) { throw 'the download has no gk.exe' }
  if (-not (Test-Path -LiteralPath (Join-Path $new 'data\VERSION'))) { throw 'the download has no data\VERSION' }

  # nothing may run from the folder while it is replaced
  $found = Get-InstallProcesses
  if ($found.Programs.Count + $found.Helpers.Count) {
    $names = @($found.Programs | ForEach-Object { $_.Name }) + @($found.Helpers | ForEach-Object { 'the launcher' }) | Select-Object -Unique
    Write-Host ''
    Write-Host "Still open: $($names -join ', ')."
    Write-Host 'They have to close before the update. Save your game first: what is not saved is lost.'
    if ($ui) {
      $answer = [Windows.Forms.MessageBox]::Show($ui, ("Still open: $($names -join ', ').`r`n`r`nThey have to close " +
        "before the update. Save your game first: what is not saved is lost.`r`n`r`nClose them and update?"),
        'Jak Enhanced Trilogy', 'OKCancel', 'Warning')
      $answer = if ($answer -eq 'OK') { '' } else { 'n' }
    } else {
      $answer = Read-Host 'Press Enter to close them and update, or type N and Enter to cancel'
    }
    if ($answer -match '^\s*[nN]') {
      $cancelled = $true
      throw 'you cancelled it, the game stays open'
    }
    Write-Step 'Closing...'
    if (-not (Close-Install)) { throw 'the game could not be closed: close it by hand' }
  }
  # (a program's file can still be held for a moment, or by something else: before anything is copied)
  foreach ($exe in Get-ChildItem -LiteralPath $Root -Filter *.exe) {
    try {
      ([IO.File]::Open($exe.FullName, 'Open', 'ReadWrite', 'None')).Close()
    } catch {
      throw "$($exe.Name) is in use by another program: close it and try again"
    }
  }

  # a backup of every file the new version replaces or removes, in backup\<the old version> (only the
  # last one kept): a copy that stops halfway is put back from it
  $removedList = Join-Path $new 'data\removed.txt'
  $removed = @()
  if (Test-Path -LiteralPath $removedList) {
    $removed = @(Get-Content -LiteralPath $removedList | Where-Object { $_.Trim() } | ForEach-Object { $_.Replace('/', '\') })
    Remove-Item -LiteralPath $removedList
  }
  $backupRoot = Join-Path $Root 'backup'
  if (Test-Path -LiteralPath $backupRoot) { Remove-Item -LiteralPath $backupRoot -Recurse -Force }
  $backup = Join-Path $backupRoot $(if ($old) { $old } else { 'previous' })
  Write-Step "Backing up the files it replaces to $backup..."
  $newBase = [IO.Path]::GetFullPath($new) + '\'
  $replaced = @(Get-ChildItem -LiteralPath $new -Recurse -File | ForEach-Object { $_.FullName.Substring($newBase.Length) }) + $removed
  foreach ($rel in $replaced) {
    $from = Join-Path $Root $rel
    if (Test-Path -LiteralPath $from -PathType Leaf) {
      $to = Join-Path $backup $rel
      [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($to))
      Copy-Item -LiteralPath $from -Destination $to -Force
    }
  }

  Write-Step "Installing over $Root..."
  $copying = $true
  Invoke-Copy $new $Root
  foreach ($rel in $removed) {
    $gone = Join-Path $Root $rel
    if (Test-Path -LiteralPath $gone -PathType Leaf) { Remove-Item -LiteralPath $gone -Force }
  }
  # data\VERSION last: an update that stops before this is offered again
  Copy-Item -LiteralPath (Join-Path $new 'data\VERSION') -Destination (Join-Path $Root 'data\VERSION') -Force
  Write-Host "Updated to $Tag."
} catch {
  # (the same words in the console and, -Gui, in a window)
  $said = New-Object Collections.Generic.List[string]
  function Say([string]$text) { Write-Host $text; $said.Add($text) }
  Write-Host ''
  Say "The update didn't work: $($_.Exception.Message)"
  if ($copying -and $backup -and (Test-Path -LiteralPath $backup)) {
    Say 'It stopped while copying: putting the old files back from the backup...'
    & robocopy $backup $Root /E /R:3 /W:2 /NDL /NFL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -lt 8) {
      Say "Done: the game is as it was ($old). You can try again at the next start, or"
    } else {
      Say "Putting them back failed too: the old files are in $backup. Start the launcher again and"
      Say 'choose Update now to finish the update, or'
    }
  } elseif ($copying) {
    Say 'It stopped while copying: some files are new, others are still the old ones. Your extracted'
    Say 'games, saves and settings are fine. Start the launcher again and choose Update now to finish it, or'
  } else {
    Say 'Nothing was changed: the game is as it was. You can try again at the next start, or'
  }
  Say 'download the new version by hand from https://github.com/stichlola/jak-enhanced-trilogy/releases/latest'
  if ($ui) {
    $ui.Hide()
    $text = ($said -join "`r`n") + "`r`n`r`nWhat it did is in " + (Join-Path $Root 'data\log\update.log')
    [void][Windows.Forms.MessageBox]::Show($text, 'Jak Enhanced Trilogy', 'OK', 'Error', 'Button1', 'DefaultDesktopOnly')
  } else {
    Read-Host 'Press Enter to carry on'
  }
} finally {
  # (a failed download doesn't stay on the disk)
  if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue }
}
if ($ui) {
  try { Stop-Transcript | Out-Null } catch { }
  $ui.Close()
}
# (the game the player chose to keep open isn't started a second time)
if (-not $cancelled) {
  # -Gui: the launcher hidden too (launcher.ps1, without the .bat's moment of console), when this version has it
  $hidden = Join-Path $Root 'data\scripts\enhanced\launcher.ps1'
  if ($Gui -and -not $env:JAK_ENHANCED_CONSOLE -and (Test-Path -LiteralPath $hidden)) {
    $command = "& ([scriptblock]::Create([IO.File]::ReadAllText('{0}'))) -Root '{1}'" -f $hidden.Replace("'", "''"), $Root.Replace("'", "''")
    $psi = New-Object Diagnostics.ProcessStartInfo 'powershell.exe', ('-NoProfile -Command "' + $command + '"')
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.WorkingDirectory = $Root
    [void][Diagnostics.Process]::Start($psi)
  } else {
    Start-Process -FilePath $launcher -WorkingDirectory $Root
  }
}
