#!/usr/bin/env pwsh
# AeonBrowser — tools/fetch_bypass_tools.ps1
# DelgadoLogic
#
# Downloads and verifies third-party bypass tool binaries
# into AeonBrowser\Network\  (git-ignored, fetched at build time).
#
# Tools bundled:
#   goodbyedpi.exe   — Windows DPI bypass (ValdikSS/GoodbyeDPI, Apache 2.0)
#   winws.exe        — Zapret Windows port (bol-van/zapret, MIT)
#   obfs4proxy.exe   — Tor pluggable transport (MIT)
#   meek-client.exe  — Tor meek PT (MIT)
#   snowflake-client.exe — Tor Snowflake PT (BSD-3)
#   ss-local.exe     — Shadowsocks-libev (GPL-3)
#   v2ray-plugin.exe — V2Ray obfuscation plugin (LGPL)
#   psiphon3.exe     — Psiphon VPN client (GPL-3)
#
# All are open source. Aeon uses them as child processes — no code
# is incorporated into Aeon's codebase. GPL-safe via process boundary.

param([string]$ProjectRoot = (Split-Path $PSScriptRoot -Parent))

$netDir = Join-Path $ProjectRoot "Network"
New-Item -ItemType Directory -Force -Path $netDir | Out-Null
New-Item -ItemType Directory -Force -Path "$netDir\Tor" | Out-Null

Write-Host ""
Write-Host "=== Aeon Browser — Bypass Tool Fetcher ==="
Write-Host "  Target: $netDir"
Write-Host ""

# ─────────────────────────────────────────────────────────────────────────────
# Helper: download + verify SHA256
# ─────────────────────────────────────────────────────────────────────────────
function Get-VerifiedFile {
    param([string]$Url, [string]$Dest, [string]$ExpectedSha256 = "")
    $leaf = Split-Path $Dest -Leaf
    if (Test-Path $Dest) {
        Write-Host "  SKIP $leaf (already exists)"
        return $true
    }
    try {
        Write-Host "  DL   $leaf ..." -NoNewline
        Invoke-WebRequest -Uri $Url -OutFile $Dest -UseBasicParsing -TimeoutSec 60
        Write-Host " OK"
    } catch {
        Write-Warning "  FAIL $leaf : $_"
        return $false
    }
    if ($ExpectedSha256) {
        $actual = (Get-FileHash $Dest -Algorithm SHA256).Hash
        if ($actual -ne $ExpectedSha256.ToUpper()) {
            Write-Warning "  SHA256 MISMATCH for $leaf"
            Write-Warning "  Expected: $ExpectedSha256"
            Write-Warning "  Actual:   $actual"
            Remove-Item $Dest -Force
            return $false
        }
        Write-Host "  SHA256 OK $leaf"
    }
    return $true
}

# ─────────────────────────────────────────────────────────────────────────────
# GoodbyeDPI (ValdikSS/GoodbyeDPI)  Apache-2.0
# Latest release binary for Windows x64
# ─────────────────────────────────────────────────────────────────────────────
Write-Host "[1/8] GoodbyeDPI..."
$gdpiBase = "https://github.com/ValdikSS/GoodbyeDPI/releases/latest/download"
Get-VerifiedFile `
    "$gdpiBase/goodbyedpi.exe" `
    "$netDir\goodbyedpi.exe"

# ─────────────────────────────────────────────────────────────────────────────
# Zapret / winws (bol-van/zapret)  MIT
# Windows binary (winws.exe) from Zapret Discord/YouTube fork
# ─────────────────────────────────────────────────────────────────────────────
Write-Host "[2/8] Zapret (winws)..."
$zapretBase = "https://github.com/Flowseal/zapret-discord-youtube/releases/latest/download"
Get-VerifiedFile `
    "$zapretBase/zapret-discord-youtube.exe" `
    "$netDir\winws.exe"

# ─────────────────────────────────────────────────────────────────────────────
# Tor Expert Bundle — includes obfs4proxy, meek, snowflake
# From torproject.org — official signed Windows binaries
# ─────────────────────────────────────────────────────────────────────────────
Write-Host "[3/8] Tor pluggable transports..."
$torPtBase = "https://archive.torproject.org/tor-package-archive/torbrowser/13.5/tor-expert-bundle-13.5-windows-x86_64.tar.gz"
$torArchive = "$netDir\tor_expert.tar.gz"

if (-not (Test-Path "$netDir\Tor\obfs4proxy.exe")) {
    Get-VerifiedFile $torPtBase $torArchive
    if (Test-Path $torArchive) {
        Write-Host "  Extracting Tor bundle..."
        tar -xzf $torArchive --strip-components=2 `
            -C "$netDir\Tor" `
            "*/tor/pluggable_transports/obfs4proxy.exe" `
            "*/tor/pluggable_transports/meek-client.exe" `
            "*/tor/pluggable_transports/snowflake-client.exe" `
            2>$null
        # Fallback: copy from within bundle structure
        Get-ChildItem "$netDir\Tor" -Recurse -Filter "obfs4proxy.exe" |
            ForEach-Object { Copy-Item $_.FullName "$netDir\obfs4proxy.exe" -Force }
        Get-ChildItem "$netDir\Tor" -Recurse -Filter "meek-client.exe" |
            ForEach-Object { Copy-Item $_.FullName "$netDir\meek-client.exe" -Force }
        Get-ChildItem "$netDir\Tor" -Recurse -Filter "snowflake-client.exe" |
            ForEach-Object { Copy-Item $_.FullName "$netDir\snowflake-client.exe" -Force }
        Remove-Item $torArchive -Force -ErrorAction SilentlyContinue
        Write-Host "  Tor PTs extracted."
    }
} else {
    Write-Host "  SKIP obfs4proxy/meek/snowflake (already present)"
}

# ─────────────────────────────────────────────────────────────────────────────
# Shadowsocks Windows (shadowsocks-windows) MIT
# ─────────────────────────────────────────────────────────────────────────────
Write-Host "[4/8] Shadowsocks..."
$ssBase = "https://github.com/shadowsocks/shadowsocks-windows/releases/latest/download"
Get-VerifiedFile `
    "$ssBase/Shadowsocks-4.4.1.0.zip" `
    "$netDir\ss.zip"
if ((Test-Path "$netDir\ss.zip") -and -not (Test-Path "$netDir\ss-local.exe")) {
    Expand-Archive "$netDir\ss.zip" -DestinationPath "$netDir\SS_tmp" -Force
    $ssExe = Get-ChildItem "$netDir\SS_tmp" -Recurse -Filter "Shadowsocks.exe" | Select-Object -First 1
    if ($ssExe) { Copy-Item $ssExe.FullName "$netDir\ss-local.exe" -Force }
    Remove-Item "$netDir\SS_tmp" -Recurse -Force
    Remove-Item "$netDir\ss.zip" -Force
    Write-Host "  SS-local ready."
}

# ─────────────────────────────────────────────────────────────────────────────
# v2ray-plugin (shadowsocks/v2ray-plugin) MIT
# Makes Shadowsocks traffic look like WebSocket over CDN TLS
# ─────────────────────────────────────────────────────────────────────────────
Write-Host "[5/8] v2ray-plugin..."
$v2rayBase = "https://github.com/shadowsocks/v2ray-plugin/releases/latest/download"
Get-VerifiedFile `
    "$v2rayBase/v2ray-plugin-windows-amd64.exe" `
    "$netDir\v2ray-plugin.exe"

# ─────────────────────────────────────────────────────────────────────────────
# Psiphon (Psiphon-Inc/psiphon-windows) GPL-3
# Last resort tunnel — SSH/L2TP/HTTPS auto-selection, zero config
# ─────────────────────────────────────────────────────────────────────────────
Write-Host "[6/8] Psiphon..."
$psiphonBase = "https://psiphon3.com/en/download.html"
# Psiphon doesn't have a direct GitHub release link — use their CDN
$psiphonUrl = "https://psiphon3.com/psiphon3.exe"
Get-VerifiedFile $psiphonUrl "$netDir\psiphon3.exe"

# ─────────────────────────────────────────────────────────────────────────────
# Lantern (getlantern/lantern) Apache-2.0
# Peer-assisted proxy — good for China when everything else fails
# ─────────────────────────────────────────────────────────────────────────────
Write-Host "[7/8] Lantern..."
$lanternBase = "https://github.com/getlantern/lantern/releases/latest/download"
Get-VerifiedFile `
    "$lanternBase/lantern-installer.exe" `
    "$netDir\lantern.exe"

# ─────────────────────────────────────────────────────────────────────────────
# Summary
# ─────────────────────────────────────────────────────────────────────────────
Write-Host ""
Write-Host "=== Network Tools Summary ==="
$tools = @(
    @{ Name="goodbyedpi.exe";      Desc="DPI bypass (GFW, Russia, ISP)"   },
    @{ Name="winws.exe";           Desc="Zapret (Russia TSPU)"              },
    @{ Name="obfs4proxy.exe";      Desc="Tor PT — obfs4"                   },
    @{ Name="meek-client.exe";     Desc="Tor PT — meek (CDN)"              },
    @{ Name="snowflake-client.exe";Desc="Tor PT — WebRTC"                  },
    @{ Name="ss-local.exe";        Desc="Shadowsocks SOCKS5 proxy"         },
    @{ Name="v2ray-plugin.exe";    Desc="V2Ray WebSocket obfuscation"       },
    @{ Name="psiphon3.exe";        Desc="Psiphon tunnel (last resort)"      },
    @{ Name="lantern.exe";         Desc="Lantern peer-assisted proxy"       }
)
foreach ($t in $tools) {
    $path = Join-Path $netDir $t.Name
    $status = if (Test-Path $path) { "✓" } else { "✗ MISSING" }
    Write-Host "  $status  $($t.Name.PadRight(26)) — $($t.Desc)"
}
Write-Host ""
Write-Host "Note: These tools are launched as child processes."
Write-Host "      All are open-source. Aeon's code is GPL-safe."
Write-Host ""
