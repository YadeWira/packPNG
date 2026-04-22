/* packPNG v0.2 - PNG/APNG lossless recompressor
 *
 * Per-frame algorithm:
 *   PNG/APNG → parse frames → inflate pixels → brute-force zlib re-encode
 *   → LZMA → .ppg   (mode 0 = match)
 *   If no match: store original deflate stream → LZMA → .ppg  (mode 1 = stored)
 *
 * CLI mirrors packJPG v4.x.
 * Targets: Linux x86-64, Windows 10/11 x86-64.
 * Deps: zlib, liblzma.
 * PPG format v2: multi-frame APNG; v1 files still decompressable.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <zlib.h>
#include <lzma.h>

#if defined(_WIN32)
#  include <windows.h>
#  include <fcntl.h>
#  include <io.h>
#endif

/* ─── version ────────────────────────────────────────────────────────────── */

static const char* subversion = ".4";
static const char* author     = "Yade Bravo (YadeWira)";
static const int   appversion = 2;   // v0.2.3

/* ─── constants ──────────────────────────────────────────────────────────── */

static const uint8_t PNG_SIG[8] = {0x89,'P','N','G','\r','\n',0x1a,'\n'};
static const uint8_t PPG_SIG[4] = {'P','P','G','1'};
static const size_t  PROBE_BYTES = 65536;
static const int     MSG_SIZE    = 512;

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
static bool sfth            = false;  // parallel brute-force within one file
static int  verbosity       = 0;
static int  num_threads     = 1;      // file-level threads (-th)
static int  sfth_threads    = 1;      // brute-force threads (-sfth, set post-parse)
static unsigned g_lzma_preset = 6u;
static std::string outdir;

static std::mutex g_print_mutex;      // serialise stdout when -th > 1

static int err_tol = 1;
thread_local char errormessage[MSG_SIZE] = "no error";

/* ─── file list ──────────────────────────────────────────────────────────── */

static std::vector<std::string> filelist;

/* ─── accumulators (atomic for -th safety) ───────────────────────────────── */

static std::atomic<int>    g_processed{0};
static std::atomic<int>    g_errors{0};
static std::atomic<double> g_acc_in{0.0};
static std::atomic<double> g_acc_out{0.0};

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
    // 26-byte fcTL chunk data (sequence_number…blend_op), empty if none.
    std::vector<uint8_t> fctl;
    bool     uses_idat = true;  // true = IDAT chunks, false = fdAT chunks
    uint32_t first_seq = 0;     // first fdAT sequence number (only when !uses_idat)
    std::vector<uint32_t> chunk_szs;  // per-chunk payload size (fdAT: excl. 4-byte seq)
    std::vector<uint8_t>  idat_raw;   // concatenated payloads
    std::vector<uint8_t>  pixels;     // inflated pixel data
};

struct PngInfo {
    bool     is_apng   = false;
    uint32_t num_plays = 0;
    std::vector<PngFrame> frames;
    std::vector<uint8_t>  pre;   // chunks before first fcTL/IDAT (IHDR, acTL, …)
    std::vector<uint8_t>  post;  // chunks after last frame (IEND, …)
};

/* ─── inflate one frame ──────────────────────────────────────────────────── */

static bool inflate_frame(PngFrame& fr) {
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

    // Read raw chunks
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

    // State machine: BEFORE → IN_IDAT → IN_FDAT
    enum State { BEFORE, IN_IDAT, IN_FDAT } state = BEFORE;
    PngFrame cur;
    cur.uses_idat = true;
    std::vector<uint8_t> post_acc;  // ancillary chunks after IDAT/fdAT, before IEND

    for (auto& c : chunks) {
        if (chunk_is(c, "acTL")) {
            info.is_apng   = true;
            if (c.data.size() >= 8) info.num_plays = rd_be32(c.data.data() + 4);
            auto cb = chunk_bytes(c);
            info.pre.insert(info.pre.end(), cb.begin(), cb.end());

        } else if (chunk_is(c, "fcTL")) {
            if (state == BEFORE) {
                cur.fctl = c.data;   // frame 0 is animated
            } else {
                // flush current frame
                if (!inflate_frame(cur)) return false;
                info.frames.push_back(std::move(cur));
                cur = PngFrame();
                cur.fctl       = c.data;
                cur.uses_idat  = false;
                cur.first_seq  = (c.data.size() >= 4) ? rd_be32(c.data.data()) + 1 : 0;
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
            // post = any inter-frame ancillary chunks + IEND
            auto cb = chunk_bytes(c);
            info.post = post_acc;
            info.post.insert(info.post.end(), cb.begin(), cb.end());

        } else {
            // Ancillary chunk
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

static bool probe_deflate(const uint8_t* px, size_t pxsz,
                          const uint8_t* tgt, size_t tgtsz,
                          int lv, int st, int wbits, int memlevel,
                          std::vector<uint8_t>& tmp)
{
    // Use Z_FINISH on the full input with a small output buffer.
    // Zlib stops writing when the buffer fills, but the bytes written are
    // the exact prefix of the full compressed stream — no SYNC_FLUSH artefacts.
    z_stream zs{};
    if (deflateInit2(&zs, lv, Z_DEFLATED, wbits, memlevel, st) != Z_OK) return false;
    tmp.resize(PROBE_BYTES + 128);
    zs.next_in   = const_cast<uint8_t*>(px);
    zs.avail_in  = (uInt)pxsz;
    zs.next_out  = tmp.data();
    zs.avail_out = (uInt)tmp.size();
    deflate(&zs, Z_FINISH);   // Z_BUF_ERROR expected when buffer fills
    size_t olen = zs.total_out;
    deflateEnd(&zs);
    if (olen == 0) return false;
    size_t cmp = std::min({olen, tgtsz, PROBE_BYTES});
    if (cmp == 0) return false;
    return memcmp(tmp.data(), tgt, cmp) == 0;
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

    // CMF byte (target[0]): bits 4-7 = CINFO, wbits = CINFO + 8.
    // Most encoders use wbits=15 (CINFO=7, CMF high nibble=7 → e.g. 0x78).
    // Android/mobile often use wbits=14 (CMF=0x68) or 13 (CMF=0x58).
    int cinfo = (target[0] >> 4) & 0xF;
    int wbits  = std::max(8, std::min(15, cinfo + 8));

    uint8_t flevel = (target[1] >> 6) & 3;
    int lv_min, lv_max;
    // zlib level→FLEVEL: 1→0, 2-5→1, 6→2, 7-9→3
    switch (flevel) {
        case 0: lv_min=1; lv_max=1; break;
        case 1: lv_min=2; lv_max=5; break;
        case 2: lv_min=6; lv_max=6; break;
        default:lv_min=7; lv_max=9; break;
    }

    static const int strats[] = {Z_DEFAULT_STRATEGY, Z_FILTERED, Z_HUFFMAN_ONLY, Z_RLE};
    std::vector<Candidate> c;

    // Primary: memlevel=8 (zlib default)
    if (lv_min <= 6 && 6 <= lv_max) c.push_back({6, Z_FILTERED, wbits, 8});
    for (int lv = lv_min; lv <= lv_max; lv++)
        for (int st : strats)
            if (!(lv == 6 && st == Z_FILTERED))
                c.push_back({lv, st, wbits, 8});

    // Extended: memlevel=9 (some encoders; probe rejects quickly if wrong)
    if (lv_min <= 6 && 6 <= lv_max) c.push_back({6, Z_FILTERED, wbits, 9});
    for (int lv = lv_min; lv <= lv_max; lv++)
        for (int st : strats)
            if (!(lv == 6 && st == Z_FILTERED))
                c.push_back({lv, st, wbits, 9});

    return c;
}

static bool find_deflate_params(const std::vector<uint8_t>& px,
                                const std::vector<uint8_t>& tgt,
                                DeflParams& p)
{
    auto cands = build_candidates(tgt);
    if (cands.empty()) return false;

    if (sfth_threads <= 1) {
        // Sequential
        std::vector<uint8_t> tmp, attempt;
        for (auto& c : cands) {
            if (!probe_deflate(px.data(), px.size(), tgt.data(), tgt.size(),
                               c.lv, c.st, c.wbits, c.memlevel, tmp))
                continue;
            if (full_deflate(px, tgt, c.lv, c.st, c.wbits, c.memlevel, attempt)) {
                p = {c.lv, c.st, c.memlevel, c.wbits}; return true;
            }
        }
        return false;
    }

    // Parallel brute-force (-sfth)
    std::atomic<bool> found{false};
    std::atomic<int>  next_cand{0};
    std::mutex        result_mu;
    DeflParams        result{};
    int nw = std::min(sfth_threads, (int)cands.size());
    std::vector<std::thread> workers;
    for (int t = 0; t < nw; t++) {
        workers.emplace_back([&]() {
            std::vector<uint8_t> tmp, attempt;
            while (!found) {
                int idx = next_cand.fetch_add(1);
                if (idx >= (int)cands.size()) break;
                auto& c = cands[idx];
                if (!probe_deflate(px.data(), px.size(), tgt.data(), tgt.size(),
                                   c.lv, c.st, c.wbits, c.memlevel, tmp))
                    continue;
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
    size_t bound = lzma_stream_buffer_bound(insz);
    out.resize(bound);
    size_t pos = 0;
    lzma_ret r = lzma_easy_buffer_encode(
        g_lzma_preset, LZMA_CHECK_CRC64, nullptr,
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

/* ─── PPG write helpers ──────────────────────────────────────────────────── */

// Compress one frame and append its serialised data to ppg_out.
// Returns false on error (errormessage set).
static bool write_frame(const PngFrame& fr, std::vector<uint8_t>& ppg_out) {
    DeflParams dp{};
    bool matched = !fr.pixels.empty() &&
                   find_deflate_params(fr.pixels, fr.idat_raw, dp);

    const uint8_t* payload = matched ? fr.pixels.data()   : fr.idat_raw.data();
    size_t         paysz   = matched ? fr.pixels.size()    : fr.idat_raw.size();

    std::vector<uint8_t> lzma_data;
    if (!lzma_enc(payload, paysz, lzma_data)) return false;

    if (verbosity >= 1) {
        std::lock_guard<std::mutex> lk(g_print_mutex);
        if (matched)
            fprintf(stdout, "  %s[match]%s  lv=%d st=%s wb=%d ml=%d  px=%zu  idat=%zu  →lzma=%zu\n",
                    col(GR), col(R), dp.level, strategy_name(dp.strategy),
                    dp.wbits, dp.memlevel,
                    fr.pixels.size(), fr.idat_raw.size(), lzma_data.size());
        else
            fprintf(stdout, "  %s[stored]%s  idat=%zu  →lzma=%zu\n",
                    col(YL), col(R), fr.idat_raw.size(), lzma_data.size());
    }

    // Frame header
    ppg_out.push_back(fr.fctl.empty() ? 0 : 1);
    if (!fr.fctl.empty()) {
        if (fr.fctl.size() != 26) {
            snprintf(errormessage, MSG_SIZE, "bad fctl size"); return false;
        }
        wr_bytes(ppg_out, fr.fctl.data(), 26);
    }
    ppg_out.push_back(fr.uses_idat ? 1 : 0);
    if (!fr.uses_idat) wr_le32(ppg_out, fr.first_seq);
    ppg_out.push_back(matched ? 0 : 1);                         // mode
    ppg_out.push_back(matched ? (uint8_t)dp.level    : 0);     // level
    ppg_out.push_back(matched ? (uint8_t)dp.strategy : 0);     // strategy
    ppg_out.push_back(matched ? (uint8_t)dp.wbits    : 15);    // wbits (CMF-derived)
    ppg_out.push_back(matched ? (uint8_t)dp.memlevel : 8);     // memlevel

    wr_le32(ppg_out, (uint32_t)fr.chunk_szs.size());
    for (uint32_t s : fr.chunk_szs) wr_le32(ppg_out, s);

    wr_le64(ppg_out, (uint64_t)paysz);
    wr_le64(ppg_out, (uint64_t)lzma_data.size());
    wr_bytes(ppg_out, lzma_data.data(), lzma_data.size());
    return true;
}

/* ─── compress PNG/APNG → PPG ────────────────────────────────────────────── */

static bool compress_png(const std::vector<uint8_t>& png_buf,
                         std::vector<uint8_t>& ppg_out)
{
    PngInfo info;
    if (!parse_png(png_buf, info)) return false;

    ppg_out.clear();
    wr_bytes(ppg_out, PPG_SIG, 4);
    ppg_out.push_back(2);                               // format version
    ppg_out.push_back(info.is_apng ? 1 : 0);           // flags
    wr_le32(ppg_out, info.num_plays);
    wr_le64(ppg_out, (uint64_t)info.pre.size());
    wr_bytes(ppg_out, info.pre.data(), info.pre.size());
    wr_le64(ppg_out, (uint64_t)info.post.size());
    wr_bytes(ppg_out, info.post.data(), info.post.size());
    wr_le32(ppg_out, (uint32_t)info.frames.size());

    for (auto& fr : info.frames)
        if (!write_frame(fr, ppg_out)) return false;

    return true;
}

/* ─── reconstruct deflate stream from stored frame data ─────────────────── */

static bool rebuild_deflate(uint8_t mode, uint8_t level, uint8_t strategy,
                            uint8_t wbits, uint8_t memlevel,
                            std::vector<uint8_t>& payload,
                            std::vector<uint8_t>& deflate_out)
{
    if (mode == 0) {
        // mode match: re-deflate the pixels back to the original stream
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
    } else {
        // mode stored: payload IS the deflate stream
        deflate_out = std::move(payload);
    }
    return true;
}

/* ─── decompress PPG → PNG/APNG ─────────────────────────────────────────── */

// Read one frame from ppg data starting at pos; advance pos.
struct FrameMeta {
    std::vector<uint8_t> fctl;
    bool     uses_idat;
    uint32_t first_seq;
    uint8_t  mode, level, strategy;
    uint8_t  wbits = 15, memlevel = 8;
    std::vector<uint32_t> chunk_szs;
    std::vector<uint8_t>  payload;
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
    if (pos + 5 > sz) { snprintf(errormessage, MSG_SIZE, "PPG truncated (mode/lv/st/wb/ml)"); return false; }
    fm.mode     = p[pos++];
    fm.level    = p[pos++];
    fm.strategy = p[pos++];
    fm.wbits    = p[pos++];
    fm.memlevel = p[pos++];

    if (pos + 4 > sz) { snprintf(errormessage, MSG_SIZE, "PPG truncated (n_chunks)"); return false; }
    uint32_t nc = rd_le32(p + pos); pos += 4;
    if (pos + nc * 4 > sz) { snprintf(errormessage, MSG_SIZE, "PPG truncated (chunk_szs)"); return false; }
    fm.chunk_szs.resize(nc);
    for (uint32_t i = 0; i < nc; i++) { fm.chunk_szs[i] = rd_le32(p + pos); pos += 4; }

    if (pos + 16 > sz) { snprintf(errormessage, MSG_SIZE, "PPG truncated (sizes)"); return false; }
    uint64_t raw_sz  = rd_le64(p + pos); pos += 8;
    uint64_t lzma_sz = rd_le64(p + pos); pos += 8;
    if (pos + lzma_sz > sz) { snprintf(errormessage, MSG_SIZE, "PPG truncated (lzma)"); return false; }
    if (!lzma_dec(p + pos, lzma_sz, fm.payload, raw_sz)) return false;
    pos += lzma_sz;
    return true;
}

static bool emit_idat_or_fdat(std::vector<uint8_t>& png_out, const FrameMeta& fm) {
    std::vector<uint8_t> payload_copy = fm.payload;
    std::vector<uint8_t> deflate_stream;
    if (!rebuild_deflate(fm.mode, fm.level, fm.strategy, fm.wbits, fm.memlevel,
                         payload_copy, deflate_stream))
        return false;

    size_t dpos = 0;
    for (size_t i = 0; i < fm.chunk_szs.size(); i++) {
        uint32_t csz = fm.chunk_szs[i];
        if (dpos + csz > deflate_stream.size()) {
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
            // Build fdAT data: seq(4) + payload
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
        // Legacy PPG v1 (single-frame PNG only)
        if (pos >= sz) { snprintf(errormessage, MSG_SIZE, "PPG v1 truncated"); return false; }
        uint8_t mode     = p[pos++];
        uint8_t level    = p[pos++];
        uint8_t strategy = p[pos++];
        uint32_t n_idat  = rd_le32(p + pos); pos += 4;
        std::vector<uint32_t> isizes(n_idat);
        for (uint32_t i = 0; i < n_idat; i++) { isizes[i] = rd_le32(p + pos); pos += 4; }
        uint64_t pre_sz = rd_le64(p + pos); pos += 8;
        std::vector<uint8_t> pre(p + pos, p + pos + pre_sz); pos += pre_sz;
        uint64_t suf_sz = rd_le64(p + pos); pos += 8;
        std::vector<uint8_t> suf(p + pos, p + pos + suf_sz); pos += suf_sz;
        uint64_t raw_sz  = rd_le64(p + pos); pos += 8;
        uint64_t lzma_sz = rd_le64(p + pos); pos += 8;
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

    if (version != 2) {
        snprintf(errormessage, MSG_SIZE, "unknown PPG version %u", version); return false;
    }

    // PPG v2
    if (pos + 1 + 4 + 8 > sz) { snprintf(errormessage, MSG_SIZE, "PPG v2 truncated (hdr)"); return false; }
    bool     is_apng   = (p[pos++] & 1) != 0;
    uint32_t num_plays = rd_le32(p + pos); pos += 4;  (void)num_plays;
    uint64_t pre_sz    = rd_le64(p + pos); pos += 8;
    if (pos + pre_sz > sz) { snprintf(errormessage, MSG_SIZE, "PPG v2 truncated (pre)"); return false; }
    std::vector<uint8_t> pre(p + pos, p + pos + pre_sz); pos += pre_sz;
    uint64_t post_sz = rd_le64(p + pos); pos += 8;
    if (pos + post_sz > sz) { snprintf(errormessage, MSG_SIZE, "PPG v2 truncated (post)"); return false; }
    std::vector<uint8_t> post(p + pos, p + pos + post_sz); pos += post_sz;
    uint32_t num_frames = rd_le32(p + pos); pos += 4;
    (void)is_apng;

    std::vector<FrameMeta> frames(num_frames);
    for (uint32_t i = 0; i < num_frames; i++)
        if (!read_frame_v2(p, sz, pos, frames[i])) return false;

    png_out.clear();
    wr_bytes(png_out, PNG_SIG, 8);
    wr_bytes(png_out, pre.data(), pre.size());
    for (auto& fm : frames) {
        // Write fcTL chunk if present
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

/* ─── file helpers ───────────────────────────────────────────────────────── */

static std::vector<uint8_t> read_file(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return {};
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return {}; }
    std::vector<uint8_t> buf(sz);
    fread(buf.data(), 1, sz, f); fclose(f);
    return buf;
}

static bool write_file(const std::string& path, const std::vector<uint8_t>& data) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    fclose(f); return ok;
}

enum FileType { F_PNG, F_PPG, F_UNK };

static FileType detect_type(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return F_UNK;
    uint8_t buf[8]; size_t n = fread(buf, 1, 8, f); fclose(f);
    if (n >= 8 && memcmp(buf, PNG_SIG, 8) == 0) return F_PNG;  // also matches APNG
    if (n >= 4 && memcmp(buf, PPG_SIG, 4) == 0) return F_PPG;
    return F_UNK;
}

static std::string replace_ext(const std::string& path, const std::string& ext) {
    auto dot = path.rfind('.');
    return (dot == std::string::npos ? path : path.substr(0, dot)) + ext;
}

static std::string make_outpath(const std::string& inpath, const std::string& ext) {
    namespace fs = std::filesystem;
    std::string base = replace_ext(fs::path(inpath).filename().string(), ext);
    if (!outdir.empty()) return (fs::path(outdir) / base).string();
    return (fs::path(inpath).parent_path() / base).string();
}

/* ─── process one file ───────────────────────────────────────────────────── */

static void process_file(const std::string& inpath) {
    namespace fs = std::filesystem;
    FileType ft = detect_type(inpath);
    if (ft == F_UNK) return;
    if (ft == F_PNG && decompress_only) return;
    if (ft == F_PPG && compress_only)   return;

    bool do_compress = (ft == F_PNG);
    std::string ext     = do_compress ? ".ppg" : ".png";
    std::string outpath = make_outpath(inpath, ext);

    if (!overwrite && fs::exists(outpath)) {
        std::lock_guard<std::mutex> lk(g_print_mutex);
        fprintf(stderr, "%sERROR%s %s: output exists\n", col(RD), col(R), inpath.c_str());
        g_errors++;
        return;
    }

    size_t in_sz = (size_t)fs::file_size(inpath);
    auto in = read_file(inpath);
    if (in.empty()) {
        std::lock_guard<std::mutex> lk(g_print_mutex);
        fprintf(stderr, "%sERROR%s %s: cannot read\n", col(RD), col(R), inpath.c_str());
        g_errors++;
        return;
    }

    std::vector<uint8_t> out;
    bool ok = do_compress ? compress_png(in, out) : decompress_ppg(in, out);
    if (!ok) {
        std::lock_guard<std::mutex> lk(g_print_mutex);
        fprintf(stderr, "%sERROR%s %s: %s\n", col(RD), col(R), inpath.c_str(), errormessage);
        g_errors++;
        return;
    }

    if (verify) {
        std::vector<uint8_t> rt;
        bool vok;
        if (do_compress) {
            // Verify compress: decompress the PPG we just produced → must equal original PNG
            vok = decompress_ppg(out, rt);
            if (!vok || rt != in) {
                std::lock_guard<std::mutex> lk(g_print_mutex);
                fprintf(stderr, "%sERROR%s %s: round-trip mismatch\n", col(RD), col(R), inpath.c_str());
                g_errors++;
                return;
            }
        } else {
            // Verify decompress: re-decompress the same PPG → must equal what we just wrote
            vok = decompress_ppg(in, rt);
            if (!vok || rt != out) {
                std::lock_guard<std::mutex> lk(g_print_mutex);
                fprintf(stderr, "%sERROR%s %s: round-trip mismatch\n", col(RD), col(R), inpath.c_str());
                g_errors++;
                return;
            }
        }
    }

    if (!dry_run && !write_file(outpath, out)) {
        std::lock_guard<std::mutex> lk(g_print_mutex);
        fprintf(stderr, "%sERROR%s %s: cannot write output\n", col(RD), col(R), inpath.c_str());
        g_errors++;
        return;
    }

    double ratio = in_sz > 0 ? 100.0 * out.size() / in_sz : 0.0;
    {
        std::lock_guard<std::mutex> lk(g_print_mutex);
        if (!module_mode)
            fprintf(stdout, "%s%s%s -> %s%s%s  %.2f%%\n",
                    col(GR), inpath.c_str(), col(R),
                    col(GR), outpath.c_str(), col(R), ratio);
    }
    g_processed++;
    // Atomic double add via CAS loop
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
            if (ext == ".png" || ext == ".apng" || ext == ".ppg")
                filelist.push_back(e.path().string());
        }
    } else {
        filelist.push_back(path);
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

    if (version != 2) {
        fprintf(stdout, "  unknown PPG version %u\n", version); return;
    }

    bool is_apng   = (buf[pos++] & 1) != 0;
    uint32_t plays = rd_le32(buf.data()+pos); pos+=4;
    uint64_t pre_sz=rd_le64(buf.data()+pos); pos+=8; pos+=pre_sz;
    uint64_t post_sz=rd_le64(buf.data()+pos); pos+=8; pos+=post_sz;
    uint32_t nf   = rd_le32(buf.data()+pos); pos+=4;

    if (is_apng)
        fprintf(stdout, "  PPG v2  APNG  %u frame(s)  loops=%u  pre=%lluB  post=%lluB\n",
                nf, plays, (unsigned long long)pre_sz, (unsigned long long)post_sz);
    else
        fprintf(stdout, "  PPG v2  PNG   %u frame(s)  pre=%lluB  post=%lluB\n",
                nf, (unsigned long long)pre_sz, (unsigned long long)post_sz);

    for (uint32_t i = 0; i < nf && pos < buf.size(); i++) {
        bool has_fctl = buf[pos++] != 0;
        if (has_fctl && pos+26 <= buf.size()) pos+=26;
        bool uses_idat = buf[pos++] != 0;
        if (!uses_idat && pos+4 <= buf.size()) pos+=4;
        if (pos+5 > buf.size()) break;
        uint8_t mode=buf[pos++], lv=buf[pos++], st=buf[pos++];
        uint8_t wb=buf[pos++], ml=buf[pos++];
        if (pos+4 > buf.size()) break;
        uint32_t nc=rd_le32(buf.data()+pos); pos+=4;
        pos+=nc*4;
        if (pos+16 > buf.size()) break;
        uint64_t raw_sz =rd_le64(buf.data()+pos); pos+=8;
        uint64_t lzma_sz=rd_le64(buf.data()+pos); pos+=8;
        pos+=lzma_sz;

        const char* modestr = (mode==0) ? "match " : "stored";
        const char* mcol    = (mode==0) ? col(GR)  : col(YL);
        fprintf(stdout,
                "  Frame %u: %s[%s]%s %s  lv=%u st=%-8s wb=%u ml=%u"
                "  raw=%llu  lzma=%llu (%.1f%%)\n",
                i, mcol, modestr, col(R),
                uses_idat ? "IDAT" : "fdAT",
                lv, strategy_name(st), wb, ml,
                (unsigned long long)raw_sz, (unsigned long long)lzma_sz,
                raw_sz > 0 ? 100.0*lzma_sz/raw_sz : 0.0);
    }
}

/* ─── show help ──────────────────────────────────────────────────────────── */

static void show_help() {
    fprintf(stdout,
        "\n%spackPNG%s v0.%i%s  •  by %s\n"
        "PNG/APNG lossless recompressor — brute-force zlib match + LZMA\n\n"
        "Usage: packPNG [subcommand] [flags] file(s)\n\n"
        "Subcommands:\n"
        "  a            compress only (.png/.apng → .ppg)\n"
        "  x            decompress only (.ppg → .png/.apng)\n"
        "  mix          both directions (default)\n"
        "  l / list     inspect .ppg files (no decompression)\n\n"
        "Flags:\n"
        "  -ver         verify round-trip after processing\n"
        "  -v0/-v1/-v2  verbosity (default 0)\n"
        "  -np          no pause after processing\n"
        "  -o           overwrite existing output files\n"
        "  -p           proceed on warnings\n"
        "  -r           recurse into subdirectories\n"
        "  -dry         dry run (no output files)\n"
        "  -m<1-9>      LZMA preset (default 6)\n"
        "  -th<N>       N file-level threads (0=auto)\n"
        "  -sfth        parallel brute-force within each file\n"
        "  -od<path>    write output to directory\n"
        "  -module      machine-friendly output\n"
        "  --no-color   disable ANSI color\n\n"
        "Examples:\n"
        "  packPNG a -ver -o image.png\n"
        "  packPNG a -sfth animation.apng\n"
        "  packPNG a -th4 -od out/ *.png\n"
        "  packPNG x archive.ppg\n\n",
        col(BC), col(R), appversion, subversion, author);
}

/* ─── main ───────────────────────────────────────────────────────────────── */

int main(int argc, char** argv)
{
    init_colors();
    if (argc < 2) { show_help(); return 0; }

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
        if      (arg == "-ver")       verify    = true;
        else if (arg == "-v0")        verbosity = 0;
        else if (arg == "-v1")        verbosity = 1;
        else if (arg == "-v2")        verbosity = 2;
        else if (arg == "-np")        wait_exit = false;
        else if (arg == "-o")         overwrite = true;
        else if (arg == "-p")         err_tol   = 2;
        else if (arg == "-r")         recursive = true;
        else if (arg == "-dry")       dry_run   = true;
        else if (arg == "-sfth")      sfth      = true;
        else if (arg == "--no-color") no_color  = true;
        else if (arg == "-module")  { module_mode = true; wait_exit = false; }
        else if (arg.size() > 2 && arg.substr(0,2) == "-m") {
            int v = atoi(arg.c_str() + 2);
            if (v >= 1 && v <= 9) g_lzma_preset = (unsigned)v;
        }
        else if (arg.size() > 3 && arg.substr(0,3) == "-th") {
            int n = atoi(arg.c_str() + 3);
            if (n == 0) { n = (int)std::thread::hardware_concurrency(); if (n < 1) n = 1; }
            num_threads = n;
        }
        else if (arg.size() > 3 && arg.substr(0,3) == "-od") {
            outdir = arg.substr(3);
            std::filesystem::create_directories(outdir);
        }
        else if (!arg.empty() && arg[0] == '-')
            fprintf(stderr, "unknown flag: %s\n", arg.c_str());
        else
            collect(arg);
    }

    // Compute sfth_threads: only active when -sfth set and not also using -th
    if (sfth) {
        if (num_threads <= 1) {
            sfth_threads = (int)std::thread::hardware_concurrency();
            if (sfth_threads < 2) sfth_threads = 4;
        }
        // If -th > 1: file-level parallelism takes priority; sfth stays 1
    }

    if (filelist.empty()) { show_help(); return 0; }

    if (!module_mode) {
        fprintf(stdout, "\n%spackPNG%s v0.%i%s  •  by %s\n\n",
                col(BC), col(R), appversion, subversion, author);
    }

    // List mode: inspect .ppg files without decompressing
    if (list_mode) {
        for (auto& f : filelist) list_ppg(f);
        if (wait_exit && !module_mode) { fprintf(stdout, "\nPress <enter> to quit\n"); getchar(); }
        return 0;
    }

    auto t0 = std::chrono::steady_clock::now();

    if (num_threads <= 1) {
        for (auto& f : filelist) process_file(f);
    } else {
        // Thread pool: atomic index into filelist
        std::atomic<size_t> next_idx{0};
        std::vector<std::thread> workers;
        for (int t = 0; t < num_threads; t++) {
            workers.emplace_back([&]() {
                while (true) {
                    size_t i = next_idx.fetch_add(1);
                    if (i >= filelist.size()) break;
                    process_file(filelist[i]);
                }
            });
        }
        for (auto& w : workers) w.join();
    }

    auto t1 = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(t1 - t0).count();

    int proc = g_processed.load(), errs = g_errors.load();
    double ai_d = g_acc_in.load(), ao_d = g_acc_out.load();

    if (module_mode) {
        fprintf(stdout, "%s %.3fs\n", errs ? "ERROR" : "OK", elapsed);
    } else if (proc > 0) {
        double ratio = ai_d > 0 ? 100.0 * ao_d / ai_d : 0.0;
        fprintf(stdout, "\n%i file(s)  %.2f%%  %.2fs\n", proc, ratio, elapsed);
        if (errs > 0) fprintf(stdout, "%s%i error(s)%s\n", col(RD), errs, col(R));
    }

    if (wait_exit && !module_mode) { fprintf(stdout, "\nPress <enter> to quit\n"); getchar(); }
    return errs ? 1 : 0;
}
