# AeonBrowser - build_icon.ps1
# DelgadoLogic
#
# Assembles all icon PNGs into a proper multi-size Windows .ico file.
# Sizes baked in: 16x16, 32x32, 48x48, 64x64, 128x128, 256x256
# Compatible with PowerShell 5.1+ (Windows 7 and above)

param(
    [string]$ProjectRoot = (Split-Path $MyInvocation.MyCommand.Path -Parent | Split-Path -Parent | Split-Path -Parent)
)

$iconsDir  = Join-Path $ProjectRoot "resources\icons"
$outputIco = Join-Path $iconsDir    "Aeon.ico"

# ---------------------------------------------------------------------------
# Resize a PNG to the target size using System.Drawing
# ---------------------------------------------------------------------------
Add-Type -AssemblyName System.Drawing

function Resize-Png {
    param([string]$Source, [int]$Size, [string]$Dest)
    try {
        $src = [System.Drawing.Image]::FromFile($Source)
        $bmp = New-Object System.Drawing.Bitmap($Size, $Size)
        $g   = [System.Drawing.Graphics]::FromImage($bmp)
        $g.InterpolationMode  = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $g.SmoothingMode      = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
        $g.PixelOffsetMode    = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
        $g.DrawImage($src, 0, 0, $Size, $Size)
        $g.Dispose()
        $src.Dispose()
        $bmp.Save($Dest, [System.Drawing.Imaging.ImageFormat]::Png)
        $bmp.Dispose()
        Write-Host "  OK ${Size}x${Size}: $(Split-Path $Dest -Leaf)"
    } catch {
        Write-Warning "  FAILED ${Size}x${Size}: $_"
    }
}

# ---------------------------------------------------------------------------
# STEP 1: Resize source PNGs
# ---------------------------------------------------------------------------
$srcLarge  = Join-Path $iconsDir "Aeon_256_source.png"
$srcMedium = Join-Path $iconsDir "Aeon_48_source.png"

if (-not (Test-Path $srcLarge)) {
    Write-Error "Missing: $srcLarge"
    exit 1
}

# Use medium (simplified) source for small sizes if it exists; else use large
if (Test-Path $srcMedium) {
    $srcSmall = $srcMedium
} else {
    $srcSmall = $srcLarge
}

Write-Host ""
Write-Host "=== Aeon Browser Icon Builder ==="
Write-Host ""

# (size, source) pairs -- small sizes use simplified design, large use detailed
$sizeMap = @(
    @{ Size = 256; Source = $srcLarge  },
    @{ Size = 128; Source = $srcLarge  },
    @{ Size = 64;  Source = $srcLarge  },
    @{ Size = 48;  Source = $srcSmall  },
    @{ Size = 32;  Source = $srcSmall  },
    @{ Size = 16;  Source = $srcSmall  }
)

$pngFiles = @()
foreach ($entry in $sizeMap) {
    $sz   = $entry.Size
    $dest = Join-Path $iconsDir "Aeon_${sz}.png"
    Resize-Png -Source $entry.Source -Size $sz -Dest $dest
    $pngFiles += $dest
}

# ---------------------------------------------------------------------------
# STEP 2: Assemble into .ico
# Try ImageMagick first; fall back to .NET binary ICO writer
# ---------------------------------------------------------------------------
$magick = Get-Command "magick.exe" -ErrorAction SilentlyContinue

if ($magick) {
    Write-Host ""
    Write-Host "Using ImageMagick..."
    & magick $pngFiles $outputIco
    Write-Host "  OK $outputIco"
} else {
    Write-Host ""
    Write-Host "ImageMagick not found -- using .NET ICO writer..."

    Add-Type -AssemblyName System.IO

    $ms = New-Object System.IO.MemoryStream
    $bw = New-Object System.IO.BinaryWriter($ms)

    # Load PNG bytes in size order (largest first for directory)
    $entries = @()
    foreach ($szEntry in $sizeMap) {
        $pf = Join-Path $iconsDir ("Aeon_" + $szEntry.Size + ".png")
        $bytes = [System.IO.File]::ReadAllBytes($pf)
        $entries += @{ Size = $szEntry.Size; Data = $bytes }
    }

    $count = $entries.Count

    # ICO header (6 bytes)
    $bw.Write([uint16]0)       # Reserved = 0
    $bw.Write([uint16]1)       # Type = 1 (ICO)
    $bw.Write([uint16]$count)  # Number of images

    # Directory offset: 6 + 16 per entry
    $dataOffset = 6 + (16 * $count)

    # ICO directory (16 bytes per entry)
    foreach ($entry in $entries) {
        $sz = $entry.Size
        # Width and height: 0 means 256
        if ($sz -eq 256) { $w = 0; $h = 0 } else { $w = $sz; $h = $sz }
        $bw.Write([byte]$w)       # Width
        $bw.Write([byte]$h)       # Height
        $bw.Write([byte]0)        # Color count (0 = true color)
        $bw.Write([byte]0)        # Reserved
        $bw.Write([uint16]1)      # Color planes
        $bw.Write([uint16]32)     # Bits per pixel
        $bw.Write([uint32]$entry.Data.Length) # Data size
        $bw.Write([uint32]$dataOffset)        # Data offset
        $dataOffset += $entry.Data.Length
    }

    # Image data (embedded PNG blobs -- supported since Windows Vista)
    foreach ($entry in $entries) {
        $bw.Write($entry.Data)
    }

    $bw.Flush()
    [System.IO.File]::WriteAllBytes($outputIco, $ms.ToArray())
    $bw.Dispose()
    $ms.Dispose()

    Write-Host "  OK $outputIco (.NET writer)"
}

# ---------------------------------------------------------------------------
# STEP 3: Copy .ico to LogicFlow installer assets
# ---------------------------------------------------------------------------
$assetsDir = Join-Path $ProjectRoot "..\..\LogicFlow\Assets\Icons"
if (Test-Path $assetsDir) {
    Copy-Item $outputIco (Join-Path $assetsDir "Aeon.ico") -Force
    Write-Host "  OK Copied to LogicFlow\Assets\Icons\Aeon.ico"
}

# Commit the updated icon files
Set-Location $ProjectRoot
git add resources\icons\Aeon*.png resources\icons\Aeon.ico 2>$null

Write-Host ""
Write-Host "=== Done! ==="
$icoSize = [math]::Round((Get-Item $outputIco).Length / 1KB, 1)
Write-Host "  Output : $outputIco ($icoSize KB)"
Write-Host "  Sizes  : 16 | 32 | 48 | 64 | 128 | 256"
Write-Host ""
