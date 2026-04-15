# AeonBrowser - cloud_build_retro.ps1
# DelgadoLogic | Build Automation
#
# PURPOSE: Provisions a GCP Compute Engine VM (Windows Server 2022),
# installs Open Watcom 2.0, uploads retro source, compiles all retro
# engine targets, and downloads the resulting binaries.
#
# TARGETS:
#   1. AEON16.EXE      - Win 3.1/3.11 16-bit browser
#   2. aeon_html4.dll   - Win 9x/2000 HTML4 renderer + VTable adapter
#
# USAGE:
#   .\cloud_build_retro.ps1 -ProjectId <gcp-project-id>
#
# PREREQUISITES:
#   - gcloud CLI authenticated with compute.admin role
#   - GCS bucket for artifacts (created automatically)

param(
    [Parameter(Mandatory=$true)]
    [string]$ProjectId,

    [string]$Zone = "us-central1-a",
    [string]$MachineType = "e2-standard-4",
    [string]$InstanceName = "aeon-retro-builder",
    [string]$ArtifactBucket = ""
)

$ErrorActionPreference = "Continue"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$RetroDir  = Join-Path $ScriptDir "retro"
$EngineDir = Join-Path $ScriptDir "engine"

# --- Preflight ----------------------------------------------------------------
Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host "|  Aeon Browser - Retro Cloud Build        |" -ForegroundColor Cyan
Write-Host "|  DelgadoLogic | $(Get-Date -Format 'yyyy-MM-dd HH:mm') EST   |" -ForegroundColor Cyan
Write-Host "========================================`n" -ForegroundColor Cyan

if (-not $ArtifactBucket) {
    $ArtifactBucket = "aeon-retro-build-$ProjectId"
}

# Verify gcloud is available
if (-not (Get-Command gcloud -ErrorAction SilentlyContinue)) {
    Write-Error "gcloud CLI not found. Install: https://cloud.google.com/sdk/docs/install"
    exit 1
}
Write-Host "  gcloud found: $(Get-Command gcloud | Select -Expand Source)" -ForegroundColor DarkGray

# --- Step 1: Create GCS bucket (if needed) ------------------------------------
Write-Host "[1/7] Creating artifact bucket: gs://$ArtifactBucket" -ForegroundColor Yellow
gsutil mb -p $ProjectId -l us-central1 "gs://$ArtifactBucket" 2>$null
if ($LASTEXITCODE -ne 0) {
    Write-Host "  Bucket already exists or was just created." -ForegroundColor DarkGray
}

# --- Step 2: Upload source files to GCS ---------------------------------------
Write-Host "[2/7] Uploading retro source to GCS..." -ForegroundColor Yellow
$sourceFiles = @(
    "aeon16.c", "aeon_html4.c", "aeon_html4.h",
    "aeon_html4_adapter.c",
    "html4.c", "html4.h",
    "wolfssl_bridge.c", "wolfssl_bridge.h",
    "makefile", "makefile_32bit"
)
foreach ($f in $sourceFiles) {
    $src = Join-Path $RetroDir $f
    if (Test-Path $src) {
        gsutil cp $src "gs://$ArtifactBucket/src/$f"
    } else {
        Write-Warning "  Missing: $f"
    }
}

# Also upload AeonEngine_Interface.h (needed by adapter)
$interfaceH = Join-Path $ScriptDir "core\engine\AeonEngine_Interface.h"
if (Test-Path $interfaceH) {
    gsutil cp $interfaceH "gs://$ArtifactBucket/src/AeonEngine_Interface.h"
}

# --- Step 2.5: Stage Open Watcom installer to GCS (avoids GitHub flakiness from GCE) ---
Write-Host "[2.5/7] Staging Open Watcom installer to GCS..." -ForegroundColor Yellow
$owCheckExists = gsutil ls "gs://$ArtifactBucket/tools/open-watcom-2_0-c-win-x64.exe" 2>&1
if ($LASTEXITCODE -ne 0) {
    $owLocalPath = Join-Path $env:TEMP "open-watcom-2_0-c-win-x64.exe"
    if (-not (Test-Path $owLocalPath) -or (Get-Item $owLocalPath).Length -lt 1000000) {
        Write-Host "  Downloading Open Watcom from GitHub to local machine..." -ForegroundColor DarkGray
        $owUrl = "https://github.com/open-watcom/open-watcom-v2/releases/download/2024-12-01-Build/open-watcom-2_0-c-win-x64.exe"
        Invoke-WebRequest -Uri $owUrl -OutFile $owLocalPath -UseBasicParsing
    }
    Write-Host "  Uploading to GCS..." -ForegroundColor DarkGray
    gsutil cp $owLocalPath "gs://$ArtifactBucket/tools/open-watcom-2_0-c-win-x64.exe"
} else {
    Write-Host "  Already staged." -ForegroundColor DarkGray
}

# --- Step 3: Create Windows VM with startup script ----------------------------
Write-Host "[3/7] Creating VM: $InstanceName ($MachineType in $Zone)..." -ForegroundColor Yellow

# Build the startup script that the VM runs on first boot
$startupScript = @'
# Aeon Retro Build - Windows Startup Script
# Runs on first boot of the Windows Server 2022 VM

$ErrorActionPreference = "Stop"
$BuildDir = "C:\aeon_retro_build"
$OWDir    = "C:\ow2"
$LogFile  = "C:\aeon_build.log"

function Log($msg) {
    $ts = Get-Date -Format "HH:mm:ss"
    "[$ts] $msg" | Tee-Object -FilePath $LogFile -Append
}

Log "=== Aeon Retro Build Starting ==="

# Create directories
New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null
New-Item -ItemType Directory -Path "$BuildDir\include" -Force | Out-Null

# Download Open Watcom 2.0 from our GCS bucket (pre-staged by build script)
Log "Downloading Open Watcom 2.0 from GCS..."
$owInstaller = "$BuildDir\ow2_install.exe"
$ErrorActionPreference = "Continue"
gsutil cp "gs://BUCKET_PLACEHOLDER/tools/open-watcom-2_0-c-win-x64.exe" $owInstaller
$ErrorActionPreference = "Stop"
if (-not (Test-Path $owInstaller) -or (Get-Item $owInstaller).Length -lt 1000000) {
    Log "FATAL: Open Watcom installer download failed"
    "FAILED" | Out-File "C:\aeon_build_done.txt"
    exit 1
}
Log "  Downloaded: $((Get-Item $owInstaller).Length) bytes"

# Extract Open Watcom using 7-Zip
Log "Installing Open Watcom via extraction..."

# Install 7-Zip via direct MSI download (silent install)
$7zMsiUrl = "https://www.7-zip.org/a/7z2409-x64.msi"
$7zMsi = "$BuildDir\7z.msi"
$ErrorActionPreference = "Continue"
& curl.exe -L -o $7zMsi --retry 3 --connect-timeout 30 $7zMsiUrl 2>$null
$ErrorActionPreference = "Stop"
if (Test-Path $7zMsi) {
    Start-Process msiexec -ArgumentList "/i `"$7zMsi`" /qn INSTALLDIR=`"C:\7zip`"" -Wait -NoNewWindow
    Start-Sleep -Seconds 3
}

# Find 7z.exe
$7z = "C:\7zip\7z.exe"
if (-not (Test-Path $7z)) {
    $7z = "C:\Program Files\7-Zip\7z.exe"
}
if (-not (Test-Path $7z)) {
    Log "FATAL: 7-Zip not found after install"
    "FAILED" | Out-File "C:\aeon_build_done.txt"
    exit 1
}
Log "  7-Zip found at: $7z"

# Extract the OW installer
New-Item -ItemType Directory -Path $OWDir -Force | Out-Null
$ErrorActionPreference = "Continue"
& $7z x $owInstaller "-o$OWDir" -y 2>&1 | Out-Null
$ErrorActionPreference = "Stop"
Start-Sleep -Seconds 3

# Verify extraction - search for wcc.exe recursively
$wcc = Get-ChildItem -Path $OWDir -Filter "wcc.exe" -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
if ($wcc) {
    # Adjust OWDir if files are in a subdirectory
    $actualBinDir = Split-Path $wcc.FullName
    $actualOWDir = Split-Path $actualBinDir
    if ($actualOWDir -ne $OWDir) {
        Log "  OW found in subdirectory, adjusting..."
        $OWDir = $actualOWDir
    }
    Log "  wcc.exe found at: $($wcc.FullName)"
} else {
    # List what we got
    Log "WARN: wcc.exe not found - listing extracted contents..."
    Get-ChildItem -Path $OWDir -Recurse -Depth 3 | Select-Object -First 50 | ForEach-Object { Log "  $($_.FullName)" }
    Log "FATAL: wcc.exe not found anywhere in extraction"
    # Upload log before dying
    gsutil cp $LogFile "gs://BUCKET_PLACEHOLDER/out/build.log" 2>$null
    "FAILED" | Out-File "C:\aeon_build_done.txt"
    exit 1
}
Log "  Open Watcom installed at: $OWDir"

# Set environment for Open Watcom
$env:WATCOM = $OWDir
$env:PATH   = "$OWDir\binw;$OWDir\binnt64;$OWDir\binnt;$env:PATH"
$env:INCLUDE = "$OWDir\h;$OWDir\h\nt;$OWDir\h\nt\ddk"
$env:LIB    = "$OWDir\lib386\nt;$OWDir\lib286\win"

# Download source from GCS
Log "Downloading source files from GCS..."
gsutil -m cp "gs://BUCKET_PLACEHOLDER/src/*" $BuildDir\

# Copy interface header to expected location
New-Item -ItemType Directory -Path "$BuildDir\..\core\engine" -Force | Out-Null
Copy-Item "$BuildDir\AeonEngine_Interface.h" "$BuildDir\..\core\engine\AeonEngine_Interface.h" -Force

# --- Build 1: AEON16.EXE (16-bit Win3.x) ------------------------------------
Log "=== Building AEON16.EXE (16-bit) ==="
Push-Location $BuildDir
try {
    # Compile each .c file
    & "$OWDir\binnt\wcc.exe" -ml -zW -bt=windows -d2 -W3 aeon16.c
    if ($LASTEXITCODE -ne 0) { Log "ERROR: wcc aeon16.c failed"; throw "compile error" }

    & "$OWDir\binnt\wcc.exe" -ml -zW -bt=windows -d2 -W3 html4.c
    if ($LASTEXITCODE -ne 0) { Log "ERROR: wcc html4.c failed"; throw "compile error" }

    & "$OWDir\binnt\wcc.exe" -ml -zW -bt=windows -d2 -W3 wolfssl_bridge.c
    if ($LASTEXITCODE -ne 0) { Log "ERROR: wcc wolfssl_bridge.c failed"; throw "compile error" }

    # Link
    & "$OWDir\binnt\wlink.exe" NAME AEON16.EXE SYSTEM windows `
        OPTION HEAPSIZE=8192 OPTION STACKSIZE=8192 OPTION MAP `
        LIBRARY "$OWDir\lib286\win\libw.lib" `
        LIBRARY "$OWDir\lib286\win\clibs.lib" `
        FILE aeon16.obj FILE html4.obj FILE wolfssl_bridge.obj
    if ($LASTEXITCODE -ne 0) { Log "ERROR: wlink aeon16 failed"; throw "link error" }

    Log "SUCCESS: AEON16.EXE built ($(Get-Item AEON16.EXE | Select -Expand Length) bytes)"

    # Upload artifact
    gsutil cp AEON16.EXE "gs://BUCKET_PLACEHOLDER/out/AEON16.EXE"
    gsutil cp AEON16.MAP "gs://BUCKET_PLACEHOLDER/out/AEON16.MAP" 2>$null
} catch {
    Log "AEON16 build FAILED: $_"
}
Pop-Location

# --- Build 2: aeon_html4.dll (32-bit Win9x/2000) ----------------------------
Log "=== Building aeon_html4.dll (32-bit) ==="
Push-Location $BuildDir
try {
    # Compile each .c file (32-bit)
    & "$OWDir\binnt\wcc386.exe" -bt=nt -d2 -W3 -wx -DAEON_HTML4_BUILD "-I$BuildDir\..\core\engine" aeon_html4.c
    if ($LASTEXITCODE -ne 0) { Log "ERROR: wcc386 aeon_html4.c failed"; throw "compile error" }

    & "$OWDir\binnt\wcc386.exe" -bt=nt -d2 -W3 -wx -DAEON_HTML4_BUILD "-I$BuildDir\..\core\engine" aeon_html4_adapter.c
    if ($LASTEXITCODE -ne 0) { Log "ERROR: wcc386 adapter failed"; throw "compile error" }

    & "$OWDir\binnt\wcc386.exe" -bt=nt -d2 -W3 -wx -DAEON_HTML4_BUILD wolfssl_bridge.c
    if ($LASTEXITCODE -ne 0) { Log "ERROR: wcc386 wolfssl failed"; throw "compile error" }

    # Link as DLL
    & "$OWDir\binnt\wlink.exe" NAME aeon_html4.dll `
        SYSTEM nt_dll INITINSTANCE TERMINSTANCE `
        OPTION MAP OPTION IMPLIB=aeon_html4.lib `
        EXPORT AeonEngine_Create `
        EXPORT AeonHTML4_Init `
        EXPORT AeonHTML4_Shutdown `
        EXPORT AeonHTML4_Render `
        EXPORT AeonHTML4_HitTest `
        LIBRARY "$OWDir\lib386\nt\kernel32.lib" `
        LIBRARY "$OWDir\lib386\nt\user32.lib" `
        LIBRARY "$OWDir\lib386\nt\gdi32.lib" `
        FILE aeon_html4.obj FILE aeon_html4_adapter.obj FILE wolfssl_bridge.obj
    if ($LASTEXITCODE -ne 0) { Log "ERROR: wlink dll failed"; throw "link error" }

    Log "SUCCESS: aeon_html4.dll built ($(Get-Item aeon_html4.dll | Select -Expand Length) bytes)"

    # Upload artifacts
    gsutil cp aeon_html4.dll "gs://BUCKET_PLACEHOLDER/out/aeon_html4.dll"
    gsutil cp aeon_html4.lib "gs://BUCKET_PLACEHOLDER/out/aeon_html4.lib" 2>$null
    gsutil cp aeon_html4.map "gs://BUCKET_PLACEHOLDER/out/aeon_html4.map" 2>$null
} catch {
    Log "aeon_html4.dll build FAILED: $_"
}
Pop-Location

# Upload build log
gsutil cp $LogFile "gs://BUCKET_PLACEHOLDER/out/build.log"

# Signal completion
Log "=== BUILD COMPLETE ==="
"DONE" | Out-File "C:\aeon_build_done.txt"
'@

# Replace bucket placeholder
$startupScript = $startupScript -replace 'BUCKET_PLACEHOLDER', $ArtifactBucket

# Write startup script to temp file
$startupFile = Join-Path $env:TEMP "aeon_retro_startup.ps1"
$startupScript | Out-File -FilePath $startupFile -Encoding UTF8

# Create the VM
gcloud compute instances create $InstanceName `
    --project=$ProjectId `
    --zone=$Zone `
    --machine-type=$MachineType `
    --image-family=windows-2022 `
    --image-project=windows-cloud `
    --boot-disk-size=100GB `
    --boot-disk-type=pd-ssd `
    --metadata-from-file="windows-startup-script-ps1=$startupFile" `
    --scopes=storage-full `
    --provisioning-model=SPOT `
    --instance-termination-action=STOP `
    --no-restart-on-failure

if ($LASTEXITCODE -ne 0) {
    Write-Error "Failed to create VM. Check project quota and billing."
    exit 1
}

Write-Host "[4/7] VM created. Waiting for build to complete..." -ForegroundColor Yellow
Write-Host "  (Estimated: 15-30 minutes for Open Watcom install + compile)" -ForegroundColor DarkGray

# --- Step 4: Poll for completion ----------------------------------------------
Write-Host "[5/7] Polling for build artifacts..." -ForegroundColor Yellow
$maxWait = 1800  # 30 minutes max
$elapsed = 0
$pollInterval = 30

while ($elapsed -lt $maxWait) {
    Start-Sleep -Seconds $pollInterval
    $elapsed += $pollInterval

    # Check if build log mentions DONE
    $buildLog = gsutil cat "gs://$ArtifactBucket/out/build.log" 2>$null
    if ($buildLog -and $buildLog -match "BUILD COMPLETE") {
        Write-Host "  Build completed in $elapsed seconds!" -ForegroundColor Green
        break
    }

    $minutes = [math]::Round($elapsed / 60, 1)
    Write-Host "  Waiting... ($minutes min elapsed)" -ForegroundColor DarkGray
}

if ($elapsed -ge $maxWait) {
    Write-Warning "Build timed out after 30 minutes. Check VM logs manually."
}

# --- Step 5: Download artifacts -----------------------------------------------
Write-Host "[6/7] Downloading build artifacts..." -ForegroundColor Yellow
$outDir = Join-Path $EngineDir "retro_out"
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

gsutil -m cp "gs://$ArtifactBucket/out/*" $outDir\
Write-Host "  Artifacts saved to: $outDir" -ForegroundColor Green

# List what we got
Get-ChildItem $outDir | ForEach-Object {
    $sizeKB = [math]::Round($_.Length / 1024, 1)
    Write-Host "    $($_.Name) - $sizeKB KB" -ForegroundColor White
}

# --- Step 6: Cleanup - delete VM ----------------------------------------------
Write-Host "[7/7] Deleting builder VM..." -ForegroundColor Yellow
gcloud compute instances delete $InstanceName `
    --project=$ProjectId `
    --zone=$Zone `
    --quiet

Write-Host "`n========================================" -ForegroundColor Green
Write-Host "|  Retro Build Complete!                   |" -ForegroundColor Green
Write-Host "|  Check: engine\retro_out\               |" -ForegroundColor Green
Write-Host "========================================`n" -ForegroundColor Green

# Download the build log for review
$logPath = Join-Path $outDir "build.log"
if (Test-Path $logPath) {
    Write-Host "Build log:" -ForegroundColor Cyan
    Get-Content $logPath
}
