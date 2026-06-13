CXX       ?= g++
CXXFLAGS   = -std=c++17 -O3 -Wall -Wextra \
             -funroll-loops -fomit-frame-pointer \
             -march=native
LIBS       = -lz -llzma
LIBS_LDF   = -lz -llzma -ldeflate
LIBS_ZSTD  = -lz -llzma -lzstd

# Third-party deps are vendored under source/vendor/ (sources + headers +
# prebuilt Linux static libs). Override any *_DIR via the environment to point
# at an external checkout instead.

# fast-lzma2 — upstream https://github.com/conor42/fast-lzma2
FL2_DIR   ?= vendor/fast-lzma2
LIBS_FL2   = -lz -llzma $(FL2_DIR)/libfast-lzma2.a -lpthread
INC_FL2    = -I$(FL2_DIR)

# kanzi-cpp — upstream https://github.com/flanglet/kanzi-cpp
# (we use $(KANZI_DIR)/build/libkanzi.a + headers under $(KANZI_DIR)/src).
# v1.8: kanzi target also links libzstd because the tovyCIP/TCIP IDAT
# passthrough and the new TCIJ (JNG) wrapper both call zstd_enc_long.
KANZI_DIR ?= vendor/kanzi-cpp
LIBS_KANZI = -lz -llzma -lzstd $(KANZI_DIR)/build/libkanzi.a -lpthread
INC_KANZI  = -I$(KANZI_DIR)/src

# packJPG — sources compiled in-tree for the kanzi-pjg (TCIJ v2 / JNG) target
PJG_DIR   ?= vendor/packJPG
PJG_SRCS   = $(PJG_DIR)/packjpg.cpp $(PJG_DIR)/aricoder.cpp $(PJG_DIR)/bitops.cpp

# preflate-rs (Rust) — powers the -preflate opt-in max-ratio mode (TCPF output).
# The static libs are large build artifacts (gitignored); (re)build them with
# `make preflate-libs PREFLATE_SRC=/path/to/preflate-rs` (needs the FFI patch in
# vendor/preflate-rs/src/ffi_oneshot.rs + dll crate-type "staticlib"). The Rust
# staticlib pulls these system libs: Linux -lpthread -ldl -lm; Windows
# -lws2_32 -luserenv -lbcrypt -lntdll (all ship with the OS → binary stays autonomous).
PREFLATE_DIR ?= vendor/preflate-rs
PREFLATE_LIB     = $(PREFLATE_DIR)/lib/linux/libpreflate_rs_0_7.a
PREFLATE_LIB_WIN = $(PREFLATE_DIR)/lib/win/libpreflate_rs_0_7.a

# TNCP — experimental neural/CM backend (opt-in -tncp). Rust staticlib built from ../TNCP.
TNCP_DIR ?= ../TNCP
TNCP_LIB  = $(TNCP_DIR)/target/release/libtncp.a
PREFLATE_RT      = -lpthread -ldl -lm
PREFLATE_RT_WIN  = -lws2_32 -luserenv -lbcrypt -lntdll
PREFLATE_SRC ?= /home/forum/git/Otros/preflate-rs

TARGET     = packPNG
SRC        = packpng.cpp

# ── Windows cross-compile (mingw-w64) ─────────────────────────────────────
# Prerequisites (run once):
#   sudo apt install libz-mingw-w64-dev
#   cd /tmp && curl -sL https://github.com/tukaani-project/xz/releases/download/v5.6.4/xz-5.6.4.tar.gz | tar xz
#   cd xz-5.6.4 && ./configure --host=x86_64-w64-mingw32 --prefix=/tmp/xz-win64 \
#     --enable-static --disable-shared --disable-xz --disable-xzdec \
#     --disable-lzmadec --disable-lzmainfo --disable-scripts --disable-doc \
#     CC=x86_64-w64-mingw32-gcc CXX=x86_64-w64-mingw32-g++ && make -j4 install

MINGW_CXX   ?= x86_64-w64-mingw32-g++
MINGW_FLAGS  = -std=c++17 -O3 -Wall -Wextra -funroll-loops -fomit-frame-pointer
XZ_WIN      ?= /tmp/xz-win64
MINGW_INC    = -I$(XZ_WIN)/include
MINGW_LIBS   = -static \
               -L$(XZ_WIN)/lib -llzma \
               -lz \
               -lstdc++ -lgcc -lgcc_eh -lws2_32
WIN_TARGET   = packPNG.exe

# ── Windows FULL build (mingw, tovyCIP: kanzi + zstd + packJPG, fully static) ──
# Produces a standalone .exe whose only imports are KERNEL32.dll + msvcrt.dll
# (both shipped with Windows) — no mingw runtime / VC++ redist needed.
# Prerequisites (cross-built once with the mingw toolchain):
#   - zstd + liblzma static libs + headers under $(MINGW_DEPS)/{lib,include}
#       zstd:  make -C lib CC=x86_64-w64-mingw32-gcc AR=x86_64-w64-mingw32-ar libzstd.a
#       xz:    ./configure --host=x86_64-w64-mingw32 --enable-static --disable-shared ...
#   - mingw libkanzi.a (cmake -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_CXX_COMPILER=...mingw...)
#   - zlib mingw from apt: sudo apt install libz-mingw-w64-dev
MINGW_DEPS    ?= vendor/mingw-deps
MINGW_KANZI   ?= $(KANZI_DIR)/build_win/libkanzi.a
WIN_FULL_TARGET = packPNG-windows-x86_64-full.exe

.PHONY: all minimal ldf zstd fl2 kanzi kanzi-pjg static tncp lib lib-win preflate-libs clean asan win win-full

# Default build: the self-contained / dependency-free binaries.
#   - Linux:   fully static (no shared libs)        → packPNG
#   - Windows: static mingw .exe (KERNEL32+msvcrt)  → packPNG-windows-x86_64-full.exe
# Both are full tovyCIP (kanzi + zstd + packJPG) using the vendored deps.
# The Windows half needs the mingw toolchain; if it's not installed, build just
# the Linux binary with `make static`. For the tiny LZMA-only dynamic build use
# `make minimal`.
all: static win-full

# Minimal LZMA-only build (smallest binary; links shared libz/liblzma at runtime).
minimal: $(SRC)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $^ $(LIBS)

# Build with libdeflate support (-ldf flag, faster inflate + mode-2 repack)
ldf: $(SRC)
	$(CXX) $(CXXFLAGS) -DUSE_LIBDEFLATE -o $(TARGET) $^ $(LIBS_LDF)

# Build with Zstd support (-zstd flag, solid Zstd block instead of LZMA)
zstd: $(SRC)
	$(CXX) $(CXXFLAGS) -DUSE_ZSTD -o $(TARGET) $^ $(LIBS_ZSTD)

# Build with fast-lzma2 support (-fl2 flag, ~2-4x faster encode at +1-2% size)
fl2: $(SRC)
	$(CXX) $(CXXFLAGS) -DUSE_FL2 $(INC_FL2) -o $(TARGET) $^ $(LIBS_FL2)

# Build with kanzi + zstd support — this is the v1.7+/v1.8 production target.
# Kanzi powers the tovyCIP pixel stream; zstd powers the IDAT-passthrough and
# the new TCIJ (JNG) head/image/tail sections.
kanzi: $(SRC)
	$(CXX) $(CXXFLAGS) -DUSE_KANZI -DUSE_ZSTD $(INC_KANZI) -o $(TARGET) $^ $(LIBS_KANZI)

# kanzi + packJPG: enables TCIJ v2 (JNG Phase 2) — JDAT/JDAA routed through packJPG
kanzi-pjg: $(SRC)
	$(CXX) $(CXXFLAGS) -DBUILD_LIB -I$(PJG_DIR) -c $(PJG_DIR)/packjpg.cpp -o packjpg_lib.o
	$(CXX) $(CXXFLAGS) -c $(PJG_DIR)/aricoder.cpp -o aricoder_lib.o
	$(CXX) $(CXXFLAGS) -c $(PJG_DIR)/bitops.cpp   -o bitops_lib.o
	$(CXX) $(CXXFLAGS) -DUSE_KANZI -DUSE_ZSTD -DUSE_PACKJPG -I$(PJG_DIR) $(INC_KANZI) \
	    -o $(TARGET) $(SRC) packjpg_lib.o aricoder_lib.o bitops_lib.o $(LIBS_KANZI)

# Fully static Linux build (full tovyCIP). Produces a single self-contained
# binary with NO shared-library dependencies (`ldd` → "not a dynamic executable")
# — portable across distros. Needs the static system libs: the .a files from
# zlib1g-dev, liblzma-dev, libzstd-dev plus libc.a (libc6-dev) and libstdc++.a.
static: $(SRC)
	$(CXX) $(CXXFLAGS) -DBUILD_LIB -I$(PJG_DIR) -c $(PJG_DIR)/packjpg.cpp -o packjpg_lib.o
	$(CXX) $(CXXFLAGS) -c $(PJG_DIR)/aricoder.cpp -o aricoder_lib.o
	$(CXX) $(CXXFLAGS) -c $(PJG_DIR)/bitops.cpp   -o bitops_lib.o
	$(CXX) $(CXXFLAGS) -static -DUSE_KANZI -DUSE_ZSTD -DUSE_PACKJPG -DUSE_PREFLATE -I$(PJG_DIR) $(INC_KANZI) \
	    -o $(TARGET) $(SRC) packjpg_lib.o aricoder_lib.o bitops_lib.o \
	    -lzstd -llzma -lz $(KANZI_DIR)/build/libkanzi.a $(PREFLATE_LIB) -lpthread $(PREFLATE_RT)

# libpackPNG — static + shared library + C API (packpng.h). Self-contained fat
# archives (packpng + packjpg + kanzi + preflate merged); consumers link only the
# system libs. Objects are -fPIC so they serve both the .a and the .so. Linux:
lib: $(SRC) packpng.h
	$(CXX) $(CXXFLAGS) -fPIC -DBUILD_LIB -I$(PJG_DIR) -c $(PJG_DIR)/packjpg.cpp -o packjpg_lib.o
	$(CXX) $(CXXFLAGS) -fPIC -c $(PJG_DIR)/aricoder.cpp -o aricoder_lib.o
	$(CXX) $(CXXFLAGS) -fPIC -c $(PJG_DIR)/bitops.cpp   -o bitops_lib.o
	$(CXX) $(CXXFLAGS) -fPIC -DBUILD_LIB -DUSE_KANZI -DUSE_ZSTD -DUSE_PACKJPG -DUSE_PREFLATE \
	    -I$(PJG_DIR) $(INC_KANZI) -c $(SRC) -o packpng_lib.o
	rm -f libpackpng.a
	printf 'create libpackpng.a\naddmod packpng_lib.o packjpg_lib.o aricoder_lib.o bitops_lib.o\naddlib $(KANZI_DIR)/build/libkanzi.a\naddlib $(PREFLATE_LIB)\nsave\nend\n' | ar -M
	$(CXX) -shared -o libpackpng.so packpng_lib.o packjpg_lib.o aricoder_lib.o bitops_lib.o \
	    $(KANZI_DIR)/build/libkanzi.a $(PREFLATE_LIB) -lzstd -llzma -lz -lpthread -ldl -lm
	-strip libpackpng.so
	@echo "built libpackpng.a + libpackpng.so + packpng.h"

# libpackPNG for Windows (mingw): static .a + DLL (+ import lib via packpng.def).
lib-win: $(SRC) packpng.h packpng.def
	$(MINGW_CXX) $(MINGW_FLAGS) -DBUILD_LIB -I$(PJG_DIR) -c $(PJG_DIR)/packjpg.cpp -o packjpg_win.o
	$(MINGW_CXX) $(MINGW_FLAGS) -c $(PJG_DIR)/aricoder.cpp -o aricoder_win.o
	$(MINGW_CXX) $(MINGW_FLAGS) -c $(PJG_DIR)/bitops.cpp   -o bitops_win.o
	$(MINGW_CXX) $(MINGW_FLAGS) -DBUILD_LIB -DLZMA_API_STATIC -DUSE_KANZI -DUSE_ZSTD -DUSE_PACKJPG -DUSE_PREFLATE \
	    -I$(PJG_DIR) $(INC_KANZI) -I$(MINGW_DEPS)/include -c $(SRC) -o packpng_win.o
	rm -f libpackpng-win.a
	printf 'create libpackpng-win.a\naddmod packpng_win.o packjpg_win.o aricoder_win.o bitops_win.o\naddlib $(MINGW_KANZI)\naddlib $(PREFLATE_LIB_WIN)\nsave\nend\n' | x86_64-w64-mingw32-ar -M
	$(MINGW_CXX) -shared -static -static-libgcc -static-libstdc++ -o packpng.dll \
	    packpng_win.o packjpg_win.o aricoder_win.o bitops_win.o \
	    $(MINGW_KANZI) $(PREFLATE_LIB_WIN) -L$(MINGW_DEPS)/lib -lzstd -llzma -lz \
	    -lws2_32 -luserenv -lbcrypt -lntdll -Wl,--out-implib,libpackpng.dll.a packpng.def
	-x86_64-w64-mingw32-strip packpng.dll
	@echo "built libpackpng-win.a + packpng.dll + libpackpng.dll.a + packpng.h"

# Experimental TNCP backend (opt-in -tncp). Builds the TNCP Rust staticlib first.
# Separate target (not in `all`) — it links an extra 21 MB staticlib and the
# backend does not beat the TCIP default; it is an experiment / archival corner.
tncp: $(SRC)
	cd $(TNCP_DIR) && cargo build --release
	$(CXX) $(CXXFLAGS) -DBUILD_LIB -I$(PJG_DIR) -c $(PJG_DIR)/packjpg.cpp -o packjpg_lib.o
	$(CXX) $(CXXFLAGS) -c $(PJG_DIR)/aricoder.cpp -o aricoder_lib.o
	$(CXX) $(CXXFLAGS) -c $(PJG_DIR)/bitops.cpp   -o bitops_lib.o
	$(CXX) $(CXXFLAGS) -static -DUSE_KANZI -DUSE_ZSTD -DUSE_PACKJPG -DUSE_PREFLATE -DUSE_TNCP -I$(PJG_DIR) $(INC_KANZI) \
	    -o $(TARGET) $(SRC) packjpg_lib.o aricoder_lib.o bitops_lib.o \
	    -lzstd -llzma -lz $(KANZI_DIR)/build/libkanzi.a $(PREFLATE_LIB) $(TNCP_LIB) -lpthread $(PREFLATE_RT)

# (Re)build the preflate-rs static libs (Linux + windows-gnu) from PREFLATE_SRC,
# then strip + copy into vendor/preflate-rs/lib/. Run once before `make`/`static`/`win-full`.
# Needs: cargo, `rustup target add x86_64-pc-windows-gnu`, and the FFI patch
# (vendor/preflate-rs/src/ffi_oneshot.rs added to the dll crate, crate-type "staticlib").
preflate-libs:
	cd $(PREFLATE_SRC) && cargo build --release -p preflate_rs_0_7
	cd $(PREFLATE_SRC) && cargo build --release --target x86_64-pc-windows-gnu -p preflate_rs_0_7
	mkdir -p $(PREFLATE_DIR)/lib/linux $(PREFLATE_DIR)/lib/win
	cp $(PREFLATE_SRC)/target/release/libpreflate_rs_0_7.a $(PREFLATE_DIR)/lib/linux/
	cp $(PREFLATE_SRC)/target/x86_64-pc-windows-gnu/release/libpreflate_rs_0_7.a $(PREFLATE_DIR)/lib/win/
	strip --strip-debug $(PREFLATE_DIR)/lib/linux/libpreflate_rs_0_7.a || true

asan: $(SRC)
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -g -o $(TARGET)_asan $^ $(LIBS)

win: $(SRC)
	$(MINGW_CXX) $(MINGW_FLAGS) $(MINGW_INC) -o $(WIN_TARGET) $^ $(MINGW_LIBS)

# Full-featured Windows build with tovyCIP (kanzi + zstd + packJPG). Statically
# linked — verify with: x86_64-w64-mingw32-objdump -x $(WIN_FULL_TARGET) | grep -i '\.dll'
# (expect only KERNEL32.dll + msvcrt.dll). Verified byte-exact round-trip on
# Windows 10, and bit-identical .ppg output to the Linux build.
win-full: $(SRC)
	$(MINGW_CXX) $(MINGW_FLAGS) -DBUILD_LIB -I$(PJG_DIR) -c $(PJG_DIR)/packjpg.cpp -o packjpg_win.o
	$(MINGW_CXX) $(MINGW_FLAGS) -c $(PJG_DIR)/aricoder.cpp -o aricoder_win.o
	$(MINGW_CXX) $(MINGW_FLAGS) -c $(PJG_DIR)/bitops.cpp   -o bitops_win.o
	$(MINGW_CXX) $(MINGW_FLAGS) -static -static-libgcc -static-libstdc++ \
	    -DLZMA_API_STATIC -DUSE_KANZI -DUSE_ZSTD -DUSE_PACKJPG -DUSE_PREFLATE \
	    -I$(PJG_DIR) $(INC_KANZI) -I$(MINGW_DEPS)/include \
	    -o $(WIN_FULL_TARGET) $(SRC) packjpg_win.o aricoder_win.o bitops_win.o \
	    -L$(MINGW_DEPS)/lib -lzstd -llzma -lz $(MINGW_KANZI) $(PREFLATE_LIB_WIN) -lws2_32 $(PREFLATE_RT_WIN)

clean:
	rm -f $(TARGET) $(TARGET)_asan $(WIN_TARGET) $(WIN_FULL_TARGET) *.o
