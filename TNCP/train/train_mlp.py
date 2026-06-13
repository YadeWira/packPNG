#!/usr/bin/env python3
"""
TNCP step-1 de-risk: train a small MLP next-pixel predictor (numpy, CPU) on PNG
pixels and measure its bits-per-image on a HELD-OUT image vs the CM / WebP.

Model: causal 2D context (W,N,NW,NE,WW,NN + prev-channel + Paeth gradient) +
channel one-hot  ->  hidden (tanh)  ->  256-way softmax over the next pixel byte.
Goal: does a small learned pixel model beat / approach TNCP's CM (so it's worth
integrating, cmix-style)? Float training here; int quantization is step 2.
"""
import sys, glob, os
import numpy as np
from PIL import Image

H_HID = int(os.environ.get("HID", "64"))
MAXTRAIN = int(os.environ.get("MAXTRAIN", "1500000"))
EPOCHS = int(os.environ.get("EPOCHS", "8"))
BS = 4096
np.random.seed(1234)

def load_rgba(path):
    im = Image.open(path).convert("RGBA")
    return np.asarray(im, dtype=np.int16)  # H,W,4

def shift(a, dy, dx):
    """Causal neighbor: a shifted so result[y,x] = a[y-dy, x-dx], zero-padded."""
    out = np.zeros_like(a)
    H, W = a.shape
    ys0, ys1 = max(0, dy), H - max(0, -dy)
    xs0, xs1 = max(0, dx), W - max(0, -dx)
    yd0, yd1 = max(0, -dy), H - max(0, dy)
    xd0, xd1 = max(0, -dx), W - max(0, dx)
    out[ys0:ys1, xs0:xs1] = a[yd0:yd1, xd0:xd1]
    return out

def features_targets(img):
    """Return (X[N,F], y[N]) over all pixel-bytes of one image."""
    H, W, C = img.shape
    feats_ch, tgt_ch = [], []
    for c in range(C):
        a = img[:, :, c]
        Wv  = shift(a, 0, 1)
        Nv  = shift(a, 1, 0)
        NWv = shift(a, 1, 1)
        NEv = shift(a, 1, -1)
        WWv = shift(a, 0, 2)
        NNv = shift(a, 2, 0)
        grad = np.clip(Wv + Nv - NWv, 0, 255)
        cprev = img[:, :, c - 1] if c > 0 else Wv  # prev channel of same pixel
        # normalize bytes to [-1,1]
        def nz(v): return (v.astype(np.float32) - 128.0) / 128.0
        base = np.stack([nz(Wv), nz(Nv), nz(NWv), nz(NEv), nz(WWv), nz(NNv),
                         nz(cprev), nz(grad)], axis=-1).reshape(-1, 8)
        onehot = np.zeros((base.shape[0], C), np.float32); onehot[:, c] = 1.0
        feats_ch.append(np.concatenate([base, onehot], axis=1))
        tgt_ch.append(a.reshape(-1).astype(np.int64))
    return np.concatenate(feats_ch, 0), np.concatenate(tgt_ch, 0)

def softmax_logits(Z):
    Z = Z - Z.max(1, keepdims=True)
    e = np.exp(Z); return e / e.sum(1, keepdims=True)

# ── data ──
test_path = sys.argv[1]
train_paths = [p for p in sys.argv[2:] if p != test_path]
print(f"train images: {len(train_paths)}  test: {os.path.basename(test_path)}")

Xs, ys = [], []
for p in train_paths:
    try:
        X, y = features_targets(load_rgba(p))
        Xs.append(X); ys.append(y)
    except Exception as e:
        print("skip", p, e)
X = np.concatenate(Xs, 0); y = np.concatenate(ys, 0)
if X.shape[0] > MAXTRAIN:
    idx = np.random.choice(X.shape[0], MAXTRAIN, replace=False)
    X, y = X[idx], y[idx]
F = X.shape[1]
print(f"train examples: {X.shape[0]}  features: {F}  hidden: {H_HID}")

# ── model (numpy MLP, Adam) ──
def xavier(a, b): return (np.random.randn(a, b) * np.sqrt(2.0 / (a + b))).astype(np.float32)
W1 = xavier(F, H_HID); b1 = np.zeros(H_HID, np.float32)
W2 = xavier(H_HID, 256); b2 = np.zeros(256, np.float32)
params = [W1, b1, W2, b2]
m = [np.zeros_like(p) for p in params]; v = [np.zeros_like(p) for p in params]
lr, beta1, beta2, eps = 2e-3, 0.9, 0.999, 1e-8
t = 0
def adam(params, grads):
    global t; t += 1
    for i,(p,g) in enumerate(zip(params, grads)):
        m[i] = beta1*m[i] + (1-beta1)*g
        v[i] = beta2*v[i] + (1-beta2)*(g*g)
        mh = m[i]/(1-beta1**t); vh = v[i]/(1-beta2**t)
        p -= lr*mh/(np.sqrt(vh)+eps)

n = X.shape[0]
for ep in range(EPOCHS):
    perm = np.random.permutation(n)
    tot = 0.0
    for s in range(0, n, BS):
        bi = perm[s:s+BS]; xb = X[bi]; yb = y[bi]
        Hpre = xb @ W1 + b1; Hact = np.tanh(Hpre)
        Z = Hact @ W2 + b2; P = softmax_logits(Z)
        # cross-entropy grad
        dZ = P.copy(); dZ[np.arange(len(yb)), yb] -= 1.0; dZ /= len(yb)
        gW2 = Hact.T @ dZ; gb2 = dZ.sum(0)
        dH = (dZ @ W2.T) * (1 - Hact*Hact); gW1 = xb.T @ dH; gb1 = dH.sum(0)
        adam(params, [gW1, gb1, gW2, gb2])
        tot += -np.log2(P[np.arange(len(yb)), yb] + 1e-12).sum()
    print(f"  epoch {ep+1}: train {tot/n:.4f} bits/byte")

# ── eval on held-out image ──
Xt, yt = features_targets(load_rgba(test_path))
Ht = np.tanh(Xt @ W1 + b1); Pt = softmax_logits(Ht @ W2 + b2)
bits = -np.log2(Pt[np.arange(len(yt)), yt] + 1e-12).sum()
nbytes = bits / 8.0
nparams = sum(p.size for p in params)
print(f"\nHELD-OUT {os.path.basename(test_path)}: {len(yt)} pixel-bytes")
print(f"  MLP next-pixel: {bits/len(yt):.4f} bits/byte  -> {nbytes:.0f} bytes")
print(f"  model params: {nparams} (~{nparams} bytes at int8)")
