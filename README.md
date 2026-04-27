# packPNG

**v1.0 — stable release.** Lossless PNG/APNG recompressor. Stores images in `.ppg` format using brute-force zlib parameter matching + solid LZMA (or Zstd) compression.

The `.ppg` container format (versions v3–v7) is now stable. Future v1.x releases will keep all existing `.ppg` files decodable.

## How it works

1. **Parse** PNG/APNG into frames and inflate each IDAT stream to raw pixels
2. **Brute-force** all zlib parameter combinations (level, strategy, window bits, memlevel) to find the one that reproduces the original deflate stream byte-exactly. Bailouts: O(1) zlib-header pre-filter (CMETHOD/FDICT/checksum), early-out after K=8 consecutive probe failures, cap of 20 full-deflate calls per IDAT.
3. **Separate** PNG filter bytes from pixel data (one filter byte per scanline interleaves with pixel bytes — separating them improves LZMA's LZ77 matching)
4. **Compress** all frames together in one solid block — LZMA by default (image-tuned `lc=4`), or Zstd with `-zstd`
5. Output: `.ppg` file, fully reversible back to the original PNG/APNG (byte-exact)

With `-ldf`: frames that don't match any zlib parameter combination are stored as raw pixels and re-encoded with libdeflate at decompression time (pixel-exact, not byte-exact).

## Benchmarks

Tested on a 30 MB corpus of 63 real-world PNGs:

| Mode | Output size | Ratio |
|------|-------------|-------|
| default (LZMA) | 20.0 MB | 69.5% |
| `-zstd` | 20.4 MB | 70.5% |

Speed bottleneck is brute-force deflate matching, dominated by `deflateInit2` cost per candidate and full-deflate verification of the pixel buffer. For PNGs from non-standard encoders (zlib-ng, libdeflate, oxipng, zopfli) where no match is found, frames fall back to `[stored]` mode — the IDAT is kept as-is and only LZMA-compressed.

## Build

**Dependencies:** zlib, liblzma, (optional) libdeflate, (optional) libzstd

```bash
make            # standard (LZMA only)
make ldf        # with libdeflate (faster inflate + -ldf mode)
make zstd       # with Zstd (-zstd flag, PPG v6/v7 format)
```

**Linux (Debian/Ubuntu):**
```bash
sudo apt install zlib1g-dev liblzma-dev libdeflate-dev libzstd-dev
make ldf
```

**Windows cross-compile (from Linux):**
```bash
sudo apt install mingw-w64 libz-mingw-w64-dev
# Build liblzma for Windows (see Makefile comments for full instructions)
make win
```

## Usage

```
packPNG [subcommand] [flags] file(s)

Subcommands:
  a            compress only  (.png/.apng -> .ppg)
  x            decompress only (.ppg -> .png/.apng)
  mix          both directions (default)
  l / list     inspect .ppg files

Flags:
  -ver         verify round-trip after processing
  -v1 / -v2    verbose output
  -np          no pause after processing
  -o           overwrite existing output files
  -r           recurse into subdirectories
  -dry         dry run (no output files written)
  -m<1-9>      LZMA preset (default 6)
  -me          LZMA extreme flag (slower, better ratio)
  -ldf         libdeflate pixel-exact fallback for unmatched frames (PPG v5)
  -zstd        use Zstd instead of LZMA (PPG v6/v7)
  -zl=<1-22>   Zstd level (default 19, requires -zstd)
  -th<N>       N file-level threads (0 = auto)
  -sfth        parallel brute-force within each file
  -od<path>    write output to directory
  -module      machine-friendly output
  --no-color   disable ANSI color
```

**Examples:**
```bash
packPNG a -ver -o image.png          # compress, verify, overwrite
packPNG a -ldf -me -o image.png      # max compression
packPNG a -th4 -od out/ *.png        # parallel batch to output dir
packPNG a -zstd image.png            # solid Zstd block instead of LZMA
packPNG x archive.ppg                # decompress
packPNG l archive.ppg                # inspect without decompressing
```

## PPG format versions

| Version | Compressor | Features |
|---------|------------|----------|
| v1 | LZMA | single-frame, per-frame block |
| v2 | LZMA | multi-frame APNG, per-frame block |
| v3 | LZMA | solid LZMA (all frames in one block) |
| v4 | LZMA | + filter-byte separation before LZMA |
| v5 | LZMA | + mode 2: libdeflate pixel-exact repack for unmatched frames |
| v6 | Zstd | solid Zstd + filter separation (alternative to v4) |
| v7 | Zstd | solid Zstd + filter separation + mode 2 (alternative to v5) |

All versions are decompressible by the current binary.

## Targets

Linux x86-64, Windows 10/11 x86-64.

## Notes for Windows users

- The `.exe` is a **mingw-w64 cross-compiled, statically linked** binary without an Authenticode signature. Windows Defender and other AVs may flag it as a generic heuristic detection (`Trojan:Win32/Wacatac`, `Win32/Heuristic`, etc.). This is a **false positive** — the binary contains no networking code and only reads/writes the files you point it at. You can verify on [VirusTotal](https://www.virustotal.com/) (most engines won't flag it; the few that do use generic names rather than specific signatures).
- If Defender quarantines it, you can either submit it as a false positive at [Microsoft's submission portal](https://www.microsoft.com/en-us/wdsi/filesubmission) or build from source on Windows directly (locally compiled binaries don't trigger reputation-based heuristics).
- `v1.0a` and later set the console code page to UTF-8 at startup so the banner renders correctly without needing `chcp 65001`.

## License

MIT
