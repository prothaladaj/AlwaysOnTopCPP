#!/usr/bin/env python3
"""
gen_icons.py  –  Generate icon_idle.ico and icon_active.ico for AlwaysOnTop.

Produces 32 × 32 pixel, 32-bpp (RGBA) .ico files containing a simple
thumbtack / push-pin silhouette.

  Idle   = blue-slate  (no window is currently pinned)
  Active = amber/orange (at least one window is pinned)

Run from anywhere; icons are written to  ../res/  relative to this script.
"""

import math
import os
import struct


# ─── Pixel grid builder ───────────────────────────────────────────────────────

def make_pin_grid(W: int, H: int):
    """
    Return a H×W grid of pixel-type codes:
      0 = transparent background
      1 = main body
      2 = highlight (lighter)
      3 = shadow   (darker)
      4 = needle tip
    """
    grid = [[0] * W for _ in range(H)]

    def dot(r, c, v):
        if 0 <= r < H and 0 <= c < W:
            grid[r][c] = v

    cx = W // 2  # horizontal centre (16 for W=32)

    # ── Pin head: horizontal ellipse ─────────────────────────────────────────
    hx  = cx        # head centre – x
    hy  = H // 4    # head centre – y  (≈ 8 for H=32)
    rxf = W * 0.40  # semi-axis x  (≈ 12.8)
    ryf = H * 0.20  # semi-axis y  (≈ 6.4)

    for r in range(H):
        for c in range(W):
            dx = (c - hx) / rxf
            dy = (r - hy) / ryf
            d  = math.sqrt(dx * dx + dy * dy)
            if d < 0.80:        # inner fill
                if dy < -0.25 and dx < -0.05:
                    grid[r][c] = 2   # top-left highlight
                else:
                    grid[r][c] = 1   # solid body
            elif d < 1.0:       # rim
                grid[r][c] = 3       # shadow rim

    # ── Centre hole (depression) ──────────────────────────────────────────────
    holeR = max(1, int(ryf * 0.55))
    holeC = max(1, int(rxf * 0.30))
    for r in range(hy - holeR, hy + holeR + 1):
        for c in range(hx - holeC, hx + holeC + 1):
            dx2 = (c - hx) / (holeC + 0.5)
            dy2 = (r - hy) / (holeR + 0.5)
            if math.sqrt(dx2 * dx2 + dy2 * dy2) < 1.0:
                dot(r, c, 3)

    # ── Shaft (needle body) ───────────────────────────────────────────────────
    shaft_top = int(hy + ryf) + 1
    shaft_bot = H - 5          # leave room for the tip
    for r in range(shaft_top, shaft_bot + 1):
        dot(r, cx,     1)
        dot(r, cx + 1, 1)
        dot(r, cx,     2)   # left-edge highlight on shaft

    # ── Needle tip ────────────────────────────────────────────────────────────
    for r in range(shaft_bot + 1, min(shaft_bot + 4, H)):
        dot(r, cx, 4)

    return grid


# ─── ICO / BMP builder ───────────────────────────────────────────────────────

_PALETTE = {
    # pixel-type → (R, G, B, A)
    "idle": {
        0: (  0,   0,   0,   0),  # transparent
        1: (100, 115, 145, 255),  # body  – blue-slate
        2: (155, 170, 200, 255),  # highlight
        3: ( 60,  75, 100, 255),  # shadow
        4: ( 45,  58,  80, 220),  # tip
    },
    "active": {
        0: (  0,   0,   0,   0),  # transparent
        1: (220, 148,  28, 255),  # body  – amber
        2: (255, 202,  80, 255),  # highlight
        3: (155,  93,   8, 255),  # shadow
        4: (110,  62,   4, 220),  # tip
    },
}


def build_ico_image(W: int, H: int, palette: dict) -> bytes:
    """Return the raw BMP image data (BITMAPINFOHEADER + pixels + AND mask)."""
    grid = make_pin_grid(W, H)

    # Pixel data in bottom-up row order (as required by Windows BMP in ICO)
    pixel_data = bytearray()
    for r in range(H - 1, -1, -1):
        for c in range(W):
            R, G, B, A = palette[grid[r][c]]
            pixel_data += bytes([B, G, R, A])   # BGRA in memory

    # AND mask  – all zeros so the alpha channel drives transparency
    stride   = ((W + 31) // 32) * 4
    and_mask = bytes(stride * H)

    # BITMAPINFOHEADER  (biHeight = 2*H for combined XOR + AND mask)
    bmp_hdr = struct.pack(
        "<IiiHHIIiiII",
        40,        # biSize
        W,         # biWidth
        H * 2,     # biHeight  (must be doubled for ICO BMP)
        1,         # biPlanes
        32,        # biBitCount
        0,         # biCompression (BI_RGB)
        W * H * 4, # biSizeImage
        0, 0,      # biXPelsPerMeter, biYPelsPerMeter
        0, 0,      # biClrUsed, biClrImportant
    )
    return bmp_hdr + bytes(pixel_data) + and_mask


def write_ico(path: str, W: int, H: int, palette: dict) -> None:
    """Write a single-image .ico file."""
    img  = build_ico_image(W, H, palette)
    off  = 6 + 16   # ICONDIR (6 bytes) + 1 × ICONDIRENTRY (16 bytes)
    # width / height 0 means 256 in ICO spec; use raw value for < 256
    w_byte = W if W < 256 else 0
    h_byte = H if H < 256 else 0
    ico_dir   = struct.pack("<HHH", 0, 1, 1)   # reserved, type=1, count=1
    ico_entry = struct.pack("<BBBBHHII",
        w_byte, h_byte, 0, 0,   # width, height, colourCount, reserved
        1, 32,                  # planes, bitCount
        len(img), off           # bytesInRes, imageOffset
    )
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(ico_dir + ico_entry + img)
    print(f"  {path}  ({os.path.getsize(path):,} bytes)")


# ─── Entry point ─────────────────────────────────────────────────────────────

if __name__ == "__main__":
    HERE   = os.path.dirname(os.path.abspath(__file__))
    RESDIR = os.path.normpath(os.path.join(HERE, "..", "res"))

    W, H = 32, 32
    print("Generating tray icons …")
    write_ico(os.path.join(RESDIR, "icon_idle.ico"),   W, H, _PALETTE["idle"])
    write_ico(os.path.join(RESDIR, "icon_active.ico"), W, H, _PALETTE["active"])
    print("Done.")
