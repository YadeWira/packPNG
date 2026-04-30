# packPNG

**v1.5 — tovyCIP archive (DEFAULT for >1 PNG).** Lossless PNG/APNG recompressor with the **tovyCIP** archive format ("Tovy Compresor de Imágenes PNG"), a multi-stream solid archive that achieves a **strict 4-axis WIN over xz preset 6** on a 23-PNG test corpus (size, comp, decST, dec-th0).

> Cuando empaquetas más de un PNG, packPNG genera automáticamente un único `archive.tcip` que vence a xz en los 4 ejes. Para 1 solo PNG sigue produciendo `.ppg` per-file.

## What's new in v1.5

| Feature | Status |
|---|---|
| **tovyCIP archive** (`.tcip`, magic `TCIP`) | NEW — default when packing >1 PNG |
| 2-stream parallel kanzi pixel decode | NEW |
| Auto `-th0` = `hw_concurrency - num_streams` | NEW (no thread oversubscription) |
| Encode reorder: biggest entry → stream 0 | NEW |
| `move`-not-`copy` in `emit_idat_or_fdat` | NEW (-9 ms dec-th0) |
| `PACKPNG_NO_FSEP_ABOVE` env var | NEW (opt-in, -3 ms dec-th0) |
| Legacy `.ppgs` magic | still readable at decode |
| `.ppg` per-file format (v1..v15) | unchanged, opt-in via `-perfile` |
| `-solid` flag | preserved as legacy alias of `-tovycip` |

## Strict 4-axis WIN vs xz preset 6

40-trial interleaved bench, AMD A8-6600K, corpus of 23 PNGs (10.5 MB raw):

| metric | xz `-m6` | packPNG v1.5 (`tovyCIP`) | Δ | winner |
|---|---:|---:|---:|:---:|
| size | 1,722,720 B | **1,722,197 B** | **-523 B** | ✅ tovyCIP |
| comp | 1.43 s | **0.88 s** | **-38 %** | ✅ tovyCIP |
| decST (single-thread) | 0.096 s | **0.072 s** | **-25 %** | ✅ tovyCIP |
| dec-th0 (multi-thread) median | 0.050 s | **0.048 s** | **-2 ms** | ✅ tovyCIP |

Pairwise dec-th0 (40 trials): **tovyCIP 26 / xz 7 / tie 7**. tovyCIP also has lower max (0.054 s vs xz 0.061 s) — more consistent.

## How it works

### Per-file `.ppg` mode (single PNG, or `-perfile`)

1. **Parse** PNG/APNG into frames and inflate each IDAT stream to raw pixels.
2. **Brute-force** all zlib parameter combinations (level / strategy / wbits / memlevel) to find the one that reproduces the original deflate stream byte-exactly. Bailouts: O(1) zlib-header pre-filter, early-out after K=8 consecutive probe failures, cap of 20 full-deflate calls per IDAT.
3. **Filter-byte separation**: split PNG filter bytes from pixel data (improves the LZ77 matching of the chosen backend).
4. **Compress** all frames in one solid block — LZMA by default; or `-zstd`, `-fl2`, `-kanzi` for alternative backends.
5. Output: `.ppg` file, fully reversible back to the original PNG/APNG (byte-exact).

### tovyCIP archive mode (DEFAULT, multi-PNG)

1. Run the per-file extract+match phase on each PNG to get its **pixel buffer** (mode 0/2) or **idat passthrough** (mode 1) plus per-PNG metadata (chunks, fctl, mode/level/strategy/wbits/memlevel).
2. **Sort entries by `px_sz` descending** (biggest entry first).
3. **Split into N streams** (N=2 by default): stream 0 holds the biggest entry alone (typically the bottleneck file), stream 1 holds the rest.
4. Encode each pixel stream **in parallel** with kanzi `RLT+BWT+SRT+ZRLT/FPAQ`:
   - stream 0: `block_size = 2 MB`, `jobs = 4` (kanzi internally parallelizes the multiple blocks of the dominant entry).
   - stream 1: `block_size = 4 MB`, `jobs = 4` (better cross-block ratio for many small entries).
5. Encode all idat-passthrough sections together with **zstd-19 long** (`windowLog = 27`, 128 MB window).
6. Build per-entry meta table with stream index + offset, zstd-compress it, and emit the `.tcip` archive.
7. **Decode** spawns one thread per pixel stream + one for zstd_idat (all parallel). With `-th0`, automatically picks `n_workers = hw_concurrency - num_streams` reconstruct workers — keeps total active threads ≤ core count, avoiding contention.

### Algorithms inside tovyCIP

- **RLT** — Run-Length Transform (compresses runs of repeated bytes).
- **BWT** — Burrows-Wheeler Transform (block-sort; clusters similar symbols).
- **SRT** — Sorted Rank Transform (post-BWT; better than MTF on this stage).
- **ZRLT** — Zero Run-Length Transform (compresses the long runs of zeros that SRT produces).
- **FPAQ** — Fast PAQ (order-0 binary arithmetic coder; bit-adaptive).
- **zstd-19 long** — for the IDAT-passthrough section, where high entropy of original deflate streams plays to zstd's long-range matcher.

## Benchmarks

Real-world corpus (23 PNGs, ~10.5 MB raw):

| Mode | Output size | Ratio | Notes |
|---|---:|---:|---|
| **`tovyCIP` (default)** | **1,720,912 B** | 16.4 % | strict 4-axis WIN over xz preset 6 |
| `xz -m6` (per-file LZMA) | 1,722,720 B | 16.4 % | baseline |
| `-kpng-max` (TPAQ ratio mode) | 1,713,806 B | 16.3 % | -8.9 KB but 2.8× slower decode |
| `-perfile` (legacy LZMA-6) | 1,722,720 B | 16.4 % | per-file `.ppg` output |

## Build

**Dependencies:** zlib, liblzma; **strongly recommended** (for tovyCIP): libzstd, libdeflate, kanzi-cpp.

```bash
# Full feature build (recommended — needed for tovyCIP)
g++ -std=c++17 -O3 -funroll-loops -fomit-frame-pointer -march=native \
    -DUSE_KANZI -DUSE_ZSTD -DUSE_LIBDEFLATE \
    -I<path-to-kanzi-cpp>/src \
    -o packPNG source/packpng.cpp \
    -lz -llzma -lzstd -ldeflate \
    <path-to-kanzi-cpp>/build/libkanzi.a \
    -lpthread

# Minimal build (per-file LZMA only — no tovyCIP)
make
```

**Linux (Debian/Ubuntu):**
```bash
sudo apt install zlib1g-dev liblzma-dev libdeflate-dev libzstd-dev
# Plus build kanzi-cpp from https://github.com/flanglet/kanzi-cpp
```

## Usage

```
packPNG [subcommand] [flags] file(s)

Subcommands:
  a            compress only
  x            decompress only
  mix          both directions (default)
  l / list     inspect .ppg files

tovyCIP archive flags (NEW in v1.5):
  -tovycip     force tovyCIP archive mode (default when >1 PNG)
  -tcip        alias of -tovycip
  -solid       legacy alias of -tovycip
  -perfile     opt out: produce traditional per-file .ppg output
  -kpng-max    tovyCIP + max-ratio TPAQ pipeline (8.9 KB smaller, 2.8× slower decode)

Per-file flags (apply to .ppg mode):
  -m<1-9>      LZMA preset (default 6)
  -me          LZMA extreme flag
  -deep        disable brute-force early-out (~2× slower, ~6 % smaller)
  -ldf         libdeflate pixel-exact fallback for unmatched frames (PPG v5)
  -zstd        per-file Zstd instead of LZMA (PPG v6/v7)
  -fl2         per-file fast-lzma2 (PPG v8/v9)
  -kanzi       per-file kanzi (PPG v10/v11)
  -kpng        per-file kanzi PNG-tuned split-stream (PPG v14/v15)
  -zl=<1-22>   Zstd level (default 19, requires -zstd)

General:
  -ver         verify round-trip after processing
  -v1 / -v2    verbose output
  -np          no pause after processing
  -o           overwrite existing output files
  -r           recurse into subdirectories
  -dry         dry run (no output files written)
  -th<N>       file-level threads (0 = auto, recommended for tovyCIP)
  -sfth        parallel brute-force within each file
  -od<path>    write output to directory or .tcip file
  -module      machine-friendly output
  --no-color   disable ANSI color
```

**Examples:**
```bash
packPNG a *.png                         # auto-tovyCIP → archive.tcip
packPNG a image.png                     # single file → image.ppg
packPNG a -tcip *.png                   # explicit tovyCIP
packPNG a -perfile *.png                # force traditional .ppg per file
packPNG a -kpng-max *.png               # max ratio (TPAQ), slower decode
packPNG a -ver -o image.png             # compress, verify, overwrite
packPNG a -th0 -od out/ *.png           # auto-thread batch to directory
packPNG x archive.tcip                  # decompress tovyCIP archive
packPNG x archive.ppgs                  # legacy archive (still works)
packPNG l image.ppg                     # inspect a per-file .ppg
```

## File formats

| Format | Magic | Use |
|---|---|---|
| **`.tcip`** | `TCIP` | tovyCIP archive (multi-PNG, default) |
| `.ppgs` | `PPGS` | legacy archive (still decoded by v1.5) |
| `.ppg` | `PPG1` | per-file (single PNG); versions v1..v15 below |

### Per-file `.ppg` versions (all decodable by v1.5)

| Version | Compressor | Features |
|---|---|---|
| v1 / v2 | LZMA | single / multi-frame, per-frame block |
| v3 / v4 / v5 | LZMA | solid + filter sep + libdeflate fallback |
| v6 / v7 | Zstd | alternative to v4 / v5 |
| v8 / v9 | fast-lzma2 | alternative to v4 / v5 |
| v10 / v11 | kanzi (single-stream) | BWT+CM backend |
| v12 / v13 | kanzi (legacy split) | v1.4 only |
| v14 / v15 | kanzi (current split) | RLT+BWT+SRT+ZRLT/FPAQ pixels + zstd-19-long idat |

### tovyCIP `.tcip` versions

| Version | Notes |
|---|---|
| **v1** | 2-stream parallel kanzi pixels + zstd-19-long idat + per-entry stream_idx tag |
| (legacy v1) | same wire layout as PPGS v1 — single-stream — still readable |
| (legacy v2) | same wire layout as PPGS v2 — multi-stream — same as TCIP v1 with old magic |

## Targets

Linux x86-64, Windows 10/11 x86-64.

## Notes for Windows users

The `.exe` is a **mingw-w64 cross-compiled, statically linked** binary without Authenticode signing. Some AVs may flag it as a generic heuristic (e.g. `Trojan:Win32/Wacatac`). This is a **false positive** — packPNG has no networking code and only touches the files you point it at. You can verify on [VirusTotal](https://www.virustotal.com/), or build from source on Windows.

If Defender quarantines it, submit as false positive at [Microsoft's portal](https://www.microsoft.com/en-us/wdsi/filesubmission), or compile locally.

## License

MIT
