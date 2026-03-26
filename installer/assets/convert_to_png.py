"""
AeonBrowser Installer Asset Converter
DelgadoLogic — Converts JPG assets with checkerboard backgrounds to real PNG+alpha.

Strategy per asset:
  - bg_circuit_tile.jpg   : No transparency needed, pure opaque background tile
  - window_frame.jpg      : Light-gray checker (#C8C8C8 / #FFFFFF pattern) -> transparent
  - logo_shield.jpg       : Dark-gray checker (#808080 / #999999 pattern)  -> transparent
  - logo_shield_small.jpg : same as logo_shield
  - progress_bar_fill_cyan.jpg : Dark-gray checker -> transparent
  - progress_bar_track.jpg     : Dark-gray checker -> transparent
  - btn_close.jpg              : Dark-gray checker -> transparent
  - label_aeon_pill.jpg        : dark checker -> transparent
  - label_delgadologic.jpg     : Light-gray checker -> transparent

Method:
  Uses "checker-aware" background removal:
  1. Detect whether asset has dark or light checker by sampling corner pixels.
  2. Create a mask: pixel is background if it matches the checker color pattern
     within a tolerance, AND the pixel is part of the checker grid.
  3. Because JPEG has compression artifacts, use a fuzzy color-distance threshold.
  4. Use flood-fill (connected component) from all four edges to find only the
     outer background (avoids punching holes in the actual artwork).
"""

import os
import sys
from pathlib import Path

try:
    from PIL import Image, ImageDraw
    import numpy as np
except ImportError:
    print("Installing Pillow and numpy...")
    os.system(f"{sys.executable} -m pip install pillow numpy")
    from PIL import Image, ImageDraw
    import numpy as np

ASSET_DIR = Path(__file__).parent
OUTPUT_DIR = ASSET_DIR  # write PNGs alongside JPGs

# ---- Asset configuration ---------------------------------------------------
# bg_mode: "none" = keep fully opaque (background tile)
#          "dark" = remove dark gray checker (~128,128,128 grid)
#          "light"= remove light gray/white checker (~190-255 grid)

ASSET_CONFIG = {
    "bg_circuit_tile.jpg":        {"bg_mode": "none",  "out": "bg_circuit_tile.png"},
    "window_frame.jpg":            {"bg_mode": "light", "out": "window_frame.png"},
    "logo_shield.jpg":             {"bg_mode": "dark",  "out": "logo_shield.png"},
    "logo_shield_small.jpg":       {"bg_mode": "dark",  "out": "logo_shield_small.png"},
    "progress_bar_fill_cyan.jpg":  {"bg_mode": "dark",  "out": "progress_bar_fill_cyan.png"},
    "progress_bar_track.jpg":      {"bg_mode": "dark",  "out": "progress_bar_track.png"},
    "btn_close.jpg":               {"bg_mode": "dark",  "out": "btn_close.png"},
    "label_aeon_pill.jpg":         {"bg_mode": "dark",  "out": "label_aeon_pill.png"},
    "label_delgadologic.jpg":      {"bg_mode": "light", "out": "label_delgadologic.png"},
}

# ---- Color-distance helper -------------------------------------------------
def color_dist(px, target):
    """Euclidean distance in RGB space."""
    return float(np.sqrt(sum((int(a) - int(b))**2 for a, b in zip(px[:3], target))))

# ---- Checker palette detection ---------------------------------------------
# Checkerboard tiles: alternating light/dark squares.
# Dark checker:  ~(128,128,128) and ~(153,153,153)  [gray-on-gray]
# Light checker: ~(190,190,190) and (255,255,255)    [white-on-light-gray]

DARK_CHECKER_COLORS  = [(128, 128, 128), (153, 153, 153), (105, 105, 105)]
LIGHT_CHECKER_COLORS = [(200, 200, 200), (255, 255, 255), (220, 220, 220), (230,230,230)]

TOLERANCE = 38  # JPEG compression fuzz

def is_checker_pixel(rgb, mode):
    """Returns True if the pixel matches the expected checker palette."""
    palettes = DARK_CHECKER_COLORS if mode == "dark" else LIGHT_CHECKER_COLORS
    return any(color_dist(rgb, c) < TOLERANCE for c in palettes)

# ---- Flood-fill mask -------------------------------------------------------
def flood_fill_background_mask(arr, mode):
    """
    Seeds flood fill from image edges. Any pixel reachable from the border
    AND matching the checker palette is marked as background.
    Returns boolean mask: True = background (will become transparent).
    """
    h, w = arr.shape[:2]
    visited = np.zeros((h, w), dtype=bool)
    mask    = np.zeros((h, w), dtype=bool)

    # BFS queue seeded with all edge pixels
    from collections import deque
    queue = deque()

    def try_add(y, x):
        if 0 <= y < h and 0 <= x < w and not visited[y, x]:
            visited[y, x] = True
            rgb = tuple(arr[y, x, :3])
            if is_checker_pixel(rgb, mode):
                mask[y, x] = True
                queue.append((y, x))

    # Seed top+bottom rows
    for x in range(w):
        try_add(0, x)
        try_add(h - 1, x)
    # Seed left+right columns
    for y in range(h):
        try_add(y, 0)
        try_add(y, w - 1)

    # BFS expand
    while queue:
        y, x = queue.popleft()
        for dy, dx in [(-1,0),(1,0),(0,-1),(0,1)]:
            ny, nx = y+dy, x+dx
            if 0 <= ny < h and 0 <= nx < w and not visited[ny, nx]:
                visited[ny, nx] = True
                rgb = tuple(arr[ny, nx, :3])
                if is_checker_pixel(rgb, mode):
                    mask[ny, nx] = True
                    queue.append((ny, nx))

    return mask

# ---- Per-pixel alpha soften (anti-alias edges) -----------------------------
def soften_edges(alpha, radius=2):
    """Slight Gaussian blur on the alpha channel for smoother edges."""
    from PIL import ImageFilter
    a_img = Image.fromarray(alpha)
    # Expand slightly then erode to clean up fringe
    return np.array(a_img.filter(ImageFilter.GaussianBlur(radius=radius)))

# ---- Main conversion -------------------------------------------------------
def convert_asset(src: Path, cfg: dict):
    out_path = OUTPUT_DIR / cfg["out"]
    img = Image.open(src).convert("RGBA")
    arr = np.array(img)

    if cfg["bg_mode"] == "none":
        # Just save as PNG, fully opaque
        img.save(out_path, "PNG")
        print(f"  [opaque]  {src.name} -> {cfg['out']}")
        return

    mode = cfg["bg_mode"]
    print(f"  [{mode} BG] {src.name} -> {cfg['out']}")

    # Build flood-fill mask
    mask = flood_fill_background_mask(arr, mode)

    # Apply mask: set alpha=0 where background
    arr[mask, 3] = 0

    # For non-masked pixels, ensure alpha=255
    arr[~mask, 3] = 255

    # Soften the alpha edge (anti-alias fringe from JPEG compression)
    alpha = arr[:, :, 3].copy()
    # Convert edge pixels to semi-transparent to smooth transition
    from scipy.ndimage import binary_dilation
    expanded = binary_dilation(mask, iterations=2)
    fringe = expanded & ~mask
    arr[fringe, 3] = 80  # semi-transparent fringe for smooth edges

    result = Image.fromarray(arr, "RGBA")
    result.save(out_path, "PNG")
    print(f"           Saved: {out_path.name} ({result.size[0]}x{result.size[1]})")

def main():
    print("=" * 60)
    print("  AeonBrowser — DelgadoLogic Asset Converter")
    print("=" * 60)

    # Try to import scipy; install if missing
    try:
        from scipy.ndimage import binary_dilation
    except ImportError:
        print("Installing scipy...")
        os.system(f"{sys.executable} -m pip install scipy")

    ok = 0
    fail = 0
    for fname, cfg in ASSET_CONFIG.items():
        src = ASSET_DIR / fname
        if not src.exists():
            print(f"  [MISS]    {fname} — NOT FOUND, skipping")
            fail += 1
            continue
        try:
            convert_asset(src, cfg)
            ok += 1
        except Exception as e:
            print(f"  [ERROR]   {fname}: {e}")
            fail += 1

    print("=" * 60)
    print(f"  Done: {ok} converted, {fail} failed")
    print("  PNGs written to: " + str(OUTPUT_DIR))
    print("=" * 60)

if __name__ == "__main__":
    main()
