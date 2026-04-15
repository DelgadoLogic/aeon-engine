# AeonBrowser — Beta Build Script
# DelgadoLogic | Build Team
#
# Builds Aeon.exe + aeon_blink_stub.dll + Rust router, then packages AeonSetup.exe
# Run from the AeonBrowser root directory as Administrator (for signing, optional).
#
# Usage: powershell -ExecutionPolicy Bypass -File build_beta.ps1

param(
    [string]$Config = "Release",
    [switch]$SkipRust = $false,
    [switch]$SkipInstaller = $false
)

Set-StrictMode -Off
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path

Write-Host ""
Write-Host "╔══════════════════════════════════════════════╗" -ForegroundColor Magenta
Write-Host "║   Aeon Browser — Beta Build Script v1.0     ║" -ForegroundColor Magenta
Write-Host "║   DelgadoLogic.tech                          ║" -ForegroundColor Magenta
Write-Host "╚══════════════════════════════════════════════╝" -ForegroundColor Magenta
Write-Host ""

# ─── Tool paths ───────────────────────────────────────────────────────────────
$VsBase    = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
$VsVcBase  = "$VsBase\VC"
$MSVC      = (Get-ChildItem "$VsVcBase\Tools\MSVC" -ErrorAction Stop | Sort-Object Name | Select-Object -Last 1).FullName
$Cl        = "$MSVC\bin\Hostx64\x64\cl.exe"
$Link      = "$MSVC\bin\Hostx64\x64\link.exe"

# SDK detection
$WinSDK    = "C:\Program Files (x86)\Windows Kits\10"
$SdkInclude= (Get-ChildItem "$WinSDK\Include" -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1).FullName
$SdkLib    = (Get-ChildItem "$WinSDK\Lib"     -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1).FullName

$Cargo     = "$env:USERPROFILE\.cargo\bin\cargo.exe"
$CMake     = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $CMake) { $CMake = "C:\Program Files\CMake\bin\cmake.exe" }

$ISCC      = "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
$BuildDir  = "$Root\build"
$OutDir    = "$Root\out\$Config"

Write-Host "[Build] MSVC: $MSVC"
Write-Host "[Build] SDK Include: $SdkInclude"
Write-Host "[Build] SDK Lib:     $SdkLib"
Write-Host "[Build] CMake: $CMake"
Write-Host "[Build] Cargo: $Cargo"
Write-Host "[Build] ISCC:  $ISCC"
Write-Host ""

# ─── Step 1: Download EasyList filter (tiny, for content blocker) ─────────────
Write-Host "[1/6] Downloading EasyList filter..." -ForegroundColor Cyan
$FilterDir = "$Root\resources\filters"
New-Item -ItemType Directory -Force -Path $FilterDir | Out-Null
$easylistUrl = "https://easylist.to/easylist/easylist.txt"
try {
    Invoke-WebRequest -Uri $easylistUrl -OutFile "$FilterDir\easylist.txt" -TimeoutSec 30 -UseBasicParsing
    Write-Host "  EasyList downloaded."
} catch {
    Write-Host "  WARNING: Could not download EasyList (no network or timeout). Skipping." -ForegroundColor Yellow
}

# ─── Step 2: Build Rust Protocol Router ───────────────────────────────────────
if (-not $SkipRust) {
    Write-Host ""
    Write-Host "[2/6] Building Rust protocol router..." -ForegroundColor Cyan
    Push-Location "$Root\router"
    try {
        & $Cargo build --release 2>&1 | Tee-Object -Variable cargoOut
        if ($LASTEXITCODE -ne 0) {
            Write-Host "  ERROR: Cargo build failed." -ForegroundColor Red
            Write-Host $cargoOut
            exit 1
        }
        Write-Host "  Rust router built: router\target\release\aeon_router.dll"
    } finally { Pop-Location }
}

# ─── Step 3: CMake configure + build (Aeon.exe + aeon_blink_stub.dll) ─────────
Write-Host ""
Write-Host "[3/6] Configuring CMake build..." -ForegroundColor Cyan
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

# Use the VS 2022 BuildTools generator
$cmakeArgs = @(
    "-S", $Root,
    "-B", $BuildDir,
    "-G", "Visual Studio 17 2022",
    "-A", "x64",
    "-DCMAKE_BUILD_TYPE=$Config",
    "-DAEON_TARGET_TIER=Pro",
    "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded"
)
& $CMake @cmakeArgs
if ($LASTEXITCODE -ne 0) { Write-Host "CMake configure FAILED" -ForegroundColor Red; exit 1 }

Write-Host ""
Write-Host "[4/6] Building Aeon.exe (this may take 2-5 minutes)..." -ForegroundColor Cyan
& $CMake --build $BuildDir --config $Config --parallel 4
if ($LASTEXITCODE -ne 0) { Write-Host "CMake build FAILED" -ForegroundColor Red; exit 1 }

Write-Host "  Build complete."
$AeonExe = Get-ChildItem "$BuildDir\$Config\Aeon.exe" -ErrorAction SilentlyContinue
if ($AeonExe) { Write-Host "  Aeon.exe: $($AeonExe.FullName)" -ForegroundColor Green }

# ─── Step 4: Copy resources ────────────────────────────────────────────────────
Write-Host ""
Write-Host "[5/6] Copying resources..." -ForegroundColor Cyan
$BinDir = "$BuildDir\$Config"
New-Item -ItemType Directory -Force -Path "$BinDir\pages" | Out-Null
New-Item -ItemType Directory -Force -Path "$BinDir\newtab" | Out-Null

Copy-Item "$Root\resources\pages\*.html" "$BinDir\pages\" -Force
Copy-Item "$Root\resources\newtab\newtab.html" "$BinDir\newtab\" -Force

# Copy router DLL if built
$RouterDll = "$Root\router\target\release\aeon_router.dll"
if (Test-Path $RouterDll) { Copy-Item $RouterDll $BinDir -Force }

Write-Host "  Resources copied."

# ─── Step 5: Package installer ─────────────────────────────────────────────────
if (-not $SkipInstaller) {
    Write-Host ""
    Write-Host "[6/6] Building installer (Inno Setup)..." -ForegroundColor Cyan
    if (Test-Path $ISCC) {
        & $ISCC "$Root\installer\AeonBrowser.iss" /Q
        $installer = Get-ChildItem "$Root\installer\Output\AeonSetup*.exe" -ErrorAction SilentlyContinue | Select-Object -Last 1
        if ($installer) {
            Write-Host "  AeonSetup.exe: $($installer.FullName)" -ForegroundColor Green
        } else {
            Write-Host "  WARNING: Installer not found in installer\Output\" -ForegroundColor Yellow
        }
    } else {
        Write-Host "  Inno Setup not found at $ISCC, skipping." -ForegroundColor Yellow
    }
}

# ─── Done ──────────────────────────────────────────────────────────────────────
Write-Host ""
Write-Host "╔══════════════════════════════════════════════╗" -ForegroundColor Green
Write-Host "║  BUILD COMPLETE                              ║" -ForegroundColor Green
Write-Host "║  Aeon.exe is ready in:                       ║" -ForegroundColor Green
Write-Host "║  $($BuildDir)\Release" -ForegroundColor Green
Write-Host "╚══════════════════════════════════════════════╝" -ForegroundColor Green
Write-Host ""
