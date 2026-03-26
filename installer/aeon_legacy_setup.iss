; aeon_legacy_setup.iss — Aeon Legacy Edition Installer
; Targets: Windows XP SP3, Vista SP2, 7, 8, 8.1
;
; The Legacy Edition uses a different Chromium base:
;   - Win7/8: Chromium 109 branch + Aeon security backports + bundled BoringSSL
;   - WinXP/Vista: Ungoogled-Chromium 109 with ONE-Core-API compatibility shims
;
; KEY DIFFERENCE from Standard: bundles its own TLS stack and CA certificate store
; so users don't depend on Windows' outdated SChannel or expired root certs.
; THIS is what lets XP users visit YouTube, HTTPS sites, and modern services.
;
; Usage:
;   iscc aeon_legacy_setup.iss
;   signtool sign /tr http://timestamp.sectigo.com /td sha256 /fd sha256 
;              /sha1 <cert-thumbprint> Output\aeon_legacy_setup.exe

#define AppName      "Aeon Browser Legacy Edition"
#define AppVersion   GetFileVersion("..\build\legacy\aeon\aeon.exe")
#define AppPublisher "Aeon Browser Project"
#define AppURL       "https://aeonbrowser.com/legacy"
#define AppExeName   "aeon.exe"

[Setup]
AppId={{B40F3C6D-8EAF-5G2B-C9D3-F6E7G8B9C1D1}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}/support
DefaultDirName={pf}\Aeon Legacy
DefaultGroupName=Aeon Legacy
AllowNoIcons=yes
LicenseFile=..\LICENSE
OutputDir=Output
OutputBaseFilename=aeon_legacy_setup
SetupIconFile=..\resources\aeon_legacy.ico
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
UninstallDisplayIcon={app}\{#AppExeName}
UninstallDisplayName={#AppName}
VersionInfoVersion={#AppVersion}
MinVersion=5.1.2600
; XP SP3 minimum. Will also run perfectly on Vista, 7, 8, 8.1.
; Chromium 49 era base for XP/Vista (separately compiled)
; Chromium 109 base for Win7/8 (auto-detected at install time)

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Shortcuts:"
Name: "defaultbrowser"; Description: "Set Aeon as my &default browser"; GroupDescription: "Browser settings:"

[Files]
; ── Shared files for all legacy Windows versions ──

; Our own CA certificate bundle (replaces expired Windows cert store)
; This is what makes HTTPS actually work on XP — no expired Let's Encrypt chain!
Source: "..\build\legacy\cacert\cacert.pem"; DestDir: "{app}\security"; Flags: ignoreversion

; BoringSSL DLL (our own TLS 1.3 stack — bypasses Windows SChannel entirely)
; XP's SChannel only supports TLS 1.0 and weak ciphers. This fixes YouTube, etc.
Source: "..\build\legacy\ssl\*.dll"; DestDir: "{app}"; Flags: ignoreversion

; ── Win7/8 specific browser binaries (Chromium 109 base + Aeon patches) ──
Source: "..\build\legacy\win7\aeon\{#AppExeName}"; DestDir: "{app}"; \
        Flags: ignoreversion; Check: IsWin7OrEight
Source: "..\build\legacy\win7\aeon\*.dll"; DestDir: "{app}"; \
        Flags: ignoreversion; Check: IsWin7OrEight
Source: "..\build\legacy\win7\aeon\*.pak"; DestDir: "{app}"; \
        Flags: ignoreversion; Check: IsWin7OrEight
Source: "..\build\legacy\win7\aeon\locales\*"; DestDir: "{app}\locales"; \
        Flags: ignoreversion recursesubdirs; Check: IsWin7OrEight

; ── XP/Vista specific browser binaries (Chromium 49 base + V8 compat patches) ──
; Note: Chromium 49 is the last official XP build. We apply Aeon security patches on top.
; For modern site COMPATIBILITY on XP, we use a forward-ported V8 engine (see below).
Source: "..\build\legacy\xp\aeon\{#AppExeName}"; DestDir: "{app}"; \
        Flags: ignoreversion; Check: IsXPOrVista
Source: "..\build\legacy\xp\aeon\*.dll"; DestDir: "{app}"; \
        Flags: ignoreversion; Check: IsXPOrVista
Source: "..\build\legacy\xp\aeon\*.pak"; DestDir: "{app}"; \
        Flags: ignoreversion; Check: IsXPOrVista
Source: "..\build\legacy\xp\aeon\locales\*"; DestDir: "{app}\locales"; \
        Flags: ignoreversion recursesubdirs; Check: IsXPOrVista

; ── ONE-Core-API shims (for XP/Vista — provides Win7 API surface to Chromium 49) ──
; This is the key compatibility layer: extends XP kernel to expose
; CreateFile2, GetOverlappedResultEx, WaitOnAddress, and ~900 other Vista/7 APIs
; Reference: https://github.com/Skulltrail192/One-Core-API
Source: "..\vendor\one-core-api\*.dll"; DestDir: "{sys}"; StrongAssemblyName: ""; \
        Flags: ignoreversion replacesameversion; Check: IsXPOrVista; \
        OnlyBelowVersion: 6.0  ; Only install on XP (not Vista+)

; ── Shared: AeonHive, updater, search, silence policy ──
Source: "..\cloud\aeon_hive.py"; DestDir: "{app}\hive"; Flags: ignoreversion
Source: "..\search\aeon_search.py"; DestDir: "{app}\search"; Flags: ignoreversion
Source: "..\updater\aeon_universal_updater.py"; DestDir: "{app}\updater"; Flags: ignoreversion
Source: "..\aeon_silence_policy.py"; DestDir: "{app}"; Flags: ignoreversion

; Bundled Python 3.8 (last version supporting Win7; XP uses 3.4)
; Chosen because AeonHive / updater need modern asyncio
Source: "..\vendor\python-legacy\*"; DestDir: "{app}\python"; \
        Flags: ignoreversion recursesubdirs

; Icon
Source: "..\resources\aeon_legacy.ico"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExeName}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Clients\StartMenuInternet\AeonBrowserLegacy\Capabilities"; \
      ValueType: string; ValueName: "ApplicationName"; ValueData: "Aeon Browser Legacy"
Root: HKCU; Subkey: "Software\Clients\StartMenuInternet\AeonBrowserLegacy\Capabilities\URLAssociations"; \
      ValueType: string; ValueName: "http"; ValueData: "AeonBrowserLegacyURL"
Root: HKCU; Subkey: "Software\Clients\StartMenuInternet\AeonBrowserLegacy\Capabilities\URLAssociations"; \
      ValueType: string; ValueName: "https"; ValueData: "AeonBrowserLegacyURL"
Root: HKCU; Subkey: "Software\RegisteredApplications"; \
      ValueType: string; ValueName: "AeonBrowserLegacy"; \
      ValueData: "Software\Clients\StartMenuInternet\AeonBrowserLegacy\Capabilities"

[Run]
; Start AeonHive after install
Filename: "{app}\python\python.exe"; \
      Parameters: "{app}\hive\aeon_hive.py"; WorkingDir: "{app}\hive"; \
      Description: "Starting Aeon network daemon"; \
      Flags: nowait postinstall skipifsilent

Filename: "{app}\{#AppExeName}"; \
      Description: "Launch Aeon"; Flags: nowait postinstall skipifsilent

[Code]
// ── Runtime detection helpers ──────────────────────────────────────────────

function GetWinVer: TWindowsVersion;
begin
  GetWindowsVersionEx(Result);
end;

function IsXPOrVista: Boolean;
var v: TWindowsVersion;
begin
  GetWindowsVersionEx(v);
  // XP = 5.1, Server 2003 = 5.2, Vista = 6.0
  Result := (v.Major = 5) or (v.Major = 6) and (v.Minor = 0);
end;

function IsWin7OrEight: Boolean;
var v: TWindowsVersion;
begin
  GetWindowsVersionEx(v);
  // Win7 = 6.1, Win8 = 6.2, Win8.1 = 6.3
  Result := (v.Major = 6) and (v.Minor >= 1);
end;

function IsWin10Plus: Boolean;
var v: TWindowsVersion;
begin
  GetWindowsVersionEx(v);
  Result := (v.Major >= 10);
end;

// ── Redirect Win10/11 users to standard installer ──────────────────────────

function InitializeSetup: Boolean;
begin
  if IsWin10Plus() then
  begin
    MsgBox(
      'You are running Windows 10 or 11.' + #13#10 +
      'Please download Aeon Standard Edition instead:' + #13#10 +
      'https://aeonbrowser.com/download' + #13#10 + #13#10 +
      'The Legacy Edition is designed for XP, Vista, 7, and 8.',
      mbInformation, MB_OK);
    Result := False;
  end else begin
    Result := True;
  end;
end;

// ── Post-install ───────────────────────────────────────────────────────────

procedure CurStepChanged(CurStep: TSetupStep);
var ResultCode: Integer;
begin
  if CurStep = ssPostInstall then
  begin
    // Install Python deps for hive/updater
    Exec(ExpandConstant('{app}\python\python.exe'),
         '-m pip install -q -r ' + ExpandConstant('{app}\hive\requirements_hive.txt'),
         ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, ResultCode);

    // Set as default browser if selected
    if IsTaskSelected('defaultbrowser') then
      Exec(ExpandConstant('{app}\{#AppExeName}'),
           '--make-default-browser', '', SW_HIDE,
           ewWaitUntilTerminated, ResultCode);

    // On XP: import our CA bundle into the OS cert store
    // so that IE and other apps also trust modern TLS certs
    if IsXPOrVista() then
      Exec('certutil.exe',
           '-f -addstore Root ' + ExpandConstant('{app}\security\cacert.pem'),
           '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  end;
end;
