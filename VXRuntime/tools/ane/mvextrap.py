#!/usr/bin/env python3
"""Motion extrapolation for the warp core: the next real frame's motion vectors from the last three, on the ANE.

The warp core predicts 3-5 refreshes past a real frame B with B's own motion vectors: a straight line. Turning while
driving or a camera easing in and out curves; the error grows with t^2 x the acceleration. This net predicts the next
frame's field from the last three (each the game's motion vectors at quarter render size, plus B's depth), so a
prediction at t can use B's vectors + t^2/2 x the change. It is trained self-supervised on recordings: the label is the
next real frame's own vectors (VXRT_NPU_INTERP_RECORD writes fNNNNN_mv_WxH.rg16f and fNNNNN_depth_WxH.r32f for every real
frame with them).

    python3 -I tools/ane/mvextrap.py --rec <rec dirs...> --out <dir> [--steps 2000]
It reports the error of "no change" (B's vectors held, the warp core today) against the net's, in frame pixels over the
next interval, then converts to Core ML (mlprogram, fp16) and times it per compute unit. Needs recordings with big
motion (walking, driving); a still scene trains nothing.
"""
import argparse, glob, os, re, time
import numpy as np
import torch, torch.nn as nn, torch.nn.functional as F


def load(d):
    seq = []
    for t in sorted(glob.glob(os.path.join(d, 'f*_mv_*x*.rg*f'))):
        m = re.search(r'f(\d+)_mv_(\d+)x(\d+)\.(rg16f|rg32f)$', t)
        n, w, h, fmt = int(m.group(1)), int(m.group(2)), int(m.group(3)), m.group(4)
        mv = np.fromfile(t, np.float16 if fmt == 'rg16f' else np.float32).astype(np.float32).reshape(h, w, 2)
        dp = glob.glob(os.path.join(d, 'f%05d_depth_*.r32f' % n))
        depth = np.fromfile(dp[0], np.float32).reshape(h, w) if dp else np.zeros((h, w), np.float32)
        seq.append((n, mv, depth))
    return seq


def quarter(a):
    t = torch.from_numpy(np.ascontiguousarray(a)).permute(2, 0, 1)[None] if a.ndim == 3 else torch.from_numpy(a)[None, None]
    return F.avg_pool2d(t, 4)


class Net(nn.Module):
    def __init__(s, c=24):
        super().__init__()
        s.f = nn.Sequential(nn.Conv2d(7, c, 3, padding=1), nn.ReLU(), nn.Conv2d(c, c, 3, padding=2, dilation=2), nn.ReLU(),
                            nn.Conv2d(c, c, 3, padding=4, dilation=4), nn.ReLU(), nn.Conv2d(c, 2, 3, padding=1))

    def forward(s, x):                       # x: mv(t-2), mv(t-1), mv(t) (2 each, UV units x 100), depth: the change
        return x[:, 4:6] + s.f(x)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rec', nargs='+', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--steps', type=int, default=2000)
    a = ap.parse_args()
    samples = []
    for d in a.rec:
        seq = load(d)
        for i in range(3, len(seq)):
            ns = [s[0] for s in seq[i - 3:i + 1]]
            if ns[-1] - ns[0] > 30:          # (a gap in the recording: frames not consecutive enough)
                continue
            x = torch.cat([quarter(seq[i - 3 + k][1]) * 100 for k in range(3)] + [quarter(seq[i - 1][2])], 1)
            y = quarter(seq[i][1]) * 100
            samples.append((x, y))
    if len(samples) < 20:
        print(f'{len(samples)} samples: not enough (record big motion: walking, driving)')
        return
    print(f'{len(samples)} samples, {tuple(samples[0][0].shape[2:])} quarter-size fields')
    split = len(samples) * 4 // 5
    tr, te = samples[:split], samples[split:]
    net = Net()
    opt = torch.optim.Adam(net.parameters(), 1e-3)
    for i in range(a.steps):
        x, y = tr[np.random.randint(len(tr))]
        loss = F.l1_loss(net(x), y)
        opt.zero_grad(); loss.backward(); opt.step()
        if i % 500 == 0:
            print(f'  step {i}: L1 {loss.item():.4f}')
    with torch.no_grad():
        hold = np.mean([F.l1_loss(x[:, 4:6], y).item() for x, y in te])
        pred = np.mean([F.l1_loss(net(x), y).item() for x, y in te])
    print(f'test: the next field\'s error (UV x 100): held (today) {hold:.4f}, the net {pred:.4f}')
    os.makedirs(a.out, exist_ok=True)
    import coremltools as ct
    ex = samples[0][0]
    ml = ct.convert(torch.jit.trace(net.eval(), ex), inputs=[ct.TensorType(name='x', shape=ex.shape)], convert_to='mlprogram',
                    compute_precision=ct.precision.FLOAT16, minimum_deployment_target=ct.target.macOS14)
    path = os.path.join(a.out, 'mvextrap.mlpackage')
    ml.save(path)
    for name, cu in (('CPU only', ct.ComputeUnit.CPU_ONLY), ('CPU + Neural Engine', ct.ComputeUnit.CPU_AND_NE)):
        m = ct.models.MLModel(path, compute_units=cu)
        x = {'x': ex.numpy()}
        m.predict(x)
        ts = []
        for _ in range(20):
            t1 = time.perf_counter(); m.predict(x); ts.append((time.perf_counter() - t1) * 1000)
        print(f'Core ML {name}: {sorted(ts)[10]:.2f} ms median')


if __name__ == '__main__':
    main()
