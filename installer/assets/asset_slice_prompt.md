# Aeon Installer — Individual Asset Extraction Prompts
> Use one prompt per session. Attach the reference image each time.
> Reference image: `installer_reference.jpg`
> Brand rule: **DelgadoLogic = ONE word, no space, always.**

---

## ASSET 1 of 9 — `bg_circuit_tile.png`

```
I am attaching a UI screenshot of a software installer window.

I need you to extract ONE specific element from this image:

TARGET: The dark circuit-board background texture panel.
- This is the inner background of the installer window — the dark near-black
  surface covered with faint circuit board traces, logic gate symbols, and
  connector lines etched into the dark background.
- Crop to the inner window bounds only. Do NOT include:
  - The outer blue glow/halo around the window
  - Any logos, text, progress bars, or UI elements
  - The rounded window frame border itself

Output requirements:
- Filename: bg_circuit_tile.png
- Format: PNG, RGBA (transparent where content was cropped away)
- Size: as large as possible from the source — do not downscale
- Label your output with the filename: bg_circuit_tile.png
```

---

## ASSET 2 of 9 — `logo_shield.png`

```
I am attaching a UI screenshot of a software installer window.

I need you to extract ONE specific element from this image:

TARGET: The large glowing Aeon shield logo in the center of the window.
- This is the main hero logo — a shield shape with a glowing purple/cyan "A"
  letterform inside it. It has a neon purple outer border and a cyan inner
  geometric "A" design.
- Include the full glow halo around the shield (do not clip the glow).
- Do NOT include: background, text below it, progress bar, or any other UI.

Output requirements:
- Filename: logo_shield.png
- Format: PNG, RGBA — transparent background, shield + full glow only
- Size: crop tightly around the glow, approximately 280x300 px minimum
- Label your output with the filename: logo_shield.png
```

---

## ASSET 3 of 9 — `logo_shield_small.png`

```
I am attaching a UI screenshot of a software installer window.

I need you to extract ONE specific element from this image:

TARGET: The small shield icon in the top-left corner of the window.
- This is the small purple/pink shield icon that appears to the LEFT of
  the "DELGADOLOGIC" header text at the top-left of the installer window.
  It has a "D" letterform inside it.
- Do NOT include the text "DELGADOLOGIC" — just the shield icon itself.
- Do NOT include any background.

Output requirements:
- Filename: logo_shield_small.png
- Format: PNG, RGBA — transparent background
- Size: approximately 32x32 px (or as large as the source allows, keep square)
- Label your output with the filename: logo_shield_small.png
```

---

## ASSET 4 of 9 — `label_delgadologic.png`

```
I am attaching a UI screenshot of a software installer window.

I need you to extract ONE specific element from this image:

TARGET: The "DELGADOLOGIC" header text in the top-left of the window.
- This is the pink/purple gradient wordmark text at the top-left of the
  installer window header bar.
- IMPORTANT: The brand name is "DelgadoLogic" — it is ONE single word,
  no space between "Delgado" and "Logic". Do not add a space.
- Extract only the text glyphs themselves.
- Do NOT include: the small shield icon to its left, any background,
  the close button, or any other element.

Output requirements:
- Filename: label_delgadologic.png
- Format: PNG, RGBA — transparent background, text only
- Size: crop tightly to the text bounding box
- Label your output with the filename: label_delgadologic.png
```

---

## ASSET 5 of 9 — `label_aeon_pill.png`

```
I am attaching a UI screenshot of a software installer window.

I need you to extract ONE specific element from this image:

TARGET: The "AEON" pill/badge label just below the large shield logo.
- This is a small rounded pill/capsule shape containing the text "AEON"
  in the center of the window, directly below the main shield logo.
- It has a dark glassmorphism-style background with a subtle border.
- Include the full pill shape with its border and background.
- Do NOT include: the shield above it, the progress bar below it,
  or any surrounding background.

Output requirements:
- Filename: label_aeon_pill.png
- Format: PNG, RGBA — transparent OUTSIDE the pill shape
- Size: crop tightly to the pill shape including its border
- Label your output with the filename: label_aeon_pill.png
```

---

## ASSET 6 of 9 — `btn_close.png`

```
I am attaching a UI screenshot of a software installer window.

I need you to extract ONE specific element from this image:

TARGET: The close button "X" in the top-right corner of the window.
- This is the small "×" or "X" symbol in the top-right corner of the
  installer window header, used to dismiss/close the installer.
- Extract only the X glyph — do NOT include surrounding background,
  the window frame, or any other element.

Output requirements:
- Filename: btn_close.png
- Format: PNG, RGBA — transparent background
- Size: approximately 24x24 px, keep it square
- Label your output with the filename: btn_close.png
```

---

## ASSET 7 of 9 — `progress_bar_track.png`

```
I am attaching a UI screenshot of a software installer window.

I need you to extract ONE specific element from this image:

TARGET: The progress bar track (the empty/unfilled portion).
- This is the horizontal bar in the lower-center of the window showing
  install progress. I want ONLY the dark grey empty track/rail shape —
  the background of the bar, not the filled cyan portion.
- Do NOT include: the cyan fill, the percentage text below it, the
  status text, or any surrounding background.
- If the track and fill are inseparable, just give me the full bar
  (track + fill) and label it clearly.

Output requirements:
- Filename: progress_bar_track.png
- Format: PNG, RGBA — transparent outside the bar shape
- Size: scale to at least 600px wide, keep aspect ratio for height (~12px)
- Label your output with the filename: progress_bar_track.png
```

---

## ASSET 8 of 9 — `progress_bar_fill_cyan.png`

```
I am attaching a UI screenshot of a software installer window.

I need you to extract ONE specific element from this image:

TARGET: The cyan/blue filled gradient portion of the progress bar.
- This is the filled left portion of the progress bar — it has a
  horizontal gradient going from dark blue on the left to bright cyan
  on the right, with a bright glowing leading edge at the right tip.
- I want JUST this gradient fill strip — no track background, no text,
  no surrounding elements.
- If you cannot isolate just the fill, recreate a matching gradient strip:
  600px wide × 10px tall, dark navy (#1E3A5F) → bright cyan (#00D4FF),
  with a white-cyan glow on the rightmost ~6px.

Output requirements:
- Filename: progress_bar_fill_cyan.png
- Format: PNG, RGBA — transparent outside the gradient strip
- Size: at least 600px wide, ~10-14px tall
- Label your output with the filename: progress_bar_fill_cyan.png
```

---

## ASSET 9 of 9 — `window_frame.png`

```
I am attaching a UI screenshot of a software installer window.

I need you to extract ONE specific element from this image:

TARGET: The outer window frame / rounded card shape.
- This is the dark rounded-rectangle panel that contains the entire
  installer UI. It has rounded corners, a subtle dark border, and a
  faint blue/purple ambient glow on the outer edge (especially top-right).
- Extract: the rounded rect shape + its border + the outer ambient glow.
- Do NOT include: any interior content (logo, text, progress bar, etc.) —
  I want the frame shell only, interior should be transparent.

Output requirements:
- Filename: window_frame.png
- Format: PNG, RGBA — interior is fully transparent, only frame/border/glow
- Size: as large as possible from the source
- Label your output with the filename: window_frame.png
```

---

## Quick Reference — Order to Request

| # | Filename | What to ask for |
|---|---|---|
| 1 | `bg_circuit_tile.png` | Circuit board background texture |
| 2 | `logo_shield.png` | Large glowing shield + A logo |
| 3 | `logo_shield_small.png` | Small header shield icon (D logo) |
| 4 | `label_delgadologic.png` | "DelgadoLogic" wordmark text only |
| 5 | `label_aeon_pill.png` | "AEON" pill badge |
| 6 | `btn_close.png` | × close button |
| 7 | `progress_bar_track.png` | Empty progress track |
| 8 | `progress_bar_fill_cyan.png` | Cyan gradient fill strip |
| 9 | `window_frame.png` | Outer window frame shell |

> Drop all 9 PNGs into `installer\assets\` when done.
