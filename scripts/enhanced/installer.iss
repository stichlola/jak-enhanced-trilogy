; Jak Enhanced Trilogy: the Windows installer (Inno Setup 6).
;
;   ISCC /DMyVersion=v0.4 /DSrc=<release folder> /O<output folder> scripts\enhanced\installer.iss
;
; make_release.py --installer builds it from the release folder it lays out. Pages: where to install,
; the player's own disc images (any order: each is recognized by its serial), the HD textures for each
; game (with the time they take). Then it copies the programs (no game data inside) and, in the same
; window, extracts each disc and compiles its game, and makes the HD textures that were picked. The
; launcher finds all of it done (ISO\iso-paths.txt, data\out\jakN) and just plays.
;
; Those pages are the fallback: a normal start shows installer_ui.ps1 instead (a dark WPF window: the
; discs found on this PC, where, the HD textures, the build's progress, play), which runs this same
; Setup again silently with /JEUI=<its folder> (the choices in, the progress out: see that script).
; /CLASSIC shows the wizard pages; /SILENT and /VERYSILENT work as before.

#ifndef MyVersion
  #define MyVersion "dev"
#endif
; the programs' size in MB, for the window's "Needs N GB" (make_release.py passes the real one)
#ifndef ProgMB
  #define ProgMB "1200"
#endif
#ifndef Src
  #define Src "..\..\out\release\Jak-Enhanced-Trilogy"
#endif

[Setup]
AppId={{5C7E2A51-3B8F-4E0A-9D4C-8A1F6E2B7C90}
AppName=Jak Enhanced Trilogy
AppVersion={#MyVersion}
AppPublisher=stichlola
AppPublisherURL=https://github.com/stichlola/jak-enhanced-trilogy
DefaultDirName={localappdata}\Programs\Jak Enhanced Trilogy
DefaultGroupName=Jak Enhanced Trilogy
PrivilegesRequired=lowest
DisableProgramGroupPage=yes
; (the welcome page says the game is free, with the buy-me-a-coffee line)
DisableWelcomePage=no
OutputBaseFilename=Setup-Jak-Enhanced-Trilogy-{#MyVersion}
; the art: cover_art.py compose (the start menu's pictures, wheel/menuN.jpg + title.png) -> game/assets/enhanced/art
SetupIconFile=..\..\game\assets\enhanced\art\jak-enhanced.ico
WizardImageFile=..\..\game\assets\enhanced\art\wizard-164x314.bmp,..\..\game\assets\enhanced\art\wizard-328x628.bmp
WizardSmallImageFile=..\..\game\assets\enhanced\art\wizard-small-55x55.bmp,..\..\game\assets\enhanced\art\wizard-small-110x110.bmp
UninstallDisplayIcon={app}\data\game\assets\enhanced\art\jak-enhanced.ico
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; English only, no language question
ShowLanguageDialog=no
; the extracted discs: about 30 GB a game
ExtraDiskSpaceRequired=32212254720
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
en.DiscsTitle=Your discs
en.DiscsSub=Pick your own disc images (.iso) of the PS2 games.
en.DiscsText=One, two or all three games, in any order (PAL, NTSC or NTSC-J). No game data comes with Jak Enhanced Trilogy: everything is made from your discs, 20-40 minutes a game, right after the install.
en.Disc=Disc image:
en.NoDisc=Pick at least one disc image.
en.Missing=File not found:
en.Unknown=Not a Jak disc this installer knows:
en.Twice=Two disc images of the same game:
en.HdTitle=HD textures
en.HdSub=Every texture redrawn 4x by an AI (Real-ESRGAN) on your graphics card.
en.HdText=Optional: the games work the same without them. They are made on this computer from your discs after the extraction and take a while (it depends on the graphics card), plus 1-4 GB a game. To make them later: delete data\out\jakN\hd-textures.asked in the install folder, the launcher asks again.
en.Hd1=Jak 1 (about 30 minutes)
en.Hd2=Jak II (about 1-2 hours)
en.Hd3=Jak 3 (about 1-2 hours)
en.WorkTitle=Making the games from your discs
en.WorkSub=This takes a while; leave this window open. The games are ready when it ends.
en.Extract=Jak %1: extracting your disc and compiling the game (20-40 minutes)...
en.Python=Installing Python for the HD textures...
en.Hd=Jak %1: making the HD textures (the long part)...
en.Failed=Something went wrong making Jak %1 (code %2). The launcher tries again at its first start; the details are in %3.
en.GameOpen=Jak Enhanced Trilogy is open from this folder, and it has to close before installing.%n%nSave your game first: what is not saved is lost.%n%nClose it now?
en.GameStays=Jak Enhanced Trilogy is still open, so nothing was installed. Close it and run this setup again.
en.GameStuck=Jak Enhanced Trilogy could not be closed, so nothing was installed. Close it by hand (or restart the computer) and run this setup again.
en.PlayNow=Play now
en.Desktop=Create a desktop shortcut
en.Steam=Add it to Steam (as a non-Steam game; Steam closes for a moment if it is open)
en.Free=Jak Enhanced is 100% free, and always will be. Made by one person in spare time: if you'd like, buy me a coffee (optional, unlocks nothing).
en.DonateHead=Jak Enhanced is 100% free — support is optional
en.Donate1=1 € — a sweet
en.Donate2=3 € — a coffee
en.Donate3=Any amount
en.Trust=Secure payment on PayPal's own page (paypal.me/mymusicminis), to Code & Craft Solutions, the author's account. You can change the amount before paying.
; the installer's window (installer_ui.ps1)
en.UiStepDiscs=Discs
en.UiStepOptions=Options
en.UiStepBuild=Build
en.UiStepPlay=Play
en.UiFound=Found on this PC
en.UiSearching=Looking for your disc images...
en.UiNoneFound=No Jak disc image found on this PC. Pick yours with Browse, or drop it on this window.
en.UiBrowse=Not listed? Browse...
en.UiDrop=or drop the file on this window
en.UiRemove=Remove from the list
en.UiNext=Next
en.UiBack=Back
en.UiOptTitle=Build your games
en.UiOptSub=Where they go, and the optional HD textures.
en.UiInstallTo=Install the games to
en.UiChange=Change
en.UiSizeNote=The programs take %1 GB; each game made from its disc goes in the same folder (about %2 GB a game).
en.UiInstall=Build the games
en.UiNeeds=Needs %1 GB · %2 has %3 GB free
en.UiNeedsOnly=Needs %1 GB
en.UiAbout=About %1
en.UiElapsed=Elapsed %1
en.UiBadDir=Pick a full folder path (like C:\Games\Jak Enhanced Trilogy).
en.UiNoSpace=Not enough space: it needs %1 GB and the drive has %2 GB free. Pick another folder.
en.UiSteps=Steps
en.UiCopy=Copying the programs...
en.UiCopyRow=Copy the programs
en.UiExtractRow=%1: extract the disc and compile the game
en.UiHdRow=%1: HD textures
en.UiSetupFailed=The setup stopped (code %1). The details are in %2.
en.UiCancelAsk=Stop now? What is not done, the launcher makes at its first start.
en.UiDoneTitle=Ready to play
en.UiDoneSub=Your games are made from your discs. Press \ in a game for the Enhanced menu.
en.UiDoneFailTitle=Done, with problems
en.UiDoneFailSub=Some steps failed; the launcher tries them again at its first start.
en.UiOpenLog=Open the log
en.UiFinish=Close

[Tasks]
Name: "desktopicon"; Description: "{cm:Desktop}"
Name: "steam"; Description: "{cm:Steam}"; Flags: unchecked

[Files]
; the dontcopy files come first: with solid compression, extracting one decompresses every file
; listed before it (the whole game when they were last: the window took ~15 s to show)
; run from the temporary folder before anything is copied: closes a game open from this folder
Source: "{#Src}\data\scripts\enhanced\update.ps1"; Flags: dontcopy
; the buy-me-a-coffee buttons on the last page (drawn by ask.ps1 -ExportIcons)
Source: "..\..\game\assets\enhanced\art\donate-*.bmp"; Flags: dontcopy
; the installer's own window (in place of the wizard pages) and its icon
Source: "installer_ui.ps1"; Flags: dontcopy
Source: "..\..\game\assets\enhanced\art\jak-enhanced.ico"; Flags: dontcopy
; (the main world the player edits comes in data\shipped: the launcher puts it in place, keeping their edits)
Source: "{#Src}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Jak Enhanced Trilogy"; Filename: "{app}\Jak Enhanced Trilogy.bat"; WorkingDir: "{app}"; IconFilename: "{app}\data\game\assets\enhanced\art\jak-enhanced.ico"; Flags: runminimized
Name: "{group}\Report a problem"; Filename: "{app}\Report a problem.bat"; WorkingDir: "{app}"; Flags: runminimized
Name: "{group}\{cm:UninstallProgram,Jak Enhanced Trilogy}"; Filename: "{uninstallexe}"
Name: "{userdesktop}\Jak Enhanced Trilogy"; Filename: "{app}\Jak Enhanced Trilogy.bat"; WorkingDir: "{app}"; IconFilename: "{app}\data\game\assets\enhanced\art\jak-enhanced.ico"; Flags: runminimized; Tasks: desktopicon

[Run]
Filename: "{app}\Jak Enhanced Trilogy.bat"; WorkingDir: "{app}"; Description: "{cm:PlayNow}"; Flags: postinstall shellexec nowait skipifsilent runminimized

[UninstallDelete]
; what the installer and the launcher made: the extracted discs, the compiled games, the HD textures
; (saves and settings stay in %APPDATA%\OpenGOAL, disc images in ISO stay)
Type: filesandordirs; Name: "{app}\data"
Type: filesandordirs; Name: "{app}\backup"
Type: files; Name: "{app}\ISO\iso-paths.txt"
Type: files; Name: "{app}\install.log"

[Code]
var
  DiscPage: TInputFileWizardPage;
  HdPage: TInputOptionWizardPage;
  WorkPage: TOutputProgressWizardPage;
  // the disc picked for each game (1-3), from the serials
  GameIso: array[1..3] of String;
  LogLines: Integer;
  // the buy-me-a-coffee buttons on the last page
  DonateHead, DonateTrust: TNewStaticText;
  DonatePanel: array[1..3] of TPanel;
  DonateLock: TBitmapImage;
  // run by the installer's window (/JEUI=<its folder>): the choices from there, the progress to there
  UiDir: String;
  UiHd: array[1..3] of Boolean;
  UiPct: Integer;

const
  DonateUrl = 'https://paypal.me/mymusicminis';
  AppKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{5C7E2A51-3B8F-4E0A-9D4C-8A1F6E2B7C90}_is1';

// a line for the installer's window (status.txt: see installer_ui.ps1)
procedure UiStatus(const S: String);
begin
  if UiDir <> '' then
    SaveStringToFile(UiDir + '\status.txt', S + #13#10, True);
end;

procedure StepText(const Id, Text: String);
begin
  if UiDir <> '' then begin
    UiPct := -1;
    UiStatus('@@step ' + Id);
  end else
    WorkPage.SetText(Text, '');
end;

procedure StepProgress(Pct: Integer);
begin
  if UiDir <> '' then begin
    if Pct <> UiPct then
      UiStatus('@@pct ' + IntToStr(Pct));
    UiPct := Pct;
  end else
    WorkPage.SetProgress(Pct, 100);
end;

procedure StepFailed(const Id: String; g, Code: Integer);
begin
  if UiDir <> '' then
    UiStatus('@@fail ' + Id + ' ' + IntToStr(Code))
  else
    MsgBox(FmtMessage(CustomMessage('Failed'), [IntToStr(g), IntToStr(Code), ExpandConstant('{app}') + '\install.log']),
           mbError, MB_OK);
end;

// the copying of the files, for the window's bar
procedure CurInstallProgressChanged(CurProgress, MaxProgress: Integer);
begin
  if (UiDir <> '') and (MaxProgress > 0) then
    if MaxProgress >= 100 then
      StepProgress(CurProgress div (MaxProgress div 100))
    else
      StepProgress(CurProgress * 100 div MaxProgress);
end;

// which game a disc image is: the serial on its boot line ("cdrom0:\SCES_524.60;1") in its first
// megabytes, as the launcher does. 0: none of the three
function DiscGame(const Path: String): Integer;
var
  Stream: TFileStream;
  Buf: AnsiString;
  Count, At: Integer;
  Id: String;
begin
  Result := 0;
  try
    Stream := TFileStream.Create(Path, fmOpenRead or fmShareDenyNone);
    try
      Count := 8 * 1024 * 1024;
      if Stream.Size < Count then
        Count := Stream.Size;
      SetLength(Buf, Count);
      Stream.ReadBuffer(Buf, Count);
    finally
      Stream.Free;
    end;
  except
    exit;
  end;
  At := Pos('cdrom0:\', Buf);
  if At = 0 then
    exit;
  Id := Copy(Buf, At + 8, 11);
  if (Id = 'SCUS_971.24') or (Id = 'SCES_503.61') or (Id = 'SCPS_150.21') then
    Result := 1
  else if (Id = 'SCUS_972.65') or (Id = 'SCES_516.08') or (Id = 'SCPS_150.57') then
    Result := 2
  else if (Id = 'SCUS_973.30') or (Id = 'SCES_524.60') or (Id = 'SCPS_150.93') then
    Result := 3;
end;

// PayPal.Me in the browser; the installer stays as it is
procedure DonateClick(Sender: TObject);
var
  Url: String;
  ErrorCode: Integer;
begin
  case TComponent(Sender).Tag of
    1: Url := DonateUrl + '/1EUR';
    2: Url := DonateUrl + '/3EUR';
  else
    Url := DonateUrl;
  end;
  ShellExec('open', Url, '', '', SW_SHOWNORMAL, ewNoWait, ErrorCode);
end;

function DonateBitmap(const Name: String; const Parent: TWinControl): TBitmapImage;
begin
  ExtractTemporaryFile('donate-' + Name + '.bmp');
  Result := TBitmapImage.Create(WizardForm);
  Result.Parent := Parent;
  Result.Stretch := True;
  Result.Bitmap.LoadFromFile(ExpandConstant('{tmp}\donate-' + Name + '.bmp'));
end;

// the Finished page: PayPal-blue buttons one under the other (icon, amount, what it buys), then
// where the money goes; placed under the "Play now" box when the page shows
procedure CreateDonateButtons;
var
  i: Integer;
  Page: TNewNotebookPage;
  Icon: TBitmapImage;
  Text: TNewStaticText;
  Names: array[1..3] of String;
begin
  Page := WizardForm.FinishedPage;
  Names[1] := 'sweet'; Names[2] := 'coffee'; Names[3] := 'heart';
  DonateHead := TNewStaticText.Create(WizardForm);
  DonateHead.Parent := Page;
  DonateHead.Caption := CustomMessage('DonateHead');
  DonateHead.Font.Style := [fsBold];
  for i := 1 to 3 do
  begin
    DonatePanel[i] := TPanel.Create(WizardForm);
    DonatePanel[i].Parent := Page;
    DonatePanel[i].BevelOuter := bvNone;
    DonatePanel[i].ParentBackground := False;
    DonatePanel[i].Color := $BA7000;
    DonatePanel[i].Cursor := crHand;
    DonatePanel[i].Tag := i;
    DonatePanel[i].OnClick := @DonateClick;
    DonatePanel[i].Height := ScaleY(26);
    DonatePanel[i].Width := ScaleX(200);
    Icon := DonateBitmap(Names[i], DonatePanel[i]);
    Icon.SetBounds(ScaleX(8), ScaleY(5), ScaleY(16), ScaleY(16));
    Icon.Cursor := crHand;
    Icon.Tag := i;
    Icon.OnClick := @DonateClick;
    Text := TNewStaticText.Create(WizardForm);
    Text.Parent := DonatePanel[i];
    Text.Caption := CustomMessage('Donate' + IntToStr(i));
    Text.Font.Color := clWhite;
    Text.Font.Style := [fsBold];
    Text.Left := ScaleX(32);
    Text.Top := (DonatePanel[i].Height - Text.Height) div 2;
    Text.Cursor := crHand;
    Text.Tag := i;
    Text.OnClick := @DonateClick;
  end;
  DonateLock := DonateBitmap('lock', Page);
  DonateTrust := TNewStaticText.Create(WizardForm);
  DonateTrust.Parent := Page;
  DonateTrust.AutoSize := False;
  DonateTrust.WordWrap := True;
  DonateTrust.ShowAccelChar := False;
  DonateTrust.Font.Color := clGrayText;
  DonateTrust.Caption := CustomMessage('Trust');
end;

// the buttons on a page (the welcome page and the finished page share them), from Top down
// (WithHead: the welcome page already says it in its text, and has no room for it)
procedure PlaceDonate(Page: TNewNotebookPage; Left, Top, Width: Integer; WithHead: Boolean);
var
  i: Integer;
begin
  DonateHead.Parent := Page;
  for i := 1 to 3 do
    DonatePanel[i].Parent := Page;
  DonateLock.Parent := Page;
  DonateTrust.Parent := Page;
  DonateHead.Visible := WithHead;
  if WithHead then
  begin
    DonateHead.SetBounds(Left, Top, Width, DonateHead.Height);
    Top := Top + DonateHead.Height + ScaleY(6);
  end;
  for i := 1 to 3 do
  begin
    DonatePanel[i].Left := Left;
    DonatePanel[i].Top := Top;
    Top := Top + DonatePanel[i].Height + ScaleY(4);
  end;
  DonateLock.SetBounds(Left, Top + ScaleY(3), ScaleY(12), ScaleY(12));
  DonateTrust.SetBounds(Left + ScaleX(16), Top + ScaleY(2), Width - ScaleX(16), ScaleY(44));
end;

procedure CurPageChanged(CurPageID: Integer);
var
  Top: Integer;
begin
  if CurPageID = wpWelcome then
  begin
    // (the label fills the page by default: down to its own text first)
    WizardForm.WelcomeLabel2.AdjustHeight;
    PlaceDonate(WizardForm.WelcomePage, WizardForm.WelcomeLabel2.Left,
      WizardForm.WelcomeLabel2.Top + WizardForm.WelcomeLabel2.Height + ScaleY(10),
      WizardForm.WelcomeLabel2.Width, False);
  end
  else if CurPageID = wpFinished then
  begin
    Top := WizardForm.FinishedLabel.Top + WizardForm.FinishedLabel.Height;
    if WizardForm.RunList.Visible then
      Top := WizardForm.RunList.Top + WizardForm.RunList.Height;
    PlaceDonate(WizardForm.FinishedPage, WizardForm.FinishedLabel.Left, Top + ScaleY(14),
      WizardForm.FinishedLabel.Width, True);
  end;
end;

procedure InitializeWizard;
var
  i: Integer;
begin
  DiscPage := CreateInputFilePage(wpSelectDir, CustomMessage('DiscsTitle'), CustomMessage('DiscsSub'),
    CustomMessage('DiscsText'));
  for i := 1 to 3 do
    DiscPage.Add(CustomMessage('Disc'), 'Disc images (*.iso)|*.iso|All files|*.*', '.iso');
  HdPage := CreateInputOptionPage(DiscPage.ID, CustomMessage('HdTitle'), CustomMessage('HdSub'),
    CustomMessage('HdText'), False, False);
  for i := 1 to 3 do
    HdPage.Add(CustomMessage('Hd' + IntToStr(i)));
  WorkPage := CreateOutputProgressPage(CustomMessage('WorkTitle'), CustomMessage('WorkSub'));
  WizardForm.WelcomeLabel2.Caption := WizardForm.WelcomeLabel2.Caption + #13#10#13#10 + CustomMessage('Free');
  CreateDonateButtons;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  i, g, count: Integer;
  path: String;
begin
  Result := True;
  // a silent install (/VERYSILENT) leaves the discs to the ISO folder and the launcher
  if (CurPageID <> DiscPage.ID) or WizardSilent then
    exit;
  for g := 1 to 3 do
    GameIso[g] := '';
  count := 0;
  for i := 0 to 2 do begin
    path := Trim(DiscPage.Values[i]);
    if path = '' then
      continue;
    if not FileExists(path) then begin
      MsgBox(CustomMessage('Missing') + #13#10 + path, mbError, MB_OK);
      Result := False;
      exit;
    end;
    g := DiscGame(path);
    if g = 0 then begin
      MsgBox(CustomMessage('Unknown') + #13#10 + path, mbError, MB_OK);
      Result := False;
      exit;
    end;
    if GameIso[g] <> '' then begin
      MsgBox(CustomMessage('Twice') + #13#10 + GameIso[g] + #13#10 + path, mbError, MB_OK);
      Result := False;
      exit;
    end;
    GameIso[g] := path;
    count := count + 1;
  end;
  if count = 0 then begin
    MsgBox(CustomMessage('NoDisc'), mbError, MB_OK);
    Result := False;
  end;
end;

function ShouldSkipPage(PageID: Integer): Boolean;
var
  g: Integer;
begin
  // the HD textures only for the games that have a disc
  Result := False;
  if PageID = HdPage.ID then
    for g := 1 to 3 do begin
      HdPage.CheckListBox.ItemEnabled[g - 1] := GameIso[g] <> '';
      if GameIso[g] = '' then
        HdPage.Values[g - 1] := False;
    end;
end;

// a line of the running program's output: under the step's title, and into install.log; a
// "[ 45%]" from the compiler moves the bar
procedure OnLog(const S: String; const Error, FirstLine: Boolean);
var
  At, Pct: Integer;
begin
  if Trim(S) = '' then
    exit;
  if UiDir <> '' then
    UiStatus('@@line ' + Copy(Trim(S), 1, 200))
  else
    WorkPage.Msg2Label.Caption := Copy(Trim(S), 1, 110);
  SaveStringToFile(ExpandConstant('{app}\install.log'), S + #13#10, True);
  LogLines := LogLines + 1;
  At := Pos('%]', S);
  if (At > 3) and (Copy(S, 1, 1) = '[') then begin
    Pct := StrToIntDef(Trim(Copy(S, 2, At - 2)), -1);
    if (Pct >= 0) and (Pct <= 100) then
      StepProgress(Pct);
  end;
end;

// a path for PowerShell's single quotes
function PsQuote(const S: String): String;
begin
  Result := S;
  StringChangeEx(Result, '''', '''''', True);
  Result := '''' + Result + '''';
end;

// a script of ours in PowerShell, as the launcher runs them (no execution policy involved)
function PsParams(const Script, Args: String): String;
begin
  Result := '-NoProfile -Command "& ([scriptblock]::Create([IO.File]::ReadAllText(' + PsQuote(Script) + '))) ' + Args + '"';
end;

// a game open from the folder: asks, closes it and waits, before anything is copied
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  App, Script: String;
  Code: Integer;
begin
  Result := '';
  App := WizardDirValue;
  if not DirExists(App) then
    exit;
  ExtractTemporaryFile('update.ps1');
  Script := ExpandConstant('{tmp}\update.ps1');
  if not Exec('powershell.exe', PsParams(Script, '-Root ' + PsQuote(App) + ' -CheckOnly'), '', SW_HIDE,
              ewWaitUntilTerminated, Code) or (Code <> 10) then
    exit;
  if SuppressibleMsgBox(CustomMessage('GameOpen'), mbConfirmation, MB_YESNO, IDYES) <> IDYES then begin
    Result := CustomMessage('GameStays');
    exit;
  end;
  if not Exec('powershell.exe', PsParams(Script, '-Root ' + PsQuote(App) + ' -CloseOnly'), '', SW_HIDE,
              ewWaitUntilTerminated, Code) or (Code <> 0) then
    Result := CustomMessage('GameStuck');
  if Result <> '' then
    UiStatus('@@fail prepare 0');
end;

function Run(const Filename, Params: String): Integer;
begin
  StepProgress(0);
  SaveStringToFile(ExpandConstant('{app}\install.log'), '> ' + Filename + ' ' + Params + #13#10, True);
  if not ExecAndLogOutput(Filename, Params, ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated,
                          Result, @OnLog) then
    Result := -1;
end;

// the Python launcher (py.exe): the one on the PATH, or the one winget installs for this user
function PythonLauncher: String;
var
  Code: Integer;
begin
  Result := ExpandConstant('{localappdata}\Programs\Python\Launcher\py.exe');
  if FileExists(Result) then
    exit;
  if Exec(ExpandConstant('{cmd}'), '/c where py', '', SW_HIDE, ewWaitUntilTerminated, Code) and (Code = 0) then begin
    Result := 'py';
    exit;
  end;
  if UiDir <> '' then
    UiStatus('@@note python')
  else
    WorkPage.SetText(CustomMessage('Python'), '');
  Run('winget', 'install --id Python.Python.3.12 -e --scope user --accept-package-agreements --accept-source-agreements');
  Result := ExpandConstant('{localappdata}\Programs\Python\Launcher\py.exe');
  if not FileExists(Result) then
    Result := 'py';
end;

function WantHd(g: Integer): Boolean;
begin
  if UiDir <> '' then
    Result := UiHd[g]
  else
    Result := HdPage.Values[g - 1];
end;

procedure MakeGames;
var
  g, Code: Integer;
  App, Data, Version, Paths, Py: String;
  Ver: AnsiString;
begin
  App := ExpandConstant('{app}');
  Data := App + '\data';
  // the launcher's list of discs
  Paths := '';
  for g := 1 to 3 do
    if GameIso[g] <> '' then
      Paths := Paths + GameIso[g] + #13#10;
  ForceDirectories(App + '\ISO');
  SaveStringToFile(App + '\ISO\iso-paths.txt', Paths, False);
  Version := '';
  if LoadStringFromFile(Data + '\VERSION', Ver) then
    Version := Trim(String(Ver));

  if UiDir = '' then
    WorkPage.Show;
  try
    for g := 1 to 3 do begin
      if GameIso[g] = '' then
        continue;
      StepText('x' + IntToStr(g), FmtMessage(CustomMessage('Extract'), [IntToStr(g)]));
      Code := Run(App + '\extractor.exe', '--game jak' + IntToStr(g) + ' --proj-path "' + Data +
                  '" -e -d -c "' + GameIso[g] + '"');
      if Code <> 0 then begin
        StepFailed('x' + IntToStr(g), g, Code);
        continue;
      end;
      if Version <> '' then
        SaveStringToFile(Data + '\out\jak' + IntToStr(g) + '\built-version.txt', Version, False);
      // (this made the levels with the replacement models as they are now)
      Run('powershell.exe', PsParams(Data + '\scripts\enhanced\refresh_models.ps1', '-Proj ' + PsQuote(Data) +
          ' -Bin ' + PsQuote(App) + ' -Game jak' + IntToStr(g) + ' -MarkOnly'));
      if WantHd(g) then begin
        StepText('hd' + IntToStr(g), FmtMessage(CustomMessage('Hd'), [IntToStr(g)]));
        // (Python first: its own line while winget installs it, then the HD textures' line again)
        Py := PythonLauncher;
        StepText('hd' + IntToStr(g), FmtMessage(CustomMessage('Hd'), [IntToStr(g)]));
        Code := Run(Py, '-3 "' + Data + '\scripts\enhanced\hd_setup.py" --games jak' +
                    IntToStr(g) + ' --bin "' + App + '"');
        if Code <> 0 then
          StepFailed('hd' + IntToStr(g), g, Code);
      end;
      // the launcher doesn't ask again (skipped ones: delete the file to be asked)
      SaveStringToFile(Data + '\out\jak' + IntToStr(g) + '\hd-textures.asked', 'asked', False);
    end;
  finally
    if UiDir = '' then
      WorkPage.Hide;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Code: Integer;
begin
  if CurStep = ssInstall then
    StepText('copy', '');
  if CurStep <> ssPostInstall then
    exit;
  // (Steam's list of non-Steam games: add_to_steam.ps1)
  if WizardIsTaskSelected('steam') then
    Exec('powershell.exe', PsParams(ExpandConstant('{app}\data\scripts\enhanced\add_to_steam.ps1'),
         '-Root ' + PsQuote(ExpandConstant('{app}'))), '', SW_HIDE, ewWaitUntilTerminated, Code);
  if (UiDir <> '') or not WizardSilent then
    MakeGames;
  UiStatus('@@done');
end;

// ---- the installer's window (installer_ui.ps1) ----------------------------------------------------

// the run the window started: the discs and the HD textures it chose
procedure LoadUiChoices;
var
  g: Integer;
  Ini: String;
begin
  Ini := UiDir + '\choices.ini';
  for g := 1 to 3 do begin
    GameIso[g] := GetIniString('discs', 'jak' + IntToStr(g), '', Ini);
    UiHd[g] := GetIniInt('hd', 'jak' + IntToStr(g), 0, 0, 1, Ini) = 1;
  end;
end;

function HasParam(const Name: String): Boolean;
var
  i: Integer;
begin
  Result := False;
  for i := 1 to ParamCount do
    if CompareText(ParamStr(i), Name) = 0 then
      Result := True;
end;

// the texts the window shows: the CustomMessages, in the language picked
procedure AddUiText(var Lines: TArrayOfString; const Line: String);
var
  S: String;
begin
  S := Line;
  StringChangeEx(S, #13#10, '\n', True);
  StringChangeEx(S, #10, '\n', True);
  SetArrayLength(Lines, GetArrayLength(Lines) + 1);
  Lines[GetArrayLength(Lines) - 1] := S;
end;

// True: the window did it all (or the player closed it); False: it could not start, the wizard instead
function RunUi: Boolean;
var
  Dir, Tmp, Prev, Name: String;
  Lines: TArrayOfString;
  Keys: TStringList;
  i, Code: Integer;
begin
  Result := False;
  Tmp := ExpandConstant('{tmp}');
  Dir := Tmp + '\ui';
  ForceDirectories(Dir);
  try
    ExtractTemporaryFile('installer_ui.ps1');
    ExtractTemporaryFile('update.ps1');
    ExtractTemporaryFile('jak-enhanced.ico');
    ExtractTemporaryFile('donate-sweet.bmp');
    ExtractTemporaryFile('donate-coffee.bmp');
    ExtractTemporaryFile('donate-heart.bmp');
    ExtractTemporaryFile('donate-lock.bmp');
  except
    exit;
  end;
  // the folder of the install there is (as the wizard would propose it)
  Prev := '';
  if not RegQueryStringValue(HKCU, AppKey, 'Inno Setup: App Path', Prev) then
    RegQueryStringValue(HKLM, AppKey, 'Inno Setup: App Path', Prev);
  if Prev = '' then
    Prev := ExpandConstant('{localappdata}\Programs\Jak Enhanced Trilogy');
  AddUiText(Lines, 'Setup=' + ExpandConstant('{srcexe}'));
  AddUiText(Lines, 'Lang=' + ActiveLanguage);
  AddUiText(Lines, 'DefaultDir=' + Prev);
  AddUiText(Lines, 'ProgMB={#ProgMB}');
  AddUiText(Lines, 'Icon=' + Tmp + '\jak-enhanced.ico');
  AddUiText(Lines, 'Update=' + Tmp + '\update.ps1');
  AddUiText(Lines, 'Art=' + Tmp);
  Keys := TStringList.Create;
  try
    Keys.CommaText := 'DiscsTitle,DiscsSub,DiscsText,Missing,Unknown,HdTitle,HdSub,HdText,Hd1,Hd2,Hd3,' +
      'WorkTitle,WorkSub,Extract,Python,Hd,Failed,GameOpen,GameStays,GameStuck,PlayNow,Desktop,Steam,Free,' +
      'DonateHead,Donate1,Donate2,Donate3,Trust,UiStepDiscs,UiStepOptions,UiStepBuild,UiStepPlay,UiFound,' +
      'UiSearching,UiNoneFound,UiBrowse,UiDrop,UiRemove,UiNext,UiBack,UiOptTitle,UiOptSub,UiInstallTo,UiChange,' +
      'UiSizeNote,UiInstall,UiNeeds,UiNeedsOnly,UiAbout,UiElapsed,UiBadDir,UiNoSpace,UiSteps,UiCopy,UiCopyRow,' +
      'UiExtractRow,UiHdRow,UiSetupFailed,UiCancelAsk,UiDoneTitle,UiDoneSub,UiDoneFailTitle,UiDoneFailSub,' +
      'UiOpenLog,UiFinish';
    for i := 0 to Keys.Count - 1 do begin
      Name := Keys[i];
      AddUiText(Lines, 'S.' + Name + '=' + CustomMessage(Name));
    end;
  finally
    Keys.Free;
  end;
  if not SaveStringsToUTF8File(Dir + '\ui.txt', Lines, False) then
    exit;
  if not Exec('powershell.exe', '-NoProfile -STA -WindowStyle Hidden -Command "& ([scriptblock]::Create([IO.File]::ReadAllText(' +
              PsQuote(Tmp + '\installer_ui.ps1') + '))) -Dir ' + PsQuote(Dir) + '"', '', SW_HIDE,
              ewWaitUntilTerminated, Code) then
    exit;
  // (3: the window failed; ui-error.txt says why)
  Result := Code = 0;
end;

function InitializeSetup: Boolean;
begin
  Result := True;
  UiPct := -1;
  UiDir := ExpandConstant('{param:JEUI|}');
  if UiDir <> '' then begin
    LoadUiChoices;
    exit;
  end;
  if WizardSilent or HasParam('/CLASSIC') then
    exit;
  // the window did the install: no wizard after it
  if RunUi then
    Result := False;
end;