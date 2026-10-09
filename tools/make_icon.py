"""Generate the embedded Windows icon from the application's geometric mark."""
from pathlib import Path
from PIL import Image, ImageDraw

root = Path(__file__).resolve().parents[1]
assets = root / "src" / "assets"
assets.mkdir(exist_ok=True)
sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
images = []
for size in sizes:
    unit = size * 4 / 32
    image = Image.new("RGBA", (size * 4, size * 4))
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((unit, unit, 31 * unit, 31 * unit), 8 * unit, fill=(49, 49, 49, 255))
    width = max(1, round(2.3 * unit))
    for points in [[(7, 11), (24, 11)], [(19, 6), (24, 11), (19, 16)],
                   [(25, 21), (8, 21)], [(13, 16), (8, 21), (13, 26)]]:
        draw.line([(round(x * unit), round(y * unit)) for x, y in points], fill="white", width=width, joint="curve")
    images.append(image.resize((size, size), Image.Resampling.LANCZOS))
images[-1].save(assets / "chuan.ico", format="ICO", sizes=[(s, s) for s in sizes], append_images=images[:-1])
images[-1].save(assets / "chuan.png")
