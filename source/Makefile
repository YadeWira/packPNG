CXX       ?= g++
CXXFLAGS   = -std=c++17 -O3 -Wall -Wextra \
             -funroll-loops -fomit-frame-pointer \
             -march=native
LIBS       = -lz -llzma
LIBS_LDF   = -lz -llzma -ldeflate
LIBS_ZSTD  = -lz -llzma -lzstd

# fast-lzma2 is not in apt; build from https://github.com/conor42/fast-lzma2
# and set FL2_DIR to its source dir (containing libfast-lzma2.a + fast-lzma2.h).
FL2_DIR   ?= /home/forum/git/Otros/fast-lzma2
LIBS_FL2   = -lz -llzma $(FL2_DIR)/libfast-lzma2.a -lpthread
INC_FL2    = -I$(FL2_DIR)

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

.PHONY: all ldf zstd fl2 clean asan win

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

# Build with libdeflate support (-ldf flag, faster inflate + mode-2 repack)
ldf: $(SRC)
	$(CXX) $(CXXFLAGS) -DUSE_LIBDEFLATE -o $(TARGET) $^ $(LIBS_LDF)

# Build with Zstd support (-zstd flag, solid Zstd block instead of LZMA)
zstd: $(SRC)
	$(CXX) $(CXXFLAGS) -DUSE_ZSTD -o $(TARGET) $^ $(LIBS_ZSTD)

# Build with fast-lzma2 support (-fl2 flag, ~2-4x faster encode at +1-2% size)
fl2: $(SRC)
	$(CXX) $(CXXFLAGS) -DUSE_FL2 $(INC_FL2) -o $(TARGET) $^ $(LIBS_FL2)

asan: $(SRC)
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -g -o $(TARGET)_asan $^ $(LIBS)

win: $(SRC)
	$(MINGW_CXX) $(MINGW_FLAGS) $(MINGW_INC) -o $(WIN_TARGET) $^ $(MINGW_LIBS)

clean:
	rm -f $(TARGET) $(TARGET)_asan $(WIN_TARGET) *.o
