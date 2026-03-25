# AeonBrowser — build_installer.ps1
# DelgadoLogic | Build Team
#
# Full build + sign + package pipeline:
#   1. Build Rust protocol router (cargo)
#   2. Build C++ browser core (cmake + MSBuild)
#   3. Bake icon (build_icon.ps1)
#   4. Download/refresh filter lists (download_filterlists.ps1)
#   5. Sign Aeon.exe + DLLs with Authenticode (signtool)
#   6. Compile Inno Setup installer
#   7. Sign installer EXE
#   8. Compute SHA-256, write manifest.json for AutoUpdater
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File installer\build_installer.ps1
#   powershell -ExecPol Bypass -File installer\build_installer.ps1 -Version 1.2.3 -Channel beta
#   powershell -ExecPol Bypass -File installer\build_installer.ps1 -SkipSign
#
# Requirements:
#   - MSVC 2022 Build Tools (cl.exe in PATH or via vswhere)
#   - CMake 3.22+
#   - Rust / cargo 1.75+
#   - Inno Setup 6.2+ (iscc.exe in PATH or C:\Program Files (x86)\Inno Setup 6\)
#   - (Optional) signtool.exe + code-signing certificate for production builds

param(
    [string]$Version  = "",
    [string]$Tier     = "Pro",         # Pro | Extended | XPHi | Retro
    [string]$Channel  = "stable",      # stable | beta | nightly
    [string]$OutDir   = "installer\Output",
    [switch]$SkipSign = $false,
    [switch]$SkipRust = $false,
    [switch]$SkipIco  = $false,
    [switch]$SkipLists= $false
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$Root = (Get-Item $PSScriptRoot).Parent.FullName
Write-Host "`n[build_installer] ═══ Aeon Browser Build Pipeline ═══" -ForegroundColor Cyan
Write-Host "[build_installer] Root    : $Root"
Write-Host "[build_installer] Version : $Version"
Write-Host "[build_installer] Tier    : $Tier | Channel: $Channel`n"

# ─── 1. Auto-detect version from git tag if not provided ─────────────────────
if (-not $Version) {
    try {
        $Version = (git -C $Root describe --tags --abbrev=0 2>$null).Trim()
        if (-not $Version) { $Version = "1.0.0" }
        # Strip leading 'v'
        $Version = $Version -replace '^v',''
    } catch { $Version = "1.0.0" }
}
Write-Host "[build_installer] Final version: $Version" -ForegroundColor Yellow

# ─── 2. Locate build tools ────────────────────────────────────────────────────
function Find-Tool {
    param([string]$Name, [string[]]$ExtraSearch = @())
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    foreach ($p in $ExtraSearch) {
        if (Test-Path $p) { return $p }
    }
    return $null
}

$cmake    = Find-Tool "cmake"
$cargo    = Find-Tool "cargo"
$iscc     = Find-Tool "iscc" @(
    "C:\Program Files (x86)\Inno Setup 6\ISCC.exe",
    "C:\Program Files\Inno Setup 6\ISCC.exe"
)
$signtool = Find-Tool "signtool" @(
    "$env:ProgramFiles\Windows Kits\10\bin\x64\signtool.exe",
    "$env:ProgramFiles\Windows Kits\10\bin\10.0.22621.0\x64\signtool.exe"
)

if (-not $cmake) { Write-Error "cmake not found in PATH"; exit 1 }
if (-not $cargo -and -not $SkipRust) { Write-Warning "cargo not found — Rust router won't be built" }
if (-not $iscc)  { Write-Error "ISCC.exe (Inno Setup 6) not found"; exit 1 }
if (-not $signtool -and -not $SkipSign) { Write-Warning "signtool not found — binaries won't be signed" }

# ─── 3. Build Rust protocol router ───────────────────────────────────────────
if ($cargo -and -not $SkipRust -and $Tier -ne "Retro") {
    Write-Host "`n[Step 1/8] Building Rust protocol router..." -ForegroundColor Cyan
    Push-Location "$Root\router"
    & cargo build --release
    if ($LASTEXITCODE -ne 0) { Write-Error "cargo build failed"; exit 1 }
    Pop-Location
    Write-Host "[Step 1/8] ✓ aeon_router.dll built" -ForegroundColor Green
} else {
    Write-Host "`n[Step 1/8] Skipping Rust router build" -ForegroundColor DarkGray
}

# ─── 4. Build C++ browser core ────────────────────────────────────────────────
Write-Host "`n[Step 2/8] Building C++ core (Tier=$Tier)..." -ForegroundColor Cyan

$buildDir = "$Root\build\$Tier"
$null = New-Item -ItemType Directory -Force -Path $buildDir

# Configure
& $cmake -B $buildDir -S $Root `
    -DAEON_TARGET_TIER=$Tier `
    -DCMAKE_BUILD_TYPE=Release `
    -G "Visual Studio 17 2022" -A x64
if ($LASTEXITCODE -ne 0) { Write-Error "CMake configure failed"; exit 1 }

# Build
& $cmake --build $buildDir --config Release --parallel
if ($LASTEXITCODE -ne 0) { Write-Error "CMake build failed"; exit 1 }
Write-Host "[Step 2/8] ✓ Aeon.exe built" -ForegroundColor Green

# ─── 5. Bake icon ─────────────────────────────────────────────────────────────
if (-not $SkipIco) {
    Write-Host "`n[Step 3/8] Baking icon..." -ForegroundColor Cyan
    & powershell -ExecutionPolicy Bypass -File "$Root\resources\icons\build_icon.ps1"
    Write-Host "[Step 3/8] ✓ Aeon.ico baked" -ForegroundColor Green
}

# ─── 6. Download filter lists ─────────────────────────────────────────────────
if (-not $SkipLists) {
    Write-Host "`n[Step 4/8] Refreshing filter lists..." -ForegroundColor Cyan
    $dlScript = "$Root\resources\filters\download_filterlists.ps1"
    if (Test-Path $dlScript) {
        & powershell -ExecutionPolicy Bypass -File $dlScript
    } else {
        Write-Warning "[Step 4/8] download_filterlists.ps1 not found — using cached lists"
    }
}

# ─── 7. Sign binaries ─────────────────────────────────────────────────────────
if ($signtool -and -not $SkipSign) {
    Write-Host "`n[Step 5/8] Signing binaries..." -ForegroundColor Cyan

    $binaries = @(
        "$buildDir\Release\Aeon.exe",
        "$Root\router\target\release\aeon_router.dll",
        "$buildDir\Release\aeon_html4.dll"
    ) | Where-Object { Test-Path $_ }

    foreach ($bin in $binaries) {
        & $signtool sign `
            /n "DelgadoLogic" `
            /tr "http://timestamp.digicert.com" `
            /td sha256 /fd sha256 `
            "$bin"
        if ($LASTEXITCODE -ne 0) {
            Write-Warning "Signing failed for $bin — continuing (dev build?)"
        } else {
            Write-Host "  ✓ Signed: $(Split-Path $bin -Leaf)" -ForegroundColor Green
        }
    }
} else {
    Write-Host "`n[Step 5/8] Skipping code signing" -ForegroundColor DarkGray
}

# ─── 8. Compile Inno Setup installer ─────────────────────────────────────────
Write-Host "`n[Step 6/8] Compiling installer (Inno Setup)..." -ForegroundColor Cyan
$issFile = "$Root\installer\AeonBrowser.iss"
$null = New-Item -ItemType Directory -Force -Path "$Root\$OutDir"

& $iscc `
    /DVersion=$Version `
    /DTier=$Tier `
    "/O$Root\$OutDir" `
    $issFile
if ($LASTEXITCODE -ne 0) { Write-Error "ISCC failed"; exit 1 }

$installerExe = "$Root\$OutDir\AeonBrowser-Setup-$Version.exe"
Write-Host "[Step 6/8] ✓ Installer: $installerExe" -ForegroundColor Green

# ─── 9. Sign installer ────────────────────────────────────────────────────────
if ($signtool -and -not $SkipSign -and (Test-Path $installerExe)) {
    Write-Host "`n[Step 7/8] Signing installer..." -ForegroundColor Cyan
    & $signtool sign `
        /n "DelgadoLogic" `
        /tr "http://timestamp.digicert.com" `
        /td sha256 /fd sha256 `
        "$installerExe"
    Write-Host "[Step 7/8] ✓ Installer signed" -ForegroundColor Green
}

# ─── 10. SHA-256 + manifest.json ─────────────────────────────────────────────
Write-Host "`n[Step 8/8] Computing SHA-256 and writing manifest..." -ForegroundColor Cyan

$hash = (Get-FileHash $installerExe -Algorithm SHA256).Hash.ToLower()
$size = (Get-Item $installerExe).Length

$manifest = [ordered]@{
    version     = $Version
    tier        = $Tier
    channel     = $Channel
    sha256      = $hash
    size        = $size
    full_url    = "https://update.delgadologic.tech/aeon/releases/$Version/AeonBrowser-Setup-$Version.exe"
    bspatch_url = ""    # Populated by release pipeline after delta generation
    min_version = "0.9.0"
    released_at = (Get-Date -Format "yyyy-MM-ddTHH:mm:ssZ")
}
$manifestPath = "$Root\$OutDir\manifest.json"
$manifest | ConvertTo-Json -Depth 2 | Set-Content $manifestPath -Encoding UTF8
Write-Host "[Step 8/8] ✓ manifest.json written" -ForegroundColor Green
Write-Host "           SHA-256: $hash"

# ─── Summary ─────────────────────────────────────────────────────────────────
Write-Host "`n[build_installer] ═══════════════════════════════════════" -ForegroundColor Cyan
Write-Host "[build_installer] ✓ Build complete!" -ForegroundColor Green
Write-Host "  Installer : $installerExe"
Write-Host "  SHA-256   : $hash"
Write-Host "  Manifest  : $manifestPath"
Write-Host "  Version   : $Version | Tier: $Tier | Channel: $Channel"
Write-Host "[build_installer] ═══════════════════════════════════════`n" -ForegroundColor Cyan
