# AeonBrowser — E2E Test Suite PowerShell Wrapper
param (
    [int]$Tier = 0,
    [string]$Filter = "",
    [switch]$All,
    [switch]$Quiet
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$runnerPath = Join-Path $scriptDir "run_all_e2e.mjs"

$nodeArgs = @($runnerPath)

if ($Tier -gt 0) {
    $nodeArgs += "--tier=$Tier"
}

if ($Filter) {
    $nodeArgs += "--filter=$Filter"
}

Write-Host "Executing AeonBrowser E2E Test Suite..." -ForegroundColor Cyan

& node @nodeArgs

if ($LASTEXITCODE -ne 0) {
    Write-Host "E2E Test Suite Failed with exit code $LASTEXITCODE" -ForegroundColor Red
    exit $LASTEXITCODE
} else {
    Write-Host "E2E Test Suite Completed Successfully! [EXIT CODE 0]" -ForegroundColor Green
    exit 0
}
