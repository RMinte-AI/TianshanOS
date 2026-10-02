#!/usr/bin/env python3
"""像素对照：python pixdiff.py board.png page.png out_dir  -> 打印 JSON，并写 diff.png / side.png。
用 Codex runtime 的 Python（Pillow + numpy）。差异阈值 24/255。"""
import sys, json
from PIL import Image
import numpy as np

a_path, b_path, out_dir = sys.argv[1:4]
A = Image.open(a_path).convert('RGB')
B = Image.open(b_path).convert('RGB')
W, H = max(A.width, B.width), max(A.height, B.height)

def pad(im):
    c = Image.new('RGB', (W, H), (255, 0, 255))
    c.paste(im, (0, 0))
    return c

Ap, Bp = pad(A), pad(B)
a = np.asarray(Ap).astype(np.int16)
b = np.asarray(Bp).astype(np.int16)
d = np.abs(a - b).max(axis=2)
mask = d > 24
res = {
    'w': W, 'h': H,
    'board_size': [A.width, A.height], 'page_size': [B.width, B.height],
    'mismatch_pct': round(float(mask.mean() * 100), 3),
    'mae': round(float(d.mean()), 3),
    'p99': float(np.percentile(d, 99)),
}
heat = (np.asarray(Bp).astype(np.float32) * 0.35 + 255 * 0.65).astype(np.uint8)
heat[mask] = [255, 0, 0]
Image.fromarray(heat).save(out_dir + '/diff.png')
side = Image.new('RGB', (W * 2 + 20, H), (40, 40, 40))
side.paste(Ap, (0, 0))
side.paste(Bp, (W + 20, 0))
side.save(out_dir + '/side.png')
gh, gw = 8, 6
grid = []
for gy in range(gh):
    row = []
    for gx in range(gw):
        y0, y1 = int(gy * H / gh), int((gy + 1) * H / gh)
        x0, x1 = int(gx * W / gw), int((gx + 1) * W / gw)
        row.append(round(float(mask[y0:y1, x0:x1].mean() * 100), 1))
    grid.append(row)
res['grid'] = grid
print(json.dumps(res))
