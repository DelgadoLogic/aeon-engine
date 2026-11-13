# make_transparent_icons.ps1
Add-Type -AssemblyName System.Drawing

$iconsDir = $PSScriptRoot
if (-not $iconsDir) { $iconsDir = (Get-Location).Path }

function Make-Transparent {
    param([string]$FilePath)
    if (-not (Test-Path $FilePath)) { return }
    
    Write-Host "Processing transparency for $FilePath..."
    $img = [System.Drawing.Bitmap]::FromFile($FilePath)
    $bmp = New-Object System.Drawing.Bitmap($img.Width, $img.Height)
    
    for ($y = 0; $y -lt $img.Height; $y++) {
        for ($x = 0; $x -lt $img.Width; $x++) {
            $pixel = $img.GetPixel($x, $y)
            # Check if pixel is white or near-white background
            if ($pixel.R -ge 230 -and $pixel.G -ge 230 -and $pixel.B -ge 230) {
                $bmp.SetPixel($x, $y, [System.Drawing.Color]::FromArgb(0, 0, 0, 0))
            } else {
                $bmp.SetPixel($x, $y, $pixel)
            }
        }
    }
    
    $img.Dispose()
    $tempPath = "$FilePath.tmp.png"
    $bmp.Save($tempPath, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    
    Remove-Item $FilePath -Force
    Move-Item $tempPath $FilePath -Force
    Write-Host "  OK Transparent $FilePath"
}

# Apply transparency to source images
Make-Transparent -FilePath (Join-Path $iconsDir "Aeon_256_source.png")
Make-Transparent -FilePath (Join-Path $iconsDir "Aeon_48_source.png")
Make-Transparent -FilePath (Join-Path $iconsDir "Aeon.png")

# Re-run build_icon.ps1 to generate all icon PNGs and Aeon.ico
& (Join-Path $iconsDir "build_icon.ps1")
