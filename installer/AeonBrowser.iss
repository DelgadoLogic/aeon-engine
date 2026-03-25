; AeonBrowser — Inno Setup Installer Script
; DelgadoLogic | Build Team
;
; Builds: AeonBrowser-Setup-{version}.exe
; Requires: Inno Setup 6.2+ (https://jrsoftware.org/isinfo.php)
;
; Usage:
;   iscc AeonBrowser.iss
;   iscc /DVersion=1.2.3 AeonBrowser.iss
;
; Output: installer\Output\AeonBrowser-Setup-{version}.exe
; Signed: via signtool.exe in post-build step (see build_installer.ps1)

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
WizardStyle=modern
WizardResizable=no
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog

; Minimum OS: Windows XP SP3 (5.1.2600)
; Full-featured: Vista+ (6.0+), Win10+ (10.0)
MinVersion=5.1.2600

; Code-signing (applied externally via build_installer.ps1 → signtool.exe)
; SignTool=STDef sign /a /n "DelgadoLogic" /tr http://timestamp.digicert.com /td sha256 /fd sha256 $f

; Uninstaller settings
UninstallDisplayIcon={app}\{#AppExeName}
UninstallDisplayName={#AppName}
CreateUninstallRegKey=yes
UpdateUninstallLogAppName=yes

; Registry: write version so AutoUpdater can read it
[Registry]
Root: HKLM; Subkey: "SOFTWARE\DelgadoLogic\Aeon"; ValueType: string; ValueName: "Version"; ValueData: "{#Version}"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "SOFTWARE\DelgadoLogic\Aeon"; ValueType: string; ValueName: "InstallDir"; ValueData: "{app}"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "SOFTWARE\DelgadoLogic\Aeon"; ValueType: dword; ValueName: "TelemetryEnabled"; ValueData: "1"; Flags: uninsdeletevalue createvalueifdoesntexist
; Default browser registration (user-scope)
Root: HKCU; Subkey: "SOFTWARE\Clients\StartMenuInternet\Aeon"; ValueType: string; ValueData: "Aeon Browser"; Flags: uninsdeletekey
Root: HKCU; Subkey: "SOFTWARE\Clients\StartMenuInternet\Aeon\shell\open\command"; ValueType: string; ValueData: """{app}\{#AppExeName}"" ""%1"""; Flags: uninsdeletekey

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"
Name: "french";  MessagesFile: "compiler:Languages\French.isl"

[Tasks]
Name: "desktopicon";    Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "quicklaunchicon";Description: "{cm:CreateQuickLaunchIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked; OnlyBelowVersion: 6.1
Name: "defaultbrowser"; Description: "Make Aeon my default browser"; GroupDescription: "Browser settings:"; Flags: unchecked

[Files]
; Main executable
Source: "..\build\Release\Aeon.exe"; DestDir: "{app}"; Flags: ignoreversion

; Rust protocol router DLL
Source: "..\router\target\release\aeon_router.dll"; DestDir: "{app}"; Flags: ignoreversion

; Retro tier engine (HTML4 GDI renderer)
Source: "..\retro\aeon_html4.dll"; DestDir: "{app}"; DestName: "aeon_html4.dll"; Flags: ignoreversion; Check: not IsWin64
Source: "..\retro\aeon_html4_x64.dll"; DestDir: "{app}"; DestName: "aeon_html4.dll"; Flags: ignoreversion; Check: IsWin64

; Resources
Source: "..\resources\newtab\*"; DestDir: "{app}\resources\newtab"; Flags: ignoreversion recursesubdirs
Source: "..\resources\downloads\*"; DestDir: "{app}\resources\downloads"; Flags: ignoreversion recursesubdirs
Source: "..\resources\history\*"; DestDir: "{app}\resources\history"; Flags: ignoreversion recursesubdirs
Source: "..\resources\passwords\*"; DestDir: "{app}\resources\passwords"; Flags: ignoreversion recursesubdirs
Source: "..\resources\settings\*"; DestDir: "{app}\resources\settings"; Flags: ignoreversion recursesubdirs

; Filter lists (bundled EasyList + EasyPrivacy snapshots)
Source: "..\resources\filters\easylist.txt"; DestDir: "{app}\resources\filters"; Flags: ignoreversion
Source: "..\resources\filters\easyprivacy.txt"; DestDir: "{app}\resources\filters"; Flags: ignoreversion
Source: "..\resources\filters\aeon_extra.txt"; DestDir: "{app}\resources\filters"; Flags: ignoreversion

; Icons
Source: "..\resources\icons\Aeon.ico"; DestDir: "{app}\resources\icons"; Flags: ignoreversion

; Crash handler
Source: "..\build\Release\AeonCrash.exe"; DestDir: "{app}"; Flags: ignoreversion; Check: FileExists('..\build\Release\AeonCrash.exe')

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExeName}"; IconFilename: "{app}\resources\icons\Aeon.ico"
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; IconFilename: "{app}\resources\icons\Aeon.ico"; Tasks: desktopicon
Name: "{userappdata}\Microsoft\Internet Explorer\Quick Launch\{#AppName}"; Filename: "{app}\{#AppExeName}"; Tasks: quicklaunchicon

[Run]
; Launch after install
Filename: "{app}\{#AppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(AppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

; Register as default browser if task selected
Filename: "{app}\{#AppExeName}"; Parameters: "--set-default-browser"; Flags: nowait runhidden; Tasks: defaultbrowser

[UninstallRun]
Filename: "{app}\{#AppExeName}"; Parameters: "--unregister"; Flags: runhidden; RunOnceId: "Unregister"

[UninstallDelete]
; Clean up user data only if asked (GDPR-friendly)
; We do NOT delete %APPDATA%\Aeon automatically — too destructive.
; User can do it manually via Settings → Clear All Data.
Type: filesandordirs; Name: "{app}\resources"
Type: filesandordirs; Name: "{app}\blocklists"

[Code]
// ─── Mutex check: prevent running installer while browser is open ───────────
function InitializeSetup(): Boolean;
var
  hMutex: THandle;
begin
  hMutex := CreateMutex(False, '{#AppMutex}');
  if (hMutex <> 0) and (GetLastError() = 183 {ERROR_ALREADY_EXISTS}) then begin
    MsgBox(
      'Aeon Browser is currently running.' + #13#10 +
      'Please close it before running the installer.',
      mbError, MB_OK);
    Result := False;
    exit;
  end;
  Result := True;
end;

// ─── Detect OS tier for UI messaging ──────────────────────────────────────
function GetWindowsVersionLabel(): String;
var
  V: TWindowsVersion;
begin
  GetWindowsVersionEx(V);
  if V.Major >= 10 then Result := 'Windows 10/11 — Full Pro Tier'
  else if V.Major = 6 then begin
    if V.Minor >= 2 then Result := 'Windows 8/8.1 — Extended Tier'
    else if V.Minor = 1 then Result := 'Windows 7 — Extended Tier'
    else Result := 'Windows Vista — Extended Tier';
  end else if V.Major = 5 then begin
    if V.Minor >= 1 then Result := 'Windows XP — XPHi Tier'
    else Result := 'Windows 2000 — Retro Tier';
  end else Result := 'Legacy Windows — Retro Tier';
end;

procedure InitializeWizard();
begin
  WizardForm.WelcomeLabel2.Caption :=
    'Detected OS: ' + GetWindowsVersionLabel() + #13#10 + #13#10 +
    WizardForm.WelcomeLabel2.Caption;
end;
