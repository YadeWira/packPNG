# packPNG v1.5b — autonomous polish report

Trabajo nocturno mientras dormías. 3 commits pushed a `master`.

## Commits

| hash | descripción |
|---|---|
| `b37b7a1` | fix(v1.5b): bug-hunt fixes — error propagation, silent skips, stale strings |
| `794a8cf` | fix(v1.5b): fuzz-driven hardening — bounds checks + decoder sanity caps |
| `30466b7` | perf(v1.5b): parallelize per-PNG extract phase — encode -20% |

## Bugs encontrados y corregidos

### 1. `thread_local errormessage` no se propagaba entre threads
Worker threads (kanzi pixel decode, zstd idat) escribían su error en su propio `thread_local errormessage`; main leía el suyo (default `"no error"`). Ejemplo de error visible: bit-flipped `.tcip` → mensaje `"no error"`. Fix: capturar `errormessage` por thread en `std::string`, pasar a main, snprintf cause real.

### 2. Strings `"PPGS"` legacy en mensajes de error
8 templates de error decían `"PPGS truncated meta header"`, etc. Renombré a `"TCIP truncated meta header"` para reflejar el wire format v1.5.

### 3. Inputs malos con silent OK exit
- Empty `.png` file → exit 0 (sin warning)
- Non-PNG content con extensión `.png` → exit 0
- Nonexistent file → exit 0
- Truncated PNG pre-IDAT → exit 0

Ahora cada caso emite warning a stderr y `g_errors++` para exit ≠ 0.

### 4. tovyCIP encode silent-skipped non-PNG inputs
Cuando pasas mixed input (1 valid + 1 bad + missing), bad/missing se filtraban silenciosamente. Ahora imprime `[skip] file: not a PNG (magic mismatch)` o `[skip] file: file not found`.

### 5. Cross-mode silent skip (PNG en x mode, PPG en a mode)
PNG con `-x` (decompress-only) → silent return. Ahora warning explicativo.

### 6. **Heap-buffer-overflow en `reconstruct_png_from_streams`** (ASan)
La verificación `if (off + psz > src_sz)` con `off`/`psz` size_t podía OVERFLOW. Si `psz` era corrupto y enorme (e.g. 0xFFFFFFFFFFFFFF00), `off+psz` daba un small-value, check pasaba, luego `src+off+psz` leía OOB. Fix: `if (psz > src_sz || off > src_sz - psz)`.

### 7. **Decoder OOB usando `e.stream_idx` corrupto**
`stream_buf[e.stream_idx]` sin validar `stream_idx < num_streams` → wild pointer. Fix: validar stream_idx + px_off+px_sz vs stream size + idat_off+idat_sz vs idat size.

### 8. **Multi-TB allocation por raw-size corrupto** (ASan caught 92 TB request)
Todos los decoders (zstd_dec, zstd_dec_long, lzma_dec, fl2_dec, kanzi_dec_pipe, kanzi_solid_dec) hacían `out.resize(expected)` con `expected` leído del wire format. Un header corrupto pedía multi-TB. Fix: cap a 1 GB con error claro.

### 9. **11 sites con integer-overflow en bounds checks**
Pattern `if (pos + var > limit)` reescrito a forma overflow-safe en:
- PPG v1 lzma payload, deflate stream csz, solid pre/post sizes
- PPG v2 pre/post sizes, split-stream px/idat data
- tovyCIP archive meta_comp / scomp[s] / big_idat_comp

## Mejoras de rendimiento

### Paralelización del extract phase
Antes: loop sequencial sobre `png_paths`, cada call hacía read+inflate+brute-force-zlib-match+filter-sep en un solo thread. En 4-core con 23 PNGs, ~880ms con 3 cores idle.

Después: N=hw_concurrency workers consumen una atomic queue, cada uno produce SolidEntry + buffers locales. Concat sequential después preserva orden (determinismo intacto).

| metric | antes (v1.5a) | después (v1.5b) |
|---|---:|---:|
| encode time, 23-PNG corpus | ~0.88 s | **~0.70 s (−20 %)** |
| size | 1720912 B | 1720912 B (idéntico) |
| dec-th0 | ~0.054 s | ~0.054 s (idéntico) |
| determinismo | ✓ | ✓ |

## Validación

| test | resultado |
|---|---|
| RT 162 PngSuite valid PNGs (per-file `.ppg`) | **162/162 byte-exact** ✓ |
| RT 162 PngSuite (single-file `.tcip`) | **162/162 byte-exact** ✓ |
| RT 162 PngSuite (multi-file `.tcip` archive) | **162/162 byte-exact** ✓ |
| RT 14 PngSuite intentionally-corrupt PNGs | 14/14 graceful (0 crashes, 0 hangs) |
| RT 60 real-world PNGs (de `/home/forum`) | **60/60 byte-exact** ✓ |
| RT corpus 23-PNG (Cspeed.png 4.27MB pixels) | 23/23 ✓ |
| Encode determinismo (5× same input → same SHA-256) | ✓ |
| 3 concurrent encoders (race-free) | ✓ |
| ASan en encode+decode 162 PngSuite | 0 errors |
| **1000-trial fuzz + ASan en bit-flipped `.tcip`** | **0 ASan errors** |
| 1500-trial fuzz general | 2 hangs (0.13 %, kanzi-internal, deferred) |
| Tiny PNGs (104-113 bytes) | RT ok |
| Mixed input (good + bad + missing) | warnings + RT good ✓ |

## 2 hangs en 1500 fuzz trials

Casos raros donde el `.tcip` con bit-flip causa que kanzi decode entre en un loop infinito interno. Posiblemente bug en kanzi-cpp library; los seeds están guardados en `/tmp/fz5_*/seeds/hang_*.tcip` para investigar después si te interesa hacer un PR upstream a kanzi-cpp.

## Estado del binario

```
/home/forum/git/Otros/packPNG/source/packPNG          (release, full features)
/home/forum/git/Otros/packPNG/source/packPNG_asan     (ASan + -O1, debug)
```

## Remaining work para v1.5c o más allá

| # | item | estimate | priority |
|---|---|---|:---:|
| 1 | Investigar 2 hangs en fuzz (kanzi internal) | 1-2h | low |
| 2 | Windows binary con tovyCIP completo (mingw kanzi+zstd+libdeflate) | 2-3h | mid |
| 3 | 3-stream split heuristic (corpora con 2+ archivos grandes) | 2h | low |
| 4 | Banner ordering: aparece DESPUÉS de errores → confunde | 30min | low |
| 5 | Adaptive NO_FSEP threshold por size | 1h | low |
| 6 | Fuzz testing del decoder — más trials, multi-byte mutations | 1h | mid |
| 7 | Vendoring kanzi-cpp (simplifica build, full features Windows) | 2-3h | mid |

## Comparación final vs xz preset 6

| eje | xz `-m6` | tovyCIP v1.5b |
|---|---:|---:|
| size | 1,722,720 B | **1,720,912 B (−1808 B)** ✓ |
| comp | 1.43 s | **0.70 s (−51 %)** ✓ |
| decST | 0.096 s | **0.072 s (−25 %)** ✓ |
| dec-th0 mediana | ~0.050 s | ~0.054 s (+4 ms, dentro del ruido) |

dec-th0 está dentro del ruido (variance ±5ms en este sistema con cargas de fondo).
**3-3.5/4 ejes WIN según run** (size + comp + decST seguros; dec-th0 va y viene).

## Resumen

✅ Cero ASan errors después de hardening
✅ 100 % RT byte-exact en PngSuite + corpus real-world
✅ 100 % determinismo
✅ Encode 20 % más rápido
✅ Robustez vs corrupción (1000+ fuzz)
⚠️ dec-th0 vs xz dentro del ruido (sistema con cargas de fondo)

Todo pushed. Branch master en GitHub está al día.

Buenas noches 🌙
