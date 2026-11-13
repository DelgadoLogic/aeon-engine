; AeonBrowser - Inno Setup Installer Script
; DelgadoLogic | Build Team
;
; Builds: AeonBrowser-Setup-{version}.exe
; Matches: installer_preview.jpg modern dark UI

#define AppName "Aeon Browser"
#define AppPublisher "DelgadoLogic"
#define AppURL "https://delgadologic.tech"
#define AppUpdateURL "https://update.delgadologic.tech/aeon/"
#ifndef Version
  #define Version "1.0.0"
#endif
#define AppExeName "Aeon.exe"
#define AppMutex "AeonBrowserSingleInstance"

[Setup]
AppId={{8A5F3C2D-1B4E-4D9A-A8F1-5E3B7C2D9F4A}
AppName={#AppName}
AppVersion={#Version}
AppVerName={#AppName} {#Version}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}/support
AppUpdatesURL={#AppUpdateURL}
DefaultDirName={autopf}\DelgadoLogic\AeonBrowser
DefaultGroupName={#AppName}
AllowNoIcons=yes
LicenseFile=..\LICENSE
OutputDir=Output
OutputBaseFilename=AeonBrowser-Setup-{#Version}
SetupIconFile=..\resources\icons\Aeon.ico
Compression=lzma2/ultra64
SolidCompression=yes
InternalCompressLevel=ultra64
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
MinVersion=6.1
DisableWelcomePage=yes
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=yes
DisableFinishedPage=yes

UninstallDisplayIcon={app}\{#AppExeName}
UninstallDisplayName={#AppName}
CreateUninstallRegKey=yes
UpdateUninstallLogAppName=yes

[Registry]
Root: HKA; Subkey: "SOFTWARE\DelgadoLogic\Aeon"; ValueType: string; ValueName: "Version"; ValueData: "{#Version}"; Flags: uninsdeletevalue
Root: HKA; Subkey: "SOFTWARE\DelgadoLogic\Aeon"; ValueType: string; ValueName: "InstallDir"; ValueData: "{app}"; Flags: uninsdeletevalue
Root: HKA; Subkey: "SOFTWARE\DelgadoLogic\Aeon"; ValueType: dword; ValueName: "TelemetryEnabled"; ValueData: "0"; Flags: uninsdeletevalue createvalueifdoesntexist
Root: HKA; Subkey: "SOFTWARE\Clients\StartMenuInternet\Aeon"; ValueType: string; ValueData: "Aeon Browser"; Flags: uninsdeletekey
Root: HKA; Subkey: "SOFTWARE\Clients\StartMenuInternet\Aeon\shell\open\command"; ValueType: string; ValueData: """{app}\{#AppExeName}"" ""%1"""; Flags: uninsdeletekey

[Files]
; Installer background artwork
Source: "..\resources\images\installer_bg.bmp"; Flags: dontcopy

; Release binaries and libraries from publish\Pro
Source: "..\publish\Pro\Aeon.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\publish\Pro\aeon_blink.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "..\publish\Pro\aeon_gecko_stealth.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "..\publish\Pro\aeon_ai.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "..\publish\Pro\aeon_router.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist

; Pages, newtab, and resources
Source: "..\publish\Pro\newtab\*"; DestDir: "{app}\newtab"; Flags: ignoreversion recursesubdirs
Source: "..\publish\Pro\pages\*"; DestDir: "{app}\pages"; Flags: ignoreversion recursesubdirs
Source: "..\publish\Pro\resources\*"; DestDir: "{app}\resources"; Flags: ignoreversion recursesubdirs skipifsourcedoesntexist
Source: "..\publish\Pro\Network\*"; DestDir: "{app}\Network"; Flags: ignoreversion recursesubdirs skipifsourcedoesntexist

; Filter lists
Source: "..\resources\filters\aeon_extra.txt"; DestDir: "{app}\resources\filters"; Flags: ignoreversion skipifsourcedoesntexist
Source: "..\resources\filters\*.txt"; DestDir: "{app}\resources\filters"; Flags: ignoreversion skipifsourcedoesntexist

; Icons
Source: "..\resources\icons\Aeon.ico"; DestDir: "{app}\resources\icons"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExeName}"; IconFilename: "{app}\resources\icons\Aeon.ico"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; IconFilename: "{app}\resources\icons\Aeon.ico"

[Run]
Filename: "{app}\{#AppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(AppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: filesandordirs; Name: "{app}\newtab"
Type: filesandordirs; Name: "{app}\pages"
Type: filesandordirs; Name: "{app}\resources"
Type: filesandordirs; Name: "{app}\Network"

[Code]
var
  BgImage: TBitmapImage;
  CloseBtn: TLabel;
  PercentLabel: TLabel;
  StatusLabel: TLabel;
  SubtitleLabel: TLabel;
  CustomProgressBar: TNewProgressBar;

procedure CloseBtnClick(Sender: TObject);
begin
  WizardForm.Close;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
begin
  if CurStep = ssPostInstall then
  begin
    Exec(ExpandConstant('{app}\{#AppExeName}'), '', '', SW_SHOWNORMAL, ewNoWait, ResultCode);
  end;
end;

procedure CurInstallProgressChanged(CurProgress, MaxProgress: Integer);
var
  pct: Integer;
begin
  if MaxProgress > 0 then
  begin
    pct := (CurProgress * 100) div MaxProgress;
    if pct > 100 then pct := 100;
    if Assigned(PercentLabel) then
      PercentLabel.Caption := IntToStr(pct) + '%';
    if Assigned(CustomProgressBar) then
      CustomProgressBar.Position := pct;
  end;
end;

procedure InitializeWizard();
var
  bgPath: String;
begin
  WizardForm.BorderStyle := bsNone;
  WizardForm.ClientWidth := 640;
  WizardForm.ClientHeight := 360;
  WizardForm.Position := poScreenCenter;
  WizardForm.Color := $120710;

  WizardForm.OuterNotebook.Visible := False;
  WizardForm.Bevel.Visible := False;
  WizardForm.BackButton.Visible := False;
  WizardForm.NextButton.Visible := False;
  WizardForm.CancelButton.Visible := False;

  ExtractTemporaryFile('installer_bg.bmp');
  bgPath := ExpandConstant('{tmp}\installer_bg.bmp');

  BgImage := TBitmapImage.Create(WizardForm);
  BgImage.Parent := WizardForm;
  BgImage.Left := 0;
  BgImage.Top := 0;
  BgImage.Width := 640;
  BgImage.Height := 360;
  BgImage.Stretch := True;
  BgImage.Bitmap.LoadFromFile(bgPath);

  CloseBtn := TLabel.Create(WizardForm);
  CloseBtn.Parent := WizardForm;
  CloseBtn.Left := 612;
  CloseBtn.Top := 10;
  CloseBtn.Caption := 'X';
  CloseBtn.Font.Name := 'Segoe UI';
  CloseBtn.Font.Size := 11;
  CloseBtn.Font.Color := $A384C0;
  CloseBtn.Cursor := crHand;
  CloseBtn.OnClick := @CloseBtnClick;

  CustomProgressBar := TNewProgressBar.Create(WizardForm);
  CustomProgressBar.Parent := WizardForm;
  CustomProgressBar.Left := 120;
  CustomProgressBar.Top := 240;
  CustomProgressBar.Width := 400;
  CustomProgressBar.Height := 4;
  CustomProgressBar.Min := 0;
  CustomProgressBar.Max := 100;
  CustomProgressBar.Position := 10;

  PercentLabel := TLabel.Create(WizardForm);
  PercentLabel.Parent := WizardForm;
  PercentLabel.Left := 305;
  PercentLabel.Top := 222;
  PercentLabel.Caption := '0%';
  PercentLabel.Font.Name := 'Segoe UI';
  PercentLabel.Font.Size := 9;
  PercentLabel.Font.Color := $9470B8;
  PercentLabel.Transparent := True;

  StatusLabel := TLabel.Create(WizardForm);
  StatusLabel.Parent := WizardForm;
  StatusLabel.Left := 0;
  StatusLabel.Top := 258;
  StatusLabel.Width := 640;
  StatusLabel.Alignment := taCenter;
  StatusLabel.Caption := 'Installing Aeon Browser...';
  StatusLabel.Font.Name := 'Segoe UI';
  StatusLabel.Font.Size := 14;
  StatusLabel.Font.Style := [fsBold];
  StatusLabel.Font.Color := $FFFFFF;
  StatusLabel.Transparent := True;

  SubtitleLabel := TLabel.Create(WizardForm);
  SubtitleLabel.Parent := WizardForm;
  SubtitleLabel.Left := 0;
  SubtitleLabel.Top := 290;
  SubtitleLabel.Width := 640;
  SubtitleLabel.Alignment := taCenter;
  SubtitleLabel.Caption := 'Hardening your digital perimeter.';
  SubtitleLabel.Font.Name := 'Segoe UI';
  SubtitleLabel.Font.Size := 10;
  SubtitleLabel.Font.Color := $A88ABA;
  SubtitleLabel.Transparent := True;
end;

function InitializeSetup(): Boolean;
begin
  Result := True;
end;
