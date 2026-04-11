# ===============================================================================
# AeonBrowser - Chromium Cloud Build Startup Script
# DelgadoLogic | Runs autonomously on GCE Windows Server 2022
#
# This script is injected as a GCE startup-script. It:
#   1. Installs Visual Studio Build Tools 2022
#   2. Installs depot_tools
#   3. Fetches Chromium source (shallow, no history)
#   4. Applies Aeon fingerprint/telemetry patches
#   5. Builds chrome.dll -> aeon_engine.dll
#   6. Uploads to gs://aeon-sovereign-artifacts/
#   7. Signals completion
#
# Expected VM: n2-highcpu-96 / c2d-highcpu-96+, Windows Server 2022, 300GB pd-ssd
# ===============================================================================

$ErrorActionPreference = "Continue"
$ProgressPreference = "SilentlyContinue"  # Speeds up Invoke-WebRequest significantly

# -- Configuration -------------------------------------------------------------
$GCS_BUCKET      = "gs://aeon-sovereign-artifacts"
$GCS_STATUS_FILE = "$GCS_BUCKET/build-status.txt"
$GCS_LOG_FILE    = "$GCS_BUCKET/build-log.txt"
$BUILD_DRIVE     = "C:"   # Use C: if no extra disk; change to D: if pd-ssd attached
$CHROMIUM_DIR    = "$BUILD_DRIVE\chromium"
$DEPOT_TOOLS_DIR = "$BUILD_DRIVE\depot_tools"
$BUILD_OUTPUT    = "$CHROMIUM_DIR\src\out\AeonRelease"
$LOG_FILE        = "$BUILD_DRIVE\aeon_build.log"
$PATCH_BUCKET    = "$GCS_BUCKET/patches"

# -- Logging -------------------------------------------------------------------
function Log {
    param([string]$Message)
    $ts = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
    $line = "[$ts] $Message"
    Write-Host $line
    Add-Content -Path $LOG_FILE -Value $line -ErrorAction SilentlyContinue
}

function Update-Status {
    param([string]$Status)
    Log "STATUS: $Status"
    $Status | gsutil cp - $GCS_STATUS_FILE 2>$null
}

function Upload-Log {
    if (Test-Path $LOG_FILE) {
        gsutil cp $LOG_FILE $GCS_LOG_FILE 2>$null
    }
}

# -- Start ---------------------------------------------------------------------
Log "==========================================================="
Log "  Aeon Browser - Chromium Cloud Build"
Log "  Machine: $env:COMPUTERNAME"
Log "  CPUs: $env:NUMBER_OF_PROCESSORS"
Log "  Started: $(Get-Date)"
Log "==========================================================="

Update-Status "PHASE_1_INSTALLING_TOOLS"

# -- Phase 1: Install Visual Studio Build Tools 2022 --------------------------
Log "Phase 1: Installing Visual Studio Build Tools 2022..."
$vsInstallerUrl = "https://aka.ms/vs/17/release/vs_buildtools.exe"
$vsInstallerPath = "$BUILD_DRIVE\vs_buildtools.exe"

$vsCheckPath = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC'
if (-not (Test-Path $vsCheckPath)) {
    Log "  Downloading VS Build Tools..."
    Invoke-WebRequest -Uri $vsInstallerUrl -OutFile $vsInstallerPath -UseBasicParsing
    
    Log "  Installing VS Build Tools - this takes 5-10 minutes..."
    # VCTools workload for Build Tools edition + individual components
    # NativeDesktop is for full VS only; VCTools installs vcvarsall.bat
    $vsArgs = @(
        "--quiet", "--wait", "--norestart",
        "--add", "Microsoft.VisualStudio.Workload.VCTools",
        "--add", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
        "--add", "Microsoft.VisualStudio.Component.VC.ATL",
        "--add", "Microsoft.VisualStudio.Component.VC.ATLMFC",
        "--add", "Microsoft.VisualStudio.Component.Windows11SDK.26100",
        "--includeRecommended"
    )
    $proc = Start-Process -FilePath $vsInstallerPath -ArgumentList $vsArgs -Wait -PassThru
    Log "  VS Build Tools install exit code: $($proc.ExitCode)"
    
    # Verify installation
    if (Test-Path $vsCheckPath) {
        Log "  VS Build Tools 2022 installed successfully."
    } else {
        Log "  ERROR: VS Build Tools installation may have failed!"
        Upload-Log
        Update-Status "FAILED_VS_INSTALL"
        exit 1
    }
} else {
    Log "  VS Build Tools already installed, skipping."
}

Upload-Log

# Install Windows Debugging Tools (provides dbghelp.dll required by Chromium)
Log "Phase 1a: Installing Windows Debugging Tools..."
$dbgHelpPath = "C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\dbghelp.dll"
if (-not (Test-Path $dbgHelpPath)) {
    $sdkUrl = "https://go.microsoft.com/fwlink/?linkid=2272610"
    $sdkInstaller = "$BUILD_DRIVE\winsdksetup.exe"
    Log "  Downloading Windows SDK installer..."
    Invoke-WebRequest -Uri $sdkUrl -OutFile $sdkInstaller -UseBasicParsing
    Log "  Installing Debugging Tools for Windows..."
    $sdkArgs = @("/features", "OptionId.WindowsDesktopDebuggers", "/quiet", "/norestart")
    $sdkProc = Start-Process -FilePath $sdkInstaller -ArgumentList $sdkArgs -Wait -PassThru
    Log "  Windows SDK Debugging Tools install exit code: $($sdkProc.ExitCode)"
    if (Test-Path $dbgHelpPath) {
        Log "  dbghelp.dll found - Debugging Tools installed successfully."
    } else {
        Log "  WARNING: dbghelp.dll not found after install, build may fail."
    }
} else {
    Log "  Debugging Tools already installed."
}
Update-Status "PHASE_1B_INSTALLING_GIT"

# -- Phase 1b: Install Git for Windows ----------------------------------------
Log "Phase 1b: Installing Git for Windows..."
if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    $gitUrl = "https://github.com/git-for-windows/git/releases/download/v2.47.1.windows.2/Git-2.47.1.2-64-bit.exe"
    $gitInstaller = "$BUILD_DRIVE\git_installer.exe"
    Log "  Downloading Git for Windows..."
    Invoke-WebRequest -Uri $gitUrl -OutFile $gitInstaller -UseBasicParsing
    Log "  Installing Git silently..."
    $gitArgs = '/VERYSILENT /NORESTART /NOCANCEL /SP- /CLOSEAPPLICATIONS /RESTARTAPPLICATIONS'
    $proc = Start-Process -FilePath $gitInstaller -ArgumentList $gitArgs -Wait -PassThru
    Log "  Git install exit code: $($proc.ExitCode)"
    # Refresh PATH to pick up git
    $gitPath = 'C:\Program Files\Git\cmd;C:\Program Files\Git\bin'
    $env:PATH = $gitPath + ';' + $env:PATH
    if (Get-Command git -ErrorAction SilentlyContinue) {
        Log "  Git installed: $(git --version)"
    } else {
        Log "  ERROR: Git installation failed!"
        Upload-Log
        Update-Status "FAILED_GIT_INSTALL"
        exit 1
    }
} else {
    Log "  Git already installed: $(git --version)"
}

# Configure git global settings for Chromium build
$gitConfigDir = "$env:USERPROFILE"
if (-not (Test-Path "$gitConfigDir\.gitconfig")) {
    New-Item -ItemType File -Path "$gitConfigDir\.gitconfig" -Force | Out-Null
}
git config --global core.autocrlf false
git config --global core.filemode false
git config --global core.fscache true
git config --global core.preloadindex true
git config --global depot-tools.allowGlobalGitConfig true
Log "  Git global config set for Chromium development."

# -- Phase 1c: Install Python 3 ----------------------------------------------
Log "Phase 1c: Checking Python..."
if (-not (Get-Command python -ErrorAction SilentlyContinue)) {
    $pyUrl = "https://www.python.org/ftp/python/3.11.9/python-3.11.9-amd64.exe"
    $pyInstaller = "$BUILD_DRIVE\python_installer.exe"
    Log "  Downloading Python 3.11..."
    Invoke-WebRequest -Uri $pyUrl -OutFile $pyInstaller -UseBasicParsing
    Log "  Installing Python silently..."
    $proc = Start-Process -FilePath $pyInstaller -ArgumentList "/quiet InstallAllUsers=1 PrependPath=1 Include_test=0" -Wait -PassThru
    Log "  Python install exit code: $($proc.ExitCode)"
    $pyPath = 'C:\Program Files\Python311;C:\Program Files\Python311\Scripts'
    $env:PATH = $pyPath + ';' + $env:PATH
    if (Get-Command python -ErrorAction SilentlyContinue) {
        Log "  Python installed: $(python --version)"
    } else {
        Log "  WARNING: Python install may need PATH refresh, continuing..."
    }
} else {
    Log "  Python already installed: $(python --version)"
}

Upload-Log
Update-Status "PHASE_2_DEPOT_TOOLS"

# -- Phase 2: Install depot_tools ---------------------------------------------
Log "Phase 2: Installing depot_tools..."

if (-not (Test-Path "$DEPOT_TOOLS_DIR\gclient.bat")) {
    Log "  Cloning depot_tools..."
    git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git $DEPOT_TOOLS_DIR
    if ($LASTEXITCODE -ne 0) {
        Log "  ERROR: Failed to clone depot_tools"
        Upload-Log
        Update-Status "FAILED_DEPOT_TOOLS"
        exit 1
    }
} else {
    Log "  depot_tools already present."
}

# Add depot_tools to PATH for this session
$env:PATH = $DEPOT_TOOLS_DIR + ';' + $env:PATH
$env:DEPOT_TOOLS_WIN_TOOLCHAIN = "0"  # Use locally installed VS, not Google's
$env:GYP_MSVS_VERSION = "2022"
$env:vs2022_install = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"

Log "  depot_tools ready. PATH updated."
Upload-Log
Update-Status "PHASE_3_CHROMIUM_FETCH"

# -- Phase 3: Fetch Chromium source -------------------------------------------
Log "Phase 3: Fetching Chromium source - shallow clone, ~20 min..."

New-Item -ItemType Directory -Force -Path $CHROMIUM_DIR | Out-Null
Set-Location $CHROMIUM_DIR

if (-not (Test-Path "$CHROMIUM_DIR\src\chrome\browser")) {
    Log "  Creating .gclient config..."
    $gclientContent = @"
solutions = [
  {
    "name": "src",
    "url": "https://chromium.googlesource.com/chromium/src.git",
    "managed": False,
    "custom_deps": {},
    "custom_vars": {
      "checkout_nacl": False,
      "checkout_telemetry_dependencies": False,
      "checkout_pgo_profiles": True,
    },
  },
]
target_os = ["win"]
"@
    # Write WITHOUT BOM - gclient Python parser chokes on UTF-8 BOM
    $utf8NoBom = New-Object System.Text.UTF8Encoding $false
    [System.IO.File]::WriteAllText("$CHROMIUM_DIR\.gclient", $gclientContent, $utf8NoBom)

    Log "  Running gclient sync - shallow, no history..."
    $syncStart = Get-Date
    gclient sync --nohooks --no-history --shallow -j $env:NUMBER_OF_PROCESSORS 2>&1 | Out-File "$BUILD_DRIVE\gclient_sync.log"
    $syncTime = (Get-Date) - $syncStart
    Log "  gclient sync completed in $([math]::Round($syncTime.TotalMinutes, 1)) minutes"
    
    if (-not (Test-Path "$CHROMIUM_DIR\src\chrome\browser")) {
        Log "  ERROR: Chromium source not found after sync!"
        Log "  --- gclient sync log tail ---"
        if (Test-Path "$BUILD_DRIVE\gclient_sync.log") {
            Get-Content "$BUILD_DRIVE\gclient_sync.log" -Tail 30 | ForEach-Object { Log "  SYNC: $_" }
            gsutil cp "$BUILD_DRIVE\gclient_sync.log" "$GCS_BUCKET/gclient_sync.log" 2>$null
        } else {
            Log "  No gclient_sync.log found!"
        }
        Upload-Log
        Update-Status "FAILED_CHROMIUM_SYNC"
        exit 1
    }
    
    Log "  Running gclient runhooks..."
    Set-Location "$CHROMIUM_DIR\src"
    gclient runhooks 2>&1 | Out-File "$BUILD_DRIVE\gclient_hooks.log"
    Log "  Hooks completed."
} else {
    Log "  Chromium source already present, skipping fetch."
    Set-Location "$CHROMIUM_DIR\src"
}

Upload-Log
Update-Status "PHASE_4_PATCHING"

# -- Phase 4: Apply Aeon patches ----------------------------------------------
Log "Phase 4: Applying Aeon patches..."

# Download patches from GCS (uploaded by the local orchestration script)
Log "  Downloading Aeon patches from $PATCH_BUCKET..."
gsutil cp "$PATCH_BUCKET/aeon_chromium.patch" "$CHROMIUM_DIR\aeon_chromium.patch" 2>$null

if (Test-Path "$CHROMIUM_DIR\aeon_chromium.patch") {
    Log "  Applying aeon_chromium.patch..."
    Set-Location "$CHROMIUM_DIR\src"
    git apply --stat "$CHROMIUM_DIR\aeon_chromium.patch" 2>&1
    git apply "$CHROMIUM_DIR\aeon_chromium.patch" 2>&1
    if ($LASTEXITCODE -ne 0) {
        Log "  WARNING: Patch apply had issues - may be already applied or needs adjustment"
    } else {
        Log "  Patch applied successfully."
    }
} else {
    Log "  WARNING: No patch file found. Building vanilla Chromium - no Aeon mods."
}

Upload-Log
Update-Status "PHASE_5_GN_CONFIGURE"

# -- Phase 5: Configure build with GN ----------------------------------------
Log "Phase 5: Configuring GN build args..."

New-Item -ItemType Directory -Force -Path $BUILD_OUTPUT | Out-Null

# GN args - stripped Chromium optimized for release
$gnArgs = @"
# Aeon Browser - Stripped Chromium Build
# DelgadoLogic | Production Release Configuration

is_official_build = true
is_debug = false
target_cpu = "x64"

# Chrome branding disabled - we are Aeon
is_chrome_branded = false
is_component_build = false

# Stripping: Remove Google services
enable_nacl = false
enable_hangout_services_extension = false
# Note: safe_browsing_mode = 0 removed - causes unresolved deps in feedback/webshare 
# Disable safe_browsing at runtime via feature flags instead
enable_reporting = false
# Note: enable_print_preview = false removed - causes unresolved type errors
# in printing::mojom::RequestPrintPreviewParams. Disable at runtime instead.

# Remove Google cloud features
enable_remoting = false
use_official_google_api_keys = false

# Performance
symbol_level = 0
remove_webcore_debug_symbols = true
blink_symbol_level = 0

# Use standard Windows toolchain
use_lld = true

# Proprietary codecs (H.264/AAC support)
proprietary_codecs = true
ffmpeg_branding = "Chrome"
"@

# Write WITHOUT BOM - gn parser chokes on UTF-8 BOM
$utf8NoBom = New-Object System.Text.UTF8Encoding $false
[System.IO.File]::WriteAllText("$BUILD_OUTPUT\args.gn", $gnArgs, $utf8NoBom)
Log "  GN args written to $BUILD_OUTPUT\args.gn"

# Run gn gen
Set-Location "$CHROMIUM_DIR\src"
gn gen $BUILD_OUTPUT 2>&1
if ($LASTEXITCODE -ne 0) {
    Log "  ERROR: gn gen failed!"
    Upload-Log
    Update-Status "FAILED_GN_GEN"
    exit 1
}
Log "  GN configuration complete."

Upload-Log
Update-Status "PHASE_6_BUILDING"

# -- Phase 6: BUILD -----------------------------------------------------------
Log "Phase 6: Building chrome.dll with autoninja - $env:NUMBER_OF_PROCESSORS cores..."
Log "  This is the big one. Estimated: 35-50 minutes on 96 cores."

$buildStart = Get-Date

# Cap parallelism to prevent OOM. Each clang-cl can use 2-4 GB for V8 objects.
# On n2-standard-32 (128 GB RAM), -j 16 is safe. On highcpu-32 (32 GB), use -j 8.
$ramGB = [math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB)
$maxJobs = [math]::Max(4, [math]::Min(16, [math]::Floor($ramGB / 4)))
Log "  Detected $ramGB GB RAM, using -j $maxJobs"
Set-Location "$CHROMIUM_DIR\src"
autoninja -C $BUILD_OUTPUT chrome -j $maxJobs 2>&1 | Tee-Object -FilePath "$BUILD_DRIVE\ninja_build.log"
$buildExitCode = $LASTEXITCODE
$buildTime = (Get-Date) - $buildStart

Log "  Build completed in $([math]::Round($buildTime.TotalMinutes, 1)) minutes (exit code: $buildExitCode)"

if ($buildExitCode -ne 0) {
    Log "  ERROR: Ninja build FAILED!"
    # Upload the ninja log for debugging
    gsutil cp "$BUILD_DRIVE\ninja_build.log" "$GCS_BUCKET/ninja_build_FAILED.log" 2>$null
    Upload-Log
    Update-Status "FAILED_BUILD"
    exit 1
}

# Verify output
$chromeDll = "$BUILD_OUTPUT\chrome.dll"
if (-not (Test-Path $chromeDll)) {
    Log "  ERROR: chrome.dll not found at $chromeDll!"
    Upload-Log
    Update-Status "FAILED_NO_DLL"
    exit 1
}

$dllSize = [math]::Round((Get-Item $chromeDll).Length / 1MB, 1)
Log "  chrome.dll built successfully! Size: ${dllSize} MB"

Upload-Log
Update-Status "PHASE_7_PACKAGING"

# -- Phase 7: Package and upload ----------------------------------------------
Log "Phase 7: Packaging aeon_engine.dll..."

# Rename chrome.dll -> aeon_engine.dll
$aeonDll = "$BUILD_OUTPUT\aeon_engine.dll"
Copy-Item $chromeDll $aeonDll -Force
Log "  Renamed chrome.dll to aeon_engine.dll - ${dllSize} MB"

# Also grab essential companion files
$companionFiles = @(
    "chrome_elf.dll",
    "icudtl.dat",
    "v8_context_snapshot.bin",
    "resources.pak",
    "chrome_100_percent.pak",
    "chrome_200_percent.pak",
    "snapshot_blob.bin",
    "libEGL.dll",
    "libGLESv2.dll",
    "vk_swiftshader.dll",
    "vulkan-1.dll",
    "vk_swiftshader_icd.json"
)

$packageDir = "$BUILD_DRIVE\aeon_engine_package"
New-Item -ItemType Directory -Force -Path $packageDir | Out-Null
Copy-Item $aeonDll "$packageDir\" -Force

foreach ($file in $companionFiles) {
    $src = "$BUILD_OUTPUT\$file"
    if (Test-Path $src) {
        Copy-Item $src "$packageDir\" -Force
        Log "  Packaged: $file"
    }
}

# Copy locales directory
if (Test-Path "$BUILD_OUTPUT\locales") {
    Copy-Item "$BUILD_OUTPUT\locales" "$packageDir\locales" -Recurse -Force
    Log "  Packaged: locales/"
}

# Create zip archive
$zipPath = "$BUILD_DRIVE\aeon_engine_package.zip"
Compress-Archive -Path "$packageDir\*" -DestinationPath $zipPath -Force
$zipSize = [math]::Round((Get-Item $zipPath).Length / 1MB, 1)
Log "  Package created: aeon_engine_package.zip - ${zipSize} MB"

# Upload to GCS
Log "  Uploading to $GCS_BUCKET..."
$timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
gsutil cp $zipPath "$GCS_BUCKET/aeon_engine_package_$timestamp.zip" 2>&1
gsutil cp $zipPath "$GCS_BUCKET/aeon_engine_package_latest.zip" 2>&1
gsutil cp $aeonDll "$GCS_BUCKET/aeon_engine.dll" 2>&1

Log "  Upload complete!"

Upload-Log
Update-Status "BUILD_COMPLETE"

# -- Done ---------------------------------------------------------------------
$totalTime = (Get-Date) - (Get-Date $buildStart).AddMinutes(-30)  # Approximate total
Log ""
Log "==========================================================="
Log "  BUILD COMPLETE"
Log "  aeon_engine.dll: $dllSize MB"
Log "  Package: $zipSize MB"
Log "  Build time: $([math]::Round($buildTime.TotalMinutes, 1)) min"
Log "  Uploaded to: $GCS_BUCKET/aeon_engine_package_latest.zip"
Log "==========================================================="
Upload-Log
