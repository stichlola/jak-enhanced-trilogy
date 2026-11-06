# Jak Enhanced Trilogy: the launcher's questions, in a window instead of the console.
#   ask.ps1 -Update -Latest v0.14.5 -Mine v0.14.0 -MB 19 [-Icon <the .ico>]   update now or later
#   ask.ps1 -Hd -Game 2 [-Icon <the .ico>]                                    the HD textures now?
# Exit code 10 yes ("Update now", "Make them now"), 11 no ("Later", "Not now", the window closed),
# anything else the window couldn't be shown: the launcher then asks in the console, or takes no.
param([switch]$Update, [string]$Latest, [string]$Mine, [string]$MB,
      [switch]$Hd, [string]$Game, [string]$Icon)
try {
  Add-Type -AssemblyName System.Windows.Forms, System.Drawing
  [Windows.Forms.Application]::EnableVisualStyles()
  if ($Update) {
    $message = ("A new version of Jak Enhanced Trilogy is out: {0} (you have {1}).`n`n" +
                "Updating downloads {2} MB. Your games and saves are kept.") -f $Latest, $Mine, $MB
    $yesText = 'Update now'; $noText = 'Later'; $yesFirst = $true
  } elseif ($Hd) {
    $name = @{ '1' = 'Jak 1'; '2' = 'Jak II'; '3' = 'Jak 3' }[$Game]
    $message = ("HD textures for {0}: every texture of the game redrawn 4x by an AI (Real-ESRGAN) on your " +
                "graphics card, from your own disc.`n`nIt takes a while: about 30 minutes for Jak 1, 1-2 hours " +
                "for Jak II or Jak 3 (depends on the graphics card), and 1-4 GB more on disk. The game works " +
                "the same without them. To be asked again, delete data\out\jak{1}\hd-textures.asked.`n`n" +
                "Make the HD textures for {0} now?") -f $name, $Game
    $yesText = 'Make them now'; $noText = 'Not now'; $yesFirst = $false
  } else {
    exit 2
  }

  $form = New-Object Windows.Forms.Form
  $form.Text = 'Jak Enhanced Trilogy'
  $form.FormBorderStyle = 'FixedDialog'
  $form.MaximizeBox = $false
  $form.MinimizeBox = $false
  $form.StartPosition = 'CenterScreen'
  $form.TopMost = $true
  $form.AutoSize = $true
  $form.AutoSizeMode = 'GrowAndShrink'
  $form.Padding = New-Object Windows.Forms.Padding 12
  $form.Font = [Drawing.SystemFonts]::MessageBoxFont
  if ($Icon -and (Test-Path -LiteralPath $Icon)) { try { $form.Icon = New-Object Drawing.Icon $Icon } catch { } }

  $layout = New-Object Windows.Forms.TableLayoutPanel
  $layout.AutoSize = $true
  $layout.Location = New-Object Drawing.Point 12, 12
  $layout.ColumnCount = 1
  $text = New-Object Windows.Forms.Label
  $text.AutoSize = $true
  $text.MaximumSize = New-Object Drawing.Size 440, 0
  $text.Margin = New-Object Windows.Forms.Padding 0, 0, 0, 16
  $text.Text = $message
  $layout.Controls.Add($text)

  $buttons = New-Object Windows.Forms.FlowLayoutPanel
  $buttons.AutoSize = $true
  $buttons.FlowDirection = 'RightToLeft'
  $buttons.Dock = 'Fill'
  $buttons.Margin = New-Object Windows.Forms.Padding 0
  $no = New-Object Windows.Forms.Button
  $no.Text = $noText
  $no.AutoSize = $true
  $no.DialogResult = 'No'
  $yes = New-Object Windows.Forms.Button
  $yes.Text = $yesText
  $yes.AutoSize = $true
  $yes.DialogResult = 'Yes'
  $buttons.Controls.Add($no)
  $buttons.Controls.Add($yes)
  $layout.Controls.Add($buttons)
  $form.Controls.Add($layout)
  # (Enter gives the answer the console gave by itself: the update yes, the HD textures no)
  $default = if ($yesFirst) { $yes } else { $no }
  $form.AcceptButton = $default
  $form.CancelButton = $no
  $form.Add_Shown({ $form.Activate(); $default.Focus() })
  $result = $form.ShowDialog()
  $form.Dispose()
  if ($result -eq 'Yes') { exit 10 }
  exit 11
} catch {
  exit 2
}
