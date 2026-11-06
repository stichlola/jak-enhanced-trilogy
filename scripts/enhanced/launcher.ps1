# Jak Enhanced Trilogy: runs the launcher ("Jak Enhanced Trilogy.bat") without its console window.
#   launcher.ps1 -Root <the release folder> [-Detach]
# The .bat starts this (with -Detach: it starts itself again without a window and returns at once),
# and this runs the .bat again hidden (JAK_ENHANCED_HIDDEN=1), its output in data\log\launcher.log.
# Hidden, the .bat writes lines for this window instead of using the console:
#   @@step <text>    a long step began (extracting a disc, compiling, the HD textures...): a status
#                    window shows it, with the last line of its output and its progress
#   @@idle           it ended
#   @@error <text>   it failed: a window says so, with the log's last lines and where the log is
#   @@info <text>    nothing to start (no disc yet): a window says so
#   @@update <tag>   the player chose to update: update.ps1 -Gui runs, in its own progress window
#   @@launch <1-3>   the game to start: started here, waited for, the report window on a crash
# The questions (a new version, the HD textures) the .bat asks itself, in windows (ask.ps1).
param([Parameter(Mandatory = $true)][string]$Root, [switch]$Detach)
$ErrorActionPreference = 'Stop'
$Root = $Root.TrimEnd('\')
$Proj = Join-Path $Root 'data'
$Me = Join-Path $Proj 'scripts\enhanced\launcher.ps1'
$Title = 'Jak Enhanced Trilogy'
$Issues = 'https://github.com/stichlola/jak-enhanced-trilogy/issues'

# a PowerShell script of this release, without a window (the install's path on its command line, for
# update.ps1 to find it)
function Start-Hidden([string]$script, [string]$arguments, [hashtable]$environment = @{}) {
  $command = "& ([scriptblock]::Create([IO.File]::ReadAllText('{0}'))) -Root '{1}' {2}" -f
             $script.Replace("'", "''"), $Root.Replace("'", "''"), $arguments
  $psi = New-Object Diagnostics.ProcessStartInfo 'powershell.exe', ('-NoProfile -Command "' + $command + '"')
  $psi.UseShellExecute = $false
  $psi.CreateNoWindow = $true
  $psi.WorkingDirectory = $Root
  foreach ($k in $environment.Keys) { $psi.EnvironmentVariables[$k] = $environment[$k] }
  return [Diagnostics.Process]::Start($psi)
}

if ($Detach) {
  # (an error before it started: exit 1, and the .bat carries on in its console)
  [void](Start-Hidden $Me '')
  exit 0
}

Add-Type -AssemblyName System.Windows.Forms, System.Drawing
[Windows.Forms.Application]::EnableVisualStyles()
$IconFile = Join-Path $Proj 'game\assets\enhanced\art\jak-enhanced.ico'
$Icon = $null
try { $Icon = New-Object Drawing.Icon $IconFile } catch { }
$logDir = Join-Path $Proj 'log'
$log = Join-Path $logDir 'launcher.log'

# (in front of every window: nothing else on the screen says the launcher stopped)
function Show-Message([string]$text, [string]$kind = 'Information', [string]$buttons = 'OK') {
  return [Windows.Forms.MessageBox]::Show($text, $Title, $buttons, $kind, 'Button1', 'DefaultDesktopOnly')
}

function Show-Failure([string]$what, [string[]]$lines) {
  $text = $what
  if ($lines.Count) { $text += "`r`n`r`nThe last messages:`r`n" + ($lines -join "`r`n") }
  $text += "`r`n`r`nEverything it wrote is in`r`n$log`r`n`r`nAsk for help at $Issues (`"Report a problem`" " +
           "sends the logs).`r`n`r`nOpen the log now?"
  if ((Show-Message $text 'Error' 'YesNo') -eq 'Yes') { Start-Process notepad.exe -ArgumentList ('"' + $log + '"') }
}

try {
  [void][IO.Directory]::CreateDirectory($logDir)
  # (the one before kept; a launcher still running holds it: then a log of its own)
  try {
    if (Test-Path -LiteralPath $log) { Move-Item -LiteralPath $log -Destination (Join-Path $logDir 'launcher.prev.log') -Force }
  } catch {
    $log = Join-Path $logDir ('launcher-' + $PID + '.log')
  }
  [IO.File]::WriteAllText($log, "Jak Enhanced Trilogy launcher, $(Get-Date)`r`n")

  # the .bat, hidden, its output into the log (not a pipe: the programs it starts could keep one open)
  $bat = Join-Path $Root 'Jak Enhanced Trilogy.bat'
  $psi = New-Object Diagnostics.ProcessStartInfo 'cmd.exe', ('/d /c ""{0}" <nul >>"{1}" 2>&1"' -f $bat, $log)
  $psi.UseShellExecute = $false
  $psi.CreateNoWindow = $true
  $psi.WorkingDirectory = $Root
  $psi.EnvironmentVariables['JAK_ENHANCED_HIDDEN'] = '1'
  $cmd = [Diagnostics.Process]::Start($psi)
  $null = $cmd.Handle

  # the status window: shown while a step runs (and when starting takes long: the disc check, GitHub)
  $form = New-Object Windows.Forms.Form
  $form.Text = $Title
  if ($Icon) { $form.Icon = $Icon }
  $form.FormBorderStyle = 'FixedDialog'
  $form.MaximizeBox = $false
  $form.StartPosition = 'CenterScreen'
  $form.ClientSize = New-Object Drawing.Size 480, 118
  $form.Font = [Drawing.SystemFonts]::MessageBoxFont
  $stepLabel = New-Object Windows.Forms.Label
  $stepLabel.SetBounds(14, 12, 452, 40)
  $stepLabel.Text = 'Starting Jak Enhanced Trilogy...'
  $bar = New-Object Windows.Forms.ProgressBar
  $bar.SetBounds(14, 56, 452, 18)
  $bar.Style = 'Marquee'
  $bar.MarqueeAnimationSpeed = 30
  $detail = New-Object Windows.Forms.Label
  $detail.SetBounds(14, 82, 452, 30)
  $detail.ForeColor = [Drawing.SystemColors]::GrayText
  $detail.AutoEllipsis = $true
  $form.Controls.AddRange(@($stepLabel, $bar, $detail))
  # (closing it doesn't stop the work: it only hides, the game still starts at the end)
  $state = @{ Closed = $false }
  $form.Add_FormClosing({ param($s, $e) if ($e.CloseReason -eq 'UserClosing') { $e.Cancel = $true; $state.Closed = $true; $form.Hide() } })

  $encoding = [Text.Encoding]::GetEncoding([Globalization.CultureInfo]::CurrentCulture.TextInfo.OEMCodePage)
  $stream = New-Object IO.FileStream $log, 'Open', 'Read', ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
  $reader = New-Object IO.StreamReader $stream, $encoding
  $clock = [Diagnostics.Stopwatch]::StartNew()
  $step = $null; $stepAt = 0; $percent = -1
  $errorText = $null; $info = $null; $update = $null; $launch = $null
  $last = New-Object Collections.Generic.List[string]
  $pending = ''
  while ($true) {
    $exited = $cmd.HasExited
    $chunk = $reader.ReadToEnd()
    if ($chunk) {
      $parts = ($pending + $chunk) -split "`r`n|`n|`r"
      $pending = $parts[-1]
      foreach ($raw in @($parts | Select-Object -First ($parts.Count - 1))) {
        # (the extractor and the compiler colour their output: "ESC[38;2;144;238;144minfo ESC[0m")
        $line = ($raw -replace "\x1b\[[0-9;?]*[A-Za-z]", '') -replace "[\x00-\x08\x0b\x0c\x0e-\x1f]", ''
        if ($line -match '^@@(\w+) ?(.*)$') {
          $arg = $Matches[2].Trim()
          switch ($Matches[1]) {
            'step' { $step = $arg; $stepAt = $clock.ElapsedMilliseconds; $percent = -1; $detail.Text = '' }
            'idle' { $step = $null; $percent = -1; $detail.Text = '' }
            'error' { $errorText = $arg }
            'info' { $info = $arg }
            'update' { $update = $arg }
            'launch' { $launch = $arg }
          }
        } elseif ($line.Trim()) {
          $last.Add($line.TrimEnd())
          if ($last.Count -gt 8) { $last.RemoveAt(0) }
          $detail.Text = $line.Trim()
          # "[ 45%]" from the compiler
          if ($line -match '\[\s*(\d{1,3})%\]') { $percent = [Math]::Min(100, [int]$Matches[1]) }
        }
      }
    }
    if ($exited) { break }
    $stepLabel.Text = if ($step) { $step } else { 'Starting Jak Enhanced Trilogy...' }
    if ($percent -ge 0) {
      $bar.Style = 'Continuous'
      $bar.Value = $percent
    } else {
      $bar.Style = 'Marquee'
    }
    $wanted = ($step -and $clock.ElapsedMilliseconds - $stepAt -gt 800) -or $clock.ElapsedMilliseconds -gt 2000
    # (in front of the other windows while the launcher works; not any more once the game starts)
    if ($wanted -and -not $form.Visible -and -not $state.Closed) { $form.TopMost = $true; $form.Show(); $form.Activate() }
    [Windows.Forms.Application]::DoEvents()
    Start-Sleep -Milliseconds 150
  }
  $reader.Close()
  $code = $cmd.ExitCode
  $lines = @($last | Select-Object -Last 6)

  if ($launch -and $code -eq 0) {
    # as the console launcher does it (data\ the working folder), without gk's own console window
    $stepLabel.Text = "Starting Jak $launch..."
    $detail.Text = ''
    $bar.Style = 'Marquee'
    $form.TopMost = $false
    $gkPsi = New-Object Diagnostics.ProcessStartInfo (Join-Path $Root 'gk.exe'), "--game jak$launch -- -boot -fakeiso"
    $gkPsi.UseShellExecute = $false
    $gkPsi.CreateNoWindow = $true
    $gkPsi.WorkingDirectory = $Proj
    $game = [Diagnostics.Process]::Start($gkPsi)
    $null = $game.Handle
    # the status window until the game's own window is up (or 30 seconds)
    $until = $clock.ElapsedMilliseconds + 30000
    while (-not $game.HasExited -and $clock.ElapsedMilliseconds -lt $until) {
      $game.Refresh()
      if ($game.MainWindowHandle -ne [IntPtr]::Zero) { break }
      [Windows.Forms.Application]::DoEvents()
      Start-Sleep -Milliseconds 200
    }
    $form.Hide()
    $form.Dispose()
    $game.WaitForExit()
    [IO.File]::AppendAllText($log, "launcher: gk.exe ended, exit code $($game.ExitCode)`r`n")
    if ($game.ExitCode -ne 0) {
      & ([scriptblock]::Create([IO.File]::ReadAllText((Join-Path $Proj 'scripts\enhanced\report.ps1')))) -Proj $Proj -Crash $game.ExitCode
    }
    exit 0
  }
  $form.TopMost = $false
  $form.Hide()
  if ($update -and $code -eq 0) {
    [void](Start-Hidden (Join-Path $Proj 'scripts\enhanced\update.ps1') ("-Tag '{0}' -Gui" -f $update.Replace("'", "''")))
    exit 0
  }
  if ($info) {
    [void](Show-Message $info)
    exit 0
  }
  [IO.File]::AppendAllText($log, "launcher: the launcher ended with exit code $code`r`n")
  if ($errorText -or $code -ne 0) {
    $what = if ($errorText) { $errorText } else { "The launcher stopped (exit code $code)." }
    if ($step) { $what += "`r`n`r`nWhile: " + $step }
    Show-Failure $what $lines
  } else {
    Show-Failure 'The launcher ended without starting a game.' $lines
  }
  exit 1
} catch {
  Show-Failure ("The launcher failed: " + $_.Exception.Message) @()
  exit 1
}
