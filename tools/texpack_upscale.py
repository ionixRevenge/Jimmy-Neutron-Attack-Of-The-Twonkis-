#!/usr/bin/env python3
"""Upscale dumped textures into a replacement pack.

    python tools/texpack_upscale.py [--dump texture_dump] [--out textures] [--scale 4]
                                    [--backend auto|esrgan|lanczos] [--model realesrgan-x4plus-anime]

Reads <dump>/<hash>.png (written by the game with TWONK_TEX_DUMP=<dump>) and writes
<out>/<hash>.png at `scale` times the size, which the game loads from textures/ (or
TWONK_TEX_DIR). Textures already in <out> are skipped unless --force is given, so a
later dump of new areas only adds what is new. Hand-made replacements can go in <out>
too: any PNG whose name starts with the hash, at a whole multiple of the original size.

Backends:
  esrgan   Real-ESRGAN (ncnn/Vulkan build in tools/realesrgan): best for this game's
           cartoon art. The anime model keeps edges clean; realesrgan-x4plus keeps more
           texture grain (wood, concrete).
  lanczos  Plain Lanczos resampling with light sharpening. No extra software.

Textures tile, so each one is padded with its own wrapped edges before upscaling and
cropped after: the new edges then match where the texture repeats. Alpha is upscaled
separately (as a grey image) so cut-outs and font edges stay sharp.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def pad_of(w, h):
    return max(1, min(16, w, h))


def wrap_pad(a, p):
    return np.pad(a, ((p, p), (p, p), (0, 0)) if a.ndim == 3 else ((p, p), (p, p)), mode="wrap")


def lanczos(img, scale):
    big = img.resize((img.width * scale, img.height * scale), Image.LANCZOS)
    return big.filter(ImageFilter.UnsharpMask(radius=1.5, percent=60, threshold=2))


def esrgan_batch(exe, models, model, scale, jobs, tmp):
    """jobs: {name: PIL image (RGB)} -> {name: upscaled PIL image}"""
    src, dst = os.path.join(tmp, "in"), os.path.join(tmp, "out")
    os.makedirs(src, exist_ok=True)
    os.makedirs(dst, exist_ok=True)
    for name, im in jobs.items():
        im.save(os.path.join(src, name + ".png"))
    cmd = [exe, "-i", src, "-o", dst, "-m", models, "-n", model, "-s", str(scale), "-f", "png"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    out = {}
    for name in jobs:
        p = os.path.join(dst, name + ".png")
        if os.path.exists(p):
            out[name] = Image.open(p).convert("RGB")
    if len(out) < len(jobs):
        sys.stderr.write(r.stderr[-2000:])
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dump", default=os.path.join(ROOT, "texture_dump"))
    ap.add_argument("--out", default=os.path.join(ROOT, "textures"))
    ap.add_argument("--scale", type=int, default=4)
    ap.add_argument("--backend", default="auto", choices=["auto", "esrgan", "lanczos"])
    ap.add_argument("--model", default="realesrgan-x4plus-anime")
    ap.add_argument("--esrgan", default=os.path.join(ROOT, "tools", "realesrgan"))
    ap.add_argument("--batch", type=int, default=64)
    ap.add_argument("--force", action="store_true")
    a = ap.parse_args()

    exe = os.path.join(a.esrgan, "realesrgan-ncnn-vulkan.exe")
    models = os.path.join(a.esrgan, "models")
    backend = a.backend
    if backend == "auto":
        backend = "esrgan" if os.path.exists(exe) else "lanczos"
    if backend == "esrgan" and not os.path.exists(exe):
        sys.exit(f"no Real-ESRGAN at {exe}")
    os.makedirs(a.out, exist_ok=True)

    names = sorted(f[:-4] for f in os.listdir(a.dump) if f.endswith(".png") and len(f) >= 20)
    todo = [n for n in names if a.force or not os.path.exists(os.path.join(a.out, n + ".png"))]
    print(f"{len(names)} dumped, {len(todo)} to upscale {a.scale}x with {backend}"
          + (f" ({a.model})" if backend == "esrgan" else ""))

    for start in range(0, len(todo), a.batch):
        chunk = todo[start:start + a.batch]
        rgb_jobs, alpha_jobs, meta = {}, {}, {}
        for n in chunk:
            im = np.asarray(Image.open(os.path.join(a.dump, n + ".png")).convert("RGBA"))
            h, w = im.shape[:2]
            p = pad_of(w, h)
            padded = wrap_pad(im, p)
            rgb_jobs[n] = Image.fromarray(padded[:, :, :3], "RGB")
            alpha = padded[:, :, 3]
            if alpha.min() != alpha.max():
                alpha_jobs[n + "_a"] = Image.fromarray(alpha, "L").convert("RGB")
            meta[n] = (w, h, p, int(alpha[0, 0]))
        if backend == "esrgan":
            tmp = tempfile.mkdtemp(prefix="texpack_")
            try:
                up = esrgan_batch(exe, models, a.model, a.scale, {**rgb_jobs, **alpha_jobs}, tmp)
            finally:
                shutil.rmtree(tmp, ignore_errors=True)
        else:
            up = {k: lanczos(v, a.scale) for k, v in {**rgb_jobs, **alpha_jobs}.items()}
        for n in chunk:
            w, h, p, a0 = meta[n]
            if n not in up:
                print(f"  {n}: upscale failed")
                continue
            s, ps = a.scale, p * a.scale
            rgb = np.asarray(up[n])[ps:ps + h * s, ps:ps + w * s]
            if n + "_a" in up:
                alpha = np.asarray(up[n + "_a"].convert("L"))[ps:ps + h * s, ps:ps + w * s]
            else:
                alpha = np.full((h * s, w * s), a0, np.uint8)
            out = np.dstack([rgb, alpha])
            if out.shape[:2] != (h * s, w * s):
                print(f"  {n}: unexpected size {out.shape}")
                continue
            Image.fromarray(out, "RGBA").save(os.path.join(a.out, n + ".png"), optimize=False)
        print(f"  {min(start + a.batch, len(todo))}/{len(todo)}")


if __name__ == "__main__":
    main()
