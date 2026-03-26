"""
AeonBrowser Installer Asset Converter v2 — DelgadoLogic
Improved per-asset strategies for cleaner alpha extraction.

APPROACHES:
  - bg_circuit_tile   : Fully opaque tile — no changes
  - window_frame      : Flood fill from edges (light checker ~200-255)
  - logo_shield       : Luminance chroma-key: checker gray pixels -> transparent
                        (dark checker ~(100-160, 100-160, 100-160) with low saturation)
  - logo_shield_small : Same as logo_shield
  - progress_bar_*    : Luminance chroma-key on dark checker
  - btn_close         : Luminance chroma-key on dark checker  
  - label_aeon_pill   : Luminance chroma-key on dark checker
  - label_delgadologic: Flood fill + interior-enclosed fill (light checker, text with holes)
"""

import os
import sys
from pathlib import Path
from collections import deque

try:
    from PIL import Image
    import numpy as np
except ImportError:
    os.system(f"{sys.executable} -m pip install pillow numpy")
    from PIL import Image
    import numpy as np

ASSET_DIR = Path(__file__).parent

# ---- Helpers ---------------------------------------------------------------

def color_dist(px, target):
    return float(np.sqrt(sum((int(a)-int(b))**2 for a,b in zip(px[:3], target))))

def is_dark_checker(rgb, tol=45):
    """Dark checker: ~(100-165, 100-165, 100-165), low saturation"""
    r, g, b = int(rgb[0]), int(rgb[1]), int(rgb[2])
    # grayscale check: R≈G≈B, value in dark-checker range
    spread = max(abs(r-g), abs(g-b), abs(r-b))
    avg = (r+g+b)//3
    return spread < 20 and 90 <= avg <= 170

def is_light_checker(rgb, tol=40):
    """Light checker: ~(190-255, 190-255, 190-255), low saturation"""
    r, g, b = int(rgb[0]), int(rgb[1]), int(rgb[2])
    spread = max(abs(r-g), abs(g-b), abs(r-b))
    avg = (r+g+b)//3
    return spread < 25 and avg >= 185

# ---- Method 1: Flood-fill from edges (for clean bordered images) -----------

def flood_fill_mask(arr, check_fn):
    """BFS from all 4 edges; marks pixels matching check_fn as background."""
    h, w = arr.shape[:2]
    visited = np.zeros((h, w), dtype=bool)
    mask    = np.zeros((h, w), dtype=bool)
    queue   = deque()

    def try_seed(y, x):
        if not visited[y, x]:
            visited[y, x] = True
            if check_fn(arr[y, x]):
                mask[y, x] = True
                queue.append((y, x))

    for x in range(w):
        try_seed(0, x); try_seed(h-1, x)
    for y in range(h):
        try_seed(y, 0); try_seed(y, w-1)

    while queue:
        y, x = queue.popleft()
        for dy, dx in [(-1,0),(1,0),(0,-1),(0,1)]:
            ny, nx = y+dy, x+dx
            if 0 <= ny < h and 0 <= nx < w and not visited[ny, nx]:
                visited[ny, nx] = True
                if check_fn(arr[ny, nx]):
                    mask[ny, nx] = True
                    queue.append((ny, nx))
    return mask

def flood_fill_interior_mask(arr, check_fn):
    """
    Full interior flood-fill: marks ALL checker pixels anywhere (including
    enclosed letter-counter holes). No edge seeding — pure global keying.
    Use only when checker color doesn't appear in the actual artwork content.
    """
    h, w = arr.shape[:2]
    mask = np.zeros((h, w), dtype=bool)
    for y in range(h):
        for x in range(w):
            if check_fn(arr[y, x]):
                mask[y, x] = True
    return mask

# ---- Method 2: Luminance chroma-key (for glow/haze assets) -----------------

def luma_chroma_key(arr, check_fn, feather=3):
    """
    Pixel-by-pixel chroma key: any pixel matching check_fn becomes transparent.
    Feathering: dilate the mask and apply 50% alpha to fringe pixels.
    """
    from scipy.ndimage import binary_dilation
    h, w = arr.shape[:2]
    mask = np.zeros((h, w), dtype=bool)
    for y in range(h):
        for x in range(w):
            if check_fn(arr[y, x]):
                mask[y, x] = True

    # Feather the edges
    expanded = binary_dilation(mask, iterations=feather)
    fringe   = expanded & ~mask

    arr[mask,   3] = 0
    arr[fringe, 3] = np.minimum(arr[fringe, 3], 60)  # semi-transparent fringe
    arr[~expanded, 3] = 255
    return mask

# ---- Converters per asset --------------------------------------------------

def convert_opaque(src, out):
    """Just JPG -> PNG, fully opaque."""
    img = Image.open(src).convert("RGB")
    img.save(out, "PNG")
    print(f"  [OPAQUE ] {src.name} -> {out.name}")

def convert_light_flood(src, out):
    """Remove light checker from edges only (window_frame)."""
    img  = Image.open(src).convert("RGBA")
    arr  = np.array(img)
    mask = flood_fill_mask(arr, is_light_checker)
    arr[mask, 3]  = 0
    arr[~mask, 3] = 255
    Image.fromarray(arr, "RGBA").save(out, "PNG")
    print(f"  [LT-EDGE] {src.name} -> {out.name}  ({mask.sum()} px removed)")

def convert_light_global(src, out):
    """Remove light checker everywhere, including inside enclosed areas (text labels)."""
    img  = Image.open(src).convert("RGBA")
    arr  = np.array(img)
    mask = flood_fill_interior_mask(arr, is_light_checker)
    arr[mask, 3]  = 0
    arr[~mask, 3] = 255
    Image.fromarray(arr, "RGBA").save(out, "PNG")
    print(f"  [LT-GLOB] {src.name} -> {out.name}  ({mask.sum()} px removed)")

def convert_dark_luma(src, out):
    """Remove dark checker using luminance chroma-key (glow/progress assets)."""
    try:
        from scipy.ndimage import binary_dilation
    except ImportError:
        os.system(f"{sys.executable} -m pip install scipy")
        from scipy.ndimage import binary_dilation
    img  = Image.open(src).convert("RGBA")
    arr  = np.array(img)
    luma_chroma_key(arr, is_dark_checker, feather=2)
    Image.fromarray(arr, "RGBA").save(out, "PNG")
    print(f"  [DK-LUMA] {src.name} -> {out.name}")

# ---- Asset dispatch --------------------------------------------------------

ASSETS = {
    "bg_circuit_tile.jpg":        ("opaque",      "bg_circuit_tile.png"),
    "window_frame.jpg":           ("light_flood",  "window_frame.png"),
    "logo_shield.jpg":            ("dark_luma",    "logo_shield.png"),
    "logo_shield_small.jpg":      ("dark_luma",    "logo_shield_small.png"),
    "progress_bar_fill_cyan.jpg": ("dark_luma",    "progress_bar_fill_cyan.png"),
    "progress_bar_track.jpg":     ("dark_luma",    "progress_bar_track.png"),
    "btn_close.jpg":              ("dark_luma",    "btn_close.png"),
    "label_aeon_pill.jpg":        ("dark_luma",    "label_aeon_pill.png"),
    "label_delgadologic.jpg":     ("light_global", "label_delgadologic.png"),
}

METHODS = {
    "opaque":      convert_opaque,
    "light_flood": convert_light_flood,
    "light_global":convert_light_global,
    "dark_luma":   convert_dark_luma,
}

def main():
    print("=" * 62)
    print("  AeonBrowser — DelgadoLogic Asset Converter v2")
    print("=" * 62)
    ok = fail = 0
    for fname, (method, outname) in ASSETS.items():
        src = ASSET_DIR / fname
        out = ASSET_DIR / outname
        if not src.exists():
            print(f"  [MISS   ] {fname} — not found")
            fail += 1
            continue
        try:
            METHODS[method](src, out)
            ok += 1
        except Exception as e:
            import traceback
            print(f"  [ERROR  ] {fname}: {e}")
            traceback.print_exc()
            fail += 1
    print("=" * 62)
    print(f"  Result: {ok} converted, {fail} failed")
    print(f"  Output: {ASSET_DIR}")
    print("=" * 62)

if __name__ == "__main__":
    main()
