; aeon_setup.iss — Inno Setup installer script for Aeon Browser
; Produces a one-click aeon_setup.exe for Windows distribution
; 
; Requirements:
;   - Inno Setup 6.x (free, innosetup.com)
;   - Build output in ..\build\win\aeon\
;   - Code signing: run signtool AFTER Inno Setup compiles
;
; Usage:
;   iscc aeon_setup.iss
;   signtool sign /tr http://timestamp.sectigo.com /td sha256 /fd sha256 
;              /a Output\aeon_setup.exe

#define AppName      "Aeon Browser"
#define AppVersion   GetFileVersion("..\build\win\aeon\aeon.exe")
#define AppPublisher "Aeon Browser Project"
#define AppURL       "https://aeonbrowser.com"
#define AppExeName   "aeon.exe"
#define AppDataName  "Aeon"

[Setup]
AppId={{A30F2B5C-7D9E-4F1A-B8C2-E5D6F7A8B9C0}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}/support
AppUpdatesURL={#AppURL}/releases
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
AllowNoIcons=yes
LicenseFile=..\LICENSE
OutputDir=Output
OutputBaseFilename=aeon_setup
SetupIconFile=..\resources\aeon.ico
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
WizardSizePercent=150
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
UninstallDisplayIcon={app}\{#AppExeName}
UninstallDisplayName={#AppName}
VersionInfoVersion={#AppVersion}
VersionInfoCompany={#AppPublisher}
VersionInfoDescription={#AppName} Installer
VersionInfoCopyright=Copyright 2025 {#AppPublisher}
MinVersion=5.1.2600
; Windows XP SP3+ (5.1 = XP, 5.2 = Server 2003, 6.0 = Vista, 6.1 = 7, 6.2 = 8, 6.3 = 8.1, 10.0 = Win10+)
; NOTE: This installer targets the Standard (Win10/11) edition.
; The Legacy installer (aeon_legacy_setup.exe) covers XP/Vista/7/8 separately.

; Digital signature will be applied post-build via signtool
; This is what clears Windows SmartScreen after ~5K downloads

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; \
      GroupDescription: "{cm:AdditionalIcons}"
Name: "defaultbrowser"; Description: "Set Aeon as my default browser"; \
      GroupDescription: "Browser settings"

[Files]
; Main browser binary
Source: "..\build\win\aeon\{#AppExeName}"; DestDir: "{app}"; Flags: ignoreversion

; Browser resources (DLLs, pak files, locales, etc.)
Source: "..\build\win\aeon\*.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\win\aeon\*.pak"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\win\aeon\*.bin"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\win\aeon\locales\*"; DestDir: "{app}\locales"; \
      Flags: ignoreversion recursesubdirs
Source: "..\build\win\aeon\resources\*"; DestDir: "{app}\resources"; \
      Flags: ignoreversion recursesubdirs

; AeonMind (local AI service)
Source: "..\build\win\aeonmind\*"; DestDir: "{app}\aeonmind"; \
      Flags: ignoreversion recursesubdirs

; AeonHive (P2P mesh daemon)
Source: "..\cloud\aeon_hive.py"; DestDir: "{app}\hive"; Flags: ignoreversion
Source: "..\requirements_hive.txt"; DestDir: "{app}\hive"; Flags: ignoreversion

; Universal updater (keeps everything current)
Source: "..\updater\aeon_universal_updater.py"; DestDir: "{app}\updater"; \
      Flags: ignoreversion

; Silence policy (shared by all background daemons)
Source: "..\aeon_silence_policy.py"; DestDir: "{app}"; Flags: ignoreversion

; Bundled Python runtime (for hive/updater — users don't install Python)
Source: "..\vendor\python\*"; DestDir: "{app}\python"; \
      Flags: ignoreversion recursesubdirs

; Icon
Source: "..\resources\aeon.ico"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExeName}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; \
      Tasks: desktopicon

[Registry]
; Register as a browser capability (for Windows "Default Apps" settings)
Root: HKCU; Subkey: "Software\Clients\StartMenuInternet\AeonBrowser\Capabilities"; \
      ValueType: string; ValueName: "ApplicationName"; ValueData: "Aeon Browser"
Root: HKCU; Subkey: "Software\Clients\StartMenuInternet\AeonBrowser\Capabilities"; \
      ValueType: string; ValueName: "ApplicationDescription"; \
      ValueData: "The browser that cannot be controlled by anyone except you."
Root: HKCU; Subkey: "Software\Clients\StartMenuInternet\AeonBrowser\Capabilities\FileAssociations"; \
      ValueType: string; ValueName: ".htm"; ValueData: "AeonBrowserHTML"
Root: HKCU; Subkey: "Software\Clients\StartMenuInternet\AeonBrowser\Capabilities\FileAssociations"; \
      ValueType: string; ValueName: ".html"; ValueData: "AeonBrowserHTML"
Root: HKCU; Subkey: "Software\Clients\StartMenuInternet\AeonBrowser\Capabilities\URLAssociations"; \
      ValueType: string; ValueName: "http"; ValueData: "AeonBrowserURL"
Root: HKCU; Subkey: "Software\Clients\StartMenuInternet\AeonBrowser\Capabilities\URLAssociations"; \
      ValueType: string; ValueName: "https"; ValueData: "AeonBrowserURL"
Root: HKCU; Subkey: "Software\RegisteredApplications"; \
      ValueType: string; ValueName: "AeonBrowser"; \
      ValueData: "Software\Clients\StartMenuInternet\AeonBrowser\Capabilities"

[Run]
; Start AeonHive in background after install
Filename: "{app}\python\python.exe"; \
      Parameters: "{app}\hive\aeon_hive.py"; \
      WorkingDir: "{app}\hive"; \
      Description: "Starting Aeon network daemon"; \
      Flags: nowait postinstall skipifsilent

; Open browser after install
Filename: "{app}\{#AppExeName}"; \
      Description: "{cm:LaunchProgram,{#StringChange(AppName, '&', '&&')}}"; \
      Flags: nowait postinstall skipifsilent

[UninstallRun]
; Stop AeonHive before uninstall
Filename: "taskkill"; Parameters: "/f /im python.exe"; Flags: nowait

[Code]
procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
begin
  if CurStep = ssPostInstall then
  begin
    // Set as default browser if user selected that task
    if IsTaskSelected('defaultbrowser') then
    begin
      Exec(ExpandConstant('{app}\{#AppExeName}'),
           '--make-default-browser', '', SW_HIDE, ewWaitUntilTerminated,
           ResultCode);
    end;

    // Install Python dependencies for hive/updater silently
    Exec(ExpandConstant('{app}\python\python.exe'),
         '-m pip install -q -r ' +
         ExpandConstant('{app}\hive\requirements_hive.txt'),
         ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, ResultCode);
  end;
end;
