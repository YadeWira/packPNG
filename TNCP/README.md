# TNCP — Tovy Neural Compresor PNG

A **lossless** neural compressor **specialized for the Network Graphics family**
(PNG / APNG / JNG / MNG) — not a generic byte compressor. A neural network predicts
the probability of each bit from **2D image context** (spatial neighbors, channel,
filter type); a byte-exact arithmetic coder turns those predictions into a
bitstream. Decode runs the *same* predictor in lockstep, so the original bytes are
reconstructed **exactly** — the byte-exactness contract packPNG lives by.

TNCP plugs in *after* `preflate` (which undoes the deflate byte-exact): it replaces
the pixel-entropy stage (WebP/kanzi) with a learned 2D pixel coder, reusing all of
packPNG's existing byte-exact machinery. See `DESIGN.md` → "Scope". The current
order-1 model is only a correctness harness; the real predictor is image-aware.

Planned magic: **`TNCP`** (Tovy Neural Compresor PNG), a future Tovy backend
alongside TVCP / TCIP / TMCP / TCIJ / TCIM.

## Why a separate folder

The neural predictor is a heavy, evolving subsystem (model, weights, training) with
its own build (Rust staticlib). Like `preflate-rs`, it links into packPNG through a
small C FFI — so it stays self-contained here and integrates without bloating
`packpng.cpp`.

## Architecture (predictor ⟂ coder)

```
              ┌──────────────┐   p(bit=1)   ┌──────────────────┐
  input ───▶  │  Predictor   │ ───────────▶ │ Arithmetic coder │ ──▶ compressed
              │ (NN / CM)    │ ◀─────────── │  (byte-exact)    │
              └──────────────┘   bit (fed    └──────────────────┘
                                  back, same on enc & dec)
```

- **Arithmetic coder** (`src/rangecoder.rs`) — the byte-exact, deterministic core.
  Carryless 32-bit binary range coder (lpaq-style). This part is *frozen-correct*:
  the NN never touches it. **Already implemented + round-trip tested.**
- **Predictor** (`src/model.rs`) — implements the `Predictor` trait
  (`predict(&self) -> u16` 12-bit prob, `update(&mut self, bit)`). The current
  baseline is an **order-1 bitwise context model** (a real, working lossless
  compressor). The neural model will be a drop-in replacement behind this trait.
- **FFI** (`src/lib.rs`) — `tncp_compress` / `tncp_decompress` / `tncp_free`,
  mirroring the preflate-rs shim, ready to link into packPNG.

## Status

| Component | State |
|---|---|
| Byte-exact arithmetic coder | ✅ implemented + tested |
| `Predictor` trait | ✅ defined |
| Baseline order-1 CM (generic harness) | ✅ working (correctness only) |
| **2D image context-mixing predictor** | ✅ **step 2 done** — see below |
| C FFI (bytes + image: compress/decompress/free) | ✅ implemented |
| Quantized **neural** predictor | ⏳ step 3 — see `DESIGN.md` |
| packPNG integration (magic TNCP) | ⏳ once it beats WebP/kanzi on pixels |

### Step 2 — 2D context-mixing + match model + SSE (measured, byte-exact)

On raw pixels of real PNGs, the 2D model (`src/image_model.rs` + `src/cm.rs`)
beats PNG deflate and kanzi handily and is closing on WebP-lossless:

| image | order-1 | 2D (7ctx) | +match+SSE | +cross-ch | **+NN ctx** | PNG deflate | WebP (TCIP) | kanzi (TVCP) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| screenshot-uk (1324×844) | 551527 | 89438 | 43749 | 32603 | **32530** | 83462 | 27656 | 76622 |
| cart (256²) | 6277 | 1473 | 781 | 760 | **729** | 1997 | — | — |
| colors (16²) | 722 | 307 | 315 | 312 | **306** | 353 | — | — |

Model: 9 contexts — spatial W/N/NW/NE/WW/NN + Paeth gradient + order-0 + **cross-channel
color** (prev channel + pixel's first channel) — plus a **1D match model** (catches
LZ-style repeats: UI/text), mixed by an integer logistic mixer, refined by an **APM
(SSE)**, with **count-adaptive StateMap counters** (fast when fresh, stable when
mature). All integer/deterministic.

Latest (byte-exact): **screenshot-uk 32295** (deflate 83462, kanzi 76622, **WebP 27656**),
**cart 725** (deflate 1997), **colors 233** (deflate 353). So TNCP is **−61% vs deflate,
crushes kanzi, ~17% behind WebP**. Cheap structural CM wins are now largely exhausted;
closing the last ~17% to WebP wants the **NN predictor (step 3)** or heavier modeling
(2-layer mixer, learned color transform). A 2nd W-keyed APM was tried and reverted.

## Build & test

```bash
cargo test          # round-trip byte-exactness
cargo build --release   # → target/release/libtncp.a  (staticlib for packPNG)
```

## The hard constraint (read `DESIGN.md`)

For lossless decode, the predictor must be **bit-identical on encode and decode,
on every platform**. Floating-point non-determinism would break byte-exactness, so
the neural inference path must be **deterministic** (fixed-point / integer, or
strictly reproducible float). This shapes the whole model design.
