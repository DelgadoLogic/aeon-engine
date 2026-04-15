# Aeon Retro Engine — Local ReactOS QEMU Test Launcher
# DelgadoLogic — Launch ReactOS LiveCD with test payload
#
# Usage: .\tools\launch_reactos.ps1 [-Headless] [-Timeout 180]
#
# Prerequisites:
#   - QEMU installed (winget install SoftwareFreedomConservancy.QEMU)
#   - ReactOS LiveCD ISO in tools/reactos-livecd.iso
#   - tls_test.exe built in retro/

param(
    [switch]$Headless,
    [int]$Timeout = 300,
    [int]$RAM = 512
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$retroDir = Split-Path -Parent $scriptDir

# Add QEMU to PATH if needed
$env:PATH = "C:\Program Files\qemu;$env:PATH"

Write-Host "=== Aeon Retro Engine — ReactOS QEMU Launcher ===" -ForegroundColor Cyan
Write-Host ""

# Verify QEMU
try {
    $ver = qemu-system-i386 --version 2>&1 | Select-Object -First 1
    Write-Host "QEMU: $ver"
} catch {
    Write-Host "ERROR: qemu-system-i386 not found!" -ForegroundColor Red
    Write-Host "Install: winget install SoftwareFreedomConservancy.QEMU"
    exit 1
}

# Verify ReactOS ISO
$isoPath = Join-Path $scriptDir "reactos-livecd.iso"
if (-not (Test-Path $isoPath)) {
    Write-Host "ERROR: ReactOS LiveCD not found at: $isoPath" -ForegroundColor Red
    Write-Host ""
    Write-Host "Download it:" -ForegroundColor Yellow
    Write-Host "  https://sourceforge.net/projects/reactos/files/ReactOS/0.4.14/"
    Write-Host "  Extract the -live.zip file and place the ISO in tools/"
    exit 1
}
$isoSize = [math]::Round((Get-Item $isoPath).Length / 1MB, 1)
Write-Host "ISO:  $isoPath ($isoSize MB)"

# Verify test executable
$testExe = Join-Path $retroDir "tls_test.exe"
if (-not (Test-Path $testExe)) {
    Write-Host "WARNING: tls_test.exe not found. ReactOS will boot but no test will run." -ForegroundColor Yellow
}

# Create payload directory with test binaries
$payloadDir = Join-Path $env:TEMP "aeon_reactos_payload"
if (Test-Path $payloadDir) { Remove-Item $payloadDir -Recurse -Force }
New-Item -ItemType Directory -Path $payloadDir | Out-Null

if (Test-Path $testExe) {
    Copy-Item $testExe $payloadDir
    Write-Host "Test: tls_test.exe copied to payload" -ForegroundColor Green
}

$dllPath = Join-Path $retroDir "aeon_html4.dll"
if (Test-Path $dllPath) {
    Copy-Item $dllPath $payloadDir
    Write-Host "DLL:  aeon_html4.dll copied to payload" -ForegroundColor Green
}

Write-Host ""
Write-Host "--- Launch Configuration ---" -ForegroundColor Cyan
Write-Host "  RAM:      ${RAM}MB"
Write-Host "  Mode:     $(if ($Headless) { 'Headless (serial)' } else { 'GUI (VGA window)' })"
Write-Host "  Network:  User-mode NAT (RTL8139)"
Write-Host "  Payload:  $payloadDir"
Write-Host ""

# Build QEMU arguments
$qemuArgs = @(
    "-m", "$RAM",
    "-boot", "d",
    "-cdrom", "`"$isoPath`"",
    "-net", "nic,model=rtl8139",
    "-net", "user",
    "-usb"
)

# Mount payload as FAT directory (available as a drive in ReactOS)
if (Test-Path $testExe) {
    $qemuArgs += @("-drive", "file=fat:rw:$payloadDir,format=raw,media=disk")
}

if ($Headless) {
    $serialLog = Join-Path $env:TEMP "reactos_serial.log"
    $qemuArgs += @("-serial", "file:$serialLog", "-display", "none", "-no-reboot")
    Write-Host "Serial log: $serialLog" -ForegroundColor DarkGray
} else {
    # GUI mode - open a window
    $qemuArgs += @("-serial", "stdio")
}

Write-Host "--- Starting QEMU ---" -ForegroundColor Green
Write-Host "qemu-system-i386 $($qemuArgs -join ' ')"
Write-Host ""
Write-Host "NOTE: ReactOS LiveCD takes 1-3 minutes to boot." -ForegroundColor Yellow
Write-Host "Once booted, your test binaries are on drive D: or E:" -ForegroundColor Yellow
Write-Host "Run: D:\tls_test.exe" -ForegroundColor Yellow
Write-Host ""

if ($Headless) {
    $process = Start-Process -FilePath "qemu-system-i386" -ArgumentList $qemuArgs -PassThru -WindowStyle Hidden
    Write-Host "QEMU PID: $($process.Id)" -ForegroundColor DarkGray
    Write-Host "Monitoring serial log for ${Timeout}s..."
    
    $elapsed = 0
    $result = "TIMEOUT"
    
    while ($elapsed -lt $Timeout) {
        Start-Sleep -Seconds 10
        $elapsed += 10
        
        if ($process.HasExited) {
            Write-Host "[$elapsed s] QEMU exited (code: $($process.ExitCode))"
            $result = "QEMU_EXIT"
            break
        }
        
        if (Test-Path $serialLog) {
            $logSize = (Get-Item $serialLog).Length
            $content = Get-Content $serialLog -Raw -ErrorAction SilentlyContinue
            
            if ($content -match "AEON_TEST_RESULT=PASS") {
                Write-Host "[$elapsed s] TEST PASSED!" -ForegroundColor Green
                $result = "PASS"
                break
            }
            if ($content -match "AEON_TEST_RESULT=FAIL") {
                Write-Host "[$elapsed s] TEST FAILED" -ForegroundColor Red
                $result = "FAIL"
                break
            }
            
            Write-Host "[$elapsed s] waiting... (log: $logSize bytes)"
        }
    }
    
    if (-not $process.HasExited) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    }
    
    Write-Host ""
    Write-Host "========================================" -ForegroundColor Cyan
    Write-Host " Result: $result (${elapsed}s)" -ForegroundColor $(if ($result -eq "PASS") { "Green" } else { "Yellow" })
    Write-Host "========================================" -ForegroundColor Cyan
    
    if (Test-Path $serialLog) {
        Write-Host ""
        Write-Host "--- Last 20 lines of serial log ---"
        Get-Content $serialLog -Tail 20 -ErrorAction SilentlyContinue
    }
} else {
    # Interactive GUI mode
    & qemu-system-i386 @qemuArgs
}

# Cleanup
Remove-Item $payloadDir -Recurse -Force -ErrorAction SilentlyContinue
