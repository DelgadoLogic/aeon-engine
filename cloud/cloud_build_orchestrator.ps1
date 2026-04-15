# ═══════════════════════════════════════════════════════════════════════════════
# AeonBrowser — Cloud Build Orchestrator
# DelgadoLogic | Run this locally to build aeon_engine.dll on GCP
#
# This script:
#   1. Uploads Aeon patches to GCS
#   2. Creates a Spot VM (n2-highcpu-96, Windows Server 2022)
#   3. Injects the build startup script
#   4. Monitors build progress via GCS status file
#   5. Downloads the finished aeon_engine.dll package
#   6. Tears down the VM + disks
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File cloud_build_orchestrator.ps1
#   powershell -ExecutionPolicy Bypass -File cloud_build_orchestrator.ps1 -SkipCreate  # Monitor only
#   powershell -ExecutionPolicy Bypass -File cloud_build_orchestrator.ps1 -TeardownOnly  # Cleanup
#
# Prerequisites:
#   - gcloud CLI authenticated (gcloud auth login)
#   - Project: aeon-browser-build
# ═══════════════════════════════════════════════════════════════════════════════

param(
    [switch]$SkipCreate   = $false,   # Skip VM creation, just monitor
    [switch]$TeardownOnly = $false,   # Only tear down existing resources
    [switch]$KeepDisk     = $false,   # Keep build disk for future incremental builds
    [string]$MachineType  = "n2-highcpu-96",
    [string]$Zone         = "us-east1-b",
    [string]$Project      = "aeon-browser-build"
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

# ── Configuration ─────────────────────────────────────────────────────────────
$VM_NAME          = "aeon-chromium-builder"
$DISK_NAME        = "aeon-build-disk"
$DISK_SIZE_GB     = 300
$IMAGE_FAMILY     = "windows-2022"
$IMAGE_PROJECT    = "windows-cloud"
$GCS_BUCKET       = "gs://aeon-sovereign-artifacts"
$GCS_STATUS_FILE  = "$GCS_BUCKET/build-status.txt"
$GCS_PACKAGE_FILE = "$GCS_BUCKET/aeon_engine_package_latest.zip"
$GCS_LOG_FILE     = "$GCS_BUCKET/build-log.txt"
$PATCHES_DIR      = (Split-Path -Parent $MyInvocation.MyCommand.Path) -replace '\\cloud$', '\patches'
$STARTUP_SCRIPT   = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) "chromium_build_vm_startup.ps1"
$LOCAL_OUTPUT_DIR  = (Split-Path -Parent $MyInvocation.MyCommand.Path) -replace '\\cloud$', '\build\CloudRelease'

Write-Host ""
Write-Host "╔══════════════════════════════════════════════════════════╗" -ForegroundColor Magenta
Write-Host "║   Aeon Browser — Cloud Build Orchestrator v1.0          ║" -ForegroundColor Magenta
Write-Host "║   DelgadoLogic.tech                                     ║" -ForegroundColor Magenta
Write-Host "╚══════════════════════════════════════════════════════════╝" -ForegroundColor Magenta
Write-Host ""
Write-Host "  Project:  $Project"
Write-Host "  Zone:     $Zone"
Write-Host "  Machine:  $MachineType"
Write-Host "  VM Name:  $VM_NAME"
Write-Host ""

# ── Teardown function ─────────────────────────────────────────────────────────
function Invoke-Teardown {
    Write-Host ""
    Write-Host "[TEARDOWN] Cleaning up cloud resources..." -ForegroundColor Yellow
    
    # Delete VM
    Write-Host "  Deleting VM: $VM_NAME..."
    gcloud compute instances delete $VM_NAME `
        --project=$Project --zone=$Zone --quiet 2>$null
    if ($LASTEXITCODE -eq 0) { Write-Host "  VM deleted." -ForegroundColor Green }
    else { Write-Host "  VM not found or already deleted." -ForegroundColor DarkGray }
    
    # Delete build disk (only if not keeping it)
    if (-not $KeepDisk) {
        Write-Host "  Deleting build disk: $DISK_NAME..."
        gcloud compute disks delete $DISK_NAME `
            --project=$Project --zone=$Zone --quiet 2>$null
        if ($LASTEXITCODE -eq 0) { Write-Host "  Disk deleted." -ForegroundColor Green }
        else { Write-Host "  Disk not found or already deleted." -ForegroundColor DarkGray }
    } else {
        Write-Host "  Keeping build disk (--KeepDisk). Future incremental builds will be faster." -ForegroundColor Cyan
    }
    
    # Clean up GCS status files
    gsutil rm $GCS_STATUS_FILE 2>$null
    gsutil rm $GCS_LOG_FILE 2>$null
    
    Write-Host "[TEARDOWN] Complete." -ForegroundColor Green
}

# ── Teardown-only mode ────────────────────────────────────────────────────────
if ($TeardownOnly) {
    Invoke-Teardown
    exit 0
}

# ── Step 0: Verify prerequisites ─────────────────────────────────────────────
Write-Host "[0/6] Verifying prerequisites..." -ForegroundColor Cyan

# Check gcloud auth
$account = gcloud auth list --filter=status:ACTIVE --format="value(account)" 2>$null
if (-not $account) {
    Write-Host "  ERROR: Not authenticated with gcloud. Run: gcloud auth login" -ForegroundColor Red
    exit 1
}
Write-Host "  Authenticated as: $account"

# Verify project exists
gcloud projects describe $Project --format="value(projectId)" 2>$null | Out-Null
if ($LASTEXITCODE -ne 0) {
    Write-Host "  ERROR: Project '$Project' not found or no access." -ForegroundColor Red
    exit 1
}
Write-Host "  Project verified: $Project"

# Verify startup script exists
if (-not (Test-Path $STARTUP_SCRIPT)) {
    Write-Host "  ERROR: Startup script not found: $STARTUP_SCRIPT" -ForegroundColor Red
    exit 1
}
Write-Host "  Startup script: $STARTUP_SCRIPT"
Write-Host ""

if (-not $SkipCreate) {
    # ── Step 1: Upload patches to GCS ─────────────────────────────────────────
    Write-Host "[1/6] Uploading Aeon patches to GCS..." -ForegroundColor Cyan
    
    if (Test-Path "$PATCHES_DIR\aeon_chromium.patch") {
        gsutil cp "$PATCHES_DIR\aeon_chromium.patch" "$GCS_BUCKET/patches/aeon_chromium.patch" 2>&1
        Write-Host "  Uploaded: aeon_chromium.patch"
    } else {
        Write-Host "  WARNING: No patches found at $PATCHES_DIR" -ForegroundColor Yellow
        Write-Host "  Building vanilla Chromium (no Aeon modifications)."
    }
    
    # Upload any additional patch files from chrome/ and third_party/ subdirs
    Get-ChildItem "$PATCHES_DIR" -Recurse -Filter "*.patch" -ErrorAction SilentlyContinue | ForEach-Object {
        if ($_.Name -ne "aeon_chromium.patch") {
            $relPath = $_.FullName.Replace($PATCHES_DIR, "").TrimStart('\').Replace('\', '/')
            gsutil cp $_.FullName "$GCS_BUCKET/patches/$relPath" 2>&1
            Write-Host "  Uploaded: $relPath"
        }
    }
    Write-Host ""

    # ── Step 2: Create the Spot VM ────────────────────────────────────────────
    Write-Host "[2/6] Creating Spot VM: $VM_NAME ($MachineType)..." -ForegroundColor Cyan
    Write-Host "  This is a SPOT/PREEMPTIBLE instance (~70% cheaper)."
    
    # Clear any previous status
    gsutil rm $GCS_STATUS_FILE 2>$null
    
    gcloud compute instances create $VM_NAME `
        --project=$Project `
        --zone=$Zone `
        --machine-type=$MachineType `
        --provisioning-model=SPOT `
        --instance-termination-action=STOP `
        --image-family=$IMAGE_FAMILY `
        --image-project=$IMAGE_PROJECT `
        --boot-disk-size=100GB `
        --boot-disk-type=pd-ssd `
        --metadata-from-file=windows-startup-script-ps1=$STARTUP_SCRIPT `
        --scopes=storage-full,compute-rw `
        --no-restart-on-failure `
        --labels=purpose=chromium-build,project=aeon-browser
    
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  ERROR: Failed to create VM!" -ForegroundColor Red
        Write-Host "  Check quota/billing at: https://console.cloud.google.com/compute" -ForegroundColor Yellow
        exit 1
    }
    
    Write-Host "  VM created successfully!" -ForegroundColor Green
    Write-Host "  The startup script will begin executing automatically."
    Write-Host ""
}

# ── Step 3: Monitor build progress ───────────────────────────────────────────
Write-Host "[3/6] Monitoring build progress..." -ForegroundColor Cyan
Write-Host "  Polling GCS status every 60 seconds."
Write-Host "  Press Ctrl+C to stop monitoring (VM will continue building)."
Write-Host ""

$startTime = Get-Date
$lastStatus = ""
$buildComplete = $false

while (-not $buildComplete) {
    Start-Sleep -Seconds 60
    $elapsed = [math]::Round(((Get-Date) - $startTime).TotalMinutes, 1)
    
    # Check if VM still exists (might have been preempted)
    $vmStatus = gcloud compute instances describe $VM_NAME `
        --project=$Project --zone=$Zone --format="value(status)" 2>$null
    
    if (-not $vmStatus) {
        Write-Host "  [$elapsed min] WARNING: VM no longer exists! May have been preempted." -ForegroundColor Red
        Write-Host "  Run this script again to retry." -ForegroundColor Yellow
        exit 1
    }
    
    if ($vmStatus -eq "TERMINATED" -or $vmStatus -eq "STOPPED") {
        Write-Host "  [$elapsed min] WARNING: VM is $vmStatus (likely preempted)." -ForegroundColor Red
        Write-Host "  Checking if build completed before termination..." -ForegroundColor Yellow
    }
    
    # Check GCS status file
    $currentStatus = gsutil cat $GCS_STATUS_FILE 2>$null
    
    if ($currentStatus -and $currentStatus -ne $lastStatus) {
        $lastStatus = $currentStatus
        
        switch -Wildcard ($currentStatus) {
            "PHASE_1*"    { Write-Host "  [$elapsed min] ⏳ Installing Visual Studio Build Tools..." -ForegroundColor Yellow }
            "PHASE_2*"    { Write-Host "  [$elapsed min] ⏳ Installing depot_tools..." -ForegroundColor Yellow }
            "PHASE_3*"    { Write-Host "  [$elapsed min] ⏳ Fetching Chromium source (~20 min)..." -ForegroundColor Yellow }
            "PHASE_4*"    { Write-Host "  [$elapsed min] ⏳ Applying Aeon patches..." -ForegroundColor Yellow }
            "PHASE_5*"    { Write-Host "  [$elapsed min] ⏳ Configuring GN build..." -ForegroundColor Yellow }
            "PHASE_6*"    { Write-Host "  [$elapsed min] 🔨 BUILDING chrome.dll (~35-50 min on 96 cores)..." -ForegroundColor Cyan }
            "PHASE_7*"    { Write-Host "  [$elapsed min] 📦 Packaging aeon_engine.dll..." -ForegroundColor Cyan }
            "BUILD_COMPLETE" {
                Write-Host ""
                Write-Host "  [$elapsed min] ✅ BUILD COMPLETE!" -ForegroundColor Green
                $buildComplete = $true
            }
            "FAILED*" {
                Write-Host "  [$elapsed min] ❌ BUILD FAILED: $currentStatus" -ForegroundColor Red
                Write-Host "  Downloading build log for inspection..." -ForegroundColor Yellow
                gsutil cp $GCS_LOG_FILE "$LOCAL_OUTPUT_DIR\build-log-FAILED.txt" 2>$null
                Write-Host "  Log saved to: $LOCAL_OUTPUT_DIR\build-log-FAILED.txt"
                Invoke-Teardown
                exit 1
            }
        }
    } elseif (-not $currentStatus) {
        Write-Host "  [$elapsed min] ⏳ Waiting for VM to boot and start building..." -ForegroundColor DarkGray
    }
}

# ── Step 4: Download the engine package ──────────────────────────────────────
Write-Host ""
Write-Host "[4/6] Downloading aeon_engine.dll package..." -ForegroundColor Cyan

New-Item -ItemType Directory -Force -Path $LOCAL_OUTPUT_DIR | Out-Null

gsutil cp "$GCS_PACKAGE_FILE" "$LOCAL_OUTPUT_DIR\aeon_engine_package.zip" 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "  ERROR: Failed to download package from GCS!" -ForegroundColor Red
    exit 1
}

$zipSize = [math]::Round((Get-Item "$LOCAL_OUTPUT_DIR\aeon_engine_package.zip").Length / 1MB, 1)
Write-Host "  Downloaded: aeon_engine_package.zip ($zipSize MB)"

# Extract
Write-Host "  Extracting..."
Expand-Archive -Path "$LOCAL_OUTPUT_DIR\aeon_engine_package.zip" -DestinationPath $LOCAL_OUTPUT_DIR -Force
Write-Host "  Extracted to: $LOCAL_OUTPUT_DIR"

# Verify aeon_engine.dll exists
if (Test-Path "$LOCAL_OUTPUT_DIR\aeon_engine.dll") {
    $dllSize = [math]::Round((Get-Item "$LOCAL_OUTPUT_DIR\aeon_engine.dll").Length / 1MB, 1)
    Write-Host "  aeon_engine.dll: $dllSize MB ✅" -ForegroundColor Green
} else {
    Write-Host "  WARNING: aeon_engine.dll not found in package!" -ForegroundColor Yellow
}

# Download build log
gsutil cp $GCS_LOG_FILE "$LOCAL_OUTPUT_DIR\build-log.txt" 2>$null
Write-Host ""

# ── Step 5: Tear down VM ─────────────────────────────────────────────────────
Write-Host "[5/6] Tearing down cloud resources..." -ForegroundColor Cyan
Invoke-Teardown
Write-Host ""

# ── Step 6: Summary ──────────────────────────────────────────────────────────
$totalElapsed = [math]::Round(((Get-Date) - $startTime).TotalMinutes, 1)

Write-Host "╔══════════════════════════════════════════════════════════╗" -ForegroundColor Green
Write-Host "║   AEON ENGINE BUILD — COMPLETE                          ║" -ForegroundColor Green
Write-Host "╠══════════════════════════════════════════════════════════╣" -ForegroundColor Green
Write-Host "║                                                          ║" -ForegroundColor Green
Write-Host "║   Output:  $LOCAL_OUTPUT_DIR" -ForegroundColor Green
Write-Host "║   Engine:  aeon_engine.dll ($dllSize MB)" -ForegroundColor Green
Write-Host "║   Time:    $totalElapsed minutes" -ForegroundColor Green
Write-Host "║                                                          ║" -ForegroundColor Green
Write-Host "║   Next: Run build_beta.ps1 to build Aeon.exe shell      ║" -ForegroundColor Green
Write-Host "║   The TierDispatcher will auto-load aeon_engine.dll      ║" -ForegroundColor Green
Write-Host "║                                                          ║" -ForegroundColor Green
Write-Host "╚══════════════════════════════════════════════════════════╝" -ForegroundColor Green
Write-Host ""
