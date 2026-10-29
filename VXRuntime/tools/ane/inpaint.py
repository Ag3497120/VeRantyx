#!/usr/bin/env python3
"""A tiny disocclusion-inpainting net for the warp core's holes, trained offline on recorded frames, run on the ANE.

The warp core (src/warp_core.m) fills a disocclusion with the background's displacement from the nearest ring: the
background's own pixels stretched over the hole. At 20 real frames a second with 3-5 predictions each, the holes get
wider (a fast-moving edge uncovers t x its motion), and stretched pixels show as smears. This net replaces the colour
of the hole pixels only: input the predicted frame with the holes masked out (RGB + mask, half resolution: the core's
block grid), output RGB; composited where the mask is set, so it can never damage what the warp got right.

Training needs no downloads: real frames recorded by VXRT_NPU_INTERP_RECORD (fNNNNN_WxH.bgra + fNNNNN.txt "real ...")
with synthetic disocclusion-shaped holes (strips along vertical and horizontal edges, a few blobs). It reports the
holes' PSNR against the ring fill (the core's), then converts to Core ML (mlprogram, fp16) and times a prediction with
each compute unit set (CPU only, CPU + GPU, CPU + Neural Engine).

    python3 -I tools/ane/inpaint.py --train <rec dirs...> --test <rec dir> --out <dir> [--steps 1500]
needs: torch, coremltools, numpy (installed locally; nothing is fetched).
"""
import argparse, glob, os, re, sys, time
import numpy as np
import torch, torch.nn as nn, torch.nn.functional as F


def load_real(dirs, step=1):
    frames = []
    for d in dirs:
        for t in sorted(glob.glob(os.path.join(d, 'f*.txt')))[::step]:
            kind = open(t).read().split()[0]
            raw = glob.glob(t[:-4] + '_*x*.bgra')
            raw = [r for r in raw if re.search(r'f\d+_\d+x\d+\.bgra$', r)]
            if kind != 'real' or not raw:
                continue
            w, h = map(int, re.search(r'_(\d+)x(\d+)\.bgra$', raw[0]).groups())
            a = np.fromfile(raw[0], np.uint8)
            if a.size != w * h * 4:
                continue
            a = a.reshape(h, w, 4)[:, :, [2, 1, 0]].astype(np.float32) / 255     # BGRA -> RGB
            frames.append(a)
    return frames


def holes(h, w, rng):
    """A disocclusion-shaped mask (1 = hole): strips along vertical/horizontal edges (an object moved sideways or up and
    uncovered the background behind it), widths 2-24 px, plus a few blobs; about 4-12% of the picture."""
    m = np.zeros((h, w), np.float32)
    for _ in range(rng.integers(3, 9)):
        if rng.random() < 0.7:                                     # (a vertical edge moved sideways)
            x, y = rng.integers(0, w), rng.integers(0, h)
            ww, hh = rng.integers(2, 25), rng.integers(16, max(17, h // 2))
        else:                                                      # (a horizontal edge moved up or down)
            x, y = rng.integers(0, w), rng.integers(0, h)
            ww, hh = rng.integers(16, max(17, w // 3)), rng.integers(2, 17)
        m[y:y + hh, x:x + ww] = 1
    for _ in range(rng.integers(0, 3)):
        cx, cy, r = rng.integers(0, w), rng.integers(0, h), rng.integers(3, 12)
        yy, xx = np.ogrid[:h, :w]
        m[(xx - cx) ** 2 + (yy - cy) ** 2 <= r * r] = 1
    return m


def ring_fill(img, m):
    """The warp core's fill, approximately: each hole pixel takes the nearest valid pixel in rings 1, 2, 4, 8, 16 out
    (the farthest-background choice is the core's; here all neighbours are background, so the first ring found)."""
    out = img.copy()
    h, w = m.shape
    todo = m > 0.5
    for r in (1, 2, 4, 8, 16, 32):
        if not todo.any():
            break
        acc = np.zeros_like(img); n = np.zeros((h, w), np.float32)
        for dy in (-r, 0, r):
            for dx in (-r, 0, r):
                if not dx and not dy:
                    continue
                sh = np.roll(np.roll(~todo, dy, 0), dx, 1)
                v = np.roll(np.roll(out, dy, 0), dx, 1)
                acc += v * sh[..., None]; n += sh
        got = todo & (n > 0)
        out[got] = acc[got] / n[got][:, None]
        todo = todo & ~got
    return out


class Tiny(nn.Module):
    """A 3-level U-Net, 16/32/48 channels (about 100k weights), small enough for the ANE at a frame's block grid. Its
    input is the warp core's own result (holes ring-filled) and the mask; it learns a correction of the hole pixels
    only, starting at zero (so at worst it is the ring fill). ANE-friendly: x2 nearest upsampling on sizes divisible
    by 4 (an arbitrary-size bilinear resize does not compile for the ANE)."""
    def __init__(s, c=16):
        super().__init__()
        s.e0 = nn.Sequential(nn.Conv2d(4, c, 3, padding=1), nn.ReLU(), nn.Conv2d(c, c, 3, padding=1), nn.ReLU())
        s.e1 = nn.Sequential(nn.Conv2d(c, 2 * c, 3, 2, 1), nn.ReLU(), nn.Conv2d(2 * c, 2 * c, 3, padding=1), nn.ReLU())
        s.e2 = nn.Sequential(nn.Conv2d(2 * c, 3 * c, 3, 2, 1), nn.ReLU(), nn.Conv2d(3 * c, 3 * c, 3, padding=2, dilation=2), nn.ReLU(),
                             nn.Conv2d(3 * c, 3 * c, 3, padding=4, dilation=4), nn.ReLU())
        s.d1 = nn.Sequential(nn.Conv2d(3 * c + 2 * c, 2 * c, 3, padding=1), nn.ReLU())
        s.d0 = nn.Sequential(nn.Conv2d(2 * c + c, c, 3, padding=1), nn.ReLU(), nn.Conv2d(c, 3, 3, padding=1))
        nn.init.zeros_(s.d0[2].weight); nn.init.zeros_(s.d0[2].bias)
        s.up = nn.Upsample(scale_factor=2, mode='nearest')

    def forward(s, x):                                             # x: RGB ring-filled, then the mask (N, 4, H, W), H, W % 4 == 0
        a = s.e0(x); b = s.e1(a); c = s.e2(b)
        u = s.d1(torch.cat([s.up(c), b], 1))
        y = s.d0(torch.cat([s.up(u), a], 1))
        m = x[:, 3:4]
        return torch.clamp(x[:, :3] + y * m, 0, 1)                 # (only the holes change)


def batch(frames, rng, n, size):
    xs, ys = [], []
    for _ in range(n):
        f = frames[rng.integers(len(frames))]
        h, w = f.shape[:2]
        y0, x0 = rng.integers(0, h - size), rng.integers(0, w - size)
        img = f[y0:y0 + size, x0:x0 + size]
        if rng.random() < 0.5:
            img = img[:, ::-1]
        m = holes(size, size, rng)
        xs.append(np.concatenate([ring_fill(img * (1 - m[..., None]), m), m[..., None]], 2)); ys.append(img)
    t = lambda a: torch.from_numpy(np.ascontiguousarray(np.stack(a))).permute(0, 3, 1, 2)
    return t(xs), t(ys)


def psnr(a, b):
    mse = float(np.mean((a - b) ** 2))
    return 10 * np.log10(1 / max(mse, 1e-10))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--train', nargs='+', required=True)
    ap.add_argument('--test', nargs='+', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--steps', type=int, default=1500)
    ap.add_argument('--size', type=int, default=128)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    tr, te = load_real(a.train), load_real(a.test, step=4)
    print(f'frames: {len(tr)} to train on, {len(te)} to test on ({tr[0].shape[1]}x{tr[0].shape[0]})')
    dev = 'mps' if torch.backends.mps.is_available() else 'cpu'
    net = Tiny().to(dev)
    print(f'net: {sum(p.numel() for p in net.parameters())} weights, training on {dev}')
    opt = torch.optim.Adam(net.parameters(), 2e-3)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, a.steps)
    rng = np.random.default_rng(1)
    t0 = time.time()
    for i in range(a.steps):
        x, y = batch(tr, rng, 16, a.size)
        x, y = x.to(dev), y.to(dev)
        o = net(x)
        m = x[:, 3:4]
        loss = (torch.abs(o - y) * m).sum() / m.sum().clamp(min=1)
        opt.zero_grad(); loss.backward(); opt.step(); sched.step()
        if i % 250 == 0 or i == a.steps - 1:
            print(f'  step {i}: L1 in the holes {loss.item():.4f} ({time.time() - t0:.0f} s)')
    net.eval().cpu()
    # the test: whole frames, the holes' PSNR, the net against the ring fill
    rng = np.random.default_rng(7)
    pn, pr = [], []
    with torch.no_grad():
        for f in te:
            f = f[:f.shape[0] // 4 * 4, :f.shape[1] // 4 * 4]
            h, w = f.shape[:2]
            m = holes(h, w, rng)
            r = ring_fill(f * (1 - m[..., None]), m)
            x = np.concatenate([r, m[..., None]], 2).astype(np.float32)
            o = net(torch.from_numpy(x).permute(2, 0, 1)[None])[0].permute(1, 2, 0).numpy()
            sel = m > 0.5
            pn.append(psnr(o[sel], f[sel])); pr.append(psnr(r[sel], f[sel]))
    print(f'test ({len(te)} frames, holes only): net {np.mean(pn):.2f} dB, ring fill (the core\'s) {np.mean(pr):.2f} dB, '
          f'net better on {sum(p > q for p, q in zip(pn, pr))} of {len(pn)}')
    torch.save(net.state_dict(), os.path.join(a.out, 'inpaint_tiny.pt'))
    # Core ML: fixed sizes (the warp core's block grid for a 1512x982 window: 756x492; Vision Pro's eye at half: 960x912)
    import coremltools as ct
    for (H, W) in ((488, 756), (912, 960)):
        ex = torch.zeros(1, 4, H, W)
        tm = torch.jit.trace(net, ex)
        ml = ct.convert(tm, inputs=[ct.TensorType(name='x', shape=ex.shape)], outputs=[ct.TensorType(name='y')],
                        convert_to='mlprogram', compute_precision=ct.precision.FLOAT16, minimum_deployment_target=ct.target.macOS14)
        path = os.path.join(a.out, f'inpaint_tiny_{W}x{H}.mlpackage')
        ml.save(path)
        x = {'x': np.random.rand(1, 4, H, W).astype(np.float32)}
        for name, cu in (('CPU only', ct.ComputeUnit.CPU_ONLY), ('CPU + GPU', ct.ComputeUnit.CPU_AND_GPU), ('CPU + Neural Engine', ct.ComputeUnit.CPU_AND_NE)):
            m = ct.models.MLModel(path, compute_units=cu)
            for _ in range(3):
                m.predict(x)
            ts = []
            for _ in range(20):
                t1 = time.perf_counter(); m.predict(x); ts.append((time.perf_counter() - t1) * 1000)
            ts.sort()
            print(f'Core ML {W}x{H} fp16, {name}: {ts[len(ts) // 2]:.2f} ms median, {ts[-2]:.2f} ms p95 (20 runs, Python call included)', flush=True)


if __name__ == '__main__':
    main()
