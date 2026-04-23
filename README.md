# packPNG

Lossless PNG/APNG recompressor. Stores images in `.ppg` format using brute-force zlib parameter matching + solid LZMA compression.

## How it works

1. **Parse** PNG/APNG into frames and inflate each IDAT stream to raw pixels
2. **Brute-force** all zlib parameter combinations (level, strategy, window bits, memlevel) to find the one that reproduces the original deflate stream byte-exactly
3. **Separate** PNG filter bytes from pixel data (one filter byte per scanline interleaves with pixel bytes — separating them improves LZMA's LZ77 matching)
4. **Compress** all frames together in one solid LZMA block (cross-frame redundancy)
5. Output: `.ppg` file, fully reversible back to the original PNG/APNG

With `-ldf`: frames that don't match any zlib parameter combination are stored as raw pixels and re-encoded with libdeflate at decompression time (pixel-exact, not byte-exact).

## Benchmarks

Tested on 20 real-world PNGs (1.97 MB total):

| Mode | Output size | Ratio |
|------|-------------|-------|
| default | 1,452 KB | 73.8% |
| `-ldf` | 1,356 KB | 68.9% |

## Build

**Dependencies:** zlib, liblzma, (optional) libdeflate

```bash
# Standard build
make

# With libdeflate support (faster inflate + -ldf mode)
make ldf
```

**Linux (Debian/Ubuntu):**
```bash
sudo apt install zlib1g-dev liblzma-dev libdeflate-dev
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
packPNG x archive.ppg                # decompress
packPNG l archive.ppg                # inspect without decompressing
```

## PPG format versions

| Version | Features |
|---------|----------|
| v1 | single-frame, per-frame LZMA |
| v2 | multi-frame APNG, per-frame LZMA |
| v3 | solid LZMA (all frames in one block) |
| v4 | + filter-byte separation before LZMA |
| v5 | + mode 2: libdeflate pixel-exact repack for unmatched frames |

All versions are decompressible by the current binary.

## Targets

Linux x86-64, Windows 10/11 x86-64.

## License

MIT
