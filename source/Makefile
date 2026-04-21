CXX      ?= g++
CXXFLAGS  = -std=c++17 -O3 -Wall -Wextra \
            -funroll-loops -fomit-frame-pointer \
            -march=native
LIBS      = -lz -llzma

TARGET    = packPNG
SRC       = packpng.cpp

.PHONY: all clean asan

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

asan: $(SRC)
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -g -o $(TARGET)_asan $^ $(LIBS)

clean:
	rm -f $(TARGET) $(TARGET)_asan *.o
