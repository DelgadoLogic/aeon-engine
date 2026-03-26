#define AppName "Aeon Browser"
#define AppVersion "1.0.0"
#define AppPublisher "Aeon Systems"
#define AppURL "https://aeonbrowser.ai"
#define AppExeName "Aeon.exe"

[Setup]
AppId={{A1B2C3D4-E5F6-7890-ABCD-EF1234567890}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}
AppUpdatesURL={#AppURL}
DefaultDirName={autopf}\AeonBrowser
DefaultGroupName={#AppName}
AllowNoIcons=yes
OutputDir=dist
OutputBaseFilename=AeonBrowser-Setup-{#AppVersion}
SetupIconFile=icons\aeon_icon.ico
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
WizardSizePercent=120
DisableProgramGroupPage=yes
PrivilegesRequired=admin
UninstallDisplayIcon={app}\Aeon.exe
CloseApplications=yes
RestartApplications=no
ShowTasksTreeLines=yes
ArchitecturesInstallIn64BitMode=x64compatible

; Custom colors for wizard
WizardImageFile=installer\wizard_banner.bmp
WizardSmallImageFile=installer\wizard_small.bmp

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon";     Description: "Create a &desktop shortcut"; GroupDescription: "Additional icons:"; Flags: unchecked
Name: "startmenuicon";   Description: "Create a &Start Menu shortcut"; GroupDescription: "Additional icons:"; Flags: checked
Name: "startup";         Description: "Start AeonMind agent at login (recommended)"; GroupDescription: "AeonMind AI Agent:"; Flags: checked
Name: "installollama";   Description: "Install Ollama AI runtime (required for AeonMind)"; GroupDescription: "AI Components:"; Flags: checked
Name: "installdns";      Description: "Install private DNS (dnscrypt-proxy)"; GroupDescription: "Privacy:"; Flags: checked
Name: "installguard";    Description: "Install AeonGuard DPI bypass"; GroupDescription: "Privacy:"; Flags: unchecked

[Files]
; Core browser
Source: "dist\AeonCore\*"; DestDir: "{app}\core"; Flags: ignoreversion recursesubdirs createallsubdirs

; AeonMind agent
Source: "agent\*";         DestDir: "{app}\agent"; Flags: ignoreversion recursesubdirs
Source: "hive\*";          DestDir: "{app}\hive";  Flags: ignoreversion recursesubdirs

; Runtime bundled components
Source: "bundled\ollama\*";        DestDir: "{app}\runtime\ollama";    Flags: ignoreversion recursesubdirs; Tasks: installollama
Source: "bundled\dnscrypt-proxy\*";DestDir: "{app}\runtime\dnscrypt";  Flags: ignoreversion recursesubdirs; Tasks: installdns
Source: "bundled\xray-core\*";     DestDir: "{app}\runtime\xray";      Flags: ignoreversion recursesubdirs
Source: "bundled\GoodbyeDPI\*";    DestDir: "{app}\runtime\goodbyedpi";Flags: ignoreversion recursesubdirs; Tasks: installguard

; Extensions
Source: "extensions\ublock\*"; DestDir: "{app}\extensions\ublock"; Flags: ignoreversion recursesubdirs

; Icons and branding
Source: "icons\*"; DestDir: "{app}\icons"; Flags: ignoreversion

; Launcher
Source: "Aeon.exe";         DestDir: "{app}"; DestName: "Aeon.exe"; Flags: ignoreversion

; Python runtime (embedded)
Source: "bundled\python\*"; DestDir: "{app}\runtime\python"; Flags: ignoreversion recursesubdirs

[Dirs]
Name: "{userappdata}\.aeon"
Name: "{userappdata}\.aeon\task_logs"
Name: "{userappdata}\.aeon\adapters"
Name: "{userappdata}\.aeon\hive"
Name: "{userappdata}\.aeon\memory"

[Icons]
Name: "{group}\{#AppName}";          Filename: "{app}\Aeon.exe"
Name: "{group}\Uninstall {#AppName}";Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}";    Filename: "{app}\Aeon.exe"; Tasks: desktopicon
Name: "{autostartmenu}\{#AppName}";  Filename: "{app}\Aeon.exe"; Tasks: startmenuicon

[Run]
; Install Python requirements for AeonMind
Filename: "{app}\runtime\python\python.exe"; \
  Parameters: "-m pip install browser-use langchain-ollama litellm flower httpx --quiet"; \
  WorkingDir: "{app}"; StatusMsg: "Installing AI components..."; \
  Flags: runhidden waituntilterminated

; Install Ollama
Filename: "{app}\runtime\ollama\OllamaSetup.exe"; \
  Parameters: "/SILENT /NORESTART"; \
  StatusMsg: "Installing Ollama AI runtime..."; \
  Flags: waituntilterminated; Tasks: installollama

; Pull default model (Phi-4)
Filename: "cmd.exe"; \
  Parameters: "/c start /min ollama pull phi4"; \
  StatusMsg: "Downloading Phi-4 AI model (this may take a while)..."; \
  Flags: runhidden; Tasks: installollama

; Install dnscrypt-proxy as service
Filename: "{app}\runtime\dnscrypt\dnscrypt-proxy.exe"; \
  Parameters: "-service install"; \
  StatusMsg: "Installing private DNS service..."; \
  Flags: runhidden waituntilterminated; Tasks: installdns

Filename: "{app}\runtime\dnscrypt\dnscrypt-proxy.exe"; \
  Parameters: "-service start"; \
  Flags: runhidden; Tasks: installdns

; Register AeonMind as startup task
Filename: "schtasks.exe"; \
  Parameters: "/Create /TN ""AeonMind"" /TR ""{app}\runtime\python\python.exe {app}\agent\aeon_mind.py --server"" /SC ONLOGON /RL HIGHEST /F"; \
  Flags: runhidden; Tasks: startup

; Launch Aeon after install
Filename: "{app}\Aeon.exe"; \
  Description: "Launch {#AppName}"; \
  Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{app}\runtime\dnscrypt\dnscrypt-proxy.exe"; Parameters: "-service stop"; Flags: runhidden
Filename: "{app}\runtime\dnscrypt\dnscrypt-proxy.exe"; Parameters: "-service uninstall"; Flags: runhidden
Filename: "schtasks.exe"; Parameters: "/Delete /TN ""AeonMind"" /F"; Flags: runhidden

[Registry]
; Register as browser
Root: HKLM; Subkey: "Software\Clients\StartMenuInternet\AeonBrowser"; \
  ValueType: string; ValueName: ""; ValueData: "Aeon Browser"; Flags: uninsdeletekey

Root: HKLM; Subkey: "Software\Clients\StartMenuInternet\AeonBrowser\Capabilities"; \
  ValueType: string; ValueName: "ApplicationDescription"; \
  ValueData: "Aeon Browser — AI-powered autonomous browser"

Root: HKLM; Subkey: "Software\Clients\StartMenuInternet\AeonBrowser\Capabilities\URLAssociations"; \
  ValueType: string; ValueName: "http";  ValueData: "AeonBrowserHTM"
Root: HKLM; Subkey: "Software\Clients\StartMenuInternet\AeonBrowser\Capabilities\URLAssociations"; \
  ValueType: string; ValueName: "https"; ValueData: "AeonBrowserHTM"

Root: HKLM; Subkey: "Software\RegisteredApplications"; \
  ValueType: string; ValueName: "AeonBrowser"; \
  ValueData: "Software\Clients\StartMenuInternet\AeonBrowser\Capabilities"

; Store install dir
Root: HKCU; Subkey: "Software\AeonBrowser"; \
  ValueType: string; ValueName: "InstallDir"; ValueData: "{app}"; Flags: uninsdeletekey

[Code]
var
  WelcomePage: TWizardPage;

function InitializeSetup(): Boolean;
begin
  Result := True;
  // Check Windows 10+
  if not IsWin64 then begin
    MsgBox('Aeon Browser requires a 64-bit version of Windows 10 or later.', mbError, MB_OK);
    Result := False;
  end;
end;

procedure InitializeWizard();
begin
  WizardForm.WelcomeLabel2.Caption :=
    'Aeon Browser is a next-generation autonomous browser with built-in AI.'#13#10 +
    ''#13#10 +
    'This will install:'#13#10 +
    '  ▸ AeonCore — privacy-hardened Chromium engine'#13#10 +
    '  ▸ AeonMind — autonomous AI agent (Phi-4 LLM)'#13#10 +
    '  ▸ AeonHive — P2P compute network client'#13#10 +
    '  ▸ AeonGuard — private DNS + DPI bypass'#13#10 +
    ''#13#10 +
    'All AI runs 100% locally. No data sent to servers.';
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if CurPageID = wpSelectTasks then begin
    // Warn if AI components deselected
    if not IsTaskSelected('installollama') then begin
      if MsgBox(
        'AeonMind requires Ollama to run AI locally.'#13#10 +
        'Without it, AI features will be unavailable.'#13#10#13#10 +
        'Install Ollama anyway?',
        mbConfirmation, MB_YESNO) = IDYES then begin
        // Re-enable task (user changed mind)
      end;
    end;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then begin
    // Write initial config
    SaveStringToFile(
      ExpandConstant('{userappdata}\.aeon\config.json'),
      '{"version":"1.0.0","model":"phi4","hive_enabled":true,' +
      '"dns_private":true,"telemetry":false,"first_run":true}',
      False
    );
  end;
end;
