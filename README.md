# packPNG

**Lossless PNG/APNG/JNG recompressor.** Each input image gets its own `.ppg`
output via the **tovyCIP** backend (kanzi BWT + zstd-19 with `--long=27`),
which beats `xz -m6` on size, encode time and decode time on real PNG corpora
— while staying byte-exact reversible to the original file.

```bash
packPNG a image.png        # → image.ppg     (PNG  → tovyCIP, TCIP magic)
packPNG a animation.apng   # → animation.ppg (APNG → tovyCIP, TCIP magic)
packPNG a image.jng        # → image.ppg     (JNG  → TCIJ wrapper)   [v1.8+]
packPNG x image.ppg        # extract back to the original byte-exact file
packPNG a -r -od out/ src/ # recurse, write all outputs into out/
```

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
  -od<path>    write output to directory or .ppg file (.tcip / .ppgs also accepted)
  -module      machine-friendly output
  --no-color   disable ANSI color
```

## File formats

Every output uses the `.ppg` extension; the decoder selects the right path
by reading the file's 4-byte magic, not its name.

| Output | Magic | When you get it |
|---|---|---|
| **`.ppg`** | `TCIP` | **PNG/APNG → tovyCIP (default since v1.6)** |
| **`.ppg`** | `TCIJ` | **JNG → TCIJ wrapper (v1.8+)** |
| `.ppg`     | `PPG1` | Per-file packPNG (legacy v1.0–v1.4 path; opt-in via `-perfile`) |
| `.tcip`    | `TCIP` | Legacy multi-entry tovyCIP archive (v1.5–v1.6) — still decodable |
| `.ppgs`    | `PPGS` | Legacy multi-entry archive (v1.4–v1.5pre) — still decodable |

The decoder accepts every historical `.ppg` version (v1..v15), every `.ppgs`
archive, every `.tcip` archive produced by earlier releases, and the new
`TCIJ` wrapper introduced in v1.8. Round-trip is byte-exact for all of them.

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
