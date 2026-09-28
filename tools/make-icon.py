"""Extract P4V's icon and make a distinguishable launcher icon.

Requires: python -m pip install pefile pillow
Run: python tools/make-icon.py C:\\Apps\\Perforce\\p4v.exe
"""
import io
import struct
import sys
from pathlib import Path

import pefile
from PIL import Image, ImageDraw


def extract_icon(exe: Path) -> bytes:
    pe = pefile.PE(str(exe))
    resources = pe.DIRECTORY_ENTRY_RESOURCE.entries

    def resource(type_id, item_id=None):
        group = next(e for e in resources if e.id == type_id)
        entry = next(e for e in group.directory.entries if item_id is None or e.id == item_id)
        lang = entry.directory.entries[0]
        data = lang.data.struct
        return pe.get_data(data.OffsetToData, data.Size)

    group = resource(14)  # RT_GROUP_ICON
    count = struct.unpack_from('<H', group, 4)[0]
    directory = bytearray(struct.pack('<HHH', 0, 1, count))
    images = bytearray()
    for n in range(count):
        width, height, colors, reserved, planes, bpp, size, icon_id = struct.unpack_from(
            '<BBBBHHIH', group, 6 + 14 * n)
        image = resource(3, icon_id)  # RT_ICON
        directory += struct.pack('<BBBBHHII', width, height, colors, reserved,
                                 planes, bpp, len(image), 6 + 16 * count + len(images))
        images += image
    return bytes(directory + images)


def main():
    exe = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(r'C:\Apps\Perforce\p4v.exe')
    output = Path(__file__).resolve().parent.parent / 'assets'
    output.mkdir(exist_ok=True)
    original = extract_icon(exe)
    (output / 'p4v-extracted.ico').write_bytes(original)
    with Image.open(io.BytesIO(original)) as icon:
        largest = max(icon.ico.sizes(), key=lambda s: s[0] * s[1])
        base = icon.ico.getimage(largest).convert('RGBA')
    canvas = base.resize((256, 256), Image.Resampling.LANCZOS)
    badge = Image.new('RGBA', canvas.size)
    draw = ImageDraw.Draw(badge)
    # A bright '+' badge marks the derived icon as a patched launcher.
    draw.ellipse((144, 144, 250, 250), fill='#142942', outline='#6ee7db', width=7)
    draw.rounded_rectangle((188, 165, 206, 229), radius=4, fill='white')
    draw.rounded_rectangle((165, 188, 229, 206), radius=4, fill='white')
    canvas.alpha_composite(badge)
    canvas.save(output / 'p4vpatch-launcher.ico', format='ICO',
                sizes=[(s, s) for s in (16, 24, 32, 48, 64, 128, 256)])
    print('Extracted assets/p4v-extracted.ico; created assets/p4vpatch-launcher.ico')


if __name__ == '__main__':
    main()
