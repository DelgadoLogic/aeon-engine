# Aeon Retro Engine — Local Win98 SE QEMU Launcher
# DelgadoLogic — Authentic Win9x testing environment
#
# Usage:
#   First time:  .\tools\launch_win98.ps1 -Install
#   Normal boot: .\tools\launch_win98.ps1
#   Headless:    .\tools\launch_win98.ps1 -Headless
#
# Prerequisites:
#   - QEMU installed
#   - Win98 SE ISO in tools/win98se.iso

param(
    [switch]$Install,       # First-time install mode
    [switch]$Headless,      # No GUI window
    [int]$RAM = 256,        # Win98 likes 256MB max
    [int]$DiskMB = 2048     # 2GB disk image
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$retroDir = Split-Path -Parent $scriptDir

# QEMU path
$env:PATH = "C:\Program Files\qemu;$env:PATH"

Write-Host "=== Aeon Retro Engine — Win98 SE QEMU Launcher ===" -ForegroundColor Cyan
Write-Host ""

# Verify QEMU
try {
    $null = qemu-system-i386 --version
} catch {
    Write-Host "ERROR: QEMU not found!" -ForegroundColor Red
    exit 1
}

$isoPath = Join-Path $scriptDir "win98se.iso"
$diskPath = Join-Path $scriptDir "win98_hdd.qcow2"

# Check for Win98 ISO
if (-not (Test-Path $isoPath)) {
    Write-Host "ERROR: Win98 SE ISO not found!" -ForegroundColor Red
    Write-Host ""
    Write-Host "Download from WinWorldPC:" -ForegroundColor Yellow
    Write-Host "  https://winworldpc.com/product/windows-98/98-second-edition"
    Write-Host ""
    Write-Host "Place the ISO as: tools\win98se.iso"
    exit 1
}

# Create disk image if needed
if ($Install -or -not (Test-Path $diskPath)) {
    Write-Host "Creating ${DiskMB}MB disk image..."
    & qemu-img create -f qcow2 $diskPath "${DiskMB}M"
    Write-Host "Disk created: $diskPath"
    $Install = $true
}

# Payload directory for test binaries
$payloadDir = Join-Path $env:TEMP "aeon_win98_payload"
if (Test-Path $payloadDir) { Remove-Item $payloadDir -Recurse -Force }
New-Item -ItemType Directory -Path $payloadDir | Out-Null

$testExe = Join-Path $retroDir "tls_test.exe"
if (Test-Path $testExe) {
    Copy-Item $testExe $payloadDir
}
$dll = Join-Path $retroDir "aeon_html4.dll"
if (Test-Path $dll) {
    Copy-Item $dll $payloadDir
}

Write-Host ""
Write-Host "--- Win98 Configuration ---" -ForegroundColor Cyan
Write-Host "  RAM:     ${RAM}MB (Win98 max recommended: 512MB)"
Write-Host "  Disk:    $diskPath"
Write-Host "  Mode:    $(if ($Install) { 'INSTALL (CD boot)' } elseif ($Headless) { 'Headless' } else { 'Normal boot' })"
Write-Host "  NIC:     RTL8139 (Win98 has built-in drivers)"
Write-Host "  Sound:   SoundBlaster 16 (sb16)"
Write-Host ""

# Build QEMU command
$qemuArgs = @(
    "-m", "$RAM",
    "-hda", "`"$diskPath`"",
    "-cdrom", "`"$isoPath`"",
    "-net", "nic,model=rtl8139",
    "-net", "user",
    "-soundhw", "sb16",
    "-vga", "cirrus",         # Cirrus VGA — Win98 has built-in drivers
    "-usb"
)

# Mount payload
if (Test-Path $testExe) {
    $qemuArgs += @("-drive", "file=fat:rw:$payloadDir,format=raw,media=disk,index=1")
}

if ($Install) {
    $qemuArgs += @("-boot", "d")  # Boot from CD-ROM
    Write-Host "INSTALL MODE: Boot from CD and install Win98." -ForegroundColor Yellow
    Write-Host "After install, run without -Install flag." -ForegroundColor Yellow
} else {
    $qemuArgs += @("-boot", "c")  # Boot from HDD
}

if ($Headless) {
    $qemuArgs += @("-display", "none", "-serial", "stdio")
} else {
    Write-Host "Opening QEMU window..." -ForegroundColor Green
}

Write-Host ""
Write-Host "Win98 Tips:" -ForegroundColor DarkGray
Write-Host "  - Click inside QEMU window to capture mouse" -ForegroundColor DarkGray
Write-Host "  - Press Ctrl+Alt+G to release mouse" -ForegroundColor DarkGray
Write-Host "  - Test binaries will be on drive D: or E:" -ForegroundColor DarkGray
Write-Host "  - Run: D:\tls_test.exe in command prompt" -ForegroundColor DarkGray
Write-Host ""

& qemu-system-i386 @qemuArgs

# Cleanup
Remove-Item $payloadDir -Recurse -Force -ErrorAction SilentlyContinue
Write-Host "Done." -ForegroundColor Green
