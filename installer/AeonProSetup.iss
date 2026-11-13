; AeonProSetup.iss — Inno Setup installer script for Aeon Browser Pro
; DelgadoLogic | Delgado Creative Enterprises LLC
; 
#define AppName      "Aeon Browser"
#define AppVersion   "0.22.0"
#define AppPublisher "DelgadoLogic"
#define AppURL       "https://delgadologic.tech"
#define AppExeName   "Aeon.exe"

[Setup]
AppId={{D31CA809-B28E-45FE-9041-8E2CF7C07399}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}
AppUpdatesURL={#AppURL}
DefaultDirName={autopf}\DelgadoLogic\AeonBrowser
DefaultGroupName=DelgadoLogic\{#AppName}
AllowNoIcons=yes
OutputDir=..\dist
OutputBaseFilename=AeonBrowser_Setup_v{#AppVersion}
SetupIconFile=..\resources\icons\Aeon.ico
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
WizardSizePercent=120
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
UninstallDisplayIcon={app}\{#AppExeName}
UninstallDisplayName={#AppName}
VersionInfoVersion={#AppVersion}
VersionInfoCompany={#AppPublisher}
VersionInfoDescription={#AppName} Setup
VersionInfoCopyright=Copyright 2026 {#AppPublisher}
CloseApplications=yes
RestartApplications=no
ArchitecturesInstallIn64BitMode=x64compatible

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "..\publish\Pro\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\resources\icons\Aeon.ico"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExeName}"; IconFilename: "{app}\Aeon.ico"
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; IconFilename: "{app}\Aeon.ico"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(AppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

[Registry]
; Register HTTP / HTTPS / Aeon protocol handler
Root: HKCU; Subkey: "Software\Classes\AeonHTML"; ValueType: string; ValueData: "Aeon HTML Document"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\AeonHTML\DefaultIcon"; ValueType: string; ValueData: "{app}\Aeon.ico,0"
Root: HKCU; Subkey: "Software\Classes\AeonHTML\shell\open\command"; ValueType: string; ValueData: """{app}\{#AppExeName}"" ""%1"""
Root: HKCU; Subkey: "Software\Classes\aeon"; ValueType: string; ValueData: "URL:Aeon Protocol"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\aeon"; ValueType: string; ValueName: "URL Protocol"; ValueData: ""
Root: HKCU; Subkey: "Software\Classes\aeon\DefaultIcon"; ValueType: string; ValueData: "{app}\Aeon.ico,0"
Root: HKCU; Subkey: "Software\Classes\aeon\shell\open\command"; ValueType: string; ValueData: """{app}\{#AppExeName}"" ""%1"""
