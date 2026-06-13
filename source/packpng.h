/* libpackPNG — public C API.
 *
 * Lossless, byte-exact recompressor for the PNG family (PNG/APNG/JNG/MNG).
 * Link against libpackpng.a plus its deps (see README "Build"):
 *   -lz -llzma -lzstd  <kanzi>.a  <preflate>.a  -lpthread [-ldl -lm]
 * (Windows mingw: ... -lws2_32 -luserenv -lbcrypt -lntdll)
 */
#ifndef PACKPNG_H
#define PACKPNG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Backend for PNG/APNG inputs. JNG/MNG inputs ignore this and use their own
 * format codec (TCIJ / TCIM). */
typedef enum {
    PACKPNG_TCIP = 0, /* default: preflate + WebP-lossless (best ratio, fast decode) */
    PACKPNG_TVCP = 1, /* fast:    kanzi BWT + zstd          (fastest, weaker ratio)  */
    PACKPNG_TMCP = 2, /* archival: preflate + kanzi-TPAQX   (max ratio, slow)        */
    PACKPNG_TPCL = 3  /* legacy:  preflate + multi-threaded LZMA2 (precomp-style)    */
} packpng_backend;

/* Compress `in_path` (.png/.apng/.jng/.mng) to `out_path` (.ppg), byte-exact.
 * Returns 0 on success, nonzero on error (see packpng_last_error). */
int packpng_compress_file(const char* in_path, const char* out_path, packpng_backend backend);

/* Decompress a .ppg `in_path`; the original file is written into `out_dir`
 * under its stored basename. The format is auto-detected by magic.
 * Returns 0 on success, nonzero on error. */
int packpng_decompress_file(const char* in_path, const char* out_dir);

/* ── In-memory API (for archivers / embedding) ──
 * Multithreading is ON by default. On success returns 0 and sets `*out` to a
 * heap buffer of `*out_len` bytes — free it with packpng_free(). On error
 * returns nonzero (see packpng_last_error) and leaves *out NULL. */

/* Compress a PNG/APNG/JNG/MNG image held in memory (`in`,`in_len`) to a .ppg in
 * memory. `name_hint` is the original filename stored in the container (may be
 * NULL). The image type is detected from `in`'s bytes, not the name. */
int packpng_compress_mem(const unsigned char* in, size_t in_len, const char* name_hint,
                         unsigned char** out, size_t* out_len, packpng_backend backend);

/* Decompress a .ppg held in memory (`in`,`in_len`) back to the original bytes. */
int packpng_decompress_mem(const unsigned char* in, size_t in_len,
                           unsigned char** out, size_t* out_len);

/* Free a buffer returned by packpng_compress_mem / packpng_decompress_mem. */
void packpng_free(unsigned char* p);

/* Override the worker-thread count for subsequent calls (0 = auto = default). */
void packpng_set_threads(int n);

/* Last error message (thread-local). Valid until the next API call. */
const char* packpng_last_error(void);

/* Library version string, e.g. "1.9". */
const char* packpng_version(void);

#ifdef __cplusplus
}
#endif

#endif /* PACKPNG_H */
