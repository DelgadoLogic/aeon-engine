# AeonBrowser - cloud_build_pro.ps1
# DelgadoLogic | Build Automation
#
# Provisions a GCP Windows VM, installs VS Build Tools + Rust + CMake,
# uploads source, builds Aeon.exe + aeon_blink.dll, runs compile-time
# validation, and downloads binaries.
#
# USAGE:
#   .\cloud_build_pro.ps1
#
# REQUIREMENTS: gcloud CLI authenticated with compute.admin role

param(
    [string]$ProjectId    = "aeon-browser-build",
    [string]$Zone         = "us-east1-d",
    [string]$MachineType  = "n2-standard-8",
    [string]$InstanceName = "aeon-pro-builder",
    [string]$Config       = "Release"
)

$ErrorActionPreference = "Continue"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$ArtifactBucket = "aeon-sovereign-artifacts"

# --- Helpers ---------------------------------------------------------
function Step($n, $msg) { Write-Host "`n  [$n] $msg" -ForegroundColor Cyan }
function OK($msg)       { Write-Host "    OK  $msg" -ForegroundColor Green }
function WARN($msg)     { Write-Host "    !!  $msg" -ForegroundColor Yellow }
function FAIL($msg)     { Write-Host "    X   $msg" -ForegroundColor Red; exit 1 }

Write-Host ""
Write-Host "  Aeon Browser - Pro Cloud Build v1.0" -ForegroundColor Magenta
Write-Host "  $(Get-Date -Format 'yyyy-MM-dd HH:mm') EST" -ForegroundColor Magenta
Write-Host ""

# --- Step 1: Delete stale VM if exists -------------------------------
Step "1/8" "Cleaning up stale VMs..."
$existing = gcloud compute instances list --project=$ProjectId --filter="name=$InstanceName" --format="value(name)" 2>$null
if ($existing) {
    Write-Host "    Deleting stale VM: $InstanceName" -ForegroundColor Yellow
    gcloud compute instances delete $InstanceName --project=$ProjectId --zone=$Zone --quiet 2>$null
    Start-Sleep 5
}
OK "Clean"

# --- Step 2: Upload source to GCS -----------------------------------
Step "2/8" "Uploading source tree to GCS..."

# Create a zip of the source tree (exclude build artifacts)
$zipPath = Join-Path $env:TEMP "aeon_pro_source.zip"
if (Test-Path $zipPath) { Remove-Item $zipPath -Force }

# Use tar instead of Compress-Archive for speed
Push-Location $Root
$excludes = @(
    "build", "out", ".git", "node_modules", "target",
    "packages", "dist_beta", "*.zip", "*.exe", "*.dll", "*.obj", "*.pdb"
)
$excludeArgs = ($excludes | ForEach-Object { "--exclude=$_" }) -join " "

# Zip the source
Write-Host "    Compressing source tree..."
Compress-Archive -Path @(
    "AeonMain.cpp",
    "CMakeLists.txt",
    "build_beta.ps1",
    "core",
    "engines",
    "router",
    "privacy",
    "telemetry",
    "ai",
    "hive",
    "updater",
    "resources",
    "installer"
) -DestinationPath $zipPath -Force -CompressionLevel Fastest
Pop-Location

# Upload zip
gsutil cp $zipPath "gs://$ArtifactBucket/pro_build/source.zip"
if ($LASTEXITCODE -ne 0) { FAIL "Failed to upload source zip" }
OK "Source uploaded to gs://$ArtifactBucket/pro_build/source.zip"

# --- Step 3: Create startup script ----------------------------------
Step "3/8" "Building VM startup script..."

$startupScript = @'
# Aeon Pro Build - Windows VM Startup Script
$ErrorActionPreference = "Continue"
$LogFile = "C:\aeon_build.log"
$BuildRoot = "C:\aeon"
$BucketName = "BUCKET_PLACEHOLDER"

function Log($msg) {
    $ts = Get-Date -Format "HH:mm:ss"
    $line = "[$ts] $msg"
    Write-Host $line
    Add-Content -Path $LogFile -Value $line -Encoding UTF8
}

function UploadLog {
    gsutil cp $LogFile "gs://$BucketName/pro_build/build.log" 2>$null
}

try {

Log "=== Aeon Pro Build Starting ==="
"BUILDING" | Out-File "C:\aeon_build_status.txt"

# Phase 1: Install VS Build Tools 2022
Log "PHASE 1: Installing VS Build Tools 2022..."
$vsUrl = "https://aka.ms/vs/17/release/vs_buildtools.exe"
$vsInstaller = "C:\vs_buildtools.exe"
curl.exe -L -o $vsInstaller $vsUrl --retry 3 --connect-timeout 60 2>$null

$vsArgs = @(
    "--quiet", "--wait", "--norestart",
    "--add", "Microsoft.VisualStudio.Workload.VCTools",
    "--add", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
    "--add", "Microsoft.VisualStudio.Component.Windows11SDK.22621",
    "--add", "Microsoft.VisualStudio.Component.VC.ATL",
    "--includeRecommended"
)
Start-Process -FilePath $vsInstaller -ArgumentList $vsArgs -Wait -NoNewWindow
Log "  VS Build Tools installed."

# Phase 1b: Install CMake
Log "PHASE 1b: Installing CMake..."
$cmakeUrl = "https://github.com/Kitware/CMake/releases/download/v3.31.6/cmake-3.31.6-windows-x86_64.msi"
$cmakeMsi = "C:\cmake.msi"
curl.exe -L -o $cmakeMsi $cmakeUrl --retry 3 2>$null
Start-Process msiexec -ArgumentList "/i `"$cmakeMsi`" /qn ADD_CMAKE_TO_PATH=System" -Wait -NoNewWindow
Start-Sleep 5
$env:PATH = "C:\Program Files\CMake\bin;$env:PATH"
Log "  CMake installed."

# Phase 1c: Install Rust
Log "PHASE 1c: Installing Rust..."
$rustupInit = "C:\rustup-init.exe"
curl.exe -L -o $rustupInit "https://win.rustup.rs/x86_64" --retry 3 2>$null
Start-Process $rustupInit -ArgumentList "-y --default-toolchain stable" -Wait -NoNewWindow
$env:PATH = "$env:USERPROFILE\.cargo\bin;$env:PATH"
Log "  Rust installed."

# Phase 1d: Install NuGet (for WebView2 SDK)
Log "PHASE 1d: Installing NuGet..."
$nugetUrl = "https://dist.nuget.org/win-x86-commandline/latest/nuget.exe"
curl.exe -L -o "C:\nuget.exe" $nugetUrl --retry 3 2>$null
Log "  NuGet installed."

UploadLog

# Phase 2: Download and extract source
Log "PHASE 2: Downloading source..."
New-Item -ItemType Directory -Path $BuildRoot -Force | Out-Null
gsutil cp "gs://$BucketName/pro_build/source.zip" "C:\source.zip"
Expand-Archive -Path "C:\source.zip" -DestinationPath $BuildRoot -Force
Log "  Source extracted to $BuildRoot"

# Phase 2b: Install WebView2 SDK via NuGet
Log "PHASE 2b: Installing WebView2 SDK..."
$blinkDir = Join-Path $BuildRoot "engines\blink"
$pkgDir = Join-Path $blinkDir "packages"
New-Item -ItemType Directory -Path $pkgDir -Force | Out-Null

Push-Location $pkgDir
C:\nuget.exe install Microsoft.Web.WebView2 -ExcludeVersion -OutputDirectory . 2>&1 | Out-Null
C:\nuget.exe install Microsoft.Windows.ImplementationLibrary -ExcludeVersion -OutputDirectory . 2>&1 | Out-Null
Pop-Location

if (Test-Path "$pkgDir\Microsoft.Web.WebView2\build\native\include\WebView2.h") {
    Log "  WebView2 SDK installed."
} else {
    Log "  WARNING: WebView2 SDK may not be fully installed."
}

UploadLog

# Phase 3: Load MSVC environment
Log "PHASE 3: Loading MSVC environment..."
$vcvars = Get-ChildItem "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" -ErrorAction SilentlyContinue
if (-not $vcvars) {
    $vcvars = Get-ChildItem "C:\Program Files (x86)\Microsoft Visual Studio" -Filter "vcvars64.bat" -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
}

if ($vcvars) {
    $vcvarsPath = if ($vcvars -is [System.IO.FileInfo]) { $vcvars.FullName } else { $vcvars.ToString() }
    cmd /c "`"$vcvarsPath`" && set" | Where-Object { $_ -match "=" } | ForEach-Object {
        $parts = $_.Split("=", 2)
        [System.Environment]::SetEnvironmentVariable($parts[0], $parts[1], "Process")
    }
    Log "  MSVC environment loaded."
} else {
    Log "FATAL: vcvars64.bat not found"
    "FAILED_MSVC" | Out-File "C:\aeon_build_status.txt"
    UploadLog
    exit 1
}

# Verify cl.exe
$clExe = Get-Command cl.exe -ErrorAction SilentlyContinue
if ($clExe) {
    Log "  cl.exe: $($clExe.Source)"
} else {
    Log "FATAL: cl.exe not in PATH after vcvars"
    "FAILED_MSVC" | Out-File "C:\aeon_build_status.txt"
    UploadLog
    exit 1
}

UploadLog

# Phase 4: Build Aeon.exe (root CMake)
Log "PHASE 4: Building Aeon.exe..."
$aeonBuild = Join-Path $BuildRoot "build"
# Remove stale CMakeCache from source zip (local machine paths don't match VM)
if (Test-Path "$aeonBuild\CMakeCache.txt") { Remove-Item "$aeonBuild\CMakeCache.txt" -Force }
if (Test-Path "$aeonBuild\CMakeFiles") { Remove-Item "$aeonBuild\CMakeFiles" -Recurse -Force }
New-Item -ItemType Directory -Path $aeonBuild -Force | Out-Null

# Need cmake in PATH
$cmakeBin = "C:\Program Files\CMake\bin\cmake.exe"
if (-not (Test-Path $cmakeBin)) {
    # Try PATH
    $cmakeBin = (Get-Command cmake -ErrorAction SilentlyContinue).Source
}

# CMake configure
Log "  Configuring CMake (Pro tier)..."
$cmakeConfigLog = "C:\aeon_cmake_config.log"
& $cmakeBin -S $BuildRoot -B $aeonBuild `
    -G "Visual Studio 17 2022" -A x64 `
    -DCMAKE_BUILD_TYPE=Release `
    -DAEON_TARGET_TIER=Pro `
    -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded 2>&1 | Tee-Object -FilePath $cmakeConfigLog
$cmakeConfigOut = Get-Content $cmakeConfigLog

$cmakeConfigStr = $cmakeConfigOut -join "`n"
Log "  CMake configure output (last 20 lines):"
$cmakeConfigOut | Select-Object -Last 20 | ForEach-Object { Log "    $_" }

if ($LASTEXITCODE -ne 0) {
    Log "FATAL: CMake configure failed"
    Log $cmakeConfigStr
    "FAILED_CMAKE_CONFIG" | Out-File "C:\aeon_build_status.txt"
    UploadLog
    exit 1
}

# Pre-build: Check Rust router import lib
Log "  Pre-link diagnostics..."
$routerImportLib = "$BuildRoot\router\target\release\aeon_router.dll.lib"
if (Test-Path $routerImportLib) {
    Log "    aeon_router.dll.lib: EXISTS ($((Get-Item $routerImportLib).Length) bytes)"
} else {
    Log "    aeon_router.dll.lib: MISSING — checking if cargo ran..."
    $cargoExists = Get-Command cargo -ErrorAction SilentlyContinue
    if ($cargoExists) {
        Log "    Cargo found: $($cargoExists.Source)"
        Log "    Attempting manual cargo build..."
        Push-Location "$BuildRoot\router"
        cargo build --release 2>&1 | ForEach-Object { Log "    [cargo] $_" }
        Pop-Location
        if (Test-Path $routerImportLib) {
            Log "    aeon_router.dll.lib: NOW EXISTS"
        } else {
            Log "    aeon_router.dll.lib: STILL MISSING after manual cargo build"
            # List what IS in target/release
            $releaseDir = "$BuildRoot\router\target\release"
            if (Test-Path $releaseDir) {
                Log "    Contents of target/release:"
                Get-ChildItem $releaseDir -File | ForEach-Object { $n = $_.Name; $s = $_.Length; Log "      $n - $s bytes" }
            } else {
                Log "    target/release directory does not exist"
            }
        }
    } else {
        Log "    CARGO NOT FOUND IN PATH"
    }
}

# CMake build — capture FULL output to file for download
Log "  Building Aeon.exe..."
$fullBuildLog = "C:\aeon_msbuild_full.log"
& $cmakeBin --build $aeonBuild --config Release --parallel 6 2>&1 | Tee-Object -FilePath $fullBuildLog
$buildExitCode = $LASTEXITCODE
$cmakeBuildOut = Get-Content $fullBuildLog

# ALWAYS upload full MSBuild output BEFORE checking exit code
gsutil cp $fullBuildLog "gs://$BucketName/pro_build/msbuild_full.log" 2>$null

$cmakeBuildStr = $cmakeBuildOut -join "`n"

# Log ALL errors found in build output
$errorLines = $cmakeBuildOut | Where-Object { $_ -match "error |LINK :|LNK\d|fatal error|cannot open" }
if ($errorLines) {
    Log "  === BUILD ERRORS FOUND ==="
    $errorLines | ForEach-Object { Log "    ERROR: $_" }
    Log "  === END ERRORS ==="
} else {
    Log "  No explicit errors found in build output"
}

Log "  CMake build output (last 50 lines):"
$cmakeBuildOut | Select-Object -Last 50 | ForEach-Object { Log "    $_" }

UploadLog

if ($buildExitCode -ne 0) {
    Log "FATAL: Aeon.exe build failed (exit code: $buildExitCode)"
    "FAILED_AEON_BUILD" | Out-File "C:\aeon_build_status.txt"
    gsutil cp "C:\aeon_build_status.txt" "gs://$BucketName/pro_build/status.txt" 2>$null
    UploadLog
    exit 1
}

$aeonExe = Get-ChildItem "$aeonBuild\Release\Aeon.exe" -ErrorAction SilentlyContinue
if ($aeonExe) {
    Log "  SUCCESS: Aeon.exe ($($aeonExe.Length) bytes)"
} else {
    Log "  WARNING: Aeon.exe not found in expected location"
    # Try finding it
    $aeonExe = Get-ChildItem $aeonBuild -Filter "Aeon.exe" -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($aeonExe) { Log "  Found at: $($aeonExe.FullName) ($($aeonExe.Length) bytes)" }
}

UploadLog

# Phase 5: Build aeon_blink.dll (engine DLL, separate CMake)
Log "PHASE 5: Building aeon_blink.dll..."
$blinkBuild = Join-Path $blinkDir "build"
# Remove stale CMakeCache from source zip (local machine paths don't match VM)
if (Test-Path "$blinkBuild\CMakeCache.txt") { Remove-Item "$blinkBuild\CMakeCache.txt" -Force }
if (Test-Path "$blinkBuild\CMakeFiles") { Remove-Item "$blinkBuild\CMakeFiles" -Recurse -Force }
New-Item -ItemType Directory -Path $blinkBuild -Force | Out-Null

Log "  Configuring blink engine CMake..."
$blinkConfigLog = "C:\aeon_blink_config.log"
& $cmakeBin -S $blinkDir -B $blinkBuild `
    -G "Visual Studio 17 2022" -A x64 `
    -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded 2>&1 | Tee-Object -FilePath $blinkConfigLog
$blinkConfigOut = Get-Content $blinkConfigLog

$blinkConfigStr = $blinkConfigOut -join "`n"
Log "  Blink CMake configure (last 15 lines):"
$blinkConfigOut | Select-Object -Last 15 | ForEach-Object { Log "    $_" }

if ($LASTEXITCODE -ne 0) {
    Log "FATAL: Blink CMake configure failed"
    "FAILED_BLINK_CONFIG" | Out-File "C:\aeon_build_status.txt"
    UploadLog
    exit 1
}

Log "  Building aeon_blink.dll..."
$fullBlinkLog = "C:\aeon_blink_msbuild_full.log"
& $cmakeBin --build $blinkBuild --config Release --parallel 6 2>&1 | Tee-Object -FilePath $fullBlinkLog
$blinkBuildOut = Get-Content $fullBlinkLog

# Upload full blink build log
gsutil cp $fullBlinkLog "gs://$BucketName/pro_build/msbuild_blink_full.log" 2>$null

$blinkBuildStr = $blinkBuildOut -join "`n"

# Extract errors
$blinkErrors = $blinkBuildOut | Where-Object { $_ -match "error |LINK :|LNK\d|fatal error|cannot open" }
if ($blinkErrors) {
    Log "  === BLINK BUILD ERRORS ==="
    $blinkErrors | ForEach-Object { Log "    ERROR: $_" }
    Log "  === END BLINK ERRORS ==="
}

Log "  Blink build output (last 50 lines):"
$blinkBuildOut | Select-Object -Last 50 | ForEach-Object { Log "    $_" }

if ($LASTEXITCODE -ne 0) {
    Log "FATAL: aeon_blink.dll build failed (exit code: $LASTEXITCODE)"
    "FAILED_BLINK_BUILD" | Out-File "C:\aeon_build_status.txt"
    UploadLog
    exit 1
}

$blinkDll = Get-ChildItem $blinkBuild -Filter "aeon_blink.dll" -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
if ($blinkDll) {
    Log "  SUCCESS: aeon_blink.dll ($($blinkDll.Length) bytes)"
} else {
    Log "  WARNING: aeon_blink.dll not found"
}

UploadLog

# Phase 6: Validation suite
Log "PHASE 6: Validation..."
$errors = @()

# Test 1: Aeon.exe exists and is > 100KB
if (-not $aeonExe -or $aeonExe.Length -lt 100000) {
    $errors += "Aeon.exe missing or too small ($($aeonExe.Length) bytes)"
}

# Test 2: aeon_blink.dll exists and is > 50KB
if (-not $blinkDll -or $blinkDll.Length -lt 50000) {
    $errors += "aeon_blink.dll missing or too small"
}

# Test 3: Verify DLL exports (dumpbin)
if ($blinkDll) {
    $dumpbin = Get-ChildItem "C:\Program Files (x86)\Microsoft Visual Studio" -Filter "dumpbin.exe" -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($dumpbin) {
        $exports = & $dumpbin.FullName /EXPORTS $blinkDll.FullName 2>&1
        $exportStr = $exports -join "`n"
        Log "  DLL Exports:"
        $exports | Select-String "AeonEngine" | ForEach-Object { Log "    $_" }

        # Check for required ABI functions
        $requiredExports = @("AeonEngine_Create")
        foreach ($req in $requiredExports) {
            if ($exportStr -notmatch $req) {
                $errors += "Missing DLL export: $req"
            }
        }
    } else {
        Log "  WARNING: dumpbin not found, skipping export verification"
    }
}

# Test 4: Check for unresolved symbols in build output
$allBuildOutput = "$cmakeBuildStr`n$blinkBuildStr"
if ($allBuildOutput -match "error LNK2019|error LNK2001|unresolved external") {
    $errors += "Unresolved external symbols detected in build"
}

# Test 5: Check no compiler errors
if ($allBuildOutput -match "fatal error C") {
    $errors += "Fatal compiler errors detected"
}

# Test 6: Verify aeon_router.dll from Rust
$routerDll = Get-ChildItem "$BuildRoot\router\target\release\aeon_router.dll" -ErrorAction SilentlyContinue
if ($routerDll) {
    Log "  aeon_router.dll: $($routerDll.Length) bytes"
} else {
    Log "  WARNING: aeon_router.dll not built (Rust may not have fully installed)"
}

# Test 7: Verify HTML resources copied
$newtabHtml = Get-ChildItem "$aeonBuild\Release\newtab\newtab.html" -ErrorAction SilentlyContinue
$settingsHtml = Get-ChildItem "$aeonBuild\Release\pages\settings.html" -ErrorAction SilentlyContinue
if (-not $newtabHtml) { $errors += "newtab.html not copied to build output" }
if (-not $settingsHtml) { $errors += "settings.html not copied to build output" }

# Test 8: Check PE headers (architecture x64)
if ($aeonExe) {
    $bytes = [System.IO.File]::ReadAllBytes($aeonExe.FullName)
    $peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
    $machine = [BitConverter]::ToUInt16($bytes, $peOffset + 4)
    if ($machine -eq 0x8664) {
        Log "  Aeon.exe: x64 PE confirmed"
    } else {
        $errors += "Aeon.exe is not x64 (machine: 0x$($machine.ToString('X4')))"
    }
}

if ($blinkDll) {
    $bytes = [System.IO.File]::ReadAllBytes($blinkDll.FullName)
    $peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
    $machine = [BitConverter]::ToUInt16($bytes, $peOffset + 4)
    if ($machine -eq 0x8664) {
        Log "  aeon_blink.dll: x64 PE confirmed"
    } else {
        $errors += "aeon_blink.dll is not x64 (machine: 0x$($machine.ToString('X4')))"
    }
}

Log ""
if ($errors.Count -eq 0) {
    Log "=== ALL VALIDATION PASSED ==="
    Log "  Aeon.exe:       $($aeonExe.Length) bytes"
    Log "  aeon_blink.dll: $($blinkDll.Length) bytes"
} else {
    Log "=== VALIDATION FAILED ==="
    foreach ($e in $errors) { Log "  ERROR: $e" }
}

UploadLog

# Phase 7: Upload artifacts
Log "PHASE 7: Uploading artifacts..."
$outDir = "C:\aeon_output"
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

if ($aeonExe) { Copy-Item $aeonExe.FullName "$outDir\Aeon.exe" -Force }
if ($blinkDll) { Copy-Item $blinkDll.FullName "$outDir\aeon_blink.dll" -Force }
if ($routerDll) { Copy-Item $routerDll.FullName "$outDir\aeon_router.dll" -Force }

# Copy HTML resources
if (Test-Path "$aeonBuild\Release\newtab") {
    Copy-Item "$aeonBuild\Release\newtab" "$outDir\newtab" -Recurse -Force
}
if (Test-Path "$aeonBuild\Release\pages") {
    Copy-Item "$aeonBuild\Release\pages" "$outDir\pages" -Recurse -Force
}

# Upload all to GCS
gsutil -m cp -r "$outDir\*" "gs://$BucketName/pro_build/out/"

Log "  Artifacts uploaded."
UploadLog

# Phase 8: Signal status
if ($errors.Count -eq 0) {
    "BUILD_COMPLETE" | Out-File "C:\aeon_build_status.txt"
    gsutil cp "C:\aeon_build_status.txt" "gs://$BucketName/pro_build/status.txt"
    Log "=== BUILD AND VALIDATION COMPLETE ==="
} else {
    "VALIDATION_FAILED" | Out-File "C:\aeon_build_status.txt"
    gsutil cp "C:\aeon_build_status.txt" "gs://$BucketName/pro_build/status.txt"
    Log "=== BUILD SUCCEEDED BUT VALIDATION FAILED ==="
}

UploadLog

} catch {
    Log "UNHANDLED ERROR: $_"
    Log $_.ScriptStackTrace
    "FAILED_UNHANDLED" | Out-File "C:\aeon_build_status.txt"
    gsutil cp "C:\aeon_build_status.txt" "gs://$BucketName/pro_build/status.txt"
    UploadLog
}
'@

# Replace bucket placeholder
$startupScript = $startupScript -replace 'BUCKET_PLACEHOLDER', $ArtifactBucket

$startupFile = Join-Path $env:TEMP "aeon_pro_startup.ps1"
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($startupFile, $startupScript, $utf8NoBom)
OK "Startup script ready"

# --- Step 4: Create the VM -------------------------------------------
Step "4/8" "Creating build VM: $InstanceName ($MachineType)"

gcloud compute instances create $InstanceName `
    --project=$ProjectId `
    --zone=$Zone `
    --machine-type=$MachineType `
    --image-family=windows-2022 `
    --image-project=windows-cloud `
    --boot-disk-size=200GB `
    --boot-disk-type=pd-ssd `
    --metadata-from-file="windows-startup-script-ps1=$startupFile" `
    --scopes=storage-full `
    --provisioning-model=SPOT `
    --instance-termination-action=STOP `
    --no-restart-on-failure

if ($LASTEXITCODE -ne 0) {
    FAIL "Failed to create VM"
}
OK "VM created"

# --- Step 5: Poll for completion -------------------------------------
Step "5/8" "Waiting for build (estimated: 15-25 minutes)..."
$maxWait = 3000  # 50 min max
$elapsed = 0
$pollInterval = 30

while ($elapsed -lt $maxWait) {
    Start-Sleep -Seconds $pollInterval
    $elapsed += $pollInterval

    $status = gsutil cat "gs://$ArtifactBucket/pro_build/status.txt" 2>$null
    if ($status) {
        $statusClean = $status.Trim()
        if ($statusClean -eq "BUILD_COMPLETE") {
            Write-Host "    Build SUCCEEDED in $([math]::Round($elapsed/60,1)) minutes!" -ForegroundColor Green
            break
        }
        if ($statusClean -match "FAILED") {
            Write-Host "    Build FAILED: $statusClean" -ForegroundColor Red
            break
        }
    }

    $minutes = [math]::Round($elapsed / 60, 1)
    Write-Host "    Waiting... ($minutes min)" -ForegroundColor DarkGray
}

if ($elapsed -ge $maxWait) {
    WARN "Build timed out after $([math]::Round($maxWait/60)) minutes"
}

# --- Step 6: Download build log --------------------------------------
Step "6/8" "Downloading build log..."
$logDir = "$Root\cloud_build_output"
New-Item -ItemType Directory -Path $logDir -Force | Out-Null
gsutil cp "gs://$ArtifactBucket/pro_build/build.log" "$logDir\build.log" 2>$null
if (Test-Path "$logDir\build.log") {
    Write-Host ""
    Write-Host "--- BUILD LOG (last 50 lines) ---" -ForegroundColor Yellow
    Get-Content "$logDir\build.log" | Select-Object -Last 50
    Write-Host "-----------------------------------" -ForegroundColor Yellow
}

# --- Step 7: Download artifacts --------------------------------------
Step "7/8" "Downloading build artifacts..."
gsutil -m cp -r "gs://$ArtifactBucket/pro_build/out/*" "$logDir\" 2>$null

Write-Host ""
Get-ChildItem $logDir -Recurse -File | ForEach-Object {
    $sizeKB = [math]::Round($_.Length / 1024, 1)
    Write-Host "    $($_.Name) - $sizeKB KB" -ForegroundColor White
}

# --- Step 8: Cleanup VM ---------------------------------------------
Step "8/8" "Deleting builder VM..."
gcloud compute instances delete $InstanceName --project=$ProjectId --zone=$Zone --quiet 2>$null
OK "VM deleted"

# Clear status file for next run
gsutil rm "gs://$ArtifactBucket/pro_build/status.txt" 2>$null

# --- Done ------------------------------------------------------------
Write-Host ""
Write-Host "  PRO BUILD COMPLETE" -ForegroundColor Green
Write-Host "  Artifacts: $logDir" -ForegroundColor Green
Write-Host ""
