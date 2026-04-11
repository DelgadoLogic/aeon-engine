# Aeon Chromium Builder - Bootstrap Script
# This tiny script downloads the real build script from GCS to avoid
# GCE metadata script runner mangling PowerShell syntax
$ErrorActionPreference = 'Stop'
$bucket = 'aeon-sovereign-artifacts'
$scriptName = 'chromium_build_vm_startup.ps1'
$localScript = "C:\$scriptName"

# Download the real build script from GCS
gsutil cp "gs://$bucket/$scriptName" $localScript

# Execute it
& $localScript
