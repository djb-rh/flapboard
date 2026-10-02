#!/usr/bin/env python3
"""Shrink photos for FlapBoard on a computer, the same way the Files page does
in the browser: upright (EXIF orientation applied), scaled so the picture
still covers 1280x720, baseline JPEG at quality 88, plus a 240x135 thumbnail
for the file list (uploaded to <folder>/.thumbs/<name> with thumb=1).

    tools/prep_photos.py OUT_DIR SRC [SRC ...]     (files or folders)
"""
import sys, os
from concurrent.futures import ProcessPoolExecutor
from PIL import Image, ImageOps

EXT = ('.jpg', '.jpeg', '.png', '.heic', '.webp', '.tif', '.tiff')

def prep(job):
    src, out = job
    name = os.path.splitext(os.path.basename(src))[0] + '.jpg'
    try:
        im = ImageOps.exif_transpose(Image.open(src)).convert('RGB')
        w, h = im.size
        k = min(1.0, max(1280 / w, 720 / h))
        main = im.resize((round(w * k), round(h * k)), Image.LANCZOS) if k < 1 else im
        main.save(os.path.join(out, name), 'JPEG', quality=88, optimize=True, progressive=False)
        kt = max(240 / w, 135 / h)
        t = im.resize((max(240, round(w * kt)), max(135, round(h * kt))), Image.LANCZOS)
        x, y = (t.width - 240) // 2, (t.height - 135) // 2
        t.crop((x, y, x + 240, y + 135)).save(os.path.join(out, '.thumbs', name), 'JPEG', quality=80)
        return name, main.size, None
    except Exception as e:
        return name, None, str(e)

def main():
    out, srcs = sys.argv[1], sys.argv[2:]
    os.makedirs(os.path.join(out, '.thumbs'), exist_ok=True)
    files = []
    for s in srcs:
        if os.path.isdir(s):
            files += sorted(os.path.join(s, f) for f in os.listdir(s) if f.lower().endswith(EXT) and not f.startswith('.'))
        else:
            files.append(s)
    with ProcessPoolExecutor() as ex:
        for i, (name, size, err) in enumerate(ex.map(prep, [(f, out) for f in files]), 1):
            print(f'{i}/{len(files)} {name} {size or "FAILED: " + err}', flush=True)

if __name__ == '__main__':
    main()
