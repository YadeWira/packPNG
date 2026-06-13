# TNCP design notes

## Scope: specialized for the PNG family (not a generic byte compressor)

TNCP is **optimized for the Network Graphics family** (PNG / APNG / JNG / MNG),
like the rest of packPNG — it is **not** a general-purpose byte compressor. The
generic order-1 model in `src/model.rs` is only a correctness harness; the real
predictor models **image structure**:

- **2D spatial context, not 1D byte history.** Each sample is predicted from its
  spatial neighbors (W = left, N = above, NW, NE) — the CALIC / LOCO-I / PNG-Paeth
  idea, but *learned*. This is where image compressors actually win.
- **Channel- and depth-aware.** RGB / RGBA / grayscale / palette, 8- and 16-bit,
  read from the IHDR. Predict per channel with cross-channel context.
- **PNG-filter-aware.** The per-scanline filter type (None/Sub/Up/Average/Paeth)
  is part of the model context.
- **Operates on preflate-undone raw pixels.** TNCP plugs in *after* the existing
  `preflate` stage (which already undoes the deflate byte-exact). So a TNCP backend
  = **preflate (exact deflate reconstruction) + neural pixel coder** in place of
  WebP/kanzi. It reuses all of packPNG's byte-exact machinery; TNCP only replaces
  the pixel-entropy stage.

By family member:
- **PNG / APNG** — the primary target (2D pixel modeling; APNG = per-frame).
- **JNG** — packJPG keeps owning JDAT (off-limits per the contract); TNCP models the
  PNG-ish parts only (IDAT alpha / wrapper).
- **MNG** — per-embedded-image, reusing the Level B structural split.

Implication: the model needs image metadata (width/height/channels/bit-depth/filter)
to build its 2D context, so TNCP is fed parsed pixel rows + IHDR, not an opaque byte
blob.

## The non-negotiable: deterministic, bit-identical prediction

TNCP is **lossless**. The decoder reconstructs each bit by running the *same*
predictor the encoder ran, fed the *same* history. If the predictor returns even a
single different probability on decode, the arithmetic decoder diverges and the
output is corrupt. Therefore:

- **Inference must be bit-reproducible across encode/decode AND across platforms**
  (Linux/Windows, different CPUs). The packPNG release binaries are cross-platform;
  a `.tncp` made on Linux must decode on Windows byte-exact.
- **Floating point is dangerous.** FMA contraction, SIMD reduction order, fast-math,
  and libm transcendental differences all make float non-deterministic across
  builds/CPUs. Options, in order of safety:
  1. **Integer / fixed-point inference** (quantized weights + integer matmul +
     a fixed integer activation/softmax). Fully deterministic. **Preferred.**
  2. Strictly-controlled float: no FMA contraction, fixed op order, no fast-math,
     table-based nonlinearities. Fragile; avoid unless forced.
- The arithmetic coder is already integer-only and deterministic, so all the risk
  lives in the predictor.

## Predictor interface (the swap point)

```rust
pub trait Predictor {
    /// 12-bit probability that the next bit is 1 (1..=4094, clamped).
    fn predict(&self) -> u16;
    /// Feed back the actual bit and advance the model state.
    fn update(&mut self, bit: u8);
}
```

Encode and decode both: `let p = model.predict(); code(p, bit); model.update(bit);`.
Any predictor that is deterministic given the same update history is valid.

## Roadmap for the neural predictor (all PNG-family / 2D-pixel from step 2 on)

1. **Baseline (done):** order-1 bitwise CM over raw bytes. Correctness harness only
   — *not* image-aware; just proves the coder + trait + FFI round-trip byte-exact.
2. **2D context-mixing (PNG-aware):** mix integer models keyed on the **spatial
   neighborhood** (W/N/NW/NE samples, current channel, filter type) with a small
   integer logistic mixer. This is "neural-lite" (1-layer net), fully integer &
   deterministic — the PAQ/lpaq/cmix recipe, but with *image* contexts. First real
   ratio win; should beat the generic baseline and the kanzi backends on pixels.
3. **Quantized NN predictor:** a small MLP/GRU over the 2D pixel context, int8/int16
   weights, integer matmul + fixed-point logistic. Trained offline (PyTorch) on PNG
   pixel corpora; weights exported to a flat binary; inference reimplemented in
   **integer** Rust. Determinism verified by an encode==decode prediction trace.
4. **Refinements:** 16-bit / palette / interlace handling, cross-channel prediction
   (e.g. predict chroma from luma), per-image model selection (`model_id`).

## Training (offline, separate from the byte-exact runtime)

- Train in PyTorch on a representative corpus (PNG pixel streams, generic bytes).
- Export quantized weights to `weights.bin` (versioned; the magic/format will pin a
  weights hash so decode uses the exact model that encoded).
- The runtime (Rust) does **integer inference only** — no PyTorch/ONNX at runtime,
  keeping packPNG's 100%-autonomous-binary property.

## Wire format (draft, for when TNCP integrates)

```
TNCP (4)  ver(1)  flags(1)  model_id(2)   # model_id pins the exact weights
namelen(2) name   orig_size(u64)          # (mirrors the TCIP container header)
coded bitstream...
```
`model_id` guarantees the decoder loads the same predictor weights that encoded —
essential for byte-exactness and for the 2.0 LTS "frozen format" promise (old
`.tncp` keep decoding as long as their `model_id` weights ship).

## Contract fit (2.0 LTS)

TNCP would be the **max-ratio** end of the family (like TMCP but learned): slow
encode/decode, best ratio. It does **not** have to obey the strict-Pareto rules of
the balanced backends — it's an opt-in archival corner, where spending compute for
ratio is the whole point (same spirit as TMCP's "brute force, but below PAQ8").
