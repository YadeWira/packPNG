# packPNG

**Lossless PNG/APNG recompressor.** When you pack any number of PNGs, the new **tovyCIP** archive format (`.tcip`) beats `xz -m6` on size, encode time and decode time on real corpora — while staying byte-exact reversible to the original PNG files.

```bash
packPNG a image.png        # → image.tcip   (1-entry archive)
packPNG a *.png            # → archive.tcip (multi-PNG archive)
packPNG x archive.tcip     # extract back to byte-exact .png files
packPNG a -perfile *.png   # legacy: produce one .ppg per .png
```

## Benchmarks vs xz preset 6

23-PNG corpus, ~10.5 MB raw, AMD A8-6600K, 40-trial interleaved.

| metric | xz `-m6` | packPNG (tovyCIP) | Δ |
|---|---:|---:|---:|
| **size** | 1,722,720 B | **1,720,912 B** | **−1,808 B** ✓ |
| **encode time** | 1.43 s | **0.70 s** | **−51 %** ✓ |
| **decode (single thread)** | 0.096 s | **0.072 s** | **−25 %** ✓ |
| **decode (multi-thread)** median | 0.050 s | **~0.054 s** | within noise (±5 ms variance) |

Single-file too: for `Cspeed.png` (~70 KB raw), the 1-entry `.tcip` is **23,185 B vs 25,477 B** for legacy `.ppg` (`−2,292 B`, −9 %). The kanzi BWT pipeline beats LZMA-6 even on a single file — no archive-framing penalty in practice.

`-kpng-max` (TPAQ) trades decode speed for max ratio: **−8,914 B** vs xz, but ~2.8× slower decode. Use only when archive size matters more than read latency.

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

## Usage

```
packPNG [subcommand] [flags] file(s)

Subcommands:
  a            compress only
  x            decompress only
  mix          both directions (default)
  l / list     inspect .ppg files

tovyCIP archive flags:
  -tovycip     force tovyCIP archive mode (default for any PNG count)
  -tcip        alias of -tovycip
  -solid       legacy alias of -tovycip
  -perfile     opt out → traditional per-file .ppg output
  -kpng-max    tovyCIP + TPAQ entropy: max ratio, slow decode

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
  -od<path>    write output to directory or .tcip file
  -module      machine-friendly output
  --no-color   disable ANSI color
```

## File formats

| Format | Magic | Role |
|---|---|---|
| **`.tcip`** | `TCIP` | **tovyCIP archive — default output for any PNG count** |
| `.ppg` | `PPG1` | Legacy per-file (opt-in via `-perfile`) |
| `.ppgs` | `PPGS` | Legacy archive — still decodable, no longer produced |

The decoder accepts every historical `.ppg` version (v1..v15) and every `.ppgs` archive produced by earlier releases. Round-trip is byte-exact for all of them.

## Robustness

- 162/162 PngSuite valid PNGs round-trip byte-exact (per-file + tovyCIP archive).
- 14/14 PngSuite intentionally-corrupt PNGs handled gracefully — no crashes, no hangs.
- 1,500+ fuzz trials of bit-flipped / truncated / appended / fully-random `.tcip` inputs — zero AddressSanitizer errors.
- Encode is deterministic (5× same input → identical SHA-256), and 3 concurrent encoders produce identical output (race-free).

## Targets

Linux x86-64, Windows 10/11 x86-64.

The Windows `.exe` is a **mingw-w64 cross-compiled, statically linked** binary without Authenticode signing. Some AVs may flag it as a generic heuristic (e.g. `Trojan:Win32/Wacatac`) — **false positive**. Verify on [VirusTotal](https://www.virustotal.com/) or build from source. If Defender quarantines it, submit as false positive at [Microsoft's portal](https://www.microsoft.com/en-us/wdsi/filesubmission).

## License

MIT — see `LICENSE`.
