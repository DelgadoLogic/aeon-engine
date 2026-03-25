# AeonBrowser — download_filterlists.ps1
# DelgadoLogic | Privacy Team
#
# Downloads and caches the bundled content-blocking filter lists.
# Run this before building the installer to ensure fresh lists are baked in.
#
# Sources:
#   - EasyList      https://easylist.to/easylist/easylist.txt
#   - EasyPrivacy   https://easylist.to/easylist/easyprivacy.txt
#   - uBlock Base   https://raw.githubusercontent.com/uBlockOrigin/uAssets/master/filters/filters.txt
#   - uBlock Privacy https://raw.githubusercontent.com/uBlockOrigin/uAssets/master/filters/privacy.txt
#   - Aeon Extra    (locally maintained — already in repo)
#
# The lists are stripped of comment-only lines to reduce size, then saved to
# resources/filters/ for bundling with the installer.
#
# The ContentBlocker.cpp picks these up at: {install_dir}\resources\filters\
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File resources\filters\download_filterlists.ps1

param(
    [string]$OutDir      = "",  # defaults to resources\filters\ in the project root
    [switch]$NoStrip     = $false,  # keep comments if true
    [int]   $MaxAgeHours = 24      # skip download if cached file is newer than this
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Continue"  # Log errors but don't stop for a single list

$Root = (Get-Item $PSScriptRoot).Parent.Parent.FullName
if (-not $OutDir) { $OutDir = "$Root\resources\filters" }
$null = New-Item -ItemType Directory -Force -Path $OutDir

Write-Host "`n[filterlists] Aeon Browser — Filter List Downloader" -ForegroundColor Cyan
Write-Host "[filterlists] Output dir: $OutDir"
Write-Host "[filterlists] Max cache age: ${MaxAgeHours}h`n"

$lists = @(
    @{
        Name     = "easylist"
        FileName = "easylist.txt"
        Url      = "https://easylist.to/easylist/easylist.txt"
        Desc     = "EasyList (ads)"
    },
    @{
        Name     = "easyprivacy"
        FileName = "easyprivacy.txt"
        Url      = "https://easylist.to/easylist/easyprivacy.txt"
        Desc     = "EasyPrivacy (trackers)"
    },
    @{
        Name     = "ublock_base"
        FileName = "ublock_base.txt"
        Url      = "https://raw.githubusercontent.com/uBlockOrigin/uAssets/master/filters/filters.txt"
        Desc     = "uBlock Origin base filters"
    },
    @{
        Name     = "ublock_privacy"
        FileName = "ublock_privacy.txt"
        Url      = "https://raw.githubusercontent.com/uBlockOrigin/uAssets/master/filters/privacy.txt"
        Desc     = "uBlock Origin privacy filters"
    },
    @{
        Name     = "annoyances"
        FileName = "annoyances.txt"
        Url      = "https://easylist.to/easylist/fanboy-annoyance.txt"
        Desc     = "Fanboy Annoyances (cookie banners, popups)"
    }
)

$totalBefore = 0
$totalAfter  = 0
$downloaded  = 0
$cached      = 0

foreach ($list in $lists) {
    $dest = Join-Path $OutDir $list.FileName
    $desc = $list.Desc

    # ── Cache check ───────────────────────────────────────────────────────────
    if (Test-Path $dest) {
        $age = (New-TimeSpan -Start (Get-Item $dest).LastWriteTime -End (Get-Date)).TotalHours
        if ($age -lt $MaxAgeHours) {
            $sz = [math]::Round((Get-Item $dest).Length / 1KB)
            Write-Host "[filterlists] CACHED  $($list.FileName) (${sz} KB, ${age:F1}h old)" -ForegroundColor DarkGray
            $cached++
            continue
        }
    }

    # ── Download ──────────────────────────────────────────────────────────────
    Write-Host "[filterlists] Downloading $desc..." -ForegroundColor Yellow
    try {
        $tempFile = [System.IO.Path]::GetTempFileName()
        $wc = New-Object System.Net.WebClient
        $wc.Headers.Add("User-Agent", "AeonBrowser-FilterUpdater/1.0")
        $wc.DownloadFile($list.Url, $tempFile)

        $rawLines   = [System.IO.File]::ReadAllLines($tempFile)
        $beforeCount = $rawLines.Count

        if (-not $NoStrip) {
            # Strip blank lines and pure-comment lines (! prefix)
            # Keep: rules (lines that start with anything except !)
            #        metadata comments we want: [Adblock Plus ...]
            $stripped = $rawLines | Where-Object {
                $_ -ne "" -and (-not ($_ -match '^\s*!') -or $_ -match '^\[')
            }
        } else {
            $stripped = $rawLines
        }

        [System.IO.File]::WriteAllLines($dest, $stripped)
        Remove-Item $tempFile -ErrorAction SilentlyContinue

        $afterCount  = $stripped.Count
        $savePct     = [math]::Round((1 - $afterCount / [Math]::Max($beforeCount,1)) * 100)
        $sizeKB      = [math]::Round((Get-Item $dest).Length / 1KB)
        $totalBefore += $beforeCount
        $totalAfter  += $afterCount
        $downloaded++

        Write-Host "[filterlists]   ✓ $($list.FileName): $afterCount rules (${sizeKB} KB, reduced ${savePct}%)" -ForegroundColor Green
    } catch {
        Write-Warning "[filterlists]   ✗ Failed: $desc — $_"
    }
}

# ─── Aeon Extra list (always kept as-is from repo) ────────────────────────────
$extraDest = Join-Path $OutDir "aeon_extra.txt"
if (-not (Test-Path $extraDest)) {
    # Create a minimal starter file if it doesn't exist
    @"
! Aeon Browser Extra Block List
! DelgadoLogic | Privacy Team
! Maintained at: resources/filters/aeon_extra.txt
! Format: EasyList/AdBlock Plus syntax
!
! Add your custom rules below. These take highest priority.
! Example:
!   ||ads.example.com^            (block domain)
!   @@||mysite.com^               (whitelist/exception)
!   ##.cookie-banner              (hide element)

! === Aeon-specific additions ===
||telemetry.microsoft.com^
||browser.events.data.msn.com^
||data.microsoft.com^
"@ | Set-Content $extraDest -Encoding UTF8
    Write-Host "[filterlists] Created starter aeon_extra.txt" -ForegroundColor Cyan
}

# ─── Summary ─────────────────────────────────────────────────────────────────
$totalKB = [math]::Round((Get-ChildItem $OutDir -Filter "*.txt" | Measure-Object -Property Length -Sum).Sum / 1KB)

Write-Host "`n[filterlists] ─────────────────────────────────────────"
Write-Host "[filterlists] Downloaded : $downloaded lists"
Write-Host "[filterlists] Cached     : $cached lists"
Write-Host "[filterlists] Total size : ${totalKB} KB"
Write-Host "[filterlists] Ready for bundling in installer`n" -ForegroundColor Green
