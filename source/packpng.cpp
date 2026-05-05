/* packPNG v1.0j - PNG/APNG lossless recompressor
 *
 * Per-frame algorithm:
 *   PNG/APNG → parse frames → inflate pixels → brute-force zlib re-encode
 *   → filter-byte separation → solid LZMA (all frames in one block) → .ppg
 *
 * PPG format v4: filter bytes separated from pixel data before LZMA
 *   → LZ77 no pierde matches por bytes de filter intercalados
 * PPG format v5: adds mode 2 (pixel-exact libdeflate repack for unmatched frames)
 *   → unmatched frames store pixels instead of raw IDAT; better LZMA ratio
 * PPG v3: solid LZMA (sin sep); v1/v2: per-frame LZMA. All readable.
 *
 * CLI mirrors packJPG v4.x.
 * Targets: Linux x86-64, Windows 10/11 x86-64.
 * Deps: zlib, liblzma [, libdeflate — compile with -DUSE_LIBDEFLATE -ldeflate].
 */

#include <algorithm>
#include <numeric>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <istream>
#include <map>
#include <mutex>
#include <streambuf>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <zlib.h>
#include <lzma.h>

#ifdef USE_LIBDEFLATE
#  include <libdeflate.h>
#endif

#ifdef USE_ZSTD
#  include <zstd.h>
#endif

#ifdef USE_FL2
#  include <fast-lzma2.h>
#endif

#ifdef USE_KANZI
#  include <sstream>
#  include "io/CompressedOutputStream.hpp"
#  include "io/CompressedInputStream.hpp"
#endif

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

#if defined(_WIN32)
#  include <windows.h>
#  include <fcntl.h>
#  include <io.h>
#endif

/* ─── version ────────────────────────────────────────────────────────────── */

static const char* subversion = "a";  // letra = bugfix-only; sin letra = feature
static const char* author     = "Yade Bravo (YadeWira)";
static const int   ver_major  = 1;    // v1.8a — bugfix release for the v1.8 line. The Windows binary used to close its console window on the help path (no-args / empty-filelist) and on every error-return path because those returned from main() without going through the existing "Press <enter> to quit" wait. Reported by xman on encode.su. Fixed by routing every user-visible exit through a wait_and_return() helper that respects -np/-module like before.
static const int   ver_minor  = 8;

/* ─── constants ──────────────────────────────────────────────────────────── */

static const uint8_t PNG_SIG[8] = {0x89,'P','N','G','\r','\n',0x1a,'\n'};
static const uint8_t PPG_SIG[4] = {'P','P','G','1'};
// tovyCIP — Tovy Compresor de Imágenes PNG. Multi-stream PNG archive.
// STRICT 4-axis WIN over xz preset 6 (size, comp, decST, dec-th0).
static const uint8_t TCIP_SIG[4] = {'T','C','I','P'};
// Legacy magic — decoder still accepts PPGS archives from v1.4-v1.5pre.
static const uint8_t PPGS_SIG[4] = {'P','P','G','S'};
// JNG (JPEG Network Graphics) container support — v1.8.
// JNG_SIG mirrors PNG_SIG but with high-bit 'J': identifies a JNG input file.
// TCIJ_SIG is the magic for JNG-derived .ppg outputs (v1.7 TCIP for PNG; TCIJ
// for JNG so legacy decoders fail cleanly with "bad TCIP magic" instead of
// silently misparsing the new layout).
static const uint8_t JNG_SIG[8]  = {0x8B,'J','N','G','\r','\n',0x1a,'\n'};
static const uint8_t TCIJ_SIG[4] = {'T','C','I','J'};
static const size_t  PROBE_BYTES    = 256u << 10;  // 256 KiB — covers most PNG IDATs in one probe
static const int     MSG_SIZE       = 512;
static       int     EARLYOUT_K     = 8;   // bail after K consecutive probe failures (-deep raises it)
static const int     MAX_FULL_CALLS = 20;  // cap on expensive full_deflate calls per IDAT

/* ─── global options ─────────────────────────────────────────────────────── */

static bool overwrite       = false;
static bool verify          = false;
static bool recursive       = false;
static bool dry_run         = false;
static bool module_mode     = false;
static bool compress_only   = false;
static bool decompress_only = false;
static bool wait_exit       = true;
static bool no_color        = false;
static bool sfth            = false;
static bool lzma_extreme    = false;
static bool ldf_repack      = false;  // -ldf: pixel-exact libdeflate fallback for unmatched frames
static bool use_zstd        = false;  // -zstd: solid Zstd block instead of LZMA
static int  g_zstd_level    = 19;
static bool use_fl2         = false;  // -fl2: solid fast-lzma2 block (LZMA2-stream, ~2-4x faster, +1-2% size)
[[maybe_unused]]
static int  g_fl2_level     = 10;     // FL2 max compression level (10) — referenced only under USE_FL2
static bool use_kanzi       = false;  // -kanzi: solid kanzi BWT+CM block (better ratio than xz at faster encode, slow decomp)
static bool use_kpng        = false;  // -kpng: PNG-tuned kanzi pipeline (LZP+BWT / CM) — drops TEXT+UTF dead weight
static bool use_solid       = false;  // -solid: PPGS archive (single kanzi context for all PNGs, jobs=4 internal)
static bool use_kpng_max    = false;  // -kpng-max: max-ratio TPAQ pipeline (RLT/TPAQ, blk=2M, jobs=8)
static bool use_perfile     = false;  // -perfile: opt out of solid auto-default (force traditional .ppg per file)
static bool g_mode_explicit = false;  // user passed an explicit mode flag (-solid/-kpng/-zstd/-fl2/-kanzi/-perfile)
[[maybe_unused]]
static int  g_kanzi_level   = 7;      // kanzi level 7 (LZP+TEXT+UTF+BWT+LZP / CM): generic Pareto-optimal vs xz-m6
static int  verbosity       = 0;
static int  num_threads     = 1;
static bool g_threads_auto  = false;  // true if user passed -th0 (auto pick)
// Filter-sep skip threshold (bytes). Pixel buffers >= this skip filter_sep at
// encode time → no unsep_filters at decode → ~5 ms faster per big file at the
// cost of ~1 KB ratio per affected file. 0 = disabled (current default).
// Set via -fastdec flag (= 4MB), -nofsep=N, or PACKPNG_NO_FSEP_ABOVE env var.
static size_t g_nofsep_above = 0;
static int  sfth_threads    = 1;
static unsigned g_lzma_preset = 6u;
static std::string outdir;
static bool fs_mode = false;  // -fs: preserve source folder structure under -od when -r expands a dir

static std::mutex g_print_mutex;

static int err_tol = 1;
thread_local char errormessage[MSG_SIZE] = "no error";

/* ─── file list ──────────────────────────────────────────────────────────── */

struct CollectedFile {
    std::string path;       // full path to the input file
    std::string src_root;   // root the user passed on the CLI (for -fs structure preserve)
                            //   empty when the file came from a direct path arg (no recurse)
};
static std::vector<CollectedFile> filelist;

/* ─── accumulators (atomic for -th safety) ───────────────────────────────── */

static std::atomic<int>    g_processed{0};
static std::atomic<int>    g_errors{0};
static std::atomic<double> g_acc_in{0.0};
static std::atomic<double> g_acc_out{0.0};

/* ─── progress bar (active only at v0 with >1 file in non-module mode) ───── */

static const int BARLEN = 36;
static std::atomic<int> g_files_done{0};
static std::atomic<int> g_spinner_idx{0};
static int  g_total_files = 0;
static bool g_show_bar    = false;

// Forward declarations — implementations after process_file
static void clear_bar();
static void draw_bar(int done);
static void finish_bar();

/* ─── color ──────────────────────────────────────────────────────────────── */

#if defined(_WIN32)
static void init_colors() {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        if (GetConsoleMode(h, &mode))
            SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
}
#else
static void init_colors() {}
#endif

static inline const char* col(const char* c) { return no_color ? "" : c; }
static const char* R  = "\x1b[0m";
static const char* BC = "\x1b[1;36m";
static const char* GR = "\x1b[32m";
static const char* RD = "\x1b[31m";
static const char* YL = "\x1b[33m";

static const char* strategy_name(int st) {
    switch (st) {
        case Z_DEFAULT_STRATEGY: return "default";
        case Z_FILTERED:         return "filtered";
        case Z_HUFFMAN_ONLY:     return "huffman";
        case Z_RLE:              return "rle";
        default:                 return "?";
    }
}

/* ─── endian helpers ─────────────────────────────────────────────────────── */

static inline uint32_t rd_be32(const uint8_t* p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
static inline uint32_t rd_le32(const uint8_t* p) {
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static inline uint64_t rd_le64(const uint8_t* p) {
    uint64_t v=0; for(int i=7;i>=0;i--) v=(v<<8)|p[i]; return v;
}
static inline void wr_be32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((x>>24)&0xFF); v.push_back((x>>16)&0xFF);
    v.push_back((x>>8)&0xFF);  v.push_back(x&0xFF);
}
static inline void wr_le32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(x&0xFF); v.push_back((x>>8)&0xFF);
    v.push_back((x>>16)&0xFF); v.push_back((x>>24)&0xFF);
}
static inline void wr_le64(std::vector<uint8_t>& v, uint64_t x) {
    for(int i=0;i<8;i++){v.push_back(x&0xFF);x>>=8;}
}
static inline void wr_bytes(std::vector<uint8_t>& v, const uint8_t* d, size_t n) {
    v.insert(v.end(), d, d+n);
}

/* ─── CRC-32 ─────────────────────────────────────────────────────────────── */

static uint32_t chunk_crc(const uint8_t* type, const uint8_t* data, size_t dlen) {
    uLong c = crc32(0L, Z_NULL, 0);
    c = crc32(c, type, 4);
    if (dlen > 0) c = crc32(c, data, (uInt)dlen);
    return (uint32_t)c;
}

/* ─── PNG chunk ──────────────────────────────────────────────────────────── */

struct PngChunk {
    uint32_t             length;
    uint8_t              type[4];
    std::vector<uint8_t> data;
    uint32_t             crc;
};

static bool chunk_is(const PngChunk& c, const char* t) {
    return memcmp(c.type, t, 4) == 0;
}

static std::vector<uint8_t> chunk_bytes(const PngChunk& c) {
    std::vector<uint8_t> out;
    wr_be32(out, c.length);
    wr_bytes(out, c.type, 4);
    wr_bytes(out, c.data.data(), c.data.size());
    wr_be32(out, c.crc);
    return out;
}

/* ─── PNG/APNG frame ─────────────────────────────────────────────────────── */

struct PngFrame {
    std::vector<uint8_t> fctl;      // 26-byte fcTL data, empty if none
    bool     uses_idat = true;
    uint32_t first_seq = 0;
    std::vector<uint32_t> chunk_szs;
    std::vector<uint8_t>  idat_raw;
    std::vector<uint8_t>  pixels;   // inflated filtered scanlines
};

struct PngInfo {
    bool     is_apng   = false;
    uint32_t num_plays = 0;
    std::vector<PngFrame> frames;
    std::vector<uint8_t>  pre;
    std::vector<uint8_t>  post;
};

/* ─── IHDR helpers ───────────────────────────────────────────────────────── */

struct IhdrInfo {
    uint32_t width = 0, height = 0;
    uint8_t  bit_depth = 8, color_type = 2;
};

// Returns bpp (bytes per pixel) for filter-byte separation. Returns 0 when
// separation doesn't make sense (palette, sub-byte depth, etc.).
static uint32_t compute_bpp(uint8_t color_type, uint8_t bit_depth) {
    if (bit_depth != 8 && bit_depth != 16) return 0;  // sub-byte or unusual
    uint32_t b = (bit_depth == 16) ? 2 : 1;
    switch (color_type) {
        case 0: return b;      // grayscale
        case 2: return 3 * b;  // RGB
        case 4: return 2 * b;  // grayscale+alpha
        case 6: return 4 * b;  // RGBA
        default: return 0;     // palette (type 3) or unknown
    }
}

// Parse IHDR from pre-chunk bytes (PNG_SIG already stripped).
// pre = raw chunk bytes starting with IHDR chunk.
static IhdrInfo parse_ihdr(const std::vector<uint8_t>& pre) {
    IhdrInfo info;
    // IHDR: 4 (length) + 4 (type) + 13 (data) + 4 (crc) = 25 bytes minimum
    if (pre.size() < 25) return info;
    // first 4 bytes = length, next 4 = "IHDR"
    if (memcmp(pre.data() + 4, "IHDR", 4) != 0) return info;
    const uint8_t* d = pre.data() + 8;  // IHDR data
    info.width      = rd_be32(d);
    info.height     = rd_be32(d + 4);
    info.bit_depth  = d[8];
    info.color_type = d[9];
    return info;
}

// Get frame dimensions: from fcTL if present, else from IHDR.
static void frame_dims(const PngFrame& fr, const IhdrInfo& base,
                       uint32_t& out_w, uint32_t& out_h) {
    if (fr.fctl.size() >= 12) {
        out_w = rd_be32(fr.fctl.data() + 4);
        out_h = rd_be32(fr.fctl.data() + 8);
    } else {
        out_w = base.width;
        out_h = base.height;
    }
}

/* ─── filter-byte separation ─────────────────────────────────────────────── */

// Separate filter bytes (1 per scanline) from pixel bytes.
// pixels = [filter][stride bytes][filter][stride bytes]...
// Output: filter_bytes (n_scanlines), px_only (n_scanlines × stride).
static bool sep_filters(const std::vector<uint8_t>& pixels, uint32_t stride,
                        std::vector<uint8_t>& filter_bytes,
                        std::vector<uint8_t>& px_only)
{
    if (stride == 0) return false;
    size_t row_bytes = stride + 1;
    if (pixels.size() % row_bytes != 0) return false;
    size_t n = pixels.size() / row_bytes;
    filter_bytes.resize(n);
    px_only.resize(n * stride);
    for (size_t r = 0; r < n; r++) {
        filter_bytes[r] = pixels[r * row_bytes];
        memcpy(px_only.data() + r * stride,
               pixels.data() + r * row_bytes + 1, stride);
    }
    return true;
}

// Reconstruct pixels from separated filter bytes + pixel bytes.
static bool unsep_filters(const uint8_t* filter_bytes, size_t n_scanlines,
                           const uint8_t* px_only, size_t stride,
                           std::vector<uint8_t>& pixels)
{
    if (stride == 0 || n_scanlines == 0) return false;
    size_t row_bytes = stride + 1;
    pixels.resize(n_scanlines * row_bytes);
    for (size_t r = 0; r < n_scanlines; r++) {
        pixels[r * row_bytes] = filter_bytes[r];
        memcpy(pixels.data() + r * row_bytes + 1,
               px_only + r * stride, stride);
    }
    return true;
}

/* ─── inflate one frame ──────────────────────────────────────────────────── */

static bool inflate_frame(PngFrame& fr) {
#ifdef USE_LIBDEFLATE
    // libdeflate inflate is ~1.4× faster than zlib; try it first
    libdeflate_decompressor* d = libdeflate_alloc_decompressor();
    if (d) {
        size_t out_cap = fr.idat_raw.size() * 5;
        fr.pixels.resize(out_cap);
        size_t actual = 0;
        libdeflate_result r = LIBDEFLATE_INSUFFICIENT_SPACE;
        for (int tries = 0; tries < 5 && r == LIBDEFLATE_INSUFFICIENT_SPACE; tries++) {
            r = libdeflate_zlib_decompress(d,
                    fr.idat_raw.data(), fr.idat_raw.size(),
                    fr.pixels.data(), fr.pixels.size(), &actual);
            if (r == LIBDEFLATE_INSUFFICIENT_SPACE) {
                out_cap *= 2;
                fr.pixels.resize(out_cap);
            }
        }
        libdeflate_free_decompressor(d);
        if (r == LIBDEFLATE_SUCCESS) { fr.pixels.resize(actual); return true; }
        fr.pixels.clear();
        // fall through to zlib
    }
#endif
    z_stream zs{};
    if (inflateInit(&zs) != Z_OK) {
        snprintf(errormessage, MSG_SIZE, "inflateInit failed"); return false;
    }
    zs.next_in  = fr.idat_raw.data();
    zs.avail_in = (uInt)fr.idat_raw.size();
    fr.pixels.resize(fr.idat_raw.size() * 4);
    size_t tot = 0; int ret;
    do {
        if (tot >= fr.pixels.size()) fr.pixels.resize(fr.pixels.size() * 2);
        zs.next_out  = fr.pixels.data() + tot;
        zs.avail_out = (uInt)(fr.pixels.size() - tot);
        ret = inflate(&zs, Z_NO_FLUSH);
        tot = zs.total_out;
    } while (ret == Z_OK && zs.avail_in > 0);
    inflateEnd(&zs);
    if (ret != Z_STREAM_END && ret != Z_OK) {
        snprintf(errormessage, MSG_SIZE, "inflate failed: %s", zs.msg ? zs.msg : "?");
        return false;
    }
    fr.pixels.resize(tot);
    return true;
}

/* ─── PNG/APNG parser ────────────────────────────────────────────────────── */

static bool parse_png(const std::vector<uint8_t>& buf, PngInfo& info) {
    if (buf.size() < 8 || memcmp(buf.data(), PNG_SIG, 8) != 0) {
        snprintf(errormessage, MSG_SIZE, "bad PNG signature"); return false;
    }

    std::vector<PngChunk> chunks;
    size_t pos = 8;
    while (pos + 12 <= buf.size()) {
        PngChunk c;
        c.length = rd_be32(buf.data() + pos); pos += 4;
        memcpy(c.type, buf.data() + pos, 4);  pos += 4;
        if (pos + c.length + 4 > buf.size()) {
            snprintf(errormessage, MSG_SIZE, "truncated chunk %.4s", c.type); return false;
        }
        c.data.assign(buf.data() + pos, buf.data() + pos + c.length);
        pos += c.length;
        c.crc = rd_be32(buf.data() + pos); pos += 4;
        uint32_t chk = chunk_crc(c.type, c.data.data(), c.data.size());
        if (chk != c.crc) {
            snprintf(errormessage, MSG_SIZE, "CRC mismatch %.4s", c.type); return false;
        }
        chunks.push_back(std::move(c));
        if (chunk_is(chunks.back(), "IEND")) break;
    }

    enum State { BEFORE, IN_IDAT, IN_FDAT } state = BEFORE;
    PngFrame cur;
    cur.uses_idat = true;
    std::vector<uint8_t> post_acc;

    for (auto& c : chunks) {
        if (chunk_is(c, "acTL")) {
            info.is_apng = true;
            if (c.data.size() >= 8) info.num_plays = rd_be32(c.data.data() + 4);
            auto cb = chunk_bytes(c);
            info.pre.insert(info.pre.end(), cb.begin(), cb.end());

        } else if (chunk_is(c, "fcTL")) {
            if (state == BEFORE) {
                cur.fctl = c.data;
            } else {
                if (!inflate_frame(cur)) return false;
                info.frames.push_back(std::move(cur));
                cur = PngFrame();
                cur.fctl      = c.data;
                cur.uses_idat = false;
                cur.first_seq = (c.data.size() >= 4) ? rd_be32(c.data.data()) + 1 : 0;
                state = IN_FDAT;
            }

        } else if (chunk_is(c, "IDAT")) {
            if (state == BEFORE) state = IN_IDAT;
            cur.chunk_szs.push_back(c.length);
            cur.idat_raw.insert(cur.idat_raw.end(), c.data.begin(), c.data.end());

        } else if (chunk_is(c, "fdAT")) {
            if (state == BEFORE) { state = IN_FDAT; cur.uses_idat = false; }
            if (c.data.size() < 4) {
                snprintf(errormessage, MSG_SIZE, "fdAT too short"); return false;
            }
            uint32_t seq = rd_be32(c.data.data());
            if (cur.chunk_szs.empty()) cur.first_seq = seq;
            uint32_t psz = c.length - 4;
            cur.chunk_szs.push_back(psz);
            cur.idat_raw.insert(cur.idat_raw.end(), c.data.begin() + 4, c.data.end());

        } else if (chunk_is(c, "IEND")) {
            if (state != BEFORE) {
                if (!inflate_frame(cur)) return false;
                info.frames.push_back(std::move(cur));
            }
            auto cb = chunk_bytes(c);
            info.post = post_acc;
            info.post.insert(info.post.end(), cb.begin(), cb.end());

        } else {
            auto cb = chunk_bytes(c);
            if (state == BEFORE)
                info.pre.insert(info.pre.end(), cb.begin(), cb.end());
            else
                post_acc.insert(post_acc.end(), cb.begin(), cb.end());
        }
    }

    if (info.frames.empty()) {
        snprintf(errormessage, MSG_SIZE, "no image frames found"); return false;
    }
    return true;
}

/* ─── brute-force zlib re-encode ─────────────────────────────────────────── */

struct DeflParams { int level, strategy, memlevel, wbits; };

// Probe result: 0 = no match, 1 = partial match (need full_deflate), 2 = complete match
// (probe encoded the full stream and verified all bytes — caller skips full_deflate).
static int probe_deflate(const uint8_t* px, size_t pxsz,
                         const uint8_t* tgt, size_t tgtsz,
                         int lv, int st, int wbits, int memlevel,
                         std::vector<uint8_t>& tmp)
{
    static const size_t QUICK = 256;
    z_stream zs{};
    if (deflateInit2(&zs, lv, Z_DEFLATED, wbits, memlevel, st) != Z_OK) return 0;
    tmp.resize(PROBE_BYTES + 128);
    zs.next_in   = const_cast<uint8_t*>(px);
    zs.avail_in  = (uInt)pxsz;

    // Stage 1: produce up to QUICK bytes, bail on prefix mismatch (cheap reject).
    zs.next_out  = tmp.data();
    zs.avail_out = (uInt)QUICK;
    int r = deflate(&zs, Z_FINISH);
    size_t produced = (size_t)zs.total_out;
    if (produced >= 3) {
        size_t cmp = std::min(produced, std::min(tgtsz, QUICK));
        if (memcmp(tmp.data(), tgt, cmp) != 0) { deflateEnd(&zs); return 0; }
    }
    bool finished = (r == Z_STREAM_END);

    // Stage 2: continue producing up to PROBE_BYTES total (only if Z_FINISH didn't end yet).
    if (!finished) {
        zs.next_out  = tmp.data() + produced;
        zs.avail_out = (uInt)(PROBE_BYTES - produced);
        r = deflate(&zs, Z_FINISH);
        produced = (size_t)zs.total_out;
        finished = (r == Z_STREAM_END);
    }
    deflateEnd(&zs);

    if (produced == 0) return 0;
    size_t cmp = std::min(produced, std::min(tgtsz, PROBE_BYTES));
    if (cmp == 0) return 0;
    if (memcmp(tmp.data(), tgt, cmp) != 0) return 0;

    // If the full stream finished within our buffer AND its length equals tgtsz AND
    // every byte we produced matches tgt — the candidate is fully verified.
    if (finished && produced == tgtsz) return 2;
    return 1;
}

static bool full_deflate(const std::vector<uint8_t>& px,
                         const std::vector<uint8_t>& tgt,
                         int lv, int st, int wbits, int memlevel,
                         std::vector<uint8_t>& out)
{
    z_stream zs{};
    if (deflateInit2(&zs, lv, Z_DEFLATED, wbits, memlevel, st) != Z_OK) return false;
    out.resize(deflateBound(&zs, px.size()) + 16);
    zs.next_in   = const_cast<uint8_t*>(px.data());
    zs.avail_in  = (uInt)px.size();
    zs.next_out  = out.data();
    zs.avail_out = (uInt)out.size();
    int r = deflate(&zs, Z_FINISH);
    size_t olen = zs.total_out;
    deflateEnd(&zs);
    return r == Z_STREAM_END && olen == tgt.size() &&
           memcmp(out.data(), tgt.data(), olen) == 0;
}

struct Candidate { int lv, st, wbits, memlevel; };

static std::vector<Candidate> build_candidates(const std::vector<uint8_t>& target) {
    if (target.size() < 2) return {};

    // O(1) zlib header validity checks — bail before any deflateInit2
    if ((target[0] & 0x0F) != 8)          return {};  // CMETHOD != deflate
    if (target[1] & 0x20)                  return {};  // FDICT=1: preset dict, unmatchable
    if ((target[0]*256u + target[1]) % 31) return {};  // invalid zlib checksum

    int cinfo  = (target[0] >> 4) & 0xF;
    int wbits0 = std::max(8, std::min(15, cinfo + 8));

    uint8_t flevel = (target[1] >> 6) & 3;
    int lv_min, lv_max;
    switch (flevel) {
        case 0: lv_min=1; lv_max=1; break;
        case 1: lv_min=2; lv_max=5; break;
        case 2: lv_min=6; lv_max=6; break;
        default:lv_min=7; lv_max=9; break;
    }

    static const int strats[] = {Z_DEFAULT_STRATEGY, Z_FILTERED, Z_HUFFMAN_ONLY, Z_RLE};
    std::vector<Candidate> c;

    auto add_sweep = [&](int wb, int ml) {
        if (lv_min <= 6 && 6 <= lv_max) c.push_back({6, Z_FILTERED, wb, ml});
        for (int lv = lv_min; lv <= lv_max; lv++)
            for (int st : strats)
                if (!(lv == 6 && st == Z_FILTERED))
                    c.push_back({lv, st, wb, ml});
    };

    add_sweep(wbits0, 8);
    add_sweep(wbits0, 9);
    for (int ml = 7; ml >= 1; ml--)
        add_sweep(wbits0, ml);
    if (wbits0 != 15) {
        add_sweep(15, 8);
        add_sweep(15, 9);
    }

    return c;
}

static bool find_deflate_params(const std::vector<uint8_t>& px,
                                const std::vector<uint8_t>& tgt,
                                DeflParams& p)
{
    auto cands = build_candidates(tgt);
    if (cands.empty()) return false;

    if (sfth_threads <= 1) {
        std::vector<uint8_t> tmp, attempt;
        int full_calls = 0, consec_fails = 0;
        for (auto& c : cands) {
            int rc = probe_deflate(px.data(), px.size(), tgt.data(), tgt.size(),
                                   c.lv, c.st, c.wbits, c.memlevel, tmp);
            if (rc == 0) {
                if (++consec_fails >= EARLYOUT_K) break;  // exotic encoder — stop probing
                continue;
            }
            consec_fails = 0;
            if (rc == 2) {                                // probe verified entire stream
                p = {c.lv, c.st, c.memlevel, c.wbits}; return true;
            }
            if (++full_calls > MAX_FULL_CALLS) break;     // cap expensive false-positive sweeps
            if (full_deflate(px, tgt, c.lv, c.st, c.wbits, c.memlevel, attempt)) {
                p = {c.lv, c.st, c.memlevel, c.wbits}; return true;
            }
        }
        return false;
    }

    // Lazy MT: most libpng-default PNGs match the first candidate. Spawning 4
    // threads costs ~5ms on Windows — worse than the single probe + full_deflate
    // for those files. Try the first candidate sequentially first; only fall
    // back to MT for the remaining candidates if it doesn't match.
    int sched_full_calls = 0, sched_consec_fails = 0;
    {
        std::vector<uint8_t> tmp, attempt;
        auto& c = cands[0];
        int rc = probe_deflate(px.data(), px.size(), tgt.data(), tgt.size(),
                               c.lv, c.st, c.wbits, c.memlevel, tmp);
        if (rc == 2) {
            p = {c.lv, c.st, c.memlevel, c.wbits}; return true;
        } else if (rc == 1) {
            sched_full_calls = 1;
            if (full_deflate(px, tgt, c.lv, c.st, c.wbits, c.memlevel, attempt)) {
                p = {c.lv, c.st, c.memlevel, c.wbits}; return true;
            }
        } else {
            sched_consec_fails = 1;
        }
    }
    if (cands.size() <= 1) return false;

    std::atomic<bool> found{false};
    std::atomic<int>  next_cand{1};                            // start at candidate 1
    std::atomic<int>  full_calls{sched_full_calls};
    std::atomic<int>  consec_fails{sched_consec_fails};
    std::mutex        result_mu;
    DeflParams        result{};
    int nw = std::min(sfth_threads, (int)cands.size() - 1);
    // Audit fix #9: in MT, consec_fails is shared across workers — ANY thread's
    // probe success resets it. So the same EARLYOUT_K threshold fires later than
    // in single-thread (where consec_fails is per-thread). Scale by nw to keep
    // roughly the same per-thread sensitivity. Avoids wasted CPU on exotic-encoder
    // images that single-thread would early-out on.
    int mt_earlyout_k = EARLYOUT_K * nw;
    std::vector<std::thread> workers;
    for (int t = 0; t < nw; t++) {
        workers.emplace_back([&, mt_earlyout_k]() {
            std::vector<uint8_t> tmp, attempt;
            while (!found
                   && consec_fails.load(std::memory_order_relaxed) < mt_earlyout_k
                   && full_calls.load(std::memory_order_relaxed) <= MAX_FULL_CALLS) {
                int idx = next_cand.fetch_add(1);
                if (idx >= (int)cands.size()) break;
                auto& c = cands[idx];
                int rc = probe_deflate(px.data(), px.size(), tgt.data(), tgt.size(),
                                       c.lv, c.st, c.wbits, c.memlevel, tmp);
                if (rc == 0) {
                    consec_fails.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                consec_fails.store(0, std::memory_order_relaxed);
                if (rc == 2) {                                  // probe already verified full stream
                    std::lock_guard<std::mutex> lk(result_mu);
                    if (!found) { result = {c.lv, c.st, c.memlevel, c.wbits}; found = true; }
                    break;
                }
                if (full_calls.fetch_add(1, std::memory_order_relaxed) >= MAX_FULL_CALLS) break;
                if (full_deflate(px, tgt, c.lv, c.st, c.wbits, c.memlevel, attempt)) {
                    std::lock_guard<std::mutex> lk(result_mu);
                    if (!found) { result = {c.lv, c.st, c.memlevel, c.wbits}; found = true; }
                }
            }
        });
    }
    for (auto& w : workers) w.join();
    if (found) { p = result; return true; }
    return false;
}

/* ─── LZMA compress / decompress ─────────────────────────────────────────── */

static bool lzma_enc(const uint8_t* in, size_t insz, std::vector<uint8_t>& out) {
    uint32_t preset = g_lzma_preset;
    if (lzma_extreme) preset |= LZMA_PRESET_EXTREME;
    // Image-tuned params: lc=4 beats default lc=3 by ~0.1% on PNG filter+pixel
    // data; lp=0 stays optimal (PNG strides aren't position-aligned). lp=0
    // is mandatory when lc=4 anyway (lc+lp <= 4).
    lzma_options_lzma opts;
    if (lzma_lzma_preset(&opts, preset)) {
        snprintf(errormessage, MSG_SIZE, "LZMA preset init failed"); return false;
    }
    opts.lc = 4;
    opts.lp = 0;
    opts.pb = 2;
    lzma_filter filters[2] = {
        { LZMA_FILTER_LZMA2, &opts },
        { LZMA_VLI_UNKNOWN,  nullptr }
    };

    // MT-LZMA path: enabled whenever -sfth is set and the total compute thread
    // count (num_threads × sfth_threads) does NOT exceed hardware_concurrency.
    // In single-file mode (1 × 4 = 4) always on. In batch (-thN -sfth) on as
    // long as N×4 ≤ hw. With -th0 -sfth on a hw-thread machine, hw×4 > hw → OFF
    // (avoids the 144-thread oversubscription on a 36-thread Xeon).
    // Block size 256K: ~2.7× single-file speedup, ~0.8% ratio cost on big files
    // (small files fit in 1 block → no penalty, no benefit either).
    int _hw     = (int)std::thread::hardware_concurrency();
    int _total  = std::max(1, num_threads) * sfth_threads;
    if (sfth_threads > 1 && (_hw <= 0 || _total <= _hw)) {
        lzma_mt mt_opts = {};
        mt_opts.flags      = 0;
        mt_opts.threads    = (uint32_t)sfth_threads;
        mt_opts.block_size = 256 * 1024;   // aggressive: 2.66× speedup, +0.76% ratio
        mt_opts.timeout    = 0;
        mt_opts.preset     = preset;
        mt_opts.filters    = filters;
        mt_opts.check      = LZMA_CHECK_CRC64;

        lzma_stream strm = LZMA_STREAM_INIT;
        if (lzma_stream_encoder_mt(&strm, &mt_opts) != LZMA_OK) {
            snprintf(errormessage, MSG_SIZE, "LZMA mt init failed"); return false;
        }
        out.resize(insz + insz / 4 + 1024);
        strm.next_in   = in;
        strm.avail_in  = insz;
        strm.next_out  = out.data();
        strm.avail_out = out.size();
        while (true) {
            lzma_ret r = lzma_code(&strm, LZMA_FINISH);
            if (r == LZMA_STREAM_END) break;
            if (r != LZMA_OK) {
                lzma_end(&strm);
                snprintf(errormessage, MSG_SIZE, "LZMA mt encode failed: %d", (int)r);
                return false;
            }
            if (strm.avail_out == 0) {
                size_t used = strm.next_out - out.data();
                out.resize(out.size() * 2);
                strm.next_out  = out.data() + used;
                strm.avail_out = out.size() - used;
            }
        }
        out.resize(strm.next_out - out.data());
        lzma_end(&strm);
        return true;
    }

    size_t bound = lzma_stream_buffer_bound(insz);
    out.resize(bound);
    size_t pos = 0;
    lzma_ret r = lzma_stream_buffer_encode(
        filters, LZMA_CHECK_CRC64, nullptr,
        in, insz, out.data(), &pos, bound);
    if (r != LZMA_OK) {
        snprintf(errormessage, MSG_SIZE, "LZMA encode failed: %d", (int)r); return false;
    }
    out.resize(pos);
    return true;
}

static bool lzma_dec(const uint8_t* in, size_t insz,
                     std::vector<uint8_t>& out, size_t expected)
{
    static const size_t MAX_RAW = 1ull << 36;  // 64 GB sanity cap (real corpora can have >1GB raw pixels)
    if (expected > MAX_RAW) {
        snprintf(errormessage, MSG_SIZE, "LZMA decode: implausible raw size %zu (>64GB)", expected);
        return false;
    }
    out.resize(expected);
    size_t in_pos = 0, out_pos = 0;
    uint64_t memlim = UINT64_MAX;
    lzma_ret r = lzma_stream_buffer_decode(
        &memlim, 0, nullptr,
        in, &in_pos, insz,
        out.data(), &out_pos, out.size());
    if (r != LZMA_OK) {
        snprintf(errormessage, MSG_SIZE, "LZMA decode failed: %d", (int)r); return false;
    }
    out.resize(out_pos);
    return true;
}

/* ─── Zstd compress / decompress ────────────────────────────────────────── */

#ifdef USE_ZSTD
static bool zstd_enc(const uint8_t* in, size_t insz, std::vector<uint8_t>& out) {
    size_t bound = ZSTD_compressBound(insz);
    out.resize(bound);
    size_t r = ZSTD_compress(out.data(), bound, in, insz, g_zstd_level);
    if (ZSTD_isError(r)) {
        snprintf(errormessage, MSG_SIZE, "ZSTD encode: %s", ZSTD_getErrorName(r)); return false;
    }
    out.resize(r); return true;
}
// Long-range zstd (windowLog=27, 128 MB window). Decoder must mirror with windowLogMax=27.
static bool zstd_enc_long(const uint8_t* in, size_t insz, int level,
                          std::vector<uint8_t>& out) {
    size_t bound = ZSTD_compressBound(insz);
    out.resize(bound);
    ZSTD_CCtx* cctx = ZSTD_createCCtx();
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, level);
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_windowLog, 27);
    size_t r = ZSTD_compress2(cctx, out.data(), bound, in, insz);
    ZSTD_freeCCtx(cctx);
    if (ZSTD_isError(r)) {
        snprintf(errormessage, MSG_SIZE, "ZSTD-long encode: %s", ZSTD_getErrorName(r)); return false;
    }
    out.resize(r); return true;
}
static bool zstd_dec_long(const uint8_t* in, size_t insz,
                          std::vector<uint8_t>& out, size_t expected) {
    // Sanity-cap the claimed raw size — corrupted .tcip headers can claim
    // an absurd "expected" that triggers a multi-TB allocation. 1 GB ceiling
    // is well above any realistic PNG batch.
    static const size_t MAX_RAW = 1ull << 36;  // 64 GB
    if (expected > MAX_RAW) {
        snprintf(errormessage, MSG_SIZE, "ZSTD-long decode: implausible raw size %zu (>64GB)", expected);
        return false;
    }
    out.resize(expected);
    ZSTD_DCtx* dctx = ZSTD_createDCtx();
    ZSTD_DCtx_setParameter(dctx, ZSTD_d_windowLogMax, 27);
    size_t r = ZSTD_decompressDCtx(dctx, out.data(), expected, in, insz);
    ZSTD_freeDCtx(dctx);
    if (ZSTD_isError(r)) {
        snprintf(errormessage, MSG_SIZE, "ZSTD-long decode: %s", ZSTD_getErrorName(r)); return false;
    }
    out.resize(r); return true;
}
static bool zstd_dec(const uint8_t* in, size_t insz,
                     std::vector<uint8_t>& out, size_t expected) {
    static const size_t MAX_RAW = 1ull << 36;  // 64 GB sanity cap (real corpora can have >1GB raw pixels)
    if (expected > MAX_RAW) {
        snprintf(errormessage, MSG_SIZE, "ZSTD decode: implausible raw size %zu (>64GB)", expected);
        return false;
    }
    out.resize(expected);
    size_t r = ZSTD_decompress(out.data(), expected, in, insz);
    if (ZSTD_isError(r)) {
        snprintf(errormessage, MSG_SIZE, "ZSTD decode: %s", ZSTD_getErrorName(r)); return false;
    }
    out.resize(r); return true;
}
#else
static bool zstd_enc(const uint8_t*, size_t, std::vector<uint8_t>&) {
    snprintf(errormessage, MSG_SIZE, "not compiled with USE_ZSTD (-DUSE_ZSTD -lzstd)"); return false;
}
static bool zstd_enc_long(const uint8_t*, size_t, int, std::vector<uint8_t>&) {
    snprintf(errormessage, MSG_SIZE, "not compiled with USE_ZSTD (-DUSE_ZSTD -lzstd)"); return false;
}
static bool zstd_dec_long(const uint8_t*, size_t, std::vector<uint8_t>&, size_t) {
    snprintf(errormessage, MSG_SIZE, "not compiled with USE_ZSTD (-DUSE_ZSTD -lzstd)"); return false;
}
static bool zstd_dec(const uint8_t*, size_t, std::vector<uint8_t>&, size_t) {
    snprintf(errormessage, MSG_SIZE, "not compiled with USE_ZSTD (-DUSE_ZSTD -lzstd)"); return false;
}
#endif

/* ─── fast-lzma2 compress / decompress ───────────────────────────────────── */

#ifdef USE_FL2
// FL2 produces a raw LZMA2 stream. Tuned to mirror the xz path's lc=4/pb=2
// image-friendly Markov model. MT threads come from sfth_threads.
static bool fl2_enc(const uint8_t* in, size_t insz, std::vector<uint8_t>& out) {
    int threads = sfth_threads > 1 ? sfth_threads : 1;
    FL2_CCtx* ctx = (threads > 1) ? FL2_createCCtxMt((unsigned)threads) : FL2_createCCtx();
    if (!ctx) { snprintf(errormessage, MSG_SIZE, "FL2 ctx alloc failed"); return false; }
    FL2_CCtx_setParameter(ctx, FL2_p_compressionLevel, g_fl2_level);
    FL2_CCtx_setParameter(ctx, FL2_p_highCompression, 1);
    FL2_CCtx_setParameter(ctx, FL2_p_literalCtxBits, 4);
    FL2_CCtx_setParameter(ctx, FL2_p_literalPosBits, 0);
    FL2_CCtx_setParameter(ctx, FL2_p_posBits, 2);
    FL2_CCtx_setParameter(ctx, FL2_p_searchDepth, 254);
    size_t bound = FL2_compressBound(insz);
    out.resize(bound);
    size_t r = FL2_compressCCtx(ctx, out.data(), bound, in, insz, g_fl2_level);
    FL2_freeCCtx(ctx);
    if (FL2_isError(r)) {
        snprintf(errormessage, MSG_SIZE, "FL2 encode: %s", FL2_getErrorName(r)); return false;
    }
    out.resize(r); return true;
}
static bool fl2_dec(const uint8_t* in, size_t insz,
                    std::vector<uint8_t>& out, size_t expected) {
    static const size_t MAX_RAW = 1ull << 36;  // 64 GB sanity cap (real corpora can have >1GB raw pixels)
    if (expected > MAX_RAW) {
        snprintf(errormessage, MSG_SIZE, "FL2 decode: implausible raw size %zu (>64GB)", expected);
        return false;
    }
    out.resize(expected);
    size_t r = FL2_decompress(out.data(), expected, in, insz);
    if (FL2_isError(r)) {
        snprintf(errormessage, MSG_SIZE, "FL2 decode: %s", FL2_getErrorName(r)); return false;
    }
    out.resize(r); return true;
}
#else
static bool fl2_enc(const uint8_t*, size_t, std::vector<uint8_t>&) {
    snprintf(errormessage, MSG_SIZE, "not compiled with USE_FL2 (-DUSE_FL2 -lfast-lzma2)"); return false;
}
static bool fl2_dec(const uint8_t*, size_t, std::vector<uint8_t>&, size_t) {
    snprintf(errormessage, MSG_SIZE, "not compiled with USE_FL2 (-DUSE_FL2 -lfast-lzma2)"); return false;
}
#endif

/* ─── kanzi (BWT + CM/ANS) compress / decompress ─────────────────────────── */

#ifdef USE_KANZI
// Kanzi level → transform/entropy mapping (mirrors BlockCompressor::getTransformAndCodec).
static void kanzi_level_params(int level, std::string& transform, std::string& entropy) {
    switch (level) {
        case 0: transform="NONE";                          entropy="NONE";    break;
        case 1: transform="LZX";                           entropy="NONE";    break;
        case 2: transform="DNA+LZ";                        entropy="HUFFMAN"; break;
        case 3: transform="TEXT+UTF+PACK+MM+LZX";          entropy="HUFFMAN"; break;
        case 4: transform="TEXT+UTF+EXE+PACK+MM+ROLZ";     entropy="NONE";    break;
        case 5: transform="TEXT+UTF+BWT+RANK+ZRLT";        entropy="ANS0";    break;
        case 6: transform="TEXT+UTF+BWT+SRT+ZRLT";         entropy="FPAQ";    break;
        case 7: transform="LZP+TEXT+UTF+BWT+LZP";          entropy="CM";      break;
        case 8: transform="EXE+RLT+TEXT+UTF+DNA";          entropy="TPAQ";    break;
        case 9: transform="EXE+RLT+TEXT+UTF+DNA";          entropy="TPAQX";   break;
        default:transform="TEXT+UTF+BWT+RANK+ZRLT";        entropy="ANS0";    break;
    }
}

// Generic kanzi encode/decode with explicit pipeline. Used by both single-stream
// (-kanzi, legacy -kpng v10/v11) and split-stream (-kpng v12/v13) paths.
static bool kanzi_enc_pipe(const uint8_t* in, size_t insz,
                           const std::string& transform, const std::string& entropy,
                           std::vector<uint8_t>& out) {
    int jobs = sfth_threads > 1 ? sfth_threads : 1;
    try {
        std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
        kanzi::CompressedOutputStream cos(ss, jobs, entropy, transform, 4 * 1024 * 1024);
        cos.write((const char*)in, (std::streamsize)insz);
        cos.close();
        std::string s = ss.str();
        out.assign(s.begin(), s.end());
        return true;
    } catch (const std::exception& e) {
        snprintf(errormessage, MSG_SIZE, "Kanzi encode: %s", e.what());
        return false;
    }
}

// Zero-copy membuf wrapper for kanzi decode — avoids the string copy that
// std::stringstream(string(...)) implies on each call (saves ~0.2ms per file
// across the corpus).
namespace { struct membuf : std::streambuf {
    membuf(const char* base, size_t size) {
        char* p = const_cast<char*>(base);
        setg(p, p, p + size);
    }
}; }

static bool kanzi_dec_pipe(const uint8_t* in, size_t insz,
                           std::vector<uint8_t>& out, size_t expected) {
    static const size_t MAX_RAW = 1ull << 36;  // 64 GB sanity cap (real corpora can have >1GB raw pixels)
    if (expected > MAX_RAW) {
        snprintf(errormessage, MSG_SIZE, "Kanzi decode: implausible raw size %zu (>64GB)", expected);
        return false;
    }
    try {
        membuf buf((const char*)in, insz);
        std::istream ss(&buf);
        kanzi::CompressedInputStream cis(ss, 1);
        out.resize(expected);
        size_t total_read = 0;
        while (total_read < expected) {
            cis.read((char*)out.data() + total_read,
                     (std::streamsize)(expected - total_read));
            std::streamsize got = cis.gcount();
            if (got <= 0) break;
            total_read += (size_t)got;
        }
        cis.close();
        if (total_read != expected) {
            snprintf(errormessage, MSG_SIZE,
                     "Kanzi decode: read %zu of %zu", total_read, expected);
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        snprintf(errormessage, MSG_SIZE, "Kanzi decode: %s", e.what());
        return false;
    }
}

// Single-stream kanzi (used by -kanzi level path).
static bool kanzi_enc(const uint8_t* in, size_t insz, std::vector<uint8_t>& out) {
    std::string transform, entropy;
    kanzi_level_params(g_kanzi_level, transform, entropy);
    return kanzi_enc_pipe(in, insz, transform, entropy, out);
}

static bool kanzi_dec(const uint8_t* in, size_t insz,
                      std::vector<uint8_t>& out, size_t expected) {
    return kanzi_dec_pipe(in, insz, out, expected);
}
#else
static bool kanzi_enc_pipe(const uint8_t*, size_t, const std::string&, const std::string&, std::vector<uint8_t>&) {
    snprintf(errormessage, MSG_SIZE, "not compiled with USE_KANZI (-DUSE_KANZI -lkanzi)"); return false;
}
static bool kanzi_dec_pipe(const uint8_t*, size_t, std::vector<uint8_t>&, size_t) {
    snprintf(errormessage, MSG_SIZE, "not compiled with USE_KANZI (-DUSE_KANZI -lkanzi)"); return false;
}
static bool kanzi_enc(const uint8_t*, size_t, std::vector<uint8_t>&) {
    snprintf(errormessage, MSG_SIZE, "not compiled with USE_KANZI (-DUSE_KANZI -lkanzi)"); return false;
}
static bool kanzi_dec(const uint8_t*, size_t, std::vector<uint8_t>&, size_t) {
    snprintf(errormessage, MSG_SIZE, "not compiled with USE_KANZI (-DUSE_KANZI -lkanzi)"); return false;
}
#endif

/* ─── compress PNG/APNG → PPG v4 (solid LZMA + filter separation) ───────── */

struct FrameEnc {
    bool       matched;
    bool       ldf_mode  = false;  // mode 2: pixel-exact, libdeflate re-encode at decomp
    uint8_t    ldf_level = 9;      // libdeflate level for mode 2 (stored in level byte)
    DeflParams dp;
    bool       filter_sep;   // filter-byte separation applied
    uint32_t   stride;       // bytes per scanline (excl. filter byte)
    uint32_t   n_scanlines;
    size_t     payload_sz;   // total bytes contributed to solid block
};

static bool compress_png(const std::vector<uint8_t>& png_buf,
                         std::vector<uint8_t>& ppg_out)
{
    PngInfo info;
    if (!parse_png(png_buf, info)) return false;

    IhdrInfo ihdr = parse_ihdr(info.pre);
    uint32_t bpp  = compute_bpp(ihdr.color_type, ihdr.bit_depth);

    std::vector<FrameEnc> enc;
    enc.reserve(info.frames.size());

    // Temporary storage for separated pixel data (to avoid holding two copies)
    std::vector<std::vector<uint8_t>> sep_filter_bufs(info.frames.size());
    std::vector<std::vector<uint8_t>> sep_pixel_bufs(info.frames.size());

    size_t total_payload = 0;

    for (size_t i = 0; i < info.frames.size(); i++) {
        auto& fr = info.frames[i];
        FrameEnc fe{};
        fe.matched = !fr.pixels.empty() &&
                     find_deflate_params(fr.pixels, fr.idat_raw, fe.dp);

        // Mode 2 fallback: when brute-force fails, store pixels via libdeflate repack
        if (!fe.matched && ldf_repack && !fr.pixels.empty()) {
#ifdef USE_LIBDEFLATE
            fe.ldf_mode  = true;
            fe.ldf_level = 9;
#endif
        }

        // Filter separation: for matched (mode 0) or ldf_mode (mode 2) frames.
        // Skip when pixels >= g_nofsep_above (set by -fastdec/-nofsep=N/env var).
        fe.filter_sep = false;
        if ((fe.matched || fe.ldf_mode) && bpp > 0
            && (g_nofsep_above == 0 || fr.pixels.size() < g_nofsep_above)) {
            uint32_t fw, fh;
            frame_dims(fr, ihdr, fw, fh);
            fe.stride      = fw * bpp;
            fe.n_scanlines = fh;
            if (fe.stride > 0 && fe.n_scanlines > 0) {
                if (sep_filters(fr.pixels, fe.stride,
                                sep_filter_bufs[i], sep_pixel_bufs[i])) {
                    fe.filter_sep = true;
                    // payload = filter_bytes + pixel_bytes (same total as pixels.size())
                }
            }
        }

        fe.payload_sz = (fe.matched || fe.ldf_mode) ? fr.pixels.size() : fr.idat_raw.size();
        total_payload += fe.payload_sz;

        if (verbosity >= 1) {
            std::lock_guard<std::mutex> lk(g_print_mutex);
            clear_bar();
            if (fe.matched)
                fprintf(stdout,
                    "  %s[match]%s  lv=%d st=%s wb=%d ml=%d  px=%zu  idat=%zu%s\n",
                    col(GR), col(R), fe.dp.level, strategy_name(fe.dp.strategy),
                    fe.dp.wbits, fe.dp.memlevel,
                    fr.pixels.size(), fr.idat_raw.size(),
                    fe.filter_sep ? "  [fsep]" : "");
            else if (fe.ldf_mode)
                fprintf(stdout, "  %s[ldf]%s    px=%zu  (libdeflate lv%u)\n",
                    col(BC), col(R), fr.pixels.size(), fe.ldf_level);
            else
                fprintf(stdout, "  %s[stored]%s  idat=%zu\n",
                    col(YL), col(R), fr.idat_raw.size());
        }
        enc.push_back(fe);
    }

    bool has_mode2 = false;
    for (auto& fe : enc) if (fe.ldf_mode) { has_mode2 = true; break; }

    // Build solid input. kpng v2 (PPG v12/v13) uses split-stream:
    //   - pixel section (matched + ldf-mode frames) → kanzi LZP+BWT / CM (best ratio for low-entropy pixels)
    //   - idat section  (stored frames)             → kanzi LZP+BWT+SRT+ZRLT / FPAQ (fast decode for high-entropy idat)
    // All other backends (legacy kanzi, fl2, zstd, lzma) keep single-combined layout.
    bool kpng_v2 = use_kanzi && use_kpng;

    std::vector<uint8_t> combined, px_buf, idat_buf;
    combined.reserve(total_payload);
    if (kpng_v2) {
        px_buf.reserve(total_payload);
        idat_buf.reserve(total_payload);
    }
    for (size_t i = 0; i < info.frames.size(); i++) {
        auto& fr = info.frames[i];
        auto& fe = enc[i];
        bool is_pixel = (fe.matched || fe.ldf_mode);
        std::vector<uint8_t>* target = kpng_v2 ? (is_pixel ? &px_buf : &idat_buf) : &combined;
        if (fe.filter_sep) {
            target->insert(target->end(),
                           sep_filter_bufs[i].begin(), sep_filter_bufs[i].end());
            target->insert(target->end(),
                           sep_pixel_bufs[i].begin(), sep_pixel_bufs[i].end());
        } else {
            const uint8_t* p = is_pixel ? fr.pixels.data() : fr.idat_raw.data();
            target->insert(target->end(), p, p + fe.payload_sz);
        }
    }

    // Encode
    std::vector<uint8_t> comp_data, comp_data_idat;
    uint64_t px_total = 0, idat_total = 0;
    const char* comp_label =
        kpng_v2     ? "kpng2" :
        use_kanzi   ? "Kanzi" :
        use_fl2     ? "FL2"   :
        use_zstd    ? "Zstd"  : "LZMA";
    if (kpng_v2) {
        // kpng v4 (PPG v14/v15): split-stream pixel/idat, optimized after empirical sweep
        //   pixel → kanzi RLT+BWT+SRT+ZRLT / FPAQ   (best ratio for filter-separated bytes,
        //                                            FPAQ ~3× faster decode than CM)
        //   idat  → zstd-19 windowLog=27            (cheap init, fast decode, good ratio
        //                                            on mode-2 deflate streams)
        // Sections encoded in parallel via std::thread; decode parallel when both sections
        // are big enough to amortize spawn (>32KB IDAT) — empty/tiny IDAT → serial.
        // Decoder auto-detects IDAT codec by magic byte (zstd 0x28B5 / xz 0xFD37 / else FL2)
        // so future hybrid encoders can mix codecs without format change.
        // Pareto vs xz preset 6: ratio ≈tied (+0.06%), comp −37%, decST −10%, dec-th0 +12%.
        px_total   = (uint64_t)px_buf.size();
        idat_total = (uint64_t)idat_buf.size();
        bool px_ok = true, idat_ok = true;
        std::thread px_t;
        if (!px_buf.empty()) {
            px_t = std::thread([&] {
                const char* t_env = getenv("PACKPNG_PERFILE_T");
                const char* e_env = getenv("PACKPNG_PERFILE_E");
                std::string t = t_env ? t_env : "RLT+BWT+SRT+ZRLT";
                std::string e = e_env ? e_env : "FPAQ";
                px_ok = kanzi_enc_pipe(px_buf.data(), px_buf.size(), t, e, comp_data);
            });
        }
        if (idat_buf.empty()) {
            comp_data_idat.clear();
        } else {
            idat_ok = zstd_enc_long(idat_buf.data(), idat_buf.size(), 19, comp_data_idat);
        }
        if (px_t.joinable()) px_t.join();
        if (!px_ok || !idat_ok) return false;
    } else if (use_kanzi) {
        if (!kanzi_enc(combined.data(), combined.size(), comp_data)) return false;
    } else if (use_fl2) {
        if (!fl2_enc(combined.data(), combined.size(), comp_data)) return false;
    } else if (use_zstd) {
        if (!zstd_enc(combined.data(), combined.size(), comp_data)) return false;
    } else {
        if (!lzma_enc(combined.data(), combined.size(), comp_data)) return false;
    }

    if (verbosity >= 1) {
        std::lock_guard<std::mutex> lk(g_print_mutex);
        clear_bar();
        if (kpng_v2) {
            fprintf(stdout,
                "  split %s: px=%zu→%zu  idat=%zu→%zu  total=%zu→%zu\n",
                comp_label, (size_t)px_total, comp_data.size(),
                (size_t)idat_total, comp_data_idat.size(),
                (size_t)(px_total + idat_total),
                comp_data.size() + comp_data_idat.size());
        } else {
            fprintf(stdout, "  solid %s: %zu → %zu (%.1f%%)\n",
                    comp_label, total_payload, comp_data.size(),
                    total_payload > 0 ? 100.0 * comp_data.size() / total_payload : 0.0);
        }
    }

    // v4/v5 = LZMA; v6/v7 = Zstd; v8/v9 = fast-lzma2; v10/v11 = kanzi single;
    // v12/v13 = legacy kpng v2 split (CM pixels + FPAQ idat — v1.4 only);
    // v14/v15 = current kpng v3 split (CM pixels + zstd-long idat).
    uint8_t fmt_ver;
    if (kpng_v2)        fmt_ver = has_mode2 ? 15u : 14u;
    else if (use_kanzi) fmt_ver = has_mode2 ? 11u : 10u;
    else if (use_fl2)   fmt_ver = has_mode2 ? 9u  : 8u;
    else if (use_zstd)  fmt_ver = has_mode2 ? 7u  : 6u;
    else                fmt_ver = has_mode2 ? 5u  : 4u;

    ppg_out.clear();
    wr_bytes(ppg_out, PPG_SIG, 4);
    ppg_out.push_back(fmt_ver);                         // format version
    ppg_out.push_back(info.is_apng ? 1 : 0);
    wr_le32(ppg_out, info.num_plays);
    wr_le64(ppg_out, (uint64_t)info.pre.size());
    wr_bytes(ppg_out, info.pre.data(), info.pre.size());
    wr_le64(ppg_out, (uint64_t)info.post.size());
    wr_bytes(ppg_out, info.post.data(), info.post.size());
    wr_le32(ppg_out, (uint32_t)enc.size());

    for (size_t i = 0; i < enc.size(); i++) {
        auto& fr = info.frames[i];
        auto& fe = enc[i];
        ppg_out.push_back(fr.fctl.empty() ? 0 : 1);
        if (!fr.fctl.empty()) {
            if (fr.fctl.size() != 26) {
                snprintf(errormessage, MSG_SIZE, "bad fctl size"); return false;
            }
            wr_bytes(ppg_out, fr.fctl.data(), 26);
        }
        ppg_out.push_back(fr.uses_idat ? 1 : 0);
        if (!fr.uses_idat) wr_le32(ppg_out, fr.first_seq);
        // mode: 0=zlib-match, 1=raw-idat, 2=ldf-pixel-exact
        uint8_t mode_byte = fe.matched ? 0 : (fe.ldf_mode ? 2 : 1);
        ppg_out.push_back(mode_byte);
        // for mode 2: level byte holds ldf_level; strategy/wbits/memlevel unused (0)
        ppg_out.push_back(fe.matched ? (uint8_t)fe.dp.level    : (fe.ldf_mode ? fe.ldf_level : 0));
        ppg_out.push_back(fe.matched ? (uint8_t)fe.dp.strategy : 0);
        ppg_out.push_back(fe.matched ? (uint8_t)fe.dp.wbits    : 0);
        ppg_out.push_back(fe.matched ? (uint8_t)fe.dp.memlevel : 0);
        // mode 2: chunk sizes unknown at compress time; write 0 → decomp emits single chunk
        uint32_t nc_out = fe.ldf_mode ? 0u : (uint32_t)fr.chunk_szs.size();
        wr_le32(ppg_out, nc_out);
        if (!fe.ldf_mode)
            for (uint32_t s : fr.chunk_szs) wr_le32(ppg_out, s);
        wr_le64(ppg_out, (uint64_t)fe.payload_sz);
        // Filter separation metadata (v4 addition)
        ppg_out.push_back(fe.filter_sep ? 1 : 0);
        if (fe.filter_sep) {
            wr_le32(ppg_out, fe.stride);
            wr_le32(ppg_out, fe.n_scanlines);
        }
    }

    if (kpng_v2) {
        // v12/v13 split-stream layout:
        //   [u64 px_raw][u64 px_comp][px_comp bytes : kanzi LZP+BWT / CM]
        //   [u64 idat_raw][u64 idat_comp][idat_comp bytes : kanzi LZP+BWT+SRT+ZRLT / FPAQ]
        wr_le64(ppg_out, px_total);
        wr_le64(ppg_out, (uint64_t)comp_data.size());
        wr_bytes(ppg_out, comp_data.data(), comp_data.size());
        wr_le64(ppg_out, idat_total);
        wr_le64(ppg_out, (uint64_t)comp_data_idat.size());
        wr_bytes(ppg_out, comp_data_idat.data(), comp_data_idat.size());
    } else {
        // Solid compressed block (LZMA v4/v5, Zstd v6/v7, FL2 v8/v9, kanzi v10/v11)
        wr_le64(ppg_out, (uint64_t)total_payload);
        wr_le64(ppg_out, (uint64_t)comp_data.size());
        wr_bytes(ppg_out, comp_data.data(), comp_data.size());
    }

    return true;
}

/* ─── reconstruct deflate stream ─────────────────────────────────────────── */

static bool rebuild_deflate(uint8_t mode, uint8_t level, uint8_t strategy,
                            uint8_t wbits, uint8_t memlevel,
                            std::vector<uint8_t>& payload,
                            std::vector<uint8_t>& deflate_out)
{
    if (mode == 0) {
        // Validate params BEFORE deflateInit2 — corrupt .tcip can claim
        // bizarre values that some zlib forks accept and then deflate()
        // loops on. Reject anything outside zlib's documented ranges.
        if (level > 9 || strategy > 4 || wbits < 8 || wbits > 15 ||
            memlevel < 1 || memlevel > 9) {
            snprintf(errormessage, MSG_SIZE,
                     "rebuild_deflate: invalid zlib params lv=%u st=%u wb=%u ml=%u",
                     level, strategy, wbits, memlevel);
            return false;
        }
        // byte-exact zlib re-encode with original params
        z_stream zs{};
        if (deflateInit2(&zs, level, Z_DEFLATED, (int)wbits, (int)memlevel, strategy) != Z_OK) {
            snprintf(errormessage, MSG_SIZE, "deflateInit2 failed"); return false;
        }
        deflate_out.resize(deflateBound(&zs, payload.size()) + 16);
        zs.next_in   = payload.data();
        zs.avail_in  = (uInt)payload.size();
        zs.next_out  = deflate_out.data();
        zs.avail_out = (uInt)deflate_out.size();
        int r = deflate(&zs, Z_FINISH);
        size_t dlen = zs.total_out;
        deflateEnd(&zs);
        if (r != Z_STREAM_END) {
            snprintf(errormessage, MSG_SIZE, "deflate re-encode failed"); return false;
        }
        deflate_out.resize(dlen);
    } else if (mode == 2) {
        // pixel-exact: re-compress pixels with libdeflate (deterministic)
#ifdef USE_LIBDEFLATE
        int ldf_lv = (level >= 1 && level <= 12) ? (int)level : 9;
        libdeflate_compressor* c = libdeflate_alloc_compressor(ldf_lv);
        if (!c) { snprintf(errormessage, MSG_SIZE, "libdeflate_alloc_compressor failed"); return false; }
        size_t bound = libdeflate_zlib_compress_bound(c, payload.size());
        deflate_out.resize(bound);
        size_t actual = libdeflate_zlib_compress(c,
                            payload.data(), payload.size(),
                            deflate_out.data(), deflate_out.size());
        libdeflate_free_compressor(c);
        if (actual == 0) {
            snprintf(errormessage, MSG_SIZE, "libdeflate compress failed"); return false;
        }
        deflate_out.resize(actual);
#else
        snprintf(errormessage, MSG_SIZE, "mode 2 requires USE_LIBDEFLATE (recompile with -DUSE_LIBDEFLATE -ldeflate)");
        return false;
#endif
    } else {
        // mode 1: raw idat bytes, pass through
        deflate_out = std::move(payload);
    }
    return true;
}

/* ─── FrameMeta (decompressor) ───────────────────────────────────────────── */

struct FrameMeta {
    std::vector<uint8_t> fctl;
    bool     uses_idat;
    uint32_t first_seq;
    uint8_t  mode, level, strategy;
    uint8_t  wbits = 15, memlevel = 8;
    std::vector<uint32_t> chunk_szs;
    std::vector<uint8_t>  payload;
    // v4 filter separation
    bool     filter_sep = false;
    uint32_t stride = 0, n_scanlines = 0;
};

static bool read_frame_v2(const uint8_t* p, size_t sz, size_t& pos, FrameMeta& fm) {
    if (pos >= sz) { snprintf(errormessage, MSG_SIZE, "PPG truncated (frame)"); return false; }
    bool has_fctl = p[pos++] != 0;
    if (has_fctl) {
        if (pos + 26 > sz) { snprintf(errormessage, MSG_SIZE, "PPG truncated (fctl)"); return false; }
        fm.fctl.assign(p + pos, p + pos + 26); pos += 26;
    }
    if (pos >= sz) { snprintf(errormessage, MSG_SIZE, "PPG truncated (uses_idat)"); return false; }
    fm.uses_idat = p[pos++] != 0;
    if (!fm.uses_idat) {
        if (pos + 4 > sz) { snprintf(errormessage, MSG_SIZE, "PPG truncated (first_seq)"); return false; }
        fm.first_seq = rd_le32(p + pos); pos += 4;
    }
    if (pos + 5 > sz) { snprintf(errormessage, MSG_SIZE, "PPG truncated (params)"); return false; }
    fm.mode     = p[pos++];
    fm.level    = p[pos++];
    fm.strategy = p[pos++];
    fm.wbits    = p[pos++];
    fm.memlevel = p[pos++];
    if (pos + 4 > sz) { snprintf(errormessage, MSG_SIZE, "PPG truncated (n_chunks)"); return false; }
    uint32_t nc = rd_le32(p + pos); pos += 4;
    // Audit fix #4: cast to uint64_t to avoid uint32 overflow when nc > 0x40000000.
    if ((uint64_t)nc * 4 > sz - pos) { snprintf(errormessage, MSG_SIZE, "PPG truncated (chunk_szs)"); return false; }
    fm.chunk_szs.resize(nc);
    for (uint32_t i = 0; i < nc; i++) { fm.chunk_szs[i] = rd_le32(p + pos); pos += 4; }
    if (pos + 16 > sz) { snprintf(errormessage, MSG_SIZE, "PPG truncated (sizes)"); return false; }
    uint64_t raw_sz  = rd_le64(p + pos); pos += 8;
    uint64_t lzma_sz = rd_le64(p + pos); pos += 8;
    if (lzma_sz > sz || pos > sz - lzma_sz) { snprintf(errormessage, MSG_SIZE, "PPG truncated (lzma)"); return false; }
    if (!lzma_dec(p + pos, lzma_sz, fm.payload, raw_sz)) return false;
    pos += lzma_sz;
    return true;
}

// Read per-frame metadata for solid-format (v3/v4/v5); no payload here.
// Returns payload_sz via out param.
static bool read_frame_solid_meta(const uint8_t* p, size_t sz, size_t& pos,
                                  FrameMeta& fm, uint64_t& payload_sz,
                                  uint8_t ppg_version)
{
    if (pos >= sz) { snprintf(errormessage, MSG_SIZE, "PPG truncated (frame meta)"); return false; }
    bool has_fctl = p[pos++] != 0;
    if (has_fctl) {
        if (pos + 26 > sz) { snprintf(errormessage, MSG_SIZE, "PPG truncated (fctl)"); return false; }
        fm.fctl.assign(p + pos, p + pos + 26); pos += 26;
    }
    if (pos >= sz) { snprintf(errormessage, MSG_SIZE, "PPG truncated (uses_idat)"); return false; }
    fm.uses_idat = p[pos++] != 0;
    if (!fm.uses_idat) {
        if (pos + 4 > sz) { snprintf(errormessage, MSG_SIZE, "PPG truncated (first_seq)"); return false; }
        fm.first_seq = rd_le32(p + pos); pos += 4;
    }
    if (pos + 5 > sz) { snprintf(errormessage, MSG_SIZE, "PPG truncated (params)"); return false; }
    fm.mode     = p[pos++];
    fm.level    = p[pos++];
    fm.strategy = p[pos++];
    fm.wbits    = p[pos++];
    fm.memlevel = p[pos++];
    if (pos + 4 > sz) { snprintf(errormessage, MSG_SIZE, "PPG truncated (n_chunks)"); return false; }
    uint32_t nc = rd_le32(p + pos); pos += 4;
    // Audit fix #4: cast to uint64_t to avoid uint32 overflow when nc > 0x40000000.
    if ((uint64_t)nc * 4 > sz - pos) { snprintf(errormessage, MSG_SIZE, "PPG truncated (chunk_szs)"); return false; }
    fm.chunk_szs.resize(nc);
    for (uint32_t i = 0; i < nc; i++) { fm.chunk_szs[i] = rd_le32(p + pos); pos += 4; }
    if (pos + 8 > sz) { snprintf(errormessage, MSG_SIZE, "PPG truncated (payload_sz)"); return false; }
    payload_sz = rd_le64(p + pos); pos += 8;

    if (ppg_version >= 4) {
        if (pos >= sz) { snprintf(errormessage, MSG_SIZE, "PPG v4+ truncated (fsep)"); return false; }
        fm.filter_sep = p[pos++] != 0;
        if (fm.filter_sep) {
            if (pos + 8 > sz) { snprintf(errormessage, MSG_SIZE, "PPG v4+ truncated (stride/n_sc)"); return false; }
            fm.stride      = rd_le32(p + pos); pos += 4;
            fm.n_scanlines = rd_le32(p + pos); pos += 4;
        }
    }
    return true;
}

static bool emit_idat_or_fdat(std::vector<uint8_t>& png_out, FrameMeta& fm) {
    // Move (not copy) — fm.payload is not used after this call. Saves a 4MB+
    // memcpy on big files (e.g., 8 ms on Cspeed.png).
    std::vector<uint8_t> payload_copy = std::move(fm.payload);

    // Undo filter separation if applied
    if (fm.filter_sep && fm.stride > 0 && fm.n_scanlines > 0) {
        // Sanity-cap stride/n_scanlines to avoid pathological allocations on
        // corrupt input (e.g. stride = 0xFFFFFFFF, n_scanlines = 0xFFFFFFFF).
        if (fm.stride > 1u<<29 || fm.n_scanlines > 1u<<29) {
            snprintf(errormessage, MSG_SIZE,
                     "filter sep: implausible stride=%u n_scanlines=%u",
                     fm.stride, fm.n_scanlines);
            return false;
        }
        size_t filter_sz = fm.n_scanlines;
        size_t pixel_sz  = (size_t)fm.n_scanlines * fm.stride;
        // Overflow check on pixel_sz product (n_scanlines × stride)
        if (fm.stride != 0 && pixel_sz / fm.stride != fm.n_scanlines) {
            snprintf(errormessage, MSG_SIZE,
                     "filter sep: stride×n_scanlines overflow"); return false;
        }
        if (payload_copy.size() != filter_sz + pixel_sz) {
            snprintf(errormessage, MSG_SIZE, "filter sep size mismatch"); return false;
        }
        std::vector<uint8_t> reconstructed;
        if (!unsep_filters(payload_copy.data(), fm.n_scanlines,
                           payload_copy.data() + filter_sz, fm.stride,
                           reconstructed))
        {
            snprintf(errormessage, MSG_SIZE, "unsep_filters failed"); return false;
        }
        payload_copy = std::move(reconstructed);
    }

    std::vector<uint8_t> deflate_stream;
    if (!rebuild_deflate(fm.mode, fm.level, fm.strategy, fm.wbits, fm.memlevel,
                         payload_copy, deflate_stream))
        return false;

    // mode 2 (ldf repack) stores empty chunk_szs; emit as single chunk
    if (fm.chunk_szs.empty()) {
        uint32_t csz = (uint32_t)deflate_stream.size();
        if (fm.uses_idat) {
            uint32_t crc = chunk_crc((const uint8_t*)"IDAT", deflate_stream.data(), csz);
            wr_be32(png_out, csz);
            wr_bytes(png_out, (const uint8_t*)"IDAT", 4);
            wr_bytes(png_out, deflate_stream.data(), csz);
            wr_be32(png_out, crc);
        } else {
            uint32_t seq = fm.first_seq;
            uint32_t total = csz + 4;
            std::vector<uint8_t> fdat_data(total);
            fdat_data[0]=(seq>>24)&0xFF; fdat_data[1]=(seq>>16)&0xFF;
            fdat_data[2]=(seq>>8)&0xFF;  fdat_data[3]=seq&0xFF;
            memcpy(fdat_data.data() + 4, deflate_stream.data(), csz);
            uint32_t crc = chunk_crc((const uint8_t*)"fdAT", fdat_data.data(), total);
            wr_be32(png_out, total);
            wr_bytes(png_out, (const uint8_t*)"fdAT", 4);
            wr_bytes(png_out, fdat_data.data(), total);
            wr_be32(png_out, crc);
        }
        return true;
    }

    size_t dpos = 0;
    for (size_t i = 0; i < fm.chunk_szs.size(); i++) {
        uint32_t csz = fm.chunk_szs[i];
        if (csz > deflate_stream.size() || dpos > deflate_stream.size() - csz) {
            snprintf(errormessage, MSG_SIZE, "deflate shorter than expected"); return false;
        }
        if (fm.uses_idat) {
            uint32_t crc = chunk_crc((const uint8_t*)"IDAT",
                                     deflate_stream.data() + dpos, csz);
            wr_be32(png_out, csz);
            wr_bytes(png_out, (const uint8_t*)"IDAT", 4);
            wr_bytes(png_out, deflate_stream.data() + dpos, csz);
            wr_be32(png_out, crc);
        } else {
            uint32_t seq = fm.first_seq + (uint32_t)i;
            uint32_t total = csz + 4;
            std::vector<uint8_t> fdat_data(total);
            fdat_data[0] = (seq>>24)&0xFF; fdat_data[1] = (seq>>16)&0xFF;
            fdat_data[2] = (seq>>8)&0xFF;  fdat_data[3] = seq&0xFF;
            memcpy(fdat_data.data() + 4, deflate_stream.data() + dpos, csz);
            uint32_t crc = chunk_crc((const uint8_t*)"fdAT", fdat_data.data(), total);
            wr_be32(png_out, total);
            wr_bytes(png_out, (const uint8_t*)"fdAT", 4);
            wr_bytes(png_out, fdat_data.data(), total);
            wr_be32(png_out, crc);
        }
        dpos += csz;
    }
    return true;
}

/* ─── solid-format decompressor (v3 + v4) ───────────────────────────────── */

static bool decompress_solid(const uint8_t* p, size_t sz, size_t pos,
                              uint8_t ppg_version, std::vector<uint8_t>& png_out)
{
    if (pos + 1 + 4 + 8 > sz) { snprintf(errormessage, MSG_SIZE, "PPG solid truncated (hdr)"); return false; }
    bool     is_apng   = (p[pos++] & 1) != 0;
    uint32_t num_plays = rd_le32(p + pos); pos += 4; (void)num_plays;
    uint64_t pre_sz    = rd_le64(p + pos); pos += 8;
    if (pre_sz > sz || pos > sz - pre_sz) { snprintf(errormessage, MSG_SIZE, "PPG solid truncated (pre)"); return false; }
    std::vector<uint8_t> pre(p + pos, p + pos + pre_sz); pos += pre_sz;
    uint64_t post_sz = rd_le64(p + pos); pos += 8;
    if (post_sz > sz || pos > sz - post_sz) { snprintf(errormessage, MSG_SIZE, "PPG solid truncated (post)"); return false; }
    std::vector<uint8_t> post(p + pos, p + pos + post_sz); pos += post_sz;
    uint32_t num_frames = rd_le32(p + pos); pos += 4;
    (void)is_apng;
    // Audit fix #3: cap num_frames by remaining file size. Solid frame meta is at
    // least 1 (uses_idat) + 5 (deflate params) + 4 (n_chunks) + 8 (payload_sz) = 18 bytes
    // when there's no fctl/first_seq/chunks. Use 18 as conservative lower bound.
    if (num_frames > (sz - pos) / 18 + 1) {
        snprintf(errormessage, MSG_SIZE, "PPG solid num_frames implausible (%u)", num_frames); return false;
    }

    std::vector<FrameMeta> frames(num_frames);
    std::vector<uint64_t>  payload_szs(num_frames);
    for (uint32_t i = 0; i < num_frames; i++)
        if (!read_frame_solid_meta(p, sz, pos, frames[i], payload_szs[i], ppg_version))
            return false;

    std::vector<uint8_t> px_part, idat_part;
    bool kpng_v2 = (ppg_version >= 12);

    if (kpng_v2) {
        // Split-stream layout (v12-v15): [u64 px_raw][u64 px_comp][px_comp]
        //                                [u64 idat_raw][u64 idat_comp][idat_comp]
        // Decode pixel + IDAT in parallel — sections are independent, halves walltime
        // when both decoders are busy (FL2/xz IDAT) and ~free when IDAT is empty.
        if (pos + 16 > sz) { snprintf(errormessage, MSG_SIZE, "PPG split truncated (px hdr)"); return false; }
        uint64_t px_raw  = rd_le64(p + pos); pos += 8;
        uint64_t px_comp = rd_le64(p + pos); pos += 8;
        if (px_comp > sz || pos > sz - px_comp) { snprintf(errormessage, MSG_SIZE, "PPG split truncated (px data)"); return false; }
        const uint8_t* px_ptr = p + pos;
        pos += px_comp;
        if (pos + 16 > sz) { snprintf(errormessage, MSG_SIZE, "PPG split truncated (idat hdr)"); return false; }
        uint64_t idat_raw  = rd_le64(p + pos); pos += 8;
        uint64_t idat_comp = rd_le64(p + pos); pos += 8;
        if (idat_comp > sz || pos > sz - idat_comp) { snprintf(errormessage, MSG_SIZE, "PPG split truncated (idat data)"); return false; }
        const uint8_t* idat_ptr = p + pos;
        pos += idat_comp;
        // Parallel decode only when BOTH sections are big enough to amortize thread spawn.
        // Empty IDAT (mode 0/1 frames) → serial; tiny IDAT (<32KB) → serial.
        const uint64_t PARALLEL_DECODE_MIN = 32 * 1024;
        bool use_parallel = (px_raw > 0) && (idat_raw >= PARALLEL_DECODE_MIN);
        bool px_ok = true, idat_ok = true;
        auto decode_px = [&]() {
            // Pixel section is always kanzi (CM in legacy v12/v13, FPAQ in v14/v15).
            px_ok = kanzi_dec_pipe(px_ptr, px_comp, px_part, px_raw);
        };
        auto decode_idat = [&]() {
            if (ppg_version >= 14) {
                // Auto-detect IDAT codec by magic byte (no format change needed):
                //   zstd magic = 28 B5 2F FD ; xz magic = FD 37 7A 58 5A 00 ; else FL2 LZMA2
                bool used_zstd = (idat_comp >= 4 && idat_ptr[0] == 0x28 &&
                                  idat_ptr[1] == 0xB5 && idat_ptr[2] == 0x2F && idat_ptr[3] == 0xFD);
                bool used_xz   = (idat_comp >= 6 && idat_ptr[0] == 0xFD && idat_ptr[1] == 0x37 &&
                                  idat_ptr[2] == 0x7A && idat_ptr[3] == 0x58);
                if (used_zstd)    idat_ok = zstd_dec_long(idat_ptr, idat_comp, idat_part, idat_raw);
                else if (used_xz) idat_ok = lzma_dec(idat_ptr, idat_comp, idat_part, idat_raw);
                else              idat_ok = fl2_dec(idat_ptr, idat_comp, idat_part, idat_raw);
            } else {
                idat_ok = kanzi_dec_pipe(idat_ptr, idat_comp, idat_part, idat_raw);
            }
        };
        if (px_raw == 0)   px_part.clear();
        if (idat_raw == 0) idat_part.clear();
        if (use_parallel) {
            std::thread px_th(decode_px);
            decode_idat();
            px_th.join();
        } else {
            if (px_raw > 0)   decode_px();
            if (idat_raw > 0) decode_idat();
        }
        if (!px_ok || !idat_ok) return false;
    } else {
        if (pos + 16 > sz) { snprintf(errormessage, MSG_SIZE, "PPG solid truncated (comp hdr)"); return false; }
        uint64_t total_raw = rd_le64(p + pos); pos += 8;
        uint64_t comp_sz   = rd_le64(p + pos); pos += 8;
        if (pos + comp_sz > sz) { snprintf(errormessage, MSG_SIZE, "PPG solid truncated (comp data)"); return false; }
        std::vector<uint8_t> combined;
        if      (ppg_version >= 10) { if (!kanzi_dec(p + pos, comp_sz, combined, total_raw)) return false; }
        else if (ppg_version >=  8) { if (!fl2_dec  (p + pos, comp_sz, combined, total_raw)) return false; }
        else if (ppg_version >=  6) { if (!zstd_dec (p + pos, comp_sz, combined, total_raw)) return false; }
        else                        { if (!lzma_dec (p + pos, comp_sz, combined, total_raw)) return false; }
        // Split combined back into per-frame payloads using mode (matched/ldf → pixel buf, stored → idat buf).
        // For non-split formats the data is sequential; we just hand each frame its portion in order.
        px_part = std::move(combined);
        // px_part holds everything in order; idat_part stays empty; the loop below will pull sequentially.
    }

    size_t px_off = 0, idat_off = 0;
    for (uint32_t i = 0; i < num_frames; i++) {
        size_t psz = (size_t)payload_szs[i];
        if (kpng_v2) {
            // mode 0 (matched) and mode 2 (ldf) → pixel buffer; mode 1 (stored) → idat buffer
            bool from_pixel = (frames[i].mode != 1);
            std::vector<uint8_t>& src = from_pixel ? px_part : idat_part;
            size_t& off = from_pixel ? px_off : idat_off;
            if (off + psz > src.size()) {
                snprintf(errormessage, MSG_SIZE, "PPG v12 payload split error frame %u (%s)",
                         i, from_pixel ? "px" : "idat"); return false;
            }
            frames[i].payload.assign(src.begin() + off, src.begin() + off + psz);
            off += psz;
        } else {
            if (px_off + psz > px_part.size()) {
                snprintf(errormessage, MSG_SIZE, "PPG solid payload split error frame %u", i); return false;
            }
            frames[i].payload.assign(px_part.begin() + px_off, px_part.begin() + px_off + psz);
            px_off += psz;
        }
    }

    png_out.clear();
    wr_bytes(png_out, PNG_SIG, 8);
    wr_bytes(png_out, pre.data(), pre.size());
    for (auto& fm : frames) {
        if (!fm.fctl.empty()) {
            uint32_t crc = chunk_crc((const uint8_t*)"fcTL", fm.fctl.data(), fm.fctl.size());
            wr_be32(png_out, (uint32_t)fm.fctl.size());
            wr_bytes(png_out, (const uint8_t*)"fcTL", 4);
            wr_bytes(png_out, fm.fctl.data(), fm.fctl.size());
            wr_be32(png_out, crc);
        }
        if (!emit_idat_or_fdat(png_out, fm)) return false;
    }
    wr_bytes(png_out, post.data(), post.size());
    return true;
}

/* ─── decompress PPG → PNG/APNG ─────────────────────────────────────────── */

static bool decompress_ppg(const std::vector<uint8_t>& ppg_buf,
                            std::vector<uint8_t>& png_out)
{
    const uint8_t* p = ppg_buf.data();
    size_t sz = ppg_buf.size();

    if (sz < 4 || memcmp(p, PPG_SIG, 4) != 0) {
        snprintf(errormessage, MSG_SIZE, "bad PPG magic"); return false;
    }
    size_t pos = 4;
    uint8_t version = p[pos++];

    if (version == 1) {
        // Audit fix #1: bounds-check every read; reject huge n_idat that would OOM.
        if (pos + 3 + 4 > sz) { snprintf(errormessage, MSG_SIZE, "PPG v1 truncated (hdr)"); return false; }
        uint8_t mode     = p[pos++];
        uint8_t level    = p[pos++];
        uint8_t strategy = p[pos++];
        uint32_t n_idat  = rd_le32(p + pos); pos += 4;
        // Each idat size is 4 bytes; cap n_idat by remaining file size.
        if ((uint64_t)n_idat * 4 > sz - pos) {
            snprintf(errormessage, MSG_SIZE, "PPG v1 truncated (idat sizes)"); return false;
        }
        std::vector<uint32_t> isizes(n_idat);
        for (uint32_t i = 0; i < n_idat; i++) { isizes[i] = rd_le32(p + pos); pos += 4; }
        if (pos + 8 > sz) { snprintf(errormessage, MSG_SIZE, "PPG v1 truncated (pre_sz)"); return false; }
        uint64_t pre_sz = rd_le64(p + pos); pos += 8;
        if (pre_sz > sz - pos) { snprintf(errormessage, MSG_SIZE, "PPG v1 truncated (pre)"); return false; }
        std::vector<uint8_t> pre(p + pos, p + pos + pre_sz); pos += pre_sz;
        if (pos + 8 > sz) { snprintf(errormessage, MSG_SIZE, "PPG v1 truncated (suf_sz)"); return false; }
        uint64_t suf_sz = rd_le64(p + pos); pos += 8;
        if (suf_sz > sz - pos) { snprintf(errormessage, MSG_SIZE, "PPG v1 truncated (suf)"); return false; }
        std::vector<uint8_t> suf(p + pos, p + pos + suf_sz); pos += suf_sz;
        if (pos + 16 > sz) { snprintf(errormessage, MSG_SIZE, "PPG v1 truncated (sz hdr)"); return false; }
        uint64_t raw_sz  = rd_le64(p + pos); pos += 8;
        uint64_t lzma_sz = rd_le64(p + pos); pos += 8;
        if (lzma_sz > sz - pos) {
            snprintf(errormessage, MSG_SIZE, "PPG v1 truncated (lzma payload)"); return false;
        }
        std::vector<uint8_t> payload;
        if (!lzma_dec(p + pos, lzma_sz, payload, raw_sz)) return false;
        std::vector<uint8_t> deflate_stream;
        if (!rebuild_deflate(mode, level, strategy, 15, 8, payload, deflate_stream)) return false;
        png_out.clear();
        wr_bytes(png_out, PNG_SIG, 8);
        wr_bytes(png_out, pre.data(), pre.size());
        size_t dpos = 0;
        for (uint32_t isz : isizes) {
            const uint8_t* cd = deflate_stream.data() + dpos;
            uint32_t crc = chunk_crc((const uint8_t*)"IDAT", cd, isz);
            wr_be32(png_out, isz);
            wr_bytes(png_out, (const uint8_t*)"IDAT", 4);
            wr_bytes(png_out, cd, isz);
            wr_be32(png_out, crc);
            dpos += isz;
        }
        wr_bytes(png_out, suf.data(), suf.size());
        return true;
    }

    if (version == 2) {
        if (pos + 1 + 4 + 8 > sz) { snprintf(errormessage, MSG_SIZE, "PPG v2 truncated (hdr)"); return false; }
        bool     is_apng   = (p[pos++] & 1) != 0;
        uint32_t num_plays = rd_le32(p + pos); pos += 4; (void)num_plays;
        uint64_t pre_sz    = rd_le64(p + pos); pos += 8;
        if (pre_sz > sz || pos > sz - pre_sz) { snprintf(errormessage, MSG_SIZE, "PPG v2 truncated (pre)"); return false; }
        std::vector<uint8_t> pre(p + pos, p + pos + pre_sz); pos += pre_sz;
        uint64_t post_sz = rd_le64(p + pos); pos += 8;
        if (post_sz > sz || pos > sz - post_sz) { snprintf(errormessage, MSG_SIZE, "PPG v2 truncated (post)"); return false; }
        std::vector<uint8_t> post(p + pos, p + pos + post_sz); pos += post_sz;
        uint32_t num_frames = rd_le32(p + pos); pos += 4;
        (void)is_apng;
        // Audit fix #2: cap num_frames by remaining file size (each frame meta has at least
        // 1 byte uses_idat + 5 bytes deflate params + 4 bytes nc + 16 bytes szs = 26 min).
        // Use 20 as conservative lower bound on per-frame meta footprint.
        if (num_frames > (sz - pos) / 20 + 1) {
            snprintf(errormessage, MSG_SIZE, "PPG v2 num_frames implausible (%u)", num_frames); return false;
        }

        std::vector<FrameMeta> frames(num_frames);
        for (uint32_t i = 0; i < num_frames; i++)
            if (!read_frame_v2(p, sz, pos, frames[i])) return false;

        png_out.clear();
        wr_bytes(png_out, PNG_SIG, 8);
        wr_bytes(png_out, pre.data(), pre.size());
        for (auto& fm : frames) {
            if (!fm.fctl.empty()) {
                uint32_t crc = chunk_crc((const uint8_t*)"fcTL", fm.fctl.data(), fm.fctl.size());
                wr_be32(png_out, (uint32_t)fm.fctl.size());
                wr_bytes(png_out, (const uint8_t*)"fcTL", 4);
                wr_bytes(png_out, fm.fctl.data(), fm.fctl.size());
                wr_be32(png_out, crc);
            }
            if (!emit_idat_or_fdat(png_out, fm)) return false;
        }
        wr_bytes(png_out, post.data(), post.size());
        return true;
    }

    if (version >= 3 && version <= 15)
        return decompress_solid(p, sz, pos, version, png_out);

    snprintf(errormessage, MSG_SIZE, "unknown PPG version %u", version); return false;
}

/* ─── pixel-level PNG comparison (for -ldf verify) ──────────────────────── */

// Returns true if both PNGs have identical inflated pixel data for all frames.
static bool compare_png_pixels(const std::vector<uint8_t>& a,
                                const std::vector<uint8_t>& b)
{
    PngInfo ia, ib;
    if (!parse_png(a, ia) || !parse_png(b, ib)) return false;
    if (ia.frames.size() != ib.frames.size()) return false;
    for (size_t i = 0; i < ia.frames.size(); i++) {
        if (ia.frames[i].pixels != ib.frames[i].pixels) return false;
    }
    return true;
}

/* ─── file helpers ───────────────────────────────────────────────────────── */

static std::vector<uint8_t> read_file(const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto sz = fs::file_size(path, ec);
    if (ec || sz == 0) return {};
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return {};
    std::vector<uint8_t> buf((size_t)sz);
    size_t got = fread(buf.data(), 1, buf.size(), f);
    fclose(f);
    // Audit fix #6: partial read = corrupt file, refuse to process garbage tail.
    if (got != buf.size()) return {};
    return buf;
}

static bool write_file(const std::string& path, const std::vector<uint8_t>& data) {
    // Audit fix #5: atomic .tmp + rename. Prevents losing the original file
    // when the encoder errors mid-stream (disk full, ctrl+c, fwrite failure).
    namespace fs = std::filesystem;
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    if (fflush(f) != 0) ok = false;
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        std::error_code ec;
        fs::remove(tmp, ec);
        return false;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        // Some Windows filesystems error rename-over-existing — fall back to remove+rename.
        fs::remove(path, ec);
        fs::rename(tmp, path, ec);
        if (ec) { fs::remove(tmp, ec); return false; }
    }
    return true;
}

enum FileType { F_PNG, F_PPG, F_TCIP, F_JNG, F_TCIJ, F_UNK };

static FileType detect_type(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return F_UNK;
    uint8_t buf[8]; size_t n = fread(buf, 1, 8, f); fclose(f);
    if (n >= 8 && memcmp(buf, PNG_SIG, 8) == 0) return F_PNG;
    if (n >= 8 && memcmp(buf, JNG_SIG, 8) == 0) return F_JNG;
    if (n >= 4 && memcmp(buf, TCIP_SIG, 4) == 0) return F_TCIP;
    if (n >= 4 && memcmp(buf, TCIJ_SIG, 4) == 0) return F_TCIJ;
    if (n >= 4 && memcmp(buf, PPGS_SIG, 4) == 0) return F_TCIP;  // legacy
    if (n >= 4 && memcmp(buf, PPG_SIG, 4) == 0) return F_PPG;
    return F_UNK;
}

static std::string replace_ext(const std::string& path, const std::string& ext) {
    auto dot = path.rfind('.');
    return (dot == std::string::npos ? path : path.substr(0, dot)) + ext;
}

// v1.7 collision-rename: when two inputs would produce the same outpath
// (e.g. -r over a tree with 177× "Imagen1(1).png" in different subdirs and
// no -fs), append "(N)" before the extension to disambiguate. Threadsafe
// because process_file runs file-level threads.
static std::mutex g_outpath_mutex;
static std::unordered_set<std::string> g_outpaths_used;

static std::string reserve_unique(const std::string& proposed) {
    namespace fs = std::filesystem;
    std::lock_guard<std::mutex> lk(g_outpath_mutex);
    if (g_outpaths_used.find(proposed) == g_outpaths_used.end()) {
        g_outpaths_used.insert(proposed);
        return proposed;
    }
    fs::path orig(proposed);
    std::string stem   = orig.stem().string();
    std::string ext    = orig.extension().string();
    fs::path    parent = orig.parent_path();
    for (int i = 1; i < 100000; i++) {
        std::string cand_name = stem + "(" + std::to_string(i) + ")" + ext;
        std::string cand = parent.empty()
            ? cand_name
            : (parent / cand_name).string();
        if (g_outpaths_used.find(cand) == g_outpaths_used.end()) {
            g_outpaths_used.insert(cand);
            return cand;
        }
    }
    g_outpaths_used.insert(proposed);
    return proposed;
}

static std::string make_outpath(const std::string& inpath, const std::string& ext,
                                const std::string& src_root) {
    namespace fs = std::filesystem;
    std::string base = replace_ext(fs::path(inpath).filename().string(), ext);
    if (!outdir.empty()) {
        // -fs: when the user expanded a directory with -r and asked to preserve
        // structure, mirror the path's parent directory (relative to src_root)
        // under outdir. e.g. -r -fs -odOUT TILIN with TILIN/sub/foo.png →
        // OUT/sub/foo.png.ppg (TILIN's basename is stripped — caesium-clt's
        // -RS semantics).
        if (fs_mode && !src_root.empty()) {
            std::error_code ec;
            fs::path rel = fs::relative(fs::path(inpath).parent_path(),
                                        fs::path(src_root), ec);
            if (!ec && !rel.empty() && rel.string() != ".") {
                fs::path full = fs::path(outdir) / rel / base;
                fs::create_directories(full.parent_path(), ec);
                return reserve_unique(full.string());
            }
        }
        return reserve_unique((fs::path(outdir) / base).string());
    }
    return reserve_unique((fs::path(inpath).parent_path() / base).string());
}

/* ─── process one file ───────────────────────────────────────────────────── */

// Bar helpers — caller must hold g_print_mutex.
static void clear_bar() {
    if (!g_show_bar) return;
    fprintf(stdout, "\r%*s\r", BARLEN + 34, "");
}

static void draw_bar(int done) {
    if (!g_show_bar) return;
    int total = g_total_files > 0 ? g_total_files : 1;
    // Throttle: skip redraw if <50ms since last; always draw the final.
    // Windows console rendering is slow (~25ms/redraw with ANSI); throttling
    // shaves up to 12s on a 450-file batch. Linux terminals are fast either way.
    static std::atomic<int64_t> last_draw_ms{0};
    if (done < total) {
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        int64_t prev = last_draw_ms.load(std::memory_order_relaxed);
        if (now_ms - prev < 50) return;
        last_draw_ms.store(now_ms, std::memory_order_relaxed);
    }
    int barpos = (done * BARLEN) / total;
#if defined(_WIN32)
    static const char* spinners[] = { "-", "\\", "|", "/" };
    const char* spin = spinners[g_spinner_idx.fetch_add(1, std::memory_order_relaxed) % 4];
    const char* fill = "#"; const char* empty = " ";
#else
    static const char* spinners[] = {
        "\xe2\xa0\x8b","\xe2\xa0\x99","\xe2\xa0\xb9","\xe2\xa0\xb8","\xe2\xa0\xbc",
        "\xe2\xa0\xb4","\xe2\xa0\xa6","\xe2\xa0\xa7","\xe2\xa0\x87","\xe2\xa0\x8f"
    };
    const char* spin = spinners[g_spinner_idx.fetch_add(1, std::memory_order_relaxed) % 10];
    const char* fill = "\xe2\x96\x88"; const char* empty = "\xe2\x96\x91";
#endif
    fprintf(stdout, "\r  %s  %3i / %-3i  %s[", spin, done, total, col(BC));
    for (int b = 0; b < barpos; b++)         fputs(fill,  stdout);
    fputs(col(R), stdout);
    for (int b = barpos; b < BARLEN; b++)    fputs(empty, stdout);
    fputs("]", stdout);
    fflush(stdout);
}

static void finish_bar() {
    if (!g_show_bar) return;
    int total = g_total_files;
#if defined(_WIN32)
    const char* check = "+"; const char* fill = "#";
#else
    const char* check = "\xe2\x9c\x93"; const char* fill = "\xe2\x96\x88";
#endif
    fprintf(stdout, "\r  %s  %3i / %-3i  %s[", check, total, total, col(BC));
    for (int b = 0; b < BARLEN; b++) fputs(fill, stdout);
    fprintf(stdout, "%s]   \n", col(R));
    fflush(stdout);
}

// Audit fix #8: keep the progress bar advancing even for files that are skipped
// (wrong type, output exists, error, etc.). Caller must hold g_print_mutex.
static inline void tick_bar() {
    int done = g_files_done.fetch_add(1, std::memory_order_relaxed) + 1;
    draw_bar(done);
}

/* ─── tovyCIP ARCHIVE — Tovy Compresor de Imágenes PNG (TCIP v1) ─────────── */
// All N PNGs go into ONE archive with a single kanzi context for all pixel
// data and a single zstd context for all IDAT data. Single context init = no
// per-file overhead. kanzi jobs=4 internal = full 4-core saturation on decode.
// Result: strict 4-axis win vs xz preset 6 (size, comp, decST, dec-th0).

// (TCIP_SIG / PPGS_SIG declared above near PPG_SIG to allow detect_type to use them)

struct SolidEntry {
    std::string name;
    std::vector<uint8_t> meta;   // serialized per-PNG metadata (no compressed streams)
    uint64_t px_off = 0, px_sz = 0;
    uint64_t idat_off = 0, idat_sz = 0;
    bool has_mode2 = false;
    uint8_t stream_idx = 0;      // PPGS v2: which kanzi pixel stream owns this entry's data
};

// Parse PNG, classify frames, append raw pixel/idat to big buffers, serialize
// per-PNG metadata into entry.meta. Mirrors compress_png parse+probe phase.
static bool extract_solid_streams(
    const std::vector<uint8_t>& png_buf,
    std::vector<uint8_t>& big_px,
    std::vector<uint8_t>& big_idat,
    SolidEntry& entry)
{
    PngInfo info;
    if (!parse_png(png_buf, info)) return false;

    IhdrInfo ihdr = parse_ihdr(info.pre);
    uint32_t bpp  = compute_bpp(ihdr.color_type, ihdr.bit_depth);

    std::vector<FrameEnc> enc;
    enc.reserve(info.frames.size());
    std::vector<std::vector<uint8_t>> sep_filter_bufs(info.frames.size());
    std::vector<std::vector<uint8_t>> sep_pixel_bufs(info.frames.size());

    for (size_t i = 0; i < info.frames.size(); i++) {
        auto& fr = info.frames[i];
        FrameEnc fe{};
        fe.matched = !fr.pixels.empty() &&
                     find_deflate_params(fr.pixels, fr.idat_raw, fe.dp);
        if (!fe.matched && ldf_repack && !fr.pixels.empty()) {
#ifdef USE_LIBDEFLATE
            fe.ldf_mode  = true;
            fe.ldf_level = 9;
#endif
        }
        fe.filter_sep = false;
        // Skip filter_sep when pixels >= g_nofsep_above (set by CLI/env).
        if ((fe.matched || fe.ldf_mode) && bpp > 0
            && (g_nofsep_above == 0 || fr.pixels.size() < g_nofsep_above)) {
            uint32_t fw, fh;
            frame_dims(fr, ihdr, fw, fh);
            fe.stride      = fw * bpp;
            fe.n_scanlines = fh;
            if (fe.stride > 0 && fe.n_scanlines > 0) {
                if (sep_filters(fr.pixels, fe.stride,
                                sep_filter_bufs[i], sep_pixel_bufs[i])) {
                    fe.filter_sep = true;
                }
            }
        }
        fe.payload_sz = (fe.matched || fe.ldf_mode) ? fr.pixels.size() : fr.idat_raw.size();
        enc.push_back(fe);
    }

    bool has_mode2 = false;
    for (auto& fe : enc) if (fe.ldf_mode) { has_mode2 = true; break; }
    entry.has_mode2 = has_mode2;

    // Append per-frame raw payloads to big buffers (split by mode)
    entry.px_off   = big_px.size();
    entry.idat_off = big_idat.size();
    for (size_t i = 0; i < info.frames.size(); i++) {
        auto& fr = info.frames[i];
        auto& fe = enc[i];
        bool is_pixel = (fe.matched || fe.ldf_mode);
        std::vector<uint8_t>* target = is_pixel ? &big_px : &big_idat;
        if (fe.filter_sep) {
            target->insert(target->end(),
                           sep_filter_bufs[i].begin(), sep_filter_bufs[i].end());
            target->insert(target->end(),
                           sep_pixel_bufs[i].begin(), sep_pixel_bufs[i].end());
        } else {
            const uint8_t* p = is_pixel ? fr.pixels.data() : fr.idat_raw.data();
            target->insert(target->end(), p, p + fe.payload_sz);
        }
    }
    entry.px_sz   = big_px.size()   - entry.px_off;
    entry.idat_sz = big_idat.size() - entry.idat_off;

    // Serialize per-PNG metadata (mirrors compress_png header layout, no streams)
    auto& m = entry.meta;
    m.clear();
    m.push_back(info.is_apng ? 1 : 0);
    wr_le32(m, info.num_plays);
    wr_le64(m, (uint64_t)info.pre.size());
    m.insert(m.end(), info.pre.begin(), info.pre.end());
    wr_le64(m, (uint64_t)info.post.size());
    m.insert(m.end(), info.post.begin(), info.post.end());
    wr_le32(m, (uint32_t)enc.size());

    for (size_t i = 0; i < enc.size(); i++) {
        auto& fr = info.frames[i];
        auto& fe = enc[i];
        m.push_back(fr.fctl.empty() ? 0 : 1);
        if (!fr.fctl.empty()) {
            if (fr.fctl.size() != 26) {
                snprintf(errormessage, MSG_SIZE, "bad fctl size"); return false;
            }
            m.insert(m.end(), fr.fctl.begin(), fr.fctl.end());
        }
        m.push_back(fr.uses_idat ? 1 : 0);
        if (!fr.uses_idat) wr_le32(m, fr.first_seq);
        uint8_t mode_byte = fe.matched ? 0 : (fe.ldf_mode ? 2 : 1);
        m.push_back(mode_byte);
        m.push_back(fe.matched ? (uint8_t)fe.dp.level    : (fe.ldf_mode ? fe.ldf_level : 0));
        m.push_back(fe.matched ? (uint8_t)fe.dp.strategy : 0);
        m.push_back(fe.matched ? (uint8_t)fe.dp.wbits    : 0);
        m.push_back(fe.matched ? (uint8_t)fe.dp.memlevel : 0);
        uint32_t nc_out = fe.ldf_mode ? 0u : (uint32_t)fr.chunk_szs.size();
        wr_le32(m, nc_out);
        if (!fe.ldf_mode)
            for (uint32_t s : fr.chunk_szs) wr_le32(m, s);
        wr_le64(m, (uint64_t)fe.payload_sz);
        m.push_back(fe.filter_sep ? 1 : 0);
        if (fe.filter_sep) {
            wr_le32(m, fe.stride);
            wr_le32(m, fe.n_scanlines);
        }
    }
    return true;
}

// Inverse of extract_solid_streams: reconstruct PNG from per-entry metadata
// + slices of already-decoded big_px / big_idat. Mirrors decompress_solid
// reconstruction phase but works on pre-decoded data.
static bool reconstruct_png_from_streams(
    const std::vector<uint8_t>& meta,
    const uint8_t* px_data, size_t px_size,
    const uint8_t* idat_data, size_t idat_size,
    std::vector<uint8_t>& png_out)
{
    const uint8_t* p = meta.data();
    size_t sz = meta.size();
    size_t pos = 0;

    if (pos + 1 + 4 + 8 > sz) {
        snprintf(errormessage, MSG_SIZE, "Solid entry meta truncated"); return false;
    }
    bool is_apng = (p[pos++] & 1) != 0; (void)is_apng;
    uint32_t num_plays = rd_le32(p + pos); pos += 4; (void)num_plays;
    uint64_t pre_sz = rd_le64(p + pos); pos += 8;
    if (pre_sz > sz || pos > sz - pre_sz) { snprintf(errormessage, MSG_SIZE, "meta truncated (pre)"); return false; }
    std::vector<uint8_t> pre(p + pos, p + pos + pre_sz); pos += pre_sz;
    uint64_t post_sz = rd_le64(p + pos); pos += 8;
    if (post_sz > sz || pos > sz - post_sz) { snprintf(errormessage, MSG_SIZE, "meta truncated (post)"); return false; }
    std::vector<uint8_t> post(p + pos, p + pos + post_sz); pos += post_sz;
    uint32_t num_frames = rd_le32(p + pos); pos += 4;
    // Sanity cap: a corrupt meta can claim billions of frames → multi-GB
    // FrameMeta allocation. APNGs in the wild rarely exceed a few hundred.
    // 100k is an absurd ceiling that would never be legitimate.
    if (num_frames > 100000u) {
        snprintf(errormessage, MSG_SIZE,
                 "TCIP entry: implausible num_frames %u (>100k)", num_frames);
        return false;
    }

    std::vector<FrameMeta> frames(num_frames);
    std::vector<uint64_t>  payload_szs(num_frames);
    for (uint32_t i = 0; i < num_frames; i++) {
        // Pass version=14 to enable filter_sep + ldf parsing
        if (!read_frame_solid_meta(p, sz, pos, frames[i], payload_szs[i], 14))
            return false;
    }

    // Slice big bufs into per-frame payloads using mode (matched/ldf → pixel; stored → idat)
    size_t px_off = 0, idat_off = 0;
    for (uint32_t i = 0; i < num_frames; i++) {
        size_t psz = (size_t)payload_szs[i];
        bool from_pixel = (frames[i].mode != 1);
        const uint8_t* src = from_pixel ? px_data : idat_data;
        size_t src_sz     = from_pixel ? px_size  : idat_size;
        size_t& off       = from_pixel ? px_off   : idat_off;
        // Bounds check rewritten to avoid integer overflow when psz is corrupt.
        // (Original `off + psz > src_sz` could wrap around if psz is huge.)
        if (psz > src_sz || off > src_sz - psz) {
            snprintf(errormessage, MSG_SIZE, "tovyCIP payload split frame %u (%s): psz=%zu off=%zu src_sz=%zu",
                     i, from_pixel ? "px" : "idat", psz, off, src_sz); return false;
        }
        frames[i].payload.assign(src + off, src + off + psz);
        off += psz;
    }

    png_out.clear();
    wr_bytes(png_out, PNG_SIG, 8);
    wr_bytes(png_out, pre.data(), pre.size());
    for (auto& fm : frames) {
        if (!fm.fctl.empty()) {
            uint32_t crc = chunk_crc((const uint8_t*)"fcTL", fm.fctl.data(), fm.fctl.size());
            wr_be32(png_out, (uint32_t)fm.fctl.size());
            wr_bytes(png_out, (const uint8_t*)"fcTL", 4);
            wr_bytes(png_out, fm.fctl.data(), fm.fctl.size());
            wr_be32(png_out, crc);
        }
        if (!emit_idat_or_fdat(png_out, fm)) return false;
    }
    wr_bytes(png_out, post.data(), post.size());
    return true;
}

#ifdef USE_KANZI
// Kanzi encode with explicit jobs/block params + explicit pipeline for solid streams.
static bool kanzi_solid_enc_pipe(const uint8_t* in, size_t insz,
                                  const std::string& transform, const std::string& entropy,
                                  int jobs, int block_size,
                                  std::vector<uint8_t>& out) {
    try {
        std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
        kanzi::CompressedOutputStream cos(ss, jobs, entropy, transform, block_size);
        cos.write((const char*)in, (std::streamsize)insz);
        cos.close();
        std::string s = ss.str();
        out.assign(s.begin(), s.end());
        return true;
    } catch (const std::exception& e) {
        snprintf(errormessage, MSG_SIZE, "Solid kanzi enc: %s", e.what()); return false;
    }
}


static bool kanzi_solid_dec(const uint8_t* in, size_t insz,
                             int jobs,
                             std::vector<uint8_t>& out, size_t expected) {
    static const size_t MAX_RAW = 1ull << 36;  // 64 GB sanity cap (real corpora can have >1GB raw pixels)
    if (expected > MAX_RAW) {
        snprintf(errormessage, MSG_SIZE, "Solid kanzi dec: implausible raw size %zu (>64GB)", expected);
        return false;
    }
    try {
        membuf mb((const char*)in, insz);
        std::istream ss(&mb);
        kanzi::CompressedInputStream cis(ss, jobs);
        out.resize(expected);
        size_t total_read = 0;
        while (total_read < expected) {
            cis.read((char*)out.data() + total_read,
                     (std::streamsize)(expected - total_read));
            auto got = cis.gcount();
            if (got <= 0) break;
            total_read += (size_t)got;
        }
        cis.close();
        if (total_read != expected) {
            snprintf(errormessage, MSG_SIZE, "Solid kanzi dec: short read"); return false;
        }
        return true;
    } catch (const std::exception& e) {
        snprintf(errormessage, MSG_SIZE, "Solid kanzi dec: %s", e.what()); return false;
    }
}

// Watchdog wrapper around kanzi_solid_dec. A corrupted .tcip can drive kanzi
// into an internal infinite loop (rare: ~3 seeds in 1500 fuzz trials). Wrap the
// decode in a future and abort with an error if it doesn't complete within
// `timeout_ms`. The orphan thread is detached but owns a SHARED COPY of the
// input bytes — so even after the parent's `buf` is freed, the kanzi thread
// keeps reading valid memory. The leaked thread keeps spinning until the
// process exits (when the OS reaps everything).
static bool kanzi_solid_dec_with_timeout(
    const uint8_t* in, size_t insz, int jobs,
    std::vector<uint8_t>& out, size_t expected,
    int timeout_ms)
{
    auto promise = std::make_shared<std::promise<bool>>();
    auto future  = promise->get_future();
    // Copy input + outputs to shared_ptr so detach is safe.
    auto in_copy    = std::make_shared<std::vector<uint8_t>>(in, in + insz);
    auto out_holder = std::make_shared<std::vector<uint8_t>>();
    auto err_holder = std::make_shared<std::string>();
    std::thread([promise, in_copy, jobs, expected, out_holder, err_holder]() {
        bool ok = kanzi_solid_dec(in_copy->data(), in_copy->size(),
                                   jobs, *out_holder, expected);
        if (!ok) *err_holder = errormessage;
        try { promise->set_value(ok); } catch (...) { /* future abandoned */ }
    }).detach();
    auto status = future.wait_for(std::chrono::milliseconds(timeout_ms));
    if (status == std::future_status::timeout) {
        snprintf(errormessage, MSG_SIZE,
                 "Solid kanzi dec: timed out after %d ms (likely corrupt bitstream)",
                 timeout_ms);
        // Leak: the detached thread keeps spinning until process exit.
        // Acceptable for a CLI tool — the OS will reap on exit.
        return false;
    }
    bool ok = future.get();
    if (ok) out = std::move(*out_holder);
    else if (!err_holder->empty()) snprintf(errormessage, MSG_SIZE, "%s", err_holder->c_str());
    return ok;
}

#else
static bool kanzi_solid_enc_pipe(const uint8_t*, size_t, const std::string&, const std::string&,
                                  int, int, std::vector<uint8_t>&) {
    snprintf(errormessage, MSG_SIZE, "tovyCIP requires USE_KANZI"); return false;
}
static bool kanzi_solid_dec(const uint8_t*, size_t, int, std::vector<uint8_t>&, size_t) {
    snprintf(errormessage, MSG_SIZE, "tovyCIP requires USE_KANZI"); return false;
}
static bool kanzi_solid_dec_with_timeout(const uint8_t*, size_t, int,
                                         std::vector<uint8_t>&, size_t, int) {
    snprintf(errormessage, MSG_SIZE, "tovyCIP requires USE_KANZI"); return false;
}
#endif

static bool compress_tovycip_archive(
    const std::vector<std::string>& png_paths,
    const std::string& out_path,
    const std::vector<std::string>& src_roots = {},
    std::vector<uint8_t>* out_bytes = nullptr)
{
    namespace fs = std::filesystem;
    std::vector<SolidEntry> entries;
    std::vector<uint8_t> big_px, big_idat;

    // Parallel extract: each PNG's brute-force zlib param match + filter
    // separation is CPU-heavy (~10-50 ms per file). Process them in parallel
    // into per-entry local buffers; concat afterward in a sequential pass.
    struct ExtractWork {
        std::string path;
        std::string name;
        std::vector<uint8_t> px;    // local pixel section
        std::vector<uint8_t> idat;  // local idat section
        SolidEntry e;
        bool ok = false;
        std::string err;
    };
    std::vector<ExtractWork> works(png_paths.size());
    for (size_t i = 0; i < png_paths.size(); i++) {
        works[i].path = png_paths[i];
        // Preserve subdir structure relative to src_root when provided.
        // Without a src_root we fall back to the basename. Stored names
        // use '/' as separator for cross-platform portability.
        std::string nm;
        const std::string& sr = (i < src_roots.size()) ? src_roots[i] : std::string();
        if (!sr.empty()) {
            std::error_code ec;
            auto rel = fs::relative(fs::path(png_paths[i]), fs::path(sr), ec);
            if (!ec && !rel.empty() && rel.native().front() != '.') {
                nm = rel.generic_string();
            }
        }
        if (nm.empty()) nm = fs::path(png_paths[i]).filename().string();
        works[i].name = nm;
    }
    // Bar progress is owned by the outer caller (main loop in v1.7+):
    // each invocation of compress_tovycip_archive corresponds to ONE input
    // PNG in per-file mode, so the per-work tick_bar() that v1.6b added would
    // double-count. Keep this encoder bar-agnostic.
    int n_extract = (int)std::thread::hardware_concurrency();
    if (n_extract < 1) n_extract = 1;
    if ((size_t)n_extract > works.size()) n_extract = (int)works.size();
    std::atomic<size_t> next_idx{0};
    std::vector<std::thread> ext_threads;
    for (int t = 0; t < n_extract; t++) {
        ext_threads.emplace_back([&] {
            for (;;) {
                size_t i = next_idx.fetch_add(1);
                if (i >= works.size()) break;
                auto& w = works[i];
                auto png_buf = read_file(w.path);
                if (png_buf.empty()) continue;
                w.e.name = w.name;
                w.ok = extract_solid_streams(png_buf, w.px, w.idat, w.e);
                if (!w.ok) w.err = errormessage;
            }
        });
    }
    for (auto& th : ext_threads) th.join();
    // Sequential concat — preserves input order for entries.
    for (auto& w : works) {
        if (!w.ok) {
            fprintf(stderr, "%sskip%s %s: %s\n", col(YL), col(R),
                    w.path.c_str(), w.err.c_str());
            continue;
        }
        // Adjust offsets to point into the global big_px / big_idat
        w.e.px_off   = big_px.size();
        w.e.idat_off = big_idat.size();
        big_px.insert(big_px.end(), w.px.begin(), w.px.end());
        big_idat.insert(big_idat.end(), w.idat.begin(), w.idat.end());
        entries.push_back(std::move(w.e));
    }
    if (entries.empty()) {
        snprintf(errormessage, MSG_SIZE, "no PNGs successfully packed"); return false;
    }
    bool has_mode2 = false;
    for (auto& e : entries) if (e.has_mode2) { has_mode2 = true; break; }

    // Reorder big_px by entries[i].px_sz DESC. Putting the biggest pixel block
    // at offset 0 lets the decoder, with kanzi jobs=1 + progressive read, start
    // reconstructing the bottleneck file (e.g. Cspeed 4.27MB) as soon as the
    // first kanzi block decodes — overlapping with subsequent block decodes.
    // Compute biggest px_sz to size kanzi blocks ≥ that file (so it fits in one
    // kanzi block).
    size_t max_px_sz = 0;
    for (auto& e : entries) if (e.px_sz > max_px_sz) max_px_sz = e.px_sz;
    {
        // Capture original (px_off, px_sz) per entry; rebuild big_px in sorted order.
        std::vector<std::pair<uint64_t,uint64_t>> orig(entries.size());
        for (size_t i = 0; i < entries.size(); i++)
            orig[i] = {entries[i].px_off, entries[i].px_sz};
        std::vector<size_t> ord(entries.size());
        std::iota(ord.begin(), ord.end(), (size_t)0);
        std::sort(ord.begin(), ord.end(), [&](size_t a, size_t b){
            if (orig[a].second != orig[b].second) return orig[a].second > orig[b].second;
            return a < b;  // stable for equal sizes
        });
        std::vector<uint8_t> new_px;
        new_px.reserve(big_px.size());
        std::vector<SolidEntry> new_entries;
        new_entries.reserve(entries.size());
        for (size_t k : ord) {
            SolidEntry e = std::move(entries[k]);
            e.px_off = (uint64_t)new_px.size();
            new_px.insert(new_px.end(),
                          big_px.begin() + orig[k].first,
                          big_px.begin() + orig[k].first + orig[k].second);
            new_entries.push_back(std::move(e));
        }
        big_px = std::move(new_px);
        entries = std::move(new_entries);
    }

    // PPGS v2: split big_px into NUM_STREAMS independent kanzi streams. With
    // entries sorted by px_sz DESC (above), stream 0 holds entries[0..N-1] (the
    // biggest one(s)), stream 1 the rest. Each stream is decoded in its own
    // thread at decode time → parallel decode bypasses the kanzi-internal
    // single-block bottleneck and cuts total decode latency.
    // Adaptive stream count:
    //   1 stream  if entries.size() < 2 OR big_px.size() < 1 MB.
    //   2 streams if biggest entry dominates (default — current behaviour).
    //   3 streams if entries[0] AND entries[1] are both >= 1MB pixels: gives
    //     each big file its own decode thread, parallelism scales further on
    //     hw_concurrency >= 4 cores.
    //   Cap at 3 — past that, per-stream kanzi wrapper overhead outweighs the
    //   parallelism win (also limits #worker / #stream on small core counts).
    int NUM_STREAMS = 1;
    if (entries.size() >= 2 && big_px.size() >= 1024*1024) {
        NUM_STREAMS = 2;
        if (entries.size() >= 3
            && entries[1].px_sz >= 1024 * 1024
            && std::thread::hardware_concurrency() >= 4) {
            NUM_STREAMS = 3;
        }
    }
    std::vector<std::vector<uint8_t>> stream_px(NUM_STREAMS);
    {
        // Capture original (px_off, px_sz) per entry, then redistribute to per-stream buffers.
        // Heuristic for N=2:  stream 0 = entries[0]; stream 1 = entries[1..].
        // Heuristic for N=3:  stream 0 = entries[0]; stream 1 = entries[1]; stream 2 = rest.
        // For N=1, all in stream 0.
        std::vector<std::pair<uint64_t,uint64_t>> orig(entries.size());
        for (size_t i = 0; i < entries.size(); i++)
            orig[i] = {entries[i].px_off, entries[i].px_sz};
        for (size_t i = 0; i < entries.size(); i++) {
            uint8_t sidx;
            if (NUM_STREAMS <= 1)      sidx = 0;
            else if (NUM_STREAMS == 2) sidx = (i == 0) ? 0 : 1;
            else /* 3 */               sidx = (i == 0) ? 0 : (i == 1) ? 1 : 2;
            entries[i].stream_idx = sidx;
            entries[i].px_off = (uint64_t)stream_px[sidx].size();
            stream_px[sidx].insert(stream_px[sidx].end(),
                                    big_px.begin() + orig[i].first,
                                    big_px.begin() + orig[i].first + orig[i].second);
        }
        // big_px no longer authoritative — per-stream buffers are.
        big_px.clear(); big_px.shrink_to_fit();
    }

    // Encode big buffers in parallel: N kanzi streams + zstd-19 long for idat.
    // Each kanzi stream gets its own thread. With 2 streams + 1 idat, that's 3
    // encode threads working in parallel.
    std::vector<std::vector<uint8_t>> stream_comp(NUM_STREAMS);
    std::vector<bool> stream_ok(NUM_STREAMS, true);
    std::vector<std::string> stream_err(NUM_STREAMS);
    std::vector<uint8_t> idat_comp;
    bool idat_ok = true;
    std::vector<std::thread> px_threads;
    for (int s = 0; s < NUM_STREAMS; s++) {
        if (stream_px[s].empty()) continue;
        px_threads.emplace_back([&, s] {
            const char* t_env = getenv("PACKPNG_SOLID_T");
            const char* e_env = getenv("PACKPNG_SOLID_E");
            const char* bs_env = getenv("PACKPNG_SOLID_BLK");
            const char* j_env = getenv("PACKPNG_SOLID_J");
            std::string t, e;
            int bs, jb;
            if (use_kpng_max) {
                t = "RLT"; e = "TPAQ"; bs = 2 * 1024 * 1024; jb = 8;
            } else {
                // Multi-stream Pareto: stream 0 holds the biggest entry (likely
                // 4MB+) — give it smaller blocks (1MB) so kanzi's internal
                // jobs=4 fully parallelizes. Stream 1 (smaller files) keeps 4MB
                // blocks for best cross-block ratio.
                t = "RLT+BWT+SRT+ZRLT"; e = "FPAQ";
                // Streams that hold a single dominant entry (s 0 always; s 1
                // when NUM_STREAMS == 3) use block=2MB so kanzi's internal
                // jobs=4 parallelizes the multiple blocks of the big file.
                // The "rest" stream uses block=4MB for better cross-block ratio.
                bool single_dominant_stream =
                    (NUM_STREAMS >= 2 && s == 0) ||
                    (NUM_STREAMS == 3 && s == 1);
                bs = single_dominant_stream ? (2 * 1024 * 1024)
                                            : (4 * 1024 * 1024);
                jb = 4;
            }
            (void)max_px_sz;
            if (t_env)  t = t_env;
            if (e_env)  e = e_env;
            if (bs_env) bs = atoi(bs_env);
            if (j_env)  jb = atoi(j_env);
            if (bs < 65536) bs = 65536;
            if (jb < 1) jb = 1;
            // Per-stream jobs cap: more streams → less per-stream parallelism
            // (we want independent threads to do useful work, not contend).
            int per_stream_jobs = std::max(1, jb / NUM_STREAMS);
            std::vector<uint8_t> out;
            bool ok = kanzi_solid_enc_pipe(stream_px[s].data(), stream_px[s].size(),
                                            t, e, per_stream_jobs, bs, out);
            stream_comp[s] = std::move(out);
            stream_ok[s] = ok;
            if (!ok) stream_err[s] = errormessage;  // capture per-thread error
        });
    }
    if (!big_idat.empty()) {
        idat_ok = zstd_enc_long(big_idat.data(), big_idat.size(), 19, idat_comp);
    }
    for (auto& t : px_threads) t.join();
    bool px_ok = true;
    for (int s = 0; s < NUM_STREAMS; s++) {
        if (!stream_ok[s]) {
            px_ok = false;
            snprintf(errormessage, MSG_SIZE, "stream %d: %s", s, stream_err[s].c_str());
            break;
        }
    }
    if (!px_ok || !idat_ok) return false;

    // PPGS v2 meta table: per entry includes a u8 stream_idx so the decoder
    // knows which kanzi stream owns the entry's pixel data.
    std::vector<uint8_t> meta_table;
    for (auto& e : entries) {
        uint16_t nlen = (uint16_t)e.name.size();
        meta_table.push_back((uint8_t)(nlen & 0xff));
        meta_table.push_back((uint8_t)((nlen >> 8) & 0xff));
        meta_table.insert(meta_table.end(), e.name.begin(), e.name.end());
        wr_le32(meta_table, (uint32_t)e.meta.size());
        meta_table.insert(meta_table.end(), e.meta.begin(), e.meta.end());
        meta_table.push_back(e.stream_idx);             // v2 addition
        wr_le64(meta_table, e.px_off);
        wr_le64(meta_table, e.px_sz);
        wr_le64(meta_table, e.idat_off);
        wr_le64(meta_table, e.idat_sz);
    }
    std::vector<uint8_t> meta_comp;
    if (!zstd_enc(meta_table.data(), meta_table.size(), meta_comp)) return false;

    // Write PPGS archive (v2 layout)
    std::vector<uint8_t> out;
    wr_bytes(out, TCIP_SIG, 4);
    out.push_back(2);                       // version 2 (multi-stream pixel)
    out.push_back(has_mode2 ? 1 : 0);
    wr_le32(out, (uint32_t)entries.size());
    wr_le32(out, (uint32_t)meta_table.size());
    wr_le32(out, (uint32_t)meta_comp.size());
    out.insert(out.end(), meta_comp.begin(), meta_comp.end());
    // Multi-stream pixel section: u8 num_streams, then per stream [u64 raw, u64 comp, bytes].
    out.push_back((uint8_t)NUM_STREAMS);
    for (int s = 0; s < NUM_STREAMS; s++) {
        wr_le64(out, (uint64_t)stream_px[s].size());
        wr_le64(out, (uint64_t)stream_comp[s].size());
        out.insert(out.end(), stream_comp[s].begin(), stream_comp[s].end());
    }
    wr_le64(out, (uint64_t)big_idat.size());
    wr_le64(out, (uint64_t)idat_comp.size());
    out.insert(out.end(), idat_comp.begin(), idat_comp.end());

    if (verbosity >= 1) {
        size_t spx_raw = 0, spx_comp = 0;
        for (int s = 0; s < NUM_STREAMS; s++) {
            spx_raw  += stream_px[s].size();
            spx_comp += stream_comp[s].size();
        }
        fprintf(stdout, "  %stovyCIP%s %zu files [%d-stream]: px=%zu→%zu idat=%zu→%zu total→%zu\n",
                col(BC), col(R), entries.size(), NUM_STREAMS,
                spx_raw, spx_comp,
                big_idat.size(), idat_comp.size(),
                out.size());
    }
    // v1.7e: caller-visible bytes for verify/dry-run paths.
    if (out_bytes) { *out_bytes = std::move(out); return true; }
    if (dry_run) return true;
    return write_file(out_path, out);
}

/* ─── JNG (JPEG Network Graphics) — Phase 1 container support ───────────────
 *
 * v1.8: parse a .jng input into 3 sections, zstd-compress each, and emit a
 * .ppg with TCIJ_SIG. Round-trip is byte-exact: head/image/tail preserve raw
 * chunk bytes (length+type+data+crc), so original CRCs survive untouched.
 *
 *   head  = bytes between JNG_SIG and the first JDAT/IDAT/JDAA/JSEP chunk
 *   image = contiguous JDAT/IDAT/JDAA/JSEP run (any ancillary chunks
 *           interleaved between image chunks ride along inside this block)
 *   tail  = everything after the last image chunk, including IEND
 *
 * Phase 2 (future) will parse `image` further, route JDAT → packJPG, and
 * store IDAT/JDAA separately so the JPEG stream stops being a compression
 * dead zone (zstd can't beat JPEG entropy).
 *
 * Wire format (TCIJ v1):
 *   4   TCIJ_SIG ('T','C','I','J')
 *   1   version  (= 1)
 *   1   flags    (reserved, 0)
 *   2   filename_len  (LE u16)
 *   N   filename      (UTF-8, original .jng basename — no path components)
 *   4   head_raw      (LE u32)   ; section sizes capped at 4 GiB except image
 *   4   head_comp     (LE u32)
 *   H   head_comp     bytes (zstd)
 *   8   image_raw     (LE u64)   ; image is the bulk — gets the u64 budget
 *   8   image_comp    (LE u64)
 *   I   image_comp    bytes (zstd-19 with --long=27)
 *   4   tail_raw      (LE u32)
 *   4   tail_comp     (LE u32)
 *   T   tail_comp     bytes (zstd)
 */

static bool parse_jng_sections(
    const std::vector<uint8_t>& jng,
    std::vector<uint8_t>& head_out,
    std::vector<uint8_t>& image_out,
    std::vector<uint8_t>& tail_out)
{
    if (jng.size() < 8 + 12 || memcmp(jng.data(), JNG_SIG, 8) != 0) {
        snprintf(errormessage, MSG_SIZE, "bad JNG signature");
        return false;
    }
    size_t pos = 8;
    size_t first_img_off = 0;
    size_t last_img_end  = 0;
    bool   seen_iend     = false;
    while (pos + 12 <= jng.size()) {
        uint32_t L = rd_be32(jng.data() + pos);
        const uint8_t* type = jng.data() + pos + 4;
        if ((uint64_t)L > (1ull << 31)) {
            snprintf(errormessage, MSG_SIZE,
                     "JNG chunk length too large: %u", L);
            return false;
        }
        size_t total = 12 + (size_t)L;
        if (pos + total > jng.size()) {
            snprintf(errormessage, MSG_SIZE, "JNG chunk overflows file");
            return false;
        }
        bool is_img =
            !memcmp(type, "JDAT", 4) || !memcmp(type, "IDAT", 4) ||
            !memcmp(type, "JDAA", 4) || !memcmp(type, "JSEP", 4);
        if (is_img) {
            if (first_img_off == 0) first_img_off = pos;
            last_img_end = pos + total;
        }
        if (!memcmp(type, "IEND", 4)) {
            seen_iend = true;
            pos += total;
            break;
        }
        pos += total;
    }
    if (!seen_iend) {
        snprintf(errormessage, MSG_SIZE, "JNG: no IEND chunk");
        return false;
    }
    if (first_img_off == 0) {
        snprintf(errormessage, MSG_SIZE,
                 "JNG: no JDAT/IDAT/JDAA/JSEP chunks");
        return false;
    }
    head_out.assign(jng.begin() + 8,             jng.begin() + first_img_off);
    image_out.assign(jng.begin() + first_img_off, jng.begin() + last_img_end);
    tail_out.assign(jng.begin() + last_img_end,  jng.begin() + pos);
    return true;
}

static bool compress_jng_to_tcij(
    const std::string& jng_path,
    const std::string& out_path,
    std::vector<uint8_t>* out_bytes = nullptr)
{
    namespace fs = std::filesystem;
    auto raw = read_file(jng_path);
    if (raw.empty()) {
        snprintf(errormessage, MSG_SIZE, "cannot read input");
        return false;
    }
    std::vector<uint8_t> head, image, tail;
    if (!parse_jng_sections(raw, head, image, tail)) return false;

    std::vector<uint8_t> head_c, image_c, tail_c;
    if (!head.empty()  && !zstd_enc(head.data(), head.size(), head_c))
        return false;
    if (!image.empty() && !zstd_enc_long(image.data(), image.size(), 19, image_c))
        return false;
    if (!tail.empty()  && !zstd_enc(tail.data(), tail.size(), tail_c))
        return false;

    std::string fname = fs::path(jng_path).filename().string();
    if (fname.size() > 0xFFFFu) fname.resize(0xFFFFu);

    std::vector<uint8_t> out;
    wr_bytes(out, TCIJ_SIG, 4);
    out.push_back(1);                        // version
    out.push_back(0);                        // flags (reserved)
    uint16_t nlen = (uint16_t)fname.size();
    out.push_back((uint8_t)(nlen & 0xFF));
    out.push_back((uint8_t)((nlen >> 8) & 0xFF));
    out.insert(out.end(), fname.begin(), fname.end());

    wr_le32(out, (uint32_t)head.size());
    wr_le32(out, (uint32_t)head_c.size());
    out.insert(out.end(), head_c.begin(), head_c.end());

    wr_le64(out, (uint64_t)image.size());
    wr_le64(out, (uint64_t)image_c.size());
    out.insert(out.end(), image_c.begin(), image_c.end());

    wr_le32(out, (uint32_t)tail.size());
    wr_le32(out, (uint32_t)tail_c.size());
    out.insert(out.end(), tail_c.begin(), tail_c.end());

    if (verbosity >= 1) {
        fprintf(stdout,
                "  %sTCIJ%s %s: head=%zu->%zu image=%zu->%zu tail=%zu->%zu total->%zu\n",
                col(BC), col(R), fname.c_str(),
                head.size(),  head_c.size(),
                image.size(), image_c.size(),
                tail.size(),  tail_c.size(),
                out.size());
    }

    if (out_bytes) { *out_bytes = std::move(out); return true; }
    if (dry_run) return true;
    return write_file(out_path, out);
}

static bool decompress_tcij(
    const std::string& archive_path,
    const std::string& out_dir)
{
    namespace fs = std::filesystem;
    auto buf = read_file(archive_path);
    // Minimum: magic(4) + ver(1) + flags(1) + namelen(2) + 3 headers(4+4+8+8+4+4 = 32)
    if (buf.size() < 4 + 1 + 1 + 2 + 4 + 4 + 8 + 8 + 4 + 4 ||
        memcmp(buf.data(), TCIJ_SIG, 4) != 0) {
        snprintf(errormessage, MSG_SIZE, "bad TCIJ magic");
        return false;
    }
    size_t pos = 4;
    uint8_t ver = buf[pos++];
    if (ver != 1) {
        snprintf(errormessage, MSG_SIZE,
                 "TCIJ: unsupported version %u (this build supports v1)", ver);
        return false;
    }
    uint8_t flags = buf[pos++]; (void)flags;

    if (pos + 2 > buf.size()) {
        snprintf(errormessage, MSG_SIZE, "TCIJ truncated header"); return false;
    }
    uint16_t nlen = (uint16_t)buf[pos] | ((uint16_t)buf[pos+1] << 8);
    pos += 2;
    if (pos + nlen > buf.size()) {
        snprintf(errormessage, MSG_SIZE, "TCIJ truncated filename"); return false;
    }
    std::string fname((const char*)buf.data() + pos, nlen);
    pos += nlen;

    auto read_section_u32 = [&](std::vector<uint8_t>& sec)->bool {
        if (pos + 8 > buf.size()) {
            snprintf(errormessage, MSG_SIZE, "TCIJ truncated section header");
            return false;
        }
        uint32_t raw  = rd_le32(buf.data() + pos); pos += 4;
        uint32_t comp = rd_le32(buf.data() + pos); pos += 4;
        if (comp > buf.size() || pos > buf.size() - comp) {
            snprintf(errormessage, MSG_SIZE, "TCIJ truncated section data");
            return false;
        }
        if (raw == 0 && comp == 0) { sec.clear(); return true; }
        if (!zstd_dec(buf.data() + pos, comp, sec, raw)) return false;
        pos += comp;
        return true;
    };
    auto read_section_u64 = [&](std::vector<uint8_t>& sec)->bool {
        if (pos + 16 > buf.size()) {
            snprintf(errormessage, MSG_SIZE, "TCIJ truncated image header");
            return false;
        }
        uint64_t raw  = rd_le64(buf.data() + pos); pos += 8;
        uint64_t comp = rd_le64(buf.data() + pos); pos += 8;
        if (comp > buf.size() || pos > buf.size() - (size_t)comp) {
            snprintf(errormessage, MSG_SIZE, "TCIJ truncated image data");
            return false;
        }
        if (raw == 0 && comp == 0) { sec.clear(); return true; }
        if (!zstd_dec_long(buf.data() + pos, (size_t)comp, sec, (size_t)raw))
            return false;
        pos += (size_t)comp;
        return true;
    };

    std::vector<uint8_t> head, image, tail;
    if (!read_section_u32(head))  return false;
    if (!read_section_u64(image)) return false;
    if (!read_section_u32(tail))  return false;

    // Reassemble original JNG: signature + head + image + tail.
    std::vector<uint8_t> jng;
    wr_bytes(jng, JNG_SIG, 8);
    jng.insert(jng.end(), head.begin(),  head.end());
    jng.insert(jng.end(), image.begin(), image.end());
    jng.insert(jng.end(), tail.begin(),  tail.end());

    // Sanitise filename (strip path components — no traversal).
    if (!fname.empty()) {
        fname = fs::path(fname).filename().string();
    }
    if (fname.empty()) {
        fname = fs::path(archive_path).stem().string();
        if (fname.empty()) fname = "out";
        fname += ".jng";
    }

    fs::path odir = out_dir.empty() ? fs::current_path() : fs::path(out_dir);
    std::error_code ec;
    fs::create_directories(odir, ec);
    fs::path full = odir / fname;
    return write_file(full.string(), jng);
}

static bool decompress_tovycip_archive(
    const std::string& archive_path,
    const std::string& out_dir)
{
    namespace fs = std::filesystem;
    auto buf = read_file(archive_path);
    if (buf.size() < 10 ||
        (memcmp(buf.data(), TCIP_SIG, 4) != 0 &&
         memcmp(buf.data(), PPGS_SIG, 4) != 0)) {
        snprintf(errormessage, MSG_SIZE, "bad TCIP magic"); return false;
    }
    size_t pos = 4;
    uint8_t ver  = buf[pos++]; (void)ver;
    uint8_t flgs = buf[pos++]; (void)flgs;
    uint32_t file_count = rd_le32(buf.data() + pos); pos += 4;
    // Sanity cap on file_count: a corrupted header could claim billions of
    // entries → multi-GB EntryRec allocation. 1M files is well above any
    // reasonable archive (and 1M × ~50B EntryRec ≈ 50 MB allocation cap).
    if (file_count > 1000000u) {
        snprintf(errormessage, MSG_SIZE,
                 "TCIP: implausible file_count %u (>1M)", file_count);
        return false;
    }

    // Read + decompress metadata table
    if (pos + 8 > buf.size()) {
        snprintf(errormessage, MSG_SIZE, "TCIP truncated meta header"); return false;
    }
    uint32_t meta_raw  = rd_le32(buf.data() + pos); pos += 4;
    uint32_t meta_comp = rd_le32(buf.data() + pos); pos += 4;
    if (meta_comp > buf.size() || pos > buf.size() - meta_comp) {
        snprintf(errormessage, MSG_SIZE, "TCIP truncated meta data"); return false;
    }
    std::vector<uint8_t> meta_table;
    if (!zstd_dec(buf.data() + pos, meta_comp, meta_table, meta_raw)) return false;
    pos += meta_comp;

    struct EntryRec {
        std::string name;
        std::vector<uint8_t> meta;
        uint64_t px_off, px_sz, idat_off, idat_sz;
        uint8_t  stream_idx;  // v2 only; v1 always 0
    };
    std::vector<EntryRec> entries(file_count);

    size_t mp = 0;
    for (uint32_t i = 0; i < file_count; i++) {
        if (mp + 2 > meta_table.size()) {
            snprintf(errormessage, MSG_SIZE, "meta table truncated at entry %u", i); return false;
        }
        uint16_t name_len = (uint16_t)(meta_table[mp] | (meta_table[mp+1] << 8));
        mp += 2;
        if (mp + name_len > meta_table.size()) {
            snprintf(errormessage, MSG_SIZE, "meta truncated entry %u name", i); return false;
        }
        entries[i].name.assign((const char*)(meta_table.data() + mp), name_len);
        mp += name_len;
        if (mp + 4 > meta_table.size()) {
            snprintf(errormessage, MSG_SIZE, "meta truncated entry %u meta_len", i); return false;
        }
        uint32_t e_meta_len = rd_le32(meta_table.data() + mp); mp += 4;
        if (mp + e_meta_len > meta_table.size()) {
            snprintf(errormessage, MSG_SIZE, "meta truncated entry %u meta", i); return false;
        }
        entries[i].meta.assign(meta_table.begin() + mp, meta_table.begin() + mp + e_meta_len);
        mp += e_meta_len;
        if (ver >= 2) {
            if (mp + 1 > meta_table.size()) {
                snprintf(errormessage, MSG_SIZE, "meta truncated entry %u stream_idx", i); return false;
            }
            entries[i].stream_idx = meta_table[mp]; mp += 1;
        } else {
            entries[i].stream_idx = 0;
        }
        if (mp + 32 > meta_table.size()) {
            snprintf(errormessage, MSG_SIZE, "meta truncated entry %u offsets", i); return false;
        }
        entries[i].px_off   = rd_le64(meta_table.data() + mp); mp += 8;
        entries[i].px_sz    = rd_le64(meta_table.data() + mp); mp += 8;
        entries[i].idat_off = rd_le64(meta_table.data() + mp); mp += 8;
        entries[i].idat_sz  = rd_le64(meta_table.data() + mp); mp += 8;
    }

    // Pixel section: v1 had a single kanzi block; v2 has `num_streams` independent
    // kanzi blocks (parallel decode lets per-file reconstruct start as soon as the
    // smaller stream finishes — the bottleneck file's stream is also smaller than
    // the v1 single-stream block).
    int num_streams = 1;
    if (ver >= 2) {
        if (pos + 1 > buf.size()) {
            snprintf(errormessage, MSG_SIZE, "TCIP truncated num_streams"); return false;
        }
        num_streams = buf[pos++];
        if (num_streams < 1 || num_streams > 16) {
            snprintf(errormessage, MSG_SIZE, "TCIP implausible num_streams=%d", num_streams); return false;
        }
    }
    std::vector<uint64_t> sraw(num_streams), scomp(num_streams);
    std::vector<const uint8_t*> sptr(num_streams);
    for (int s = 0; s < num_streams; s++) {
        if (pos + 16 > buf.size()) {
            snprintf(errormessage, MSG_SIZE, "TCIP truncated px stream %d header", s); return false;
        }
        sraw[s]  = rd_le64(buf.data() + pos); pos += 8;
        scomp[s] = rd_le64(buf.data() + pos); pos += 8;
        if (scomp[s] > buf.size() || pos > buf.size() - scomp[s]) {
            snprintf(errormessage, MSG_SIZE, "TCIP truncated px stream %d data", s); return false;
        }
        sptr[s] = buf.data() + pos;
        pos += scomp[s];
    }

    if (pos + 16 > buf.size()) {
        snprintf(errormessage, MSG_SIZE, "TCIP truncated idat header"); return false;
    }
    uint64_t big_idat_raw  = rd_le64(buf.data() + pos); pos += 8;
    uint64_t big_idat_comp = rd_le64(buf.data() + pos); pos += 8;
    if (big_idat_comp > buf.size() || pos > buf.size() - big_idat_comp) {
        snprintf(errormessage, MSG_SIZE, "TCIP truncated idat data"); return false;
    }
    const uint8_t* big_idat_ptr = buf.data() + pos;

    // Decode each pixel stream in its own thread (parallel), plus zstd idat in
    // main. With NUM_STREAMS=2, two kanzi decoders run on two cores in parallel.
    // Each kanzi instance uses jobs=2 internally — fewer than the single-stream
    // case (jobs=4) because we already have inter-stream parallelism.
    std::vector<std::vector<uint8_t>> stream_buf(num_streams);
    std::vector<bool> stream_ok(num_streams, true);
    std::vector<std::thread> px_threads;
    // Capture each worker's thread_local errormessage so main can report the
    // actual cause (otherwise the parent's errormessage stays at default
    // "no error" since errormessage is thread_local).
    std::vector<std::string> stream_err(num_streams);
    for (int s = 0; s < num_streams; s++) {
        if (sraw[s] == 0) continue;
        px_threads.emplace_back([&, s] {
            int per_stream_jobs = (num_streams >= 2 && s == 0) ? 4
                                : (num_streams >= 2)            ? 2
                                                                : 4;
            // Watchdog: corrupted bitstreams can drive kanzi into an internal
            // infinite loop. 30 s is generous (8MB of valid data decodes in ~25 ms);
            // if we hit the timeout the input is almost certainly malformed.
            stream_ok[s] = kanzi_solid_dec_with_timeout(
                sptr[s], scomp[s], per_stream_jobs,
                stream_buf[s], (size_t)sraw[s],
                /*timeout_ms=*/ 30000);  // 30s — generous; valid 8MB decode is ~25ms
            if (!stream_ok[s]) stream_err[s] = errormessage;
        });
    }
    std::vector<uint8_t> big_idat;
    bool idat_ok = true;
    std::string idat_err;
    if (big_idat_raw > 0) {
        idat_ok = zstd_dec_long(big_idat_ptr, big_idat_comp, big_idat, big_idat_raw);
        if (!idat_ok) idat_err = errormessage;
    }
    for (auto& t : px_threads) t.join();
    bool px_ok = true;
    for (int s = 0; s < num_streams; s++) {
        if (!stream_ok[s]) {
            px_ok = false;
            snprintf(errormessage, MSG_SIZE, "stream %d: %s", s, stream_err[s].c_str());
            break;
        }
    }
    if (!idat_ok && px_ok) {
        snprintf(errormessage, MSG_SIZE, "idat: %s", idat_err.c_str());
    }
    if (!px_ok || !idat_ok) return false;

    // Reconstruct + write each PNG. Parallelize across entries — deflate re-encode
    // is the per-entry bottleneck, so file-level parallelism scales linearly.
    // Sort by raw size DESC so workers pick big files FIRST.
    std::vector<size_t> order(entries.size());
    for (size_t i = 0; i < entries.size(); i++) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return (entries[a].px_sz + entries[a].idat_sz) >
               (entries[b].px_sz + entries[b].idat_sz);
    });
    std::error_code ec;
    fs::create_directories(out_dir, ec);
    std::atomic<int> errors{0};
    std::atomic<size_t> nidx{0};
    int n_workers;
    if (g_threads_auto && ver >= 2 && num_streams >= 2) {
        // Auto (-th0) + PPGS v2 multi-stream: leave one core per parallel stream
        // decoder. Empirically -th2 beats -th0=hw=4 on AMD A8 4-core because
        // 4 reconstruct workers + 2 stream-decode threads + 1 zstd thread = 7
        // threads on 4 cores → contention. Subtracting num_streams from hw keeps
        // total active threads ≤ hw, no oversubscription, ≈ −2 ms on dec-th0.
        int hw = (int)std::thread::hardware_concurrency();
        if (hw < 1) hw = 1;
        n_workers = std::max(1, hw - num_streams);
    } else {
        // Explicit user choice (-th<N>) or single-stream mode: honor num_threads.
        n_workers = num_threads;
    }
    if (n_workers < 1) n_workers = 1;
    if ((size_t)n_workers > entries.size()) n_workers = (int)entries.size();
    std::vector<std::thread> workers;
    workers.reserve(n_workers);
    for (int t = 0; t < n_workers; t++) {
        workers.emplace_back([&]() {
            for (;;) {
                size_t k = nidx.fetch_add(1);
                if (k >= order.size()) break;
                size_t i = order[k];
                auto& e = entries[i];
                // Bounds-check entry's claimed stream + offsets vs decoded buffers.
                // (Corrupted .tcip can claim a non-existent stream or offsets past EOF.)
                if (e.stream_idx >= (uint8_t)stream_buf.size()) {
                    std::lock_guard<std::mutex> lk(g_print_mutex);
                    fprintf(stderr, "%sERROR%s %s: stream_idx=%u out of range (have %zu streams)\n",
                            col(RD), col(R), e.name.c_str(),
                            (unsigned)e.stream_idx, stream_buf.size());
                    errors++;
                    continue;
                }
                const std::vector<uint8_t>& sb = stream_buf[e.stream_idx];
                if (e.px_sz > sb.size() || e.px_off > sb.size() - e.px_sz) {
                    std::lock_guard<std::mutex> lk(g_print_mutex);
                    fprintf(stderr, "%sERROR%s %s: px_off=%llu+px_sz=%llu exceeds stream %u size %zu\n",
                            col(RD), col(R), e.name.c_str(),
                            (unsigned long long)e.px_off,
                            (unsigned long long)e.px_sz,
                            (unsigned)e.stream_idx, sb.size());
                    errors++;
                    continue;
                }
                if (e.idat_sz > big_idat.size() || e.idat_off > big_idat.size() - e.idat_sz) {
                    std::lock_guard<std::mutex> lk(g_print_mutex);
                    fprintf(stderr, "%sERROR%s %s: idat_off=%llu+idat_sz=%llu exceeds idat size %zu\n",
                            col(RD), col(R), e.name.c_str(),
                            (unsigned long long)e.idat_off,
                            (unsigned long long)e.idat_sz,
                            big_idat.size());
                    errors++;
                    continue;
                }
                const uint8_t* px_data   = sb.data() + e.px_off;
                const uint8_t* idat_data = big_idat.data() + e.idat_off;
                std::vector<uint8_t> png_out;
                if (!reconstruct_png_from_streams(e.meta, px_data, e.px_sz,
                                                  idat_data, e.idat_sz, png_out)) {
                    std::lock_guard<std::mutex> lk(g_print_mutex);
                    fprintf(stderr, "%sERROR%s %s: %s\n", col(RD), col(R), e.name.c_str(), errormessage);
                    errors++;
                    continue;
                }
                std::string outpath = (fs::path(out_dir) / e.name).string();
                // Recreate subdir structure carried in e.name (v1.6.1+).
                // For legacy archives whose e.name is a basename, parent_path()
                // is just out_dir which already exists — create_directories is
                // a no-op then.
                {
                    std::error_code mkec;
                    fs::create_directories(fs::path(outpath).parent_path(), mkec);
                }
                if (!write_file(outpath, png_out)) {
                    std::lock_guard<std::mutex> lk(g_print_mutex);
                    fprintf(stderr, "%sERROR%s write %s\n", col(RD), col(R), outpath.c_str());
                    errors++;
                }
            }
        });
    }
    for (auto& w : workers) w.join();
    int err_count = errors.load();
    if (err_count > 0) {
        snprintf(errormessage, MSG_SIZE, "%d errors during extract", err_count);
    }
    return err_count == 0;
}

static void process_file(const std::string& inpath, const std::string& src_root = "") {
    namespace fs = std::filesystem;
    FileType ft = detect_type(inpath);
    if (ft == F_UNK) {
        std::lock_guard<std::mutex> lk(g_print_mutex);
        clear_bar();
        // Distinguish: doesn't exist vs exists-but-not-recognized
        std::error_code ec;
        if (!fs::exists(inpath, ec)) {
            fprintf(stderr, "%sERROR%s %s: file not found\n",
                    col(RD), col(R), inpath.c_str());
        } else {
            fprintf(stderr, "%sskip%s %s: not a recognized PNG / .ppg "
                            "(magic mismatch or empty)\n",
                    col(YL), col(R), inpath.c_str());
        }
        g_errors++;
        tick_bar();
        return;
    }
    if (ft == F_TCIP || ft == F_TCIJ) {
        // TCIP = PNG/APNG-derived solid archive; TCIJ = JNG-derived (v1.8).
        // Both extract into outdir; both report input bytes only (output
        // bytes for extract require a directory walk we skip).
        std::string odir = outdir.empty()
            ? fs::path(inpath).parent_path().string()
            : outdir;
        if (odir.empty()) odir = ".";
        std::error_code ec;
        uint64_t in_sz_a = (uint64_t)fs::file_size(inpath, ec);
        bool ok = (ft == F_TCIP)
            ? decompress_tovycip_archive(inpath, odir)
            : decompress_tcij(inpath, odir);
        if (!ok) {
            std::lock_guard<std::mutex> lk(g_print_mutex);
            clear_bar();
            fprintf(stderr, "%sERROR%s %s: %s\n",
                    col(RD), col(R), inpath.c_str(), errormessage);
            g_errors++;
        } else {
            g_processed++;
            double prev = g_acc_in.load();
            while (!g_acc_in.compare_exchange_weak(prev, prev + (double)in_sz_a)) {}
        }
        std::lock_guard<std::mutex> lk(g_print_mutex);
        tick_bar();
        return;
    }
    if (ft == F_PNG && decompress_only) {
        std::lock_guard<std::mutex> lk(g_print_mutex);
        clear_bar();
        fprintf(stderr, "%sskip%s %s: PNG input in decompress-only (x) mode\n",
                col(YL), col(R), inpath.c_str());
        tick_bar();
        return;
    }
    if (ft == F_PPG && compress_only) {
        std::lock_guard<std::mutex> lk(g_print_mutex);
        clear_bar();
        fprintf(stderr, "%sskip%s %s: .ppg input in compress-only (a) mode\n",
                col(YL), col(R), inpath.c_str());
        tick_bar();
        return;
    }

    bool do_compress = (ft == F_PNG);
    std::string ext     = do_compress ? ".ppg" : ".png";
    std::string outpath = make_outpath(inpath, ext, src_root);

    if (!overwrite && fs::exists(outpath)) {
        std::lock_guard<std::mutex> lk(g_print_mutex);
        clear_bar();
        fprintf(stderr, "%sERROR%s %s: output exists\n", col(RD), col(R), inpath.c_str());
        g_errors++;
        tick_bar();
        return;
    }

    size_t in_sz = (size_t)fs::file_size(inpath);
    auto in = read_file(inpath);
    if (in.empty()) {
        std::lock_guard<std::mutex> lk(g_print_mutex);
        clear_bar();
        fprintf(stderr, "%sERROR%s %s: cannot read\n", col(RD), col(R), inpath.c_str());
        g_errors++;
        tick_bar();
        return;
    }

    std::vector<uint8_t> out;
    bool ok = do_compress ? compress_png(in, out) : decompress_ppg(in, out);
    if (!ok) {
        std::lock_guard<std::mutex> lk(g_print_mutex);
        clear_bar();
        fprintf(stderr, "%sERROR%s %s: %s\n", col(RD), col(R), inpath.c_str(), errormessage);
        g_errors++;
        tick_bar();
        return;
    }

    if (verify) {
        std::vector<uint8_t> rt;
        bool vok;
        if (do_compress) {
            vok = decompress_ppg(out, rt);
            // -ldf produces pixel-exact (not byte-exact) output; compare pixels
            bool match = ldf_repack ? compare_png_pixels(rt, in) : (rt == in);
            if (!vok || !match) {
                std::lock_guard<std::mutex> lk(g_print_mutex);
                clear_bar();
                fprintf(stderr, "%sERROR%s %s: round-trip mismatch\n", col(RD), col(R), inpath.c_str());
                g_errors++;
                tick_bar();
                return;
            }
        } else {
            vok = decompress_ppg(in, rt);
            if (!vok || rt != out) {
                std::lock_guard<std::mutex> lk(g_print_mutex);
                clear_bar();
                fprintf(stderr, "%sERROR%s %s: round-trip mismatch\n", col(RD), col(R), inpath.c_str());
                g_errors++;
                tick_bar();
                return;
            }
        }
    }

    if (!dry_run && !write_file(outpath, out)) {
        std::lock_guard<std::mutex> lk(g_print_mutex);
        clear_bar();
        fprintf(stderr, "%sERROR%s %s: cannot write output\n", col(RD), col(R), inpath.c_str());
        g_errors++;
        tick_bar();
        return;
    }

    double ratio = in_sz > 0 ? 100.0 * out.size() / in_sz : 0.0;
    {
        std::lock_guard<std::mutex> lk(g_print_mutex);
        // Per-file line: skipped at v0 when bar is active (bar replaces it).
        if (!module_mode && (verbosity >= 1 || !g_show_bar)) {
            bool expanded = (ratio > 100.0);
            const char* pc = expanded ? col(YL) : col(GR);
            clear_bar();   // no-op when !g_show_bar
            fprintf(stdout, "%s%s%s -> %s%s%s  %.2f%%%s\n",
                    pc, inpath.c_str(), col(R),
                    pc, outpath.c_str(), col(R), ratio,
                    expanded ? " [incompressible — consider skipping]" : "");
        }
        g_processed++;
        tick_bar();
    }
    double prev = g_acc_in.load();
    while (!g_acc_in.compare_exchange_weak(prev, prev + (double)in_sz)) {}
    prev = g_acc_out.load();
    while (!g_acc_out.compare_exchange_weak(prev, prev + (double)out.size())) {}
}

/* ─── collect files ──────────────────────────────────────────────────────── */

static void collect(const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::is_directory(path, ec) && recursive) {
        for (auto& e : fs::recursive_directory_iterator(path, ec)) {
            if (!e.is_regular_file(ec)) continue;
            auto ext = e.path().extension().string();
            for (auto& ch : ext) ch = (char)tolower((unsigned char)ch);
            // .tcip / .ppgs (legacy archive ext) included so -r picks them up;
            // .jng added in v1.8; detect_type sniffs magic to dispatch.
            if (ext == ".png" || ext == ".apng" || ext == ".ppg"
             || ext == ".jng"
             || ext == ".tcip" || ext == ".ppgs")
                filelist.push_back({ e.path().string(), path });
        }
    } else {
        filelist.push_back({ path, std::string() });
    }
}

/* ─── list PPG info ──────────────────────────────────────────────────────── */

static void list_ppg(const std::string& path) {
    namespace fs = std::filesystem;
    auto buf = read_file(path);
    if (buf.size() < 5 || memcmp(buf.data(), PPG_SIG, 4) != 0) {
        fprintf(stdout, "%s%s%s: not a PPG file\n", col(RD), path.c_str(), col(R));
        return;
    }
    size_t pos = 4;
    uint8_t version = buf[pos++];
    size_t file_sz = fs::exists(path) ? (size_t)fs::file_size(path) : buf.size();

    fprintf(stdout, "%s%s%s  (%zu B)\n", col(BC), path.c_str(), col(R), file_sz);

    if (version == 1) {
        if (pos + 3 > buf.size()) { fprintf(stdout, "  truncated\n"); return; }
        uint8_t mode=buf[pos++], lv=buf[pos++], st=buf[pos++];
        uint32_t ni = rd_le32(buf.data()+pos); pos+=4; pos+=ni*4;
        uint64_t pre_sz=rd_le64(buf.data()+pos); pos+=8; pos+=pre_sz;
        uint64_t suf_sz=rd_le64(buf.data()+pos); pos+=8; pos+=suf_sz;
        uint64_t raw_sz=rd_le64(buf.data()+pos); pos+=8;
        uint64_t lzma_sz=rd_le64(buf.data()+pos);
        fprintf(stdout, "  PPG v1  PNG  1 frame\n");
        fprintf(stdout, "  Frame 0: [%s] lv=%u st=%s  raw=%llu  lzma=%llu\n",
                mode==0?"match":"stored", lv, strategy_name(st),
                (unsigned long long)raw_sz, (unsigned long long)lzma_sz);
        fprintf(stdout, "  pre=%lluB  suf=%lluB\n",
                (unsigned long long)pre_sz, (unsigned long long)suf_sz);
        return;
    }

    if (version == 2) {
        bool is_apng   = (buf[pos++] & 1) != 0;
        uint32_t plays = rd_le32(buf.data()+pos); pos+=4;
        uint64_t pre_sz=rd_le64(buf.data()+pos); pos+=8; pos+=pre_sz;
        uint64_t post_sz=rd_le64(buf.data()+pos); pos+=8; pos+=post_sz;
        uint32_t nf   = rd_le32(buf.data()+pos); pos+=4;
        fprintf(stdout, "  PPG v2  %s  %u frame(s)%s\n",
                is_apng?"APNG":"PNG", nf,
                is_apng ? (" loops=" + std::to_string(plays)).c_str() : "");
        for (uint32_t i = 0; i < nf && pos < buf.size(); i++) {
            bool has_fctl = buf[pos++] != 0;
            if (has_fctl && pos+26 <= buf.size()) pos+=26;
            bool uses_idat = buf[pos++] != 0;
            if (!uses_idat && pos+4 <= buf.size()) pos+=4;
            if (pos+5 > buf.size()) break;
            uint8_t mode=buf[pos++], lv=buf[pos++], st=buf[pos++];
            uint8_t wb=buf[pos++], ml=buf[pos++];
            if (pos+4 > buf.size()) break;
            uint32_t nc=rd_le32(buf.data()+pos); pos+=4; pos+=nc*4;
            if (pos+16 > buf.size()) break;
            uint64_t raw_sz =rd_le64(buf.data()+pos); pos+=8;
            uint64_t lzma_sz=rd_le64(buf.data()+pos); pos+=8; pos+=lzma_sz;
            const char* mcol = (mode==0) ? col(GR) : col(YL);
            fprintf(stdout, "  Frame %u: %s[%s]%s %s  lv=%u st=%-8s wb=%u ml=%u"
                    "  raw=%llu  lzma=%llu (%.1f%%)\n",
                    i, mcol, mode==0?"match ":"stored", col(R),
                    uses_idat?"IDAT":"fdAT", lv, strategy_name(st), wb, ml,
                    (unsigned long long)raw_sz, (unsigned long long)lzma_sz,
                    raw_sz > 0 ? 100.0*lzma_sz/raw_sz : 0.0);
        }
        return;
    }

    if (version >= 3 && version <= 15) {
        bool is_apng   = (buf[pos++] & 1) != 0;
        uint32_t plays = rd_le32(buf.data()+pos); pos+=4;
        uint64_t pre_sz=rd_le64(buf.data()+pos); pos+=8; pos+=pre_sz;
        uint64_t post_sz=rd_le64(buf.data()+pos); pos+=8; pos+=post_sz;
        uint32_t nf   = rd_le32(buf.data()+pos); pos+=4;
        const char* compressor = (version >= 14) ? "kpng4" :
                                 (version >= 12) ? "kpng2" :
                                 (version >= 10) ? "Kanzi" :
                                 (version >= 8)  ? "FL2"   :
                                 (version >= 6)  ? "Zstd"  : "LZMA";
        const char* fmtdesc = (version == 3) ? "" :
                              (version == 4 || version == 6 || version == 8 || version == 10 ||
                               version == 12 || version == 14) ?
                                  " + filter sep" : " + filter sep + ldf-repack";
        fprintf(stdout, "  PPG v%u  %s  %u frame(s)%s  [solid %s%s]\n",
                version, is_apng?"APNG":"PNG", nf,
                is_apng ? (" loops=" + std::to_string(plays)).c_str() : "",
                compressor, fmtdesc);

        for (uint32_t i = 0; i < nf && pos < buf.size(); i++) {
            bool has_fctl = buf[pos++] != 0;
            if (has_fctl && pos+26 <= buf.size()) pos+=26;
            bool uses_idat = buf[pos++] != 0;
            if (!uses_idat && pos+4 <= buf.size()) pos+=4;
            if (pos+5 > buf.size()) break;
            uint8_t mode=buf[pos++], lv=buf[pos++], st=buf[pos++];
            uint8_t wb=buf[pos++], ml=buf[pos++];
            if (pos+4 > buf.size()) break;
            uint32_t nc=rd_le32(buf.data()+pos); pos+=4; pos+=nc*4;
            if (pos+8 > buf.size()) break;
            uint64_t psz=rd_le64(buf.data()+pos); pos+=8;
            bool fsep = false; uint32_t stride=0, nsc=0;
            if (version >= 4 && pos < buf.size()) {
                fsep = buf[pos++] != 0;
                if (fsep && pos+8 <= buf.size()) {
                    stride = rd_le32(buf.data()+pos); pos+=4;
                    nsc    = rd_le32(buf.data()+pos); pos+=4;
                }
            }
            const char* mcol = (mode==0) ? col(GR) : (mode==2) ? col(BC) : col(YL);
            const char* mname = (mode==0) ? "match " : (mode==2) ? "ldf   " : "stored";
            std::string extra;
            if (mode == 2) extra += " ldf_lv=" + std::to_string(lv);
            if (fsep) extra += " [fsep stride=" + std::to_string(stride) + " rows=" + std::to_string(nsc) + "]";
            fprintf(stdout, "  Frame %u: %s[%s]%s %s  lv=%u st=%-8s wb=%u ml=%u"
                    "  payload=%llu%s\n",
                    i, mcol, mname, col(R),
                    uses_idat?"IDAT":"fdAT", lv, strategy_name(st), wb, ml,
                    (unsigned long long)psz, extra.c_str());
        }
        if (version >= 12) {
            // split-stream: [u64 px_raw][u64 px_comp][px_comp][u64 idat_raw][u64 idat_comp][idat_comp]
            if (pos+16 <= buf.size()) {
                uint64_t px_raw  = rd_le64(buf.data()+pos); pos+=8;
                uint64_t px_comp = rd_le64(buf.data()+pos); pos+=8;
                pos += px_comp;
                uint64_t idat_raw = 0, idat_comp = 0;
                if (pos+16 <= buf.size()) {
                    idat_raw  = rd_le64(buf.data()+pos); pos+=8;
                    idat_comp = rd_le64(buf.data()+pos);
                }
                // For v14/v15: sniff IDAT first byte to label codec used by hybrid encode.
                const char* idat_lbl = (version >= 14) ? "zstd-long" : "FPAQ";
                if (version >= 14 && idat_comp >= 4) {
                    size_t idp_off = pos + 8;  // pos is at idat_comp; data starts after +8
                    if (idp_off + 4 <= buf.size()) {
                        const uint8_t* idp = buf.data() + idp_off;
                        if      (idp[0] == 0x28 && idp[1] == 0xB5) idat_lbl = "zstd-long";
                        else if (idp[0] == 0xFD && idp[1] == 0x37) idat_lbl = "xz";
                        else                                       idat_lbl = "FL2";
                    }
                }
                fprintf(stdout, "  %s split: px=%llu→%llu (%s)  idat=%llu→%llu (%s)  total=%llu→%llu\n",
                        (version >= 14) ? "kpng4" : "kpng2",
                        (unsigned long long)px_raw, (unsigned long long)px_comp,
                        (version >= 14) ? "FPAQ" : "CM",
                        (unsigned long long)idat_raw, (unsigned long long)idat_comp,
                        idat_lbl,
                        (unsigned long long)(px_raw + idat_raw),
                        (unsigned long long)(px_comp + idat_comp));
            }
        } else if (pos+16 <= buf.size()) {
            uint64_t total_raw=rd_le64(buf.data()+pos); pos+=8;
            uint64_t comp_sz  =rd_le64(buf.data()+pos);
            const char* solid_label =
                (version >= 10) ? "Kanzi" :
                (version >= 8)  ? "FL2"   :
                (version >= 6)  ? "Zstd"  : "LZMA";
            fprintf(stdout, "  Solid %s: %llu → %llu (%.1f%%)\n",
                    solid_label,
                    (unsigned long long)total_raw, (unsigned long long)comp_sz,
                    total_raw > 0 ? 100.0*comp_sz/total_raw : 0.0);
        }
        return;
    }

    fprintf(stdout, "  unknown PPG version %u\n", version);
}

/* ─── show help ──────────────────────────────────────────────────────────── */

static void show_help() {
    fprintf(stdout,
        "\n%spackPNG%s v%d.%d%s  •  by %s\n"
        "PNG/APNG/JNG lossless recompressor — tovyCIP backend (kanzi BWT + zstd-19-long)\n\n"
        "Usage: packPNG [subcommand] [flags] file(s)\n\n"
        "Subcommands:\n"
        "  a            compress only (.png/.apng/.jng → .ppg)\n"
        "  x            decompress only (.ppg → .png/.apng/.jng)\n"
        "  mix          both directions (default)\n"
        "  l / list     inspect .ppg files (no decompression)\n\n"
        "Flags:\n"
        "  -ver         verify round-trip after processing\n"
        "  -v0/-v1/-v2  verbosity (default 0)\n"
        "  -np          no pause after processing\n"
        "  -o           overwrite existing output files\n"
        "  -p           proceed on warnings\n"
        "  -r           recurse into subdirectories\n"
        "  -fs          preserve source folder structure under -od (use with -r)\n"
        "  -dry         dry run (no output files)\n"
        "  -m<1-9>      legacy LZMA backend (preset 1..9). Implies opt-out of\n"
        "                 tovyCIP default → per-file zlib+LZMA (v1.0-v1.4 path).\n"
        "                 Useful for A/B comparison vs the kanzi backend.\n"
        "  -me          LZMA extreme flag (slower, better ratio)\n"
        "  -deep        disable brute-force early-out — explores every candidate\n"
        "                 even after consecutive misses. On modern PNG encoders\n"
        "                 (libpng-default-ish) it's a near no-op since match is\n"
        "                 found on candidate 0; on exotic / older encoders it can\n"
        "                 buy a few percent at the cost of slower encode.\n"
        "  -ldf         libdeflate pixel-exact fallback for unmatched frames (PPG v5)\n"
        "  -fl2         fast-lzma2 backend (PPG v8/v9, ~2-4x faster, +1-2%% size)\n"
        "  -kanzi       kanzi BWT+CM backend (PPG v10/v11, default level 7; slow decomp)\n"
        "  -kanzi<1-9>  kanzi with explicit level (5=balanced, 7=generic sweet spot, 9=TPAQX max ratio)\n"
        "  -kpng        kpng v4 — split-stream PNG-aware encoder (PPG v14/v15)\n"
        "                 pixels      → kanzi RLT+BWT+SRT+ZRLT / FPAQ (best-balance ratio/decode)\n"
        "                 stored idat → zstd-19 with --long=27 (cheap init, fast decode)\n"
        "                 sections encoded in parallel; decode auto-detects idat codec by magic\n"
        "                 vs xz preset 6: ratio ≈tied, comp −37%%, decST −10%%, dec-th0 +12%%\n"
        "  -tovycip     tovyCIP — Tovy Compresor de Imágenes PNG (DEFAULT, per-file)\n"
        "                 Pipeline (always per-file in v1.7+):\n"
        "                   pixels  → kanzi RLT+BWT+SRT+ZRLT/FPAQ\n"
        "                   stored idat → zstd-19 with --long=27\n"
        "                 1 PNG → 1 .ppg ; if outpaths collide they are renamed\n"
        "                   (e.g. image.ppg → image(1).ppg → image(2).ppg).\n"
        "  -tcip        alias of -tovycip (no behavioural difference).\n"
        "  -solid       alias of -tovycip (legacy name from the v1.5/v1.6 archive\n"
        "                 era; archive grouping was removed in v1.7).\n"
        "  -perfile     opt out of the tovyCIP default and route through the\n"
        "                 legacy per-file LZMA path (v1.0–v1.4 backend).\n"
        "  -fastdec     skip filter-byte separation for files ≥4MB pixels (≈ −5 ms decode\n"
        "                 per big file, ≈ +1 KB ratio per affected file). Equivalent to\n"
        "                 -nofsep=4194304 / PACKPNG_NO_FSEP_ABOVE=4194304.\n"
        "  -nofsep=N    set filter_sep skip threshold to N bytes (0 = disabled = default)\n"
        "  -kpng-max    tovyCIP + max-ratio TPAQ entropy (per-file in v1.7+)\n"
        "                 pixels: kanzi RLT / TPAQ block=2MB jobs=8 (DNA/EXE/UTF/TEXT skipped\n"
        "                   as no-ops on PNG pixel data — confirmed empirically)\n"
        "                 trade-off: best ratio (≈ −0.5%% vs xz-6 on screenshots),\n"
        "                   slower encode (≈ −33%% throughput) and noticeably slower\n"
        "                   decode (~+130 ms / file). Use when ratio > read latency.\n"
        "  -th<N>       N file-level threads (0=auto)\n"
        "  -sfth        single-file parallelism: 4 threads inside each file —\n"
        "                 parallel brute-force zlib match + parallel kanzi encode\n"
        "                 (tovyCIP path) or MT-LZMA (legacy -m<N>/-perfile path).\n"
        "                 Best for big single-file invocations; for batches combine\n"
        "                 with -th<N/4> to keep total = N×4 ≤ hw_concurrency.\n"
        "  -od<path>    write output to directory\n"
        "  -module      machine-friendly output\n"
        "  --no-color   disable ANSI color\n\n"
        "Examples:\n"
        "  packPNG a -ver -o image.png\n"
        "  packPNG a -me -sfth animation.apng\n"
        "  packPNG a -th4 -od out/ *.png\n"
        "  packPNG x archive.ppg\n\n",
        col(BC), col(R), ver_major, ver_minor, subversion, author);
}

/* ─── main ───────────────────────────────────────────────────────────────── */

#ifndef BUILD_LIB
// v1.8a: every user-visible exit from main() routes through this so a
// double-clicked .exe (or an .exe launched via shortcut on Windows) keeps
// its console open until the user reads the help text or error message
// and presses Enter. -np and -module both clear wait_exit so machine /
// scripted invocations skip the prompt as before.
static int wait_and_return(int code) {
    if (wait_exit && !module_mode) {
        fprintf(stdout, "\nPress <enter> to quit\n");
        getchar();
    }
    return code;
}

int main(int argc, char** argv)
{
#ifdef _WIN32
    // Console UTF-8 output so the bullets/arrows in banner/help render correctly
    SetConsoleOutputCP(CP_UTF8);
#endif
    init_colors();
    // Legacy env var for filter_sep skip threshold (CLI flags override below).
    if (const char* e = getenv("PACKPNG_NO_FSEP_ABOVE")) g_nofsep_above = (size_t)atoll(e);
    if (argc < 2) { show_help(); return wait_and_return(0); }

    bool list_mode = false;
    int ai = 1;
    {
        std::string s = argv[1];
        if      (s == "a")              { compress_only   = true; ai++; }
        else if (s == "x")              { decompress_only = true; ai++; }
        else if (s == "mix")            { ai++; }
        else if (s == "l" || s == "list"){ list_mode = true; ai++; }
    }

    for (; ai < argc; ai++) {
        std::string arg = argv[ai];
        if      (arg == "-ver")       verify       = true;
        else if (arg == "-v0")        verbosity    = 0;
        else if (arg == "-v1")        verbosity    = 1;
        else if (arg == "-v2")        verbosity    = 2;
        else if (arg == "-np")        wait_exit    = false;
        else if (arg == "-o")         overwrite    = true;
        else if (arg == "-p")         err_tol      = 2;
        else if (arg == "-r")         recursive    = true;
        else if (arg == "-fs")        fs_mode      = true;
        else if (arg == "-dry")       dry_run      = true;
        else if (arg == "-sfth")      sfth         = true;
        else if (arg == "-deep")      EARLYOUT_K   = 1 << 30;  // disable early-out → better ratio, ~2× time
        else if (arg == "-me")        lzma_extreme = true;
        else if (arg == "-ldf")       ldf_repack   = true;
        else if (arg == "-zstd")      { use_zstd     = true; g_mode_explicit = true; }
        else if (arg == "-fl2")       { use_fl2      = true; g_mode_explicit = true; }
        else if (arg == "-kanzi")     { use_kanzi    = true; g_mode_explicit = true; }
        else if (arg.rfind("-kanzi", 0) == 0 && arg.size() == 7 &&
                 arg[6] >= '1' && arg[6] <= '9') {
            use_kanzi = true; g_mode_explicit = true;
            g_kanzi_level = arg[6] - '0';
        }
        else if (arg == "-kpng")      { use_kanzi = true; use_kpng = true; g_mode_explicit = true; }
        else if (arg == "-solid"      // legacy alias
              || arg == "-tovycip"
              || arg == "-tcip")      { use_kanzi = true; use_kpng = true; use_solid = true; g_mode_explicit = true; }
        else if (arg == "-kpng-max")  { use_kanzi = true; use_kpng = true; use_solid = true; use_kpng_max = true; g_mode_explicit = true; }
        else if (arg == "-perfile")   { use_perfile = true; g_mode_explicit = true; }
        else if (arg == "-fastdec")   g_nofsep_above = 4 * 1024 * 1024;  // skip filter_sep on files >= 4MB pixels
        else if (arg == "--no-color") no_color     = true;
        else if (arg == "-module")  { module_mode = true; wait_exit = false; }
        else if (arg.size() > 4 && arg.substr(0,4) == "-zl=") {
            int v = atoi(arg.c_str() + 4);
            if (v < 1 || v > 22) {
                fprintf(stderr, "invalid -zl=<N>: expected 1..22, got '%s'\n",
                        arg.c_str() + 4);
                return wait_and_return(2);
            }
            g_zstd_level = v;
        }
        else if (arg.rfind("-nofsep=", 0) == 0) {
            const char* val = arg.c_str() + 8;
            if (!*val) {
                fprintf(stderr, "missing value: -nofsep=<N>\n");
                return wait_and_return(2);
            }
            char* end = nullptr;
            long long n = strtoll(val, &end, 10);
            if (!end || *end != '\0' || n < 0) {
                fprintf(stderr, "invalid -nofsep=<N>: expected non-negative int, got '%s'\n", val);
                return wait_and_return(2);
            }
            g_nofsep_above = (size_t)n;
        }
        else if (arg.size() > 2 && arg.substr(0,2) == "-m") {
            // Reject non-numeric values (e.g. -mZ silently became no-op pre-v1.7e).
            const char* val = arg.c_str() + 2;
            char* end = nullptr;
            long n = strtol(val, &end, 10);
            if (!end || *end != '\0' || n < 1 || n > 9) {
                fprintf(stderr, "invalid -m<N>: expected 1..9, got '%s'\n", val);
                return wait_and_return(2);
            }
            g_lzma_preset = (unsigned)n;
            // v1.7b: explicit -m<N> opts out of tovyCIP auto-default and
            // routes through the legacy per-file LZMA path (compress_png).
            // Lets users A/B compare backends on the same corpus.
            g_mode_explicit = true;
        }
        else if (arg.size() > 3 && arg.substr(0,3) == "-th") {
            const char* val = arg.c_str() + 3;
            char* end = nullptr;
            long n = strtol(val, &end, 10);
            if (!end || *end != '\0' || n < 0) {
                fprintf(stderr, "invalid -th<N>: expected non-negative int, got '%s'\n", val);
                return wait_and_return(2);
            }
            if (n == 0) {
                g_threads_auto = true;
                n = (long)std::thread::hardware_concurrency();
                if (n < 1) n = 1;
            }
            num_threads = (int)n;
        }
        else if (arg == "-od") {
            // -od without glued path: clearer message than "unknown flag: -od"
            fprintf(stderr, "missing value: -od<path> (glue the path: -od/some/dir)\n");
            return wait_and_return(2);
        }
        else if (arg.size() > 3 && arg.substr(0,3) == "-od") {
            outdir = arg.substr(3);
            std::filesystem::create_directories(outdir);
        }
        else if (!arg.empty() && arg[0] == '-') {
            // Audit fix #10: abort on unknown flag (consistent with packJPG).
            // Previously the warning was printed and the flag silently dropped,
            // so typos like `-deeP` would be ignored without affecting behavior.
            fprintf(stderr, "unknown flag: %s\n", arg.c_str());
            return wait_and_return(2);
        }
        else
            collect(arg);
    }

    if (sfth) {
        // packJPG-style: fixed cap regardless of -thN. Combine as -th<N/4> -sfth.
        // Cap at 4 because beyond that the brute-force candidate sweep hits
        // diminishing returns (atomic counter + result mutex contention).
        int hw = (int)std::thread::hardware_concurrency();
        if (hw < 1) hw = 1;
        sfth_threads = std::min(4, hw);
        if (!module_mode && hw < 4) {
            fprintf(stderr, "Warning: -sfth works best with 4+ cores (detected: %d)\n", hw);
        }
    }

    {
        int n = (int)use_zstd + (int)use_fl2 + (int)use_kanzi;
        if (n > 1) {
            if (!module_mode) {
                fprintf(stderr, "Warning: -kanzi, -fl2, -zstd are mutually exclusive; precedence kanzi > fl2 > zstd.\n");
            }
            if (use_kanzi)      { use_fl2 = false; use_zstd = false; }
            else if (use_fl2)   { use_zstd = false; }
        }
    }

    if (filelist.empty()) { show_help(); return wait_and_return(0); }

    if (!module_mode) {
        fprintf(stdout, "\n%spackPNG%s v%d.%d%s  •  by %s\n\n",
                col(BC), col(R), ver_major, ver_minor, subversion, author);
        // Force flush so the banner reaches the user BEFORE any subsequent
        // stderr writes (warnings, errors). Without this, when stdout is
        // pipe-buffered (e.g. piped to less/tee/head), stderr (unbuffered) can
        // print first → confusing "error before program identifies itself" UX.
        fflush(stdout);
    }

    if (list_mode) {
        for (auto& f : filelist) list_ppg(f.path);
        return wait_and_return(0);
    }

    auto t0 = std::chrono::steady_clock::now();

    g_total_files = (int)filelist.size();
    g_show_bar    = !module_mode && g_total_files > 1;

    // v1.7 default: tovyCIP algorithm (kanzi RLT+BWT+SRT+ZRLT/FPAQ +
    // zstd-19-long for IDAT-passthrough), applied PER-FILE. Each input PNG
    // produces its own .ppg containing exactly 1 entry. No archive grouping —
    // packPNG is a per-file recompressor; tovyCIP is just the backend.
    // Output collisions in flat -od layouts are resolved by reserve_unique()
    // appending (N) before the extension.
    {
        // v1.8: JNG inputs go through the same per-file dispatch loop as PNG
        // (tovyCIP for PNG entries, TCIJ wrapper for JNG entries). Counting
        // either type triggers the default tovyCIP backend.
        int n_inputs = 0;
        for (auto& f : filelist) {
            FileType ft = detect_type(f.path);
            if (ft == F_PNG || ft == F_JNG) n_inputs++;
        }
        if (!g_mode_explicit && n_inputs >= 1 && (compress_only || !decompress_only)) {
            use_kanzi = true;
            use_kpng  = true;
            use_solid = true;
        }
    }

    if (use_solid && (compress_only || !decompress_only)) {
        // Per-file tovyCIP: each PNG → its own .ppg via compress_tovycip_archive
        // invoked with a 1-element path list. JNG entries (v1.8+) take the
        // TCIJ wrapper path. Threaded across files honoring -th<N> (auto when
        // -th0). The kanzi encoder uses 4 jobs internally per file, so the
        // file-level pool stays modest to avoid oversubscription.
        namespace fs = std::filesystem;

        // Collect PNG / JNG paths (skip + warn on the rest). We carry the
        // file type alongside the path so encode_one can dispatch without
        // re-sniffing the magic.
        std::vector<std::string> paths;
        std::vector<FileType>    types;
        std::vector<std::string> src_roots;
        std::vector<std::string> skip_not_png;
        std::vector<std::string> skip_not_found;
        for (auto& f : filelist) {
            FileType ft = detect_type(f.path);
            if (ft == F_PNG || ft == F_JNG) {
                paths.push_back(f.path);
                types.push_back(ft);
                src_roots.push_back(f.src_root);
            } else {
                std::error_code ec;
                if (!std::filesystem::exists(f.path, ec))
                    skip_not_found.push_back(f.path);
                else
                    skip_not_png.push_back(f.path);
            }
        }
        // Compact skip summary by default; full list under -v1+.
        // For a single missing file, always show the path — count alone is useless
        // when there's only one file.
        if (!skip_not_found.empty()) {
            if (skip_not_found.size() == 1) {
                fprintf(stderr, "%sskipped%s missing file: %s\n",
                        col(YL), col(R), skip_not_found[0].c_str());
            } else {
                fprintf(stderr, "%sskipped%s %zu missing file(s)\n",
                        col(YL), col(R), skip_not_found.size());
                if (verbosity >= 1) {
                    for (auto& p : skip_not_found)
                        fprintf(stderr, "    %s\n", p.c_str());
                }
            }
        }
        if (!skip_not_png.empty()) {
            // Group by lowercase extension so the user sees what got rejected.
            std::map<std::string, int> ext_counts;
            for (auto& p : skip_not_png) {
                auto ext = std::filesystem::path(p).extension().string();
                for (auto& c : ext) c = (char)tolower((unsigned char)c);
                if (ext.empty()) ext = "(no ext)";
                ext_counts[ext]++;
            }
            std::string breakdown;
            for (auto& kv : ext_counts) {
                if (!breakdown.empty()) breakdown += ", ";
                breakdown += std::to_string(kv.second) + " " + kv.first;
            }
            fprintf(stderr, "%sskipped%s %zu non-PNG/JNG file(s) — %s\n",
                    col(YL), col(R), skip_not_png.size(), breakdown.c_str());
            if (verbosity >= 1) {
                for (auto& p : skip_not_png)
                    fprintf(stderr, "    %s\n", p.c_str());
            }
        }
        if (paths.empty()) {
            fprintf(stderr, "%sERROR%s no PNG/JNG files to compress\n",
                    col(RD), col(R));
            return wait_and_return(1);
        }

        // Reset bar so it tracks per-file completion across the pool.
        g_files_done = 0;
        g_total_files = (int)paths.size();
        std::atomic<int> errors{0};
        std::atomic<uint64_t> total_in{0}, total_out{0};

        // Resolve -od into a directory (file paths are no longer meaningful for
        // per-file mode — if -od ends in .ppg we treat it as the dir to mkdir).
        if (!outdir.empty()) {
            std::error_code ec;
            fs::create_directories(outdir, ec);
        }

        // File-level pool. Kanzi uses 4 jobs internally per file, so cap pool
        // size at hw_concurrency / 2 (or honor -thN if user set it).
        int pool = (g_threads_auto || num_threads <= 0)
            ? std::max(1, (int)std::thread::hardware_concurrency() / 2)
            : num_threads;
        if ((size_t)pool > paths.size()) pool = (int)paths.size();
        std::atomic<size_t> next_idx{0};
        std::vector<std::thread> workers;

        auto encode_one = [&](size_t i) {
            auto&     path     = paths[i];
            FileType  ftype    = types[i];
            auto&     src_root = src_roots[i];
            std::string outpath = make_outpath(path, ".ppg", src_root);
            std::error_code ec;
            auto pp = fs::path(outpath).parent_path();
            if (!pp.empty()) fs::create_directories(pp, ec);
            std::vector<std::string> p1{path};
            std::vector<std::string> r1{src_root};
            uint64_t in_sz  = (uint64_t)fs::file_size(path, ec);

            // v1.7e: encode to in-memory buffer first so we can honor
            // -ver / -dry without writing anything we don't have to.
            // v1.8: dispatch by file type — PNG/APNG → tovyCIP (TCIP),
            //                              JNG     → TCIJ wrapper.
            std::vector<uint8_t> archive_bytes;
            bool enc_ok = (ftype == F_JNG)
                ? compress_jng_to_tcij(path, outpath, &archive_bytes)
                : compress_tovycip_archive(p1, outpath, r1, &archive_bytes);
            if (!enc_ok) {
                std::lock_guard<std::mutex> lk(g_print_mutex);
                clear_bar();
                fprintf(stderr, "%sERROR%s %s: %s\n",
                        col(RD), col(R), path.c_str(), errormessage);
                errors++;
                tick_bar();
                return;
            }

            // -ver: round-trip through a verify temp dir, byte-compare the
            // extracted file against the original. Writes archive_bytes to a
            // sibling .verify file (cleaned up after) so the existing
            // path-based decoders can be reused. JNG goes through
            // decompress_tcij; PNG/APNG through decompress_tovycip_archive.
            if (verify) {
                std::string vbase   = outpath + ".verify";
                std::string vfile   = vbase + ".tcip";
                std::string vdir    = vbase + ".d";
                fs::create_directories(vdir, ec);
                bool vok = false;
                bool match = false;
                if (write_file(vfile, archive_bytes)) {
                    bool dec_ok = (ftype == F_JNG)
                        ? decompress_tcij(vfile, vdir)
                        : decompress_tovycip_archive(vfile, vdir);
                    if (dec_ok) {
                        // The single decoded file lives somewhere under vdir
                        // (basename or relative path inside vdir, depending
                        // on src_root). Walk recursively to find it.
                        std::error_code rec;
                        for (auto& de : fs::recursive_directory_iterator(vdir, rec)) {
                            if (!de.is_regular_file(rec)) continue;
                            auto rt = read_file(de.path().string());
                            auto orig = read_file(path);
                            if (!rt.empty() && !orig.empty()) {
                                // -ldf pixel-compare only applies to PNG path
                                // (lossless re-encode of unmatched IDATs); JNG
                                // round-trip is always byte-exact.
                                match = (ftype != F_JNG && ldf_repack)
                                    ? compare_png_pixels(rt, orig)
                                    : (rt == orig);
                                vok = true;
                            }
                            break;
                        }
                    }
                }
                std::error_code cec;
                fs::remove_all(vdir, cec);
                fs::remove(vfile, cec);
                if (!vok || !match) {
                    std::lock_guard<std::mutex> lk(g_print_mutex);
                    clear_bar();
                    fprintf(stderr, "%sERROR%s %s: round-trip mismatch\n",
                            col(RD), col(R), path.c_str());
                    errors++;
                    tick_bar();
                    return;
                }
            }

            // -dry: skip the final write but keep the encode + verify side-effects.
            if (!dry_run) {
                if (!write_file(outpath, archive_bytes)) {
                    std::lock_guard<std::mutex> lk(g_print_mutex);
                    clear_bar();
                    fprintf(stderr, "%sERROR%s %s: cannot write output\n",
                            col(RD), col(R), path.c_str());
                    errors++;
                    tick_bar();
                    return;
                }
                uint64_t out_sz = (uint64_t)fs::file_size(outpath, ec);
                total_in  += in_sz;
                total_out += out_sz;
            } else {
                // Dry-run: still tally totals from the in-memory buffer so
                // the final summary line is meaningful.
                total_in  += in_sz;
                total_out += (uint64_t)archive_bytes.size();
            }
            tick_bar();
        };

        for (int t = 0; t < pool; t++) {
            workers.emplace_back([&] {
                for (;;) {
                    size_t i = next_idx.fetch_add(1);
                    if (i >= paths.size()) break;
                    encode_one(i);
                }
            });
        }
        for (auto& th : workers) th.join();
        finish_bar();

        if (verbosity >= 0) {
            uint64_t in_b = total_in.load();
            uint64_t out_b = total_out.load();
            int ok_count = (int)paths.size() - errors.load();
            double ratio = in_b > 0 ? (100.0 * out_b / in_b) : 0.0;
            auto t1 = std::chrono::steady_clock::now();
            double dt = std::chrono::duration<double>(t1 - t0).count();
            fprintf(stdout, "%d file(s)  %.2f%%  %.2fs\n", ok_count, ratio, dt);
        }
        return wait_and_return(errors.load() > 0 ? 1 : 0);
    }

    // v1.7c: pre-filter filelist for the legacy per-file paths (-m, -zstd,
    // -fl2, -kanzi, -kpng, -perfile) so skipped inputs don't print one line
    // each from inside process_file. Mirrors the compact summary already used
    // by the tovyCIP block above.
    {
        namespace fs = std::filesystem;
        std::vector<CollectedFile> kept;
        std::vector<std::string> skip_compress;    // .ppg in `a` mode
        std::vector<std::string> skip_decompress;  // .png in `x` mode
        std::vector<std::string> skip_bad_magic;   // file exists but magic invalid
        std::vector<std::string> skip_missing;     // file path doesn't exist
        std::vector<std::string> skip_jng_legacy;  // v1.8: JNG needs default backend
        for (auto& f : filelist) {
            FileType ft = detect_type(f.path);
            if (ft == F_PNG && decompress_only) {
                skip_decompress.push_back(f.path);
            } else if ((ft == F_PPG || ft == F_TCIP || ft == F_TCIJ)
                       && compress_only) {
                skip_compress.push_back(f.path);
            } else if (ft == F_JNG) {
                // We only get here if g_mode_explicit forced us off the
                // default tovyCIP path (-perfile / -m / -zstd / etc.).
                // Phase 1 JNG support is wired exclusively through the
                // tovyCIP block above; legacy backends can't encode JNG.
                skip_jng_legacy.push_back(f.path);
            } else if (ft == F_UNK) {
                std::error_code ec;
                if (!fs::exists(f.path, ec)) skip_missing.push_back(f.path);
                else                          skip_bad_magic.push_back(f.path);
            } else {
                kept.push_back(f);
            }
        }
        if (!skip_compress.empty()) {
            std::map<std::string, int> ext_counts;
            for (auto& p : skip_compress) {
                auto ext = fs::path(p).extension().string();
                for (auto& c : ext) c = (char)tolower((unsigned char)c);
                if (ext.empty()) ext = "(no ext)";
                ext_counts[ext]++;
            }
            std::string breakdown;
            for (auto& kv : ext_counts) {
                if (!breakdown.empty()) breakdown += ", ";
                breakdown += std::to_string(kv.second) + " " + kv.first;
            }
            fprintf(stderr, "%sskipped%s %zu non-PNG file(s) — %s\n",
                    col(YL), col(R), skip_compress.size(), breakdown.c_str());
            if (verbosity >= 1) {
                for (auto& p : skip_compress)
                    fprintf(stderr, "    %s\n", p.c_str());
            }
        }
        if (!skip_decompress.empty()) {
            fprintf(stderr, "%sskipped%s %zu PNG input(s) in decompress-only mode\n",
                    col(YL), col(R), skip_decompress.size());
            if (verbosity >= 1) {
                for (auto& p : skip_decompress)
                    fprintf(stderr, "    %s\n", p.c_str());
            }
        }
        if (!skip_jng_legacy.empty()) {
            fprintf(stderr,
                    "%sskipped%s %zu JNG input(s): JNG requires the default tovyCIP backend\n"
                    "         (remove -perfile / -m / -zstd / -fl2 / -kanzi / -kpng to enable)\n",
                    col(YL), col(R), skip_jng_legacy.size());
            if (verbosity >= 1) {
                for (auto& p : skip_jng_legacy)
                    fprintf(stderr, "    %s\n", p.c_str());
            }
        }
        if (!skip_bad_magic.empty()) {
            fprintf(stderr, "%sskipped%s %zu file(s) with invalid magic (not a PNG / .ppg)\n",
                    col(YL), col(R), skip_bad_magic.size());
            if (verbosity >= 1) {
                for (auto& p : skip_bad_magic)
                    fprintf(stderr, "    %s\n", p.c_str());
            }
        }
        if (!skip_missing.empty()) {
            if (skip_missing.size() == 1) {
                fprintf(stderr, "%sERROR%s missing file: %s\n",
                        col(RD), col(R), skip_missing[0].c_str());
            } else {
                fprintf(stderr, "%sERROR%s %zu missing file(s)\n",
                        col(RD), col(R), skip_missing.size());
                if (verbosity >= 1) {
                    for (auto& p : skip_missing)
                        fprintf(stderr, "    %s\n", p.c_str());
                }
            }
            g_errors += (int)skip_missing.size();
        }
        filelist = std::move(kept);
        g_total_files = (int)filelist.size();
    }

    if (num_threads <= 1) {
        for (auto& f : filelist) process_file(f.path, f.src_root);
    } else {
        std::atomic<size_t> next_idx{0};
        std::vector<std::thread> workers;
        for (int t = 0; t < num_threads; t++) {
            workers.emplace_back([&]() {
                while (true) {
                    size_t i = next_idx.fetch_add(1);
                    if (i >= filelist.size()) break;
                    process_file(filelist[i].path, filelist[i].src_root);
                }
            });
        }
        for (auto& w : workers) w.join();
    }

    // Replace the in-progress bar with a final completed bar (✓ done/total).
    if (g_show_bar) {
        std::lock_guard<std::mutex> lk(g_print_mutex);
        finish_bar();
    }

    auto t1 = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(t1 - t0).count();

    int proc = g_processed.load(), errs = g_errors.load();
    double ai_d = g_acc_in.load(), ao_d = g_acc_out.load();

    if (module_mode) {
        fprintf(stdout, "%s %.3fs\n", errs ? "ERROR" : "OK", elapsed);
    } else if (proc > 0) {
        // In decompress-only mode (`x`) the ratio (out/in) is the inverse
        // of compression and isn't tracked accurately for archive extracts,
        // so omit it. Compress mode prints the full size + ratio + time.
        if (decompress_only) {
            fprintf(stdout, "\n%i file(s)  %.2fs\n", proc, elapsed);
        } else {
            double ratio = ai_d > 0 ? 100.0 * ao_d / ai_d : 0.0;
            fprintf(stdout, "\n%i file(s)  %.2f%%  %.2fs\n", proc, ratio, elapsed);
        }
        if (errs > 0) fprintf(stdout, "%s%i error(s)%s\n", col(RD), errs, col(R));
    }

    return wait_and_return(errs ? 1 : 0);
}
#endif // !BUILD_LIB
