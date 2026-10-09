#!/usr/bin/env python3
"""Generate the NTFS4Mac app icon (NTFS4Mac/Assets.xcassets/AppIcon.appiconset).

Design: a macOS-style rounded square in a deep blue gradient, an external
drive with an "NTFS" label and a green activity light, and a "4 Mac" tag
underneath. Drawn at 2048 px and downsampled for clean edges.

Usage: python3 scripts/make-icon.py [--font-dir DIR]
Needs Pillow and the Inter font family (Inter-Black, Inter-Bold).
"""

import argparse
import json
import os
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "NTFS4Mac" / "Assets.xcassets" / "AppIcon.appiconset"
S = 2048  # working canvas; scaled down at the end


def lerp(a, b, t):
    return tuple(round(a[i] + (b[i] - a[i]) * t) for i in range(len(a)))


def vertical_gradient(size, top, bottom):
    w, h = size
    grad = Image.new("RGBA", (1, h))
    for y in range(h):
        grad.putpixel((0, y), lerp(top, bottom, y / max(h - 1, 1)))
    return grad.resize((w, h))


def rounded_mask(size, box, radius):
    mask = Image.new("L", size, 0)
    ImageDraw.Draw(mask).rounded_rectangle(box, radius=radius, fill=255)
    return mask


def paste_gradient(canvas, box, radius, top, bottom):
    x0, y0, x1, y1 = box
    grad = vertical_gradient((x1 - x0, y1 - y0), top, bottom)
    layer = Image.new("RGBA", canvas.size, (0, 0, 0, 0))
    layer.paste(grad, (x0, y0))
    canvas.alpha_composite(Image.composite(layer, Image.new("RGBA", canvas.size), rounded_mask(canvas.size, box, radius)))


def shadow(canvas, box, radius, offset, blur, alpha):
    x0, y0, x1, y1 = box
    sh = Image.new("RGBA", canvas.size, (0, 0, 0, 0))
    ImageDraw.Draw(sh).rounded_rectangle((x0, y0 + offset, x1, y1 + offset), radius=radius, fill=(0, 0, 0, alpha))
    canvas.alpha_composite(sh.filter(ImageFilter.GaussianBlur(blur)))


def centered_text(draw, center, text, font, fill):
    l, t, r, b = draw.textbbox((0, 0), text, font=font)
    draw.text((center[0] - (r - l) / 2 - l, center[1] - (b - t) / 2 - t), text, font=font, fill=fill)


def draw_icon(font_dir):
    black = ImageFont.truetype(str(font_dir / "Inter-Black.otf"), 300)
    bold = ImageFont.truetype(str(font_dir / "Inter-Bold.otf"), 150)

    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))

    # Body: Apple's macOS grid puts an 824/1024 squircle-ish rounded square
    # centred with room for the drop shadow.
    m = round(S * 100 / 1024)
    body = (m, m, S - m, S - m)
    radius = round(S * 185 / 1024)
    shadow(img, body, radius, offset=round(S * 0.012), blur=round(S * 0.02), alpha=110)
    paste_gradient(img, body, radius, (54, 140, 255, 255), (10, 47, 128, 255))

    # Soft top highlight.
    hl = Image.new("RGBA", img.size, (0, 0, 0, 0))
    ImageDraw.Draw(hl).ellipse((m - S * 0.2, m - S * 0.62, S - m + S * 0.2, S * 0.48), fill=(255, 255, 255, 34))
    hl = Image.composite(hl, Image.new("RGBA", img.size), rounded_mask(img.size, body, radius))
    img.alpha_composite(hl.filter(ImageFilter.GaussianBlur(S * 0.05)))

    # External drive.
    dw, dh = round(S * 0.56), round(S * 0.33)
    dx0 = (S - dw) // 2
    dy0 = round(S * 0.27)
    drive = (dx0, dy0, dx0 + dw, dy0 + dh)
    dr = round(S * 0.055)
    shadow(img, drive, dr, offset=round(S * 0.018), blur=round(S * 0.018), alpha=120)
    paste_gradient(img, drive, dr, (250, 252, 255, 255), (206, 216, 232, 255))

    d = ImageDraw.Draw(img)
    # Bottom band of the drive (the "base") and its seam.
    band_h = round(dh * 0.24)
    band = (dx0, dy0 + dh - band_h, dx0 + dw, dy0 + dh)
    band_layer = Image.new("RGBA", img.size, (0, 0, 0, 0))
    ImageDraw.Draw(band_layer).rectangle(band, fill=(170, 184, 206, 255))
    img.alpha_composite(Image.composite(band_layer, Image.new("RGBA", img.size), rounded_mask(img.size, drive, dr)))
    d = ImageDraw.Draw(img)
    d.line((dx0 + dr * 0.3, band[1], dx0 + dw - dr * 0.3, band[1]), fill=(140, 156, 182, 255), width=round(S * 0.004))

    # Activity light and vents on the base.
    led_r = round(S * 0.016)
    led_c = (dx0 + dw - round(S * 0.075), band[1] + band_h // 2)
    glow = Image.new("RGBA", img.size, (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse((led_c[0] - led_r * 2.2, led_c[1] - led_r * 2.2, led_c[0] + led_r * 2.2, led_c[1] + led_r * 2.2), fill=(60, 230, 120, 120))
    img.alpha_composite(glow.filter(ImageFilter.GaussianBlur(led_r)))
    d = ImageDraw.Draw(img)
    d.ellipse((led_c[0] - led_r, led_c[1] - led_r, led_c[0] + led_r, led_c[1] + led_r), fill=(48, 209, 88, 255))
    for i in range(4):
        vx = dx0 + round(S * 0.07) + i * round(S * 0.035)
        d.rounded_rectangle((vx, led_c[1] - round(S * 0.012), vx + round(S * 0.014), led_c[1] + round(S * 0.012)),
                            radius=round(S * 0.006), fill=(128, 144, 170, 255))

    # "NTFS" label on the drive face.
    face_center = (S // 2, dy0 + (dh - band_h) // 2 + round(S * 0.004))
    centered_text(d, face_center, "NTFS", black, (14, 52, 128, 255))

    # "4 Mac" tag below the drive.
    tag_w, tag_h = round(S * 0.36), round(S * 0.13)
    tx0 = (S - tag_w) // 2
    ty0 = round(S * 0.665)
    tag = (tx0, ty0, tx0 + tag_w, ty0 + tag_h)
    shadow(img, tag, tag_h // 2, offset=round(S * 0.008), blur=round(S * 0.012), alpha=90)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle(tag, radius=tag_h // 2, fill=(255, 255, 255, 255))
    centered_text(d, (S // 2, ty0 + tag_h // 2), "4 Mac", bold, (10, 47, 128, 255))

    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--font-dir", default=os.environ.get("INTER_FONT_DIR", "/usr/share/fonts/opentype/inter"))
    args = ap.parse_args()

    master = draw_icon(Path(args.font_dir)).resize((1024, 1024), Image.LANCZOS)
    OUT.mkdir(parents=True, exist_ok=True)

    images = []
    for points in (16, 32, 128, 256, 512):
        for scale in (1, 2):
            px = points * scale
            name = f"icon_{points}x{points}{'@2x' if scale == 2 else ''}.png"
            master.resize((px, px), Image.LANCZOS).save(OUT / name, optimize=True)
            images.append({"idiom": "mac", "size": f"{points}x{points}", "scale": f"{scale}x", "filename": name})

    (OUT / "Contents.json").write_text(json.dumps({"images": images, "info": {"author": "xcode", "version": 1}}, indent=2) + "\n")
    (OUT.parent / "Contents.json").write_text(json.dumps({"info": {"author": "xcode", "version": 1}}, indent=2) + "\n")
    master.save(ROOT / "docs" / "icon.png", optimize=True)
    print(f"wrote {OUT} and docs/icon.png")


if __name__ == "__main__":
    main()
