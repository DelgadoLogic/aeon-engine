# tests/run_test_omni_license.ps1
# Runs the automated cryptographic verification test harness for OmniLicense & AutoUpdater

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path "$PSScriptRoot\..").Path
Push-Location $Root

try {
    Write-Host "====================================================================" -ForegroundColor Cyan
    Write-Host " Compiling & Running Track 3 OmniLicense & AutoUpdater Crypto Suite " -ForegroundColor Cyan
    Write-Host "====================================================================" -ForegroundColor Cyan

    cmd /c "call `"$Root\run_msvc_cmake.bat`" && cl.exe /nologo /std:c++17 /EHsc /bigobj /utf-8 /O2 /I. /Icore /Icore/session /Fo:tests\ tests\test_omni_license.cpp core\billing\OmniLicense.cpp updater\AutoUpdater.cpp /Fe:tests\test_omni_license.exe /link bcrypt.lib crypt32.lib wbemuuid.lib shell32.lib ole32.lib oleaut32.lib advapi32.lib wininet.lib ws2_32.lib wintrust.lib && tests\test_omni_license.exe"
    $exitCode = $LASTEXITCODE

    Get-ChildItem -Path "$Root\tests\*.obj" -ErrorAction SilentlyContinue | Remove-Item -Force

    if ($exitCode -eq 0) {
        Write-Host "`nAll cryptographic verification tests passed successfully!" -ForegroundColor Green
    } else {
        Write-Host "`nCryptographic verification tests failed with code $exitCode" -ForegroundColor Red
        exit $exitCode
    }
} finally {
    Pop-Location
}
