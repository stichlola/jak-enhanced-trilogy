; Jak Enhanced Trilogy: the Windows installer (Inno Setup 6).
;
;   ISCC /DMyVersion=v0.4 /DSrc=<release folder> /O<output folder> scripts\enhanced\installer.iss
;
; make_release.py --installer builds it from the release folder it lays out. Pages: where to install,
; the player's own disc images (any order: each is recognized by its serial), the HD textures for each
; game (with the time they take). Then it copies the programs (no game data inside) and, in the same
; window, extracts each disc and compiles its game, and makes the HD textures that were picked. The
; launcher finds all of it done (ISO\iso-paths.txt, data\out\jakN) and just plays.

#ifndef MyVersion
  #define MyVersion "dev"
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
OutputBaseFilename=Setup-Jak-Enhanced-Trilogy-{#MyVersion}
; the art: cover_art.py compose (the game wheel's portraits) -> game/assets/enhanced/art
SetupIconFile=..\..\game\assets\enhanced\art\jak-enhanced.ico
WizardImageFile=..\..\game\assets\enhanced\art\wizard-164x314.bmp,..\..\game\assets\enhanced\art\wizard-328x628.bmp
WizardSmallImageFile=..\..\game\assets\enhanced\art\wizard-small-55x55.bmp,..\..\game\assets\enhanced\art\wizard-small-110x110.bmp
UninstallDisplayIcon={app}\data\game\assets\enhanced\art\jak-enhanced.ico
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; the extracted discs: about 30 GB a game
ExtraDiskSpaceRequired=32212254720
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"
Name: "it"; MessagesFile: "compiler:Languages\Italian.isl"

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
en.PlayNow=Play now
en.Desktop=Create a desktop shortcut
it.DiscsTitle=I tuoi dischi
it.DiscsSub=Scegli le immagini (.iso) dei tuoi dischi PS2.
it.DiscsText=Uno, due o tutti e tre i giochi, in qualsiasi ordine (PAL, NTSC o NTSC-J). Jak Enhanced Trilogy non contiene dati dei giochi: tutto si ricava dai tuoi dischi, 20-40 minuti a gioco, subito dopo l'installazione.
it.Disc=Immagine disco:
it.NoDisc=Scegli almeno un'immagine disco.
it.Missing=File non trovato:
it.Unknown=Non è un disco di Jak che l'installer conosce:
it.Twice=Due immagini dello stesso gioco:
it.HdTitle=Texture HD
it.HdSub=Ogni texture ridisegnata 4x da un'IA (Real-ESRGAN) sulla tua scheda video.
it.HdText=Facoltative: i giochi funzionano uguale senza. Si fanno su questo computer dai tuoi dischi dopo l'estrazione e richiedono tempo (dipende dalla scheda video), più 1-4 GB a gioco. Per farle dopo: cancella data\out\jakN\hd-textures.asked nella cartella d'installazione, il launcher lo richiede.
it.Hd1=Jak 1 (circa 30 minuti)
it.Hd2=Jak II (circa 1-2 ore)
it.Hd3=Jak 3 (circa 1-2 ore)
it.WorkTitle=Preparazione dei giochi dai tuoi dischi
it.WorkSub=Richiede tempo: lascia aperta questa finestra. Quando finisce i giochi sono pronti.
it.Extract=Jak %1: estrazione del disco e compilazione del gioco (20-40 minuti)...
it.Python=Installazione di Python per le texture HD...
it.Hd=Jak %1: creazione delle texture HD (la parte lunga)...
it.Failed=Qualcosa è andato storto preparando Jak %1 (codice %2). Il launcher riprova al primo avvio; i dettagli sono in %3.
it.PlayNow=Gioca ora
it.Desktop=Crea un collegamento sul desktop

[Tasks]
Name: "desktopicon"; Description: "{cm:Desktop}"

[Files]
Source: "{#Src}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Jak Enhanced Trilogy"; Filename: "{app}\Jak Enhanced Trilogy.bat"; WorkingDir: "{app}"; IconFilename: "{app}\data\game\assets\enhanced\art\jak-enhanced.ico"
Name: "{group}\{cm:UninstallProgram,Jak Enhanced Trilogy}"; Filename: "{uninstallexe}"
Name: "{userdesktop}\Jak Enhanced Trilogy"; Filename: "{app}\Jak Enhanced Trilogy.bat"; WorkingDir: "{app}"; IconFilename: "{app}\data\game\assets\enhanced\art\jak-enhanced.ico"; Tasks: desktopicon

[Run]
Filename: "{app}\Jak Enhanced Trilogy.bat"; WorkingDir: "{app}"; Description: "{cm:PlayNow}"; Flags: postinstall shellexec nowait skipifsilent

[UninstallDelete]
; what the installer and the launcher made: the extracted discs, the compiled games, the HD textures
; (saves and settings stay in %APPDATA%\OpenGOAL, disc images in ISO stay)
Type: filesandordirs; Name: "{app}\data"
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
  WorkPage.Msg2Label.Caption := Copy(Trim(S), 1, 110);
  SaveStringToFile(ExpandConstant('{app}\install.log'), S + #13#10, True);
  LogLines := LogLines + 1;
  At := Pos('%]', S);
  if (At > 3) and (Copy(S, 1, 1) = '[') then begin
    Pct := StrToIntDef(Trim(Copy(S, 2, At - 2)), -1);
    if (Pct >= 0) and (Pct <= 100) then
      WorkPage.SetProgress(Pct, 100);
  end;
end;

function Run(const Filename, Params: String): Integer;
begin
  WorkPage.SetProgress(0, 100);
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
  WorkPage.SetText(CustomMessage('Python'), '');
  Run('winget', 'install --id Python.Python.3.12 -e --scope user --accept-package-agreements --accept-source-agreements');
  Result := ExpandConstant('{localappdata}\Programs\Python\Launcher\py.exe');
  if not FileExists(Result) then
    Result := 'py';
end;

procedure MakeGames;
var
  g, Code: Integer;
  App, Data, Version, Paths: String;
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

  WorkPage.Show;
  try
    for g := 1 to 3 do begin
      if GameIso[g] = '' then
        continue;
      WorkPage.SetText(FmtMessage(CustomMessage('Extract'), [IntToStr(g)]), '');
      Code := Run(App + '\extractor.exe', '--game jak' + IntToStr(g) + ' --proj-path "' + Data +
                  '" -e -d -c "' + GameIso[g] + '"');
      if Code <> 0 then begin
        MsgBox(FmtMessage(CustomMessage('Failed'), [IntToStr(g), IntToStr(Code), App + '\install.log']),
               mbError, MB_OK);
        continue;
      end;
      if Version <> '' then
        SaveStringToFile(Data + '\out\jak' + IntToStr(g) + '\built-version.txt', Version, False);
      if HdPage.Values[g - 1] then begin
        WorkPage.SetText(FmtMessage(CustomMessage('Hd'), [IntToStr(g)]), '');
        Code := Run(PythonLauncher, '-3 "' + Data + '\scripts\enhanced\hd_setup.py" --games jak' +
                    IntToStr(g) + ' --bin "' + App + '"');
        if Code <> 0 then
          MsgBox(FmtMessage(CustomMessage('Failed'), [IntToStr(g), IntToStr(Code), App + '\install.log']),
                 mbError, MB_OK);
      end;
      // the launcher doesn't ask again (skipped ones: delete the file to be asked)
      SaveStringToFile(Data + '\out\jak' + IntToStr(g) + '\hd-textures.asked', 'asked', False);
    end;
  finally
    WorkPage.Hide;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and not WizardSilent then
    MakeGames;
end;
