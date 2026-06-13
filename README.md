# packPNG — 2.0 LTS

**Lossless, byte-exact recompressor for the PNG family — PNG / APNG / JNG / MNG.**
The default backend undoes the deflate exactly (preflate) and re-stores the image
with **WebP-lossless**, reconstructing the **byte-identical original file** (same
SHA-256). One self-contained, cross-platform binary; also available as a
[library](https://github.com/YadeWira/packPNG/wiki/Library).

```bash
packPNG a image.png        # → image.ppg     (PNG/APNG → TCIP, preflate + WebP-lossless)
packPNG a image.jng        # → image.ppg     (JNG  → TCIJ, packJPG)
packPNG a clip.mng         # → clip.ppg      (MNG  → TCIM container)
packPNG x image.ppg        # restore the exact original file (byte-identical)
packPNG a -r -od out/ src/ # recurse, write all outputs into out/
```

> **2.0 LTS — the wire format is frozen.** From 2.0 onward every `2.0x` release
> decodes any other `2.0x`'s output. Magics spell out the backend: **TCIP**
> (default, preflate + WebP-lossless), **TVCP** (`-fast`, kanzi + zstd), **TMCP**
> (`-preflate-max`, kanzi-TPAQX), **TPCL** (`-tpcl`, preflate + LZMA2), **TCIJ**
> (JNG), **TCIM** (MNG). See the **[wiki](https://github.com/YadeWira/packPNG/wiki)**
> for backends, benchmarks (incl. vs precomp) and the library API.

> **v1.8:** adds JNG (JPEG Network Graphics) container support. JNG inputs are
> parsed into 3 sections (head / image / tail), each `zstd-19` compressed, and
> emitted as `.ppg` with internal magic `TCIJ`. Round-trip is byte-exact on the
> sembiance JNG corpus (gray/color, IDAT/JDAA alpha, progressive). Phase 1
> ships the wrapper format; Phase 2 will route JDAT through packJPG for a
> ~25 % additional win on the JPEG portion.

> **v1.7:** packPNG returned to a 1-input → 1-output design — multi-PNG
> archives were removed in favour of per-file `.ppg` output. The tovyCIP
> algorithm stayed; it just runs on each file independently now. Output
> collisions (e.g. two `image.png` from different dirs without `-fs`) are
> renamed `image.ppg`, `image(1).ppg`, `image(2).ppg`. Legacy multi-entry
> `.tcip` / `.ppgs` archives from v1.4–v1.6 still decode unchanged.

## Benchmarks vs xz preset 6

23-PNG corpus, ~10.5 MB raw, AMD A8-6600K, 40-trial interleaved.

| metric | xz `-m6` | packPNG (tovyCIP) | Δ |
|---|---:|---:|---:|
| **size** | 1,722,720 B | **1,720,912 B** | **−1,808 B** ✓ |
| **encode time** | 1.43 s | **0.70 s** | **−51 %** ✓ |
| **decode (single thread)** | 0.096 s | **0.072 s** | **−25 %** ✓ |
| **decode (multi-thread)** median | 0.050 s | **~0.054 s** | within noise (±5 ms variance) |

Single-file too: for `Cspeed.png` (~70 KB raw), the 1-entry tovyCIP `.ppg` is **23,185 B vs 25,477 B** for the per-file `.ppg` (`-perfile` mode) (`−2,292 B`, −9 %). The kanzi BWT pipeline beats LZMA-6 even on a single file — no archive-framing penalty in practice.

`-kpng-max` (TPAQ) trades decode speed for max ratio: **−8,914 B** vs xz, but ~2.8× slower decode. Use only when archive size matters more than read latency.

## packPNG vs precomp

[precomp](https://github.com/schnaader/precomp-cpp) is the reference byte-exact PNG
recompressor (also preflate-based). It treats a PNG as a *deflate stream* and
re-compresses it with LZMA2; packPNG un-filters to *pixels* and uses a real image
codec. Both reproduce the original file **byte-for-byte** — same job, different ratio.

17 real-world PNGs (1.52 MB total), 56-core Xeon E5-2690 v4. Every row verified
byte-exact (17/17). Lower ratio = smaller = better.

| backend | what it does | ratio | encode | decode |
|---|---|---:|---:|---:|
| **TCIP** (default) | preflate + WebP-lossless | **45.7 %** | 2.38 s | 0.72 s |
| **TMCP** (`-preflate-max`) | preflate + kanzi-TPAQX (archival) | 47.6 % | 11.8 s | 11.5 s |
| **TPCL** (`-tpcl`) | preflate + multi-threaded LZMA2 | 64.3 % | 1.68 s | 0.58 s |
| **TVCP** (`-fast`) | kanzi BWT + zstd | 85.6 % | 0.39 s | 0.04 s |
| precomp 0.4.8 | preflate + LZMA2 | 75.3 % | 2.21 s | 0.98 s |

- **TCIP** (the default) is **39 % smaller** than precomp — modelling the image with
  WebP-lossless beats LZMA2-on-deflated-bytes.
- **TPCL** uses precomp's *exact* recipe (preflate + LZMA2) yet wins on **all three
  axes** — smaller (64 vs 75 %) and faster — thanks to a newer preflate + LZMA2.
- **TVCP** trades ratio for raw speed (~6× faster decode than precomp).
- precomp's `-intense` / `-brute` don't change PNG results (the IDAT is already found
  by default; those modes only help raw/embedded zlib in other container types).

> Speed methodology: packPNG times are `-th0` (parallel across files); precomp is
> per-file (it has no batch mode). For a *single* file precomp's encode/decode is
> competitive — it multi-threads LZMA2 internally — so packPNG's speed edge is in
> batch. Ratio is mode-independent.

## How tovyCIP works

1. **Parse** each PNG/APNG into frames; **inflate** each IDAT stream to raw pixels.
2. **Brute-force** zlib parameters (level, strategy, wbits, memlevel) to find the combination that reproduces the original deflate stream byte-exactly. If found → mode 0 (store raw pixels). Else → mode 1 (store the deflate stream as-is) or, with `-ldf`, mode 2 (libdeflate pixel-exact repack).
3. **Filter-byte separation**: split the per-scanline filter byte from the pixel payload (clusters similar bytes for better BWT).
4. **Sort entries** by pixel-buffer size descending — biggest entry first.
5. **Split into 2 kanzi streams**: stream 0 = biggest entry alone (block 2 MB); stream 1 = rest (block 4 MB). Both encode in parallel threads. Pipeline: `RLT + BWT + SRT + ZRLT / FPAQ`.
6. **IDAT-passthrough section**: zstd-19 with `windowLog=27` (long-range, 128 MB window).
7. **Decode** spawns one thread per pixel stream + one for zstd_idat (all parallel). With `-th0`, automatically picks `n_workers = hw_concurrency − num_streams` to avoid thread oversubscription.

### Algorithms inside tovyCIP

| | |
|---|---|
| **RLT** | Run-Length Transform — collapses runs of repeated bytes |
| **BWT** | Burrows-Wheeler Transform — block-sort that clusters similar symbols |
| **SRT** | Sorted Rank Transform — post-BWT recoding (better than MTF on this stage) |
| **ZRLT** | Zero Run-Length Transform — collapses the long zero runs SRT produces |
| **FPAQ** | Fast PAQ — order-0 binary arithmetic coder, bit-adaptive |
| **zstd-19 long** | for IDAT-passthrough; high-entropy already-compressed deflate streams |

## Build

```bash
# Full feature build (recommended — needed for tovyCIP)
g++ -std=c++17 -O3 -funroll-loops -fomit-frame-pointer -march=native \
    -DUSE_KANZI -DUSE_ZSTD -DUSE_LIBDEFLATE \
    -I<path-to-kanzi-cpp>/src \
    -o packPNG source/packpng.cpp \
    -lz -llzma -lzstd -ldeflate \
    <path-to-kanzi-cpp>/build/libkanzi.a -lpthread

# Minimal build — per-file LZMA only, no tovyCIP
make
```

Debian/Ubuntu deps: `apt install zlib1g-dev liblzma-dev libdeflate-dev libzstd-dev`. Plus build [kanzi-cpp](https://github.com/flanglet/kanzi-cpp) into a static lib.

Windows cross-compile: `make win` (currently LZMA-only — for tovyCIP on Windows, build mingw kanzi-cpp + libzstd + libdeflate locally).

## Library (libpackPNG)

packPNG is also a **library** — static (`.a`) and shared (`.so` / `.dll`) — with a
small C API (`source/packpng.h`) so archivers and asset pipelines can embed the
codec. **Multithreading is on by default.**

```bash
make lib       # Linux:   libpackpng.a + libpackpng.so + packpng.h
make lib-win   # Windows: libpackpng-win.a + packpng.dll + libpackpng.dll.a + packpng.def
```

```c
#include "packpng.h"

/* File → file */
packpng_compress_file("image.png", "image.ppg", PACKPNG_TCIP);
packpng_decompress_file("image.ppg", "out/");   /* → out/image.png, byte-exact */

/* In-memory (buffer → buffer) — for embedding */
unsigned char* out; size_t out_len;
packpng_compress_mem(png, png_len, "image.png", &out, &out_len, PACKPNG_TCIP);
/* ... use out/out_len ... */  packpng_free(out);
```

Prebuilt SDK bundles (static + shared + header) are attached to every
[release](https://github.com/YadeWira/packPNG/releases). Full API and link lines:
**[wiki → Library](https://github.com/YadeWira/packPNG/wiki/Library)**.

## Usage

```
packPNG [subcommand] [flags] file(s)

Subcommands:
  a            compress only
  x            decompress only
  mix          both directions (default)
  l / list     inspect .ppg files

tovyCIP backend (tovyCIP == TCIP == the default PNG backend):
  (default)    no flag needed. Since v1.9 the tovyCIP default is preflate +
                 WebP-lossless (magic TCIP): undo the deflate byte-exact, store the
                 image with WebP-lossless (method 5) → best ratio (≈ −54% on
                 real-world PNGs) with fast decode (~40-90ms/file) and reasonable
                 encode (~0.3-2.3s/file; scales with pixels). -fast for max speed.
  -tcip        alias for the default (== -tovycip == -solid).
  -fast        OLD tovyCIP: kanzi RLT+BWT+SRT+ZRLT/FPAQ + zstd-19 idat (magic TVCP).
                 Much faster encode/decode, weaker ratio. Use when speed matters most.
  -preflate    explicit alias of the default (preflate + WebP-lossless, TCIP).
  -preflate-max  preflate + kanzi-TPAQX (TMCP). Superseded by the default (which now
                 beats it on both ratio AND decode); archival/edge only.
  -perfile     opt out → legacy per-file LZMA path (v1.0–v1.4 backend).
  -kpng-max    old kanzi tovyCIP + TPAQ entropy (max ratio of the -fast family).

Per-file (.ppg) flags:
  -m<1-9>      LZMA preset (default 6)
  -me          LZMA extreme flag
  -deep        disable brute-force early-out (~2× slower, ~6 % smaller)
  -ldf         libdeflate pixel-exact fallback for unmatched frames
  -zstd        per-file Zstd instead of LZMA
  -fl2         per-file fast-lzma2
  -kanzi       per-file kanzi
  -kpng        per-file kanzi PNG-tuned split-stream
  -zl=<1-22>   Zstd level (default 19)

General:
  -ver         verify round-trip after processing
  -v1 / -v2    verbose output
  -np          no pause after processing
  -o           overwrite existing output files
  -r           recurse into subdirectories
  -dry         dry run (no output files written)
  -th<N>       worker threads (0 = auto, recommended for tovyCIP)
  -sfth        parallel brute-force within each file
  -od<path>    write output to directory or .ppg file (.tcip / .ppgs also accepted)
  -module      machine-friendly output
  --no-color   disable ANSI color
```

## File formats

Every output uses the `.ppg` extension; the decoder selects the right path
by reading the file's 4-byte magic, not its name.

| Output | Magic | Expansion | When you get it |
|---|---|---|---|
| **`.ppg`** | `TCIP` | **T**ovy **C**ompresor de **I**mágenes **P**NG | **PNG/APNG DEFAULT (preflate + WebP-lossless): max ratio + fast decode** |
| `.ppg`     | `TVCP` | **T**ovy **V**eloz **C**ompresor **P**NG | PNG/APNG `-fast` (kanzi+zstd): faster encode/decode, weaker ratio |
| **`.ppg`** | `TMCP` | **T**ovy **M**áximo **C**ompresor **P**NG | PNG/APNG `-preflate-max` (preflate + kanzi-TPAQX): extreme ratio |
| **`.ppg`** | `TCIJ` | **T**ovy **C**ompresor de **I**mágenes **J**NG | JNG → wrapper (v1.8+) |
| **`.ppg`** | `TCIM` | **T**ovy **C**ompresor de **I**mágenes **M**NG | MNG → whole-file preflate container (v1.9+); store-raw fallback so output never bloats |
| `.ppg`     | `PPG1` | — | Per-file packPNG (legacy v1.0–v1.4 path; opt-in via `-perfile`) |
| `.ppgs`    | `PPGS` | — | Legacy multi-entry archive (v1.4–v1.5pre) — still decodable |

The decoder still accepts the historical per-file `.ppg` versions (v1..v15) and
the `.ppgs` archives. **The v2 magic scheme drops pre-2.0 compatibility for the
solid backends**: `TCIP` now means the preflate+WebP default (old v1.x `.ppg`/`.tcip`
used `TCIP` for the kanzi backend, which is now `TVCP`). Version 2.0 will freeze the
wire format and stop breaking it. Round-trip is byte-exact for everything the
current build produces.

### TCIJ wire format (v1.8, JNG inputs)

| Offset | Size | Field |
|---:|---:|---|
| 0  | 4  | `TCIJ` magic |
| 4  | 1  | version (= 1) |
| 5  | 1  | flags (reserved, 0) |
| 6  | 2  | filename_len (LE u16) |
| 8  | N  | filename (UTF-8, original `.jng` basename) |
| …  | 4 + 4 | head raw / comp size (LE u32) |
| …  | H  | head bytes (zstd) — JNG chunks before the first JDAT/IDAT/JDAA/JSEP |
| …  | 8 + 8 | image raw / comp size (LE u64) |
| …  | I  | image bytes (zstd-19 `--long=27`) — contiguous JDAT/IDAT/JDAA/JSEP run |
| …  | 4 + 4 | tail raw / comp size (LE u32) |
| …  | T  | tail bytes (zstd) — post-image chunks incl. `IEND` |

Reconstruction is `JNG_SIG (8 B) + head + image + tail`; original chunk
length / type / data / CRC bytes survive untouched, so byte-exact roundtrip
is guaranteed regardless of how the source JNG was encoded.

### TCIM (v1.9, MNG inputs)

MNG (Multiple-image Network Graphics) is a container that embeds whole PNG,
JNG and Delta-PNG datastreams plus zlib-compressed ancillary chunks. Level-A
support treats the `.mng` as one opaque blob and runs it through the **preflate
whole-file container** (the same backend as `-preflate`): every embedded
deflate/zlib stream is undone to plain bytes + byte-exact corrections, then the
whole thing is recompressed. The wire format is identical to `TCIP` — only the
4-byte magic differs (`TCIM`):

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | `TCIM` magic |
| 4 | 1 | version (= 1) |
| 5 | 1 | flags — bit0: payload is stored raw (no preflate) |
| 6 | 2 | filename_len (LE u16) |
| 8 | N | filename (UTF-8, original `.mng` basename) |
| … | 8 | original size (LE u64) |
| … | 8 | payload size (LE u64) |
| … | P | payload (preflate container, or raw original if flags bit0 set) |

If the preflate container fails or doesn't beat the original (e.g. an MNG that
is mostly already-compressed JPEG), the **store-raw fallback** kicks in
(flags bit0 = 1, payload = original bytes), so the output never bloats beyond
the ~31-byte header. Measured on real MNGs: `fire6.mng` 40.7 %, `abydos.mng`
71.8 %, `input.mng` 83.5 %, `EXAMPLE2.MNG` 94.3 % — all byte-exact round-trip.

## Robustness

- 162/162 PngSuite valid PNGs round-trip byte-exact (per-file + tovyCIP archive).
- 14/14 PngSuite intentionally-corrupt PNGs handled gracefully — no crashes, no hangs.
- 1,500+ fuzz trials of bit-flipped / truncated / appended / fully-random tovyCIP archive inputs — zero AddressSanitizer errors.
- Encode is deterministic (5× same input → identical SHA-256), and 3 concurrent encoders produce identical output (race-free).

## Targets

Linux x86-64, Windows 10/11 x86-64.

The Windows `.exe` is a **mingw-w64 cross-compiled, statically linked** binary without Authenticode signing. Some AVs may flag it as a generic heuristic (e.g. `Trojan:Win32/Wacatac`) — **false positive**. Verify on [VirusTotal](https://www.virustotal.com/) or build from source. If Defender quarantines it, submit as false positive at [Microsoft's portal](https://www.microsoft.com/en-us/wdsi/filesubmission).

## License

MIT — see `LICENSE`.
