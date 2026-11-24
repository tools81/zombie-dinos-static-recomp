; Inno Setup 6 script, compiled by tools/package.ps1.
;
; The program writes only to its own folder (saves\), so the default folder is
; C:\Games\Zombie Dinos: no administrator rights needed, nothing in AppData.
; During setup the player points it at their own disc image; the program itself
; checks the choice (--check-disc), and it is stored in saves\disc_path.txt.
;
; Silent: setup.exe /VERYSILENT /DISC="D:\CD-i\Zombie Dinos" [/DIR="..."] [/TASKS="portable"]

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef Repo
  #define Repo "tools81/zombie-dinos-static-recomp"
#endif
#ifndef StageDir
  #define StageDir "..\dist\stage"
#endif
#ifndef OutDir
  #define OutDir "..\dist"
#endif
#define Title "Zombie Dinos from Planet Zeltoid"
#define Exe "zombiedinos.exe"

[Setup]
AppId={{6C0D5B7E-93B4-4E0B-8E57-2F1C9A7D4E31}
AppName={#Title}
AppVersion={#AppVersion}
AppVerName={#Title} {#AppVersion}
AppPublisher=zombie-dinos-static-recomp
AppPublisherURL=https://github.com/{#Repo}
DefaultDirName={sd}\Games\Zombie Dinos
DefaultGroupName={#Title}
DisableProgramGroupPage=yes
DisableDirPage=no
UsePreviousTasks=yes
PrivilegesRequired=lowest
MinVersion=10.0.18362
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutDir}
OutputBaseFilename=ZombieDinos-{#AppVersion}-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\{#Exe}
AppMutex=ZombieDinosRecompRunning
; a portable install leaves nothing in the registry or the Start menu
Uninstallable=not WizardIsTaskSelected('portable')
CreateUninstallRegKey=not WizardIsTaskSelected('portable')

[Tasks]
Name: "normal"; Description: "Standard: Start menu entry and uninstaller"; GroupDescription: "Type of installation:"; Flags: exclusive
Name: "normal\desktop"; Description: "Desktop shortcut"
Name: "portable"; Description: "Portable: only the program folder (for a USB stick, or to move later)"; GroupDescription: "Type of installation:"; Flags: exclusive unchecked

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion

[Dirs]
Name: "{app}\saves"

[Icons]
Name: "{group}\{#Title}"; Filename: "{app}\{#Exe}"; Parameters: "--portable"; WorkingDir: "{app}"; Tasks: normal
Name: "{group}\Uninstall {#Title}"; Filename: "{uninstallexe}"; Tasks: normal
Name: "{autodesktop}\{#Title}"; Filename: "{app}\{#Exe}"; Parameters: "--portable"; WorkingDir: "{app}"; Tasks: normal\desktop

[Run]
Filename: "{app}\{#Exe}"; Parameters: "--portable"; WorkingDir: "{app}"; Description: "Play now"; Flags: postinstall nowait skipifsilent

[UninstallDelete]
; saves and quicksaves are the player's and stay
Type: files; Name: "{app}\{#Exe}.old"
Type: files; Name: "{app}\saves\recomp.log"
Type: files; Name: "{app}\saves\ramdump.bin"
Type: files; Name: "{app}\saves\update_failed.txt"
Type: filesandordirs; Name: "{app}\saves\update"

[Code]
var
  DiscPage: TWizardPage;
  DiscEdit: TNewEdit;
  Checker: String;

function DiscOk(P: String): Boolean;
var
  Code: Integer;
begin
  Result := False;
  P := RemoveBackslashUnlessRoot(Trim(P));
  if P = '' then exit;
  { "X:\" would escape the closing quote }
  if (Length(P) > 0) and (P[Length(P)] = '\') then P := P + '.';
  if Checker = '' then begin
    ExtractTemporaryFile('{#Exe}');
    Checker := ExpandConstant('{tmp}\{#Exe}');
  end;
  Result := Exec(Checker, '--check-disc "' + P + '"', '', SW_HIDE, ewWaitUntilTerminated, Code) and (Code = 0);
end;

procedure BrowseFile(Sender: TObject);
var
  F: String;
begin
  F := DiscEdit.Text;
  if not FileExists(F) then F := '';
  if GetOpenFileName('Select the disc image', F, '', 'CD-i disc image (*.cue;*.bin)|*.cue;*.bin|All files|*.*', 'cue') then
    DiscEdit.Text := F;
end;

procedure BrowseFolder(Sender: TObject);
var
  D: String;
begin
  D := DiscEdit.Text;
  if BrowseForFolder('Select the folder that holds the .cue / .bin files', D, False) then DiscEdit.Text := D;
end;

procedure InitializeWizard;
var
  L: TNewStaticText;
  B: TNewButton;
begin
  DiscPage := CreateCustomPage(wpSelectDir, 'Game files', 'Where is your copy of the game?');
  L := TNewStaticText.Create(DiscPage);
  L.Parent := DiscPage.Surface;
  L.AutoSize := False; L.WordWrap := True;
  L.Width := DiscPage.SurfaceWidth; L.Height := ScaleY(75);
  L.Caption := 'The game runs from your own Philips CD-i disc image (.cue and .bin, 2352 bytes per sector), ' +
    'which is not included. Select the file, or the folder it is in (for example "rom").' + #13#10#13#10 +
    'The files are used where they are, so keep them there.';
  DiscEdit := TNewEdit.Create(DiscPage);
  DiscEdit.Parent := DiscPage.Surface;
  DiscEdit.Top := ScaleY(82); DiscEdit.Width := DiscPage.SurfaceWidth;
  DiscEdit.Text := ExpandConstant('{param:DISC|}');
  B := TNewButton.Create(DiscPage);
  B.Parent := DiscPage.Surface;
  B.Top := ScaleY(114); B.Width := ScaleX(110); B.Height := ScaleY(25);
  B.Caption := 'Select &file...'; B.OnClick := @BrowseFile;
  B := TNewButton.Create(DiscPage);
  B.Parent := DiscPage.Surface;
  B.Left := ScaleX(120); B.Top := ScaleY(114); B.Width := ScaleX(110); B.Height := ScaleY(25);
  B.Caption := 'Select f&older...'; B.OnClick := @BrowseFolder;
end;

{ reinstalling: offer the saved location; first time: files beside the installer }
procedure CurPageChanged(CurPageID: Integer);
var
  Lines: TArrayOfString;
  S: String;
begin
  if (CurPageID = DiscPage.ID) and (DiscEdit.Text = '') then begin
    if LoadStringsFromFile(WizardDirValue + '\saves\disc_path.txt', Lines) and (GetArrayLength(Lines) > 0) then begin
      S := Lines[0];
      if (Pos(':', S) = 0) and (Pos('\\', S) <> 1) then S := AddBackslash(WizardDirValue) + S;
      if FileExists(S) or DirExists(S) then DiscEdit.Text := S;
    end;
    if (DiscEdit.Text = '') and DiscOk(ExpandConstant('{src}')) then DiscEdit.Text := ExpandConstant('{src}');
  end;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = DiscPage.ID) and not WizardSilent then begin
    if Trim(DiscEdit.Text) = '' then
      Result := MsgBox('No game files selected. Continue without them?' + #13#10#13#10 +
        'The game will ask for them when it starts.', mbConfirmation, MB_YESNO) = IDYES
    else if not DiscOk(DiscEdit.Text) then begin
      MsgBox('This is not a readable image of the {#Title} disc:' + #13#10#13#10 + Trim(DiscEdit.Text) + #13#10#13#10 +
        'Choose the .cue or .bin, or the folder that holds them.', mbError, MB_OK);
      Result := False;
    end;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  A: TArrayOfString;
  Root, P: String;
begin
  if CurStep <> ssPostInstall then exit;
  P := RemoveBackslashUnlessRoot(Trim(DiscEdit.Text));
  if not DiscOk(P) then begin
    if P <> '' then Log('disc image not valid, not saved: ' + P);
    exit;
  end;
  { inside the program folder: store it relative, so the folder can be moved }
  Root := AddBackslash(ExpandConstant('{app}'));
  if CompareText(P, RemoveBackslash(Root)) = 0 then P := '.'
  else if CompareText(Copy(P, 1, Length(Root)), Root) = 0 then Delete(P, 1, Length(Root));
  SetArrayLength(A, 1);
  A[0] := P;
  SaveStringsToUTF8File(ExpandConstant('{app}\saves\disc_path.txt'), A, False);
end;
