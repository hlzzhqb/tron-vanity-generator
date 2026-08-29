# TRON Vanity Generator — MSYS2 / MinGW-w64
#
#   pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-openssl mingw-w64-x86_64-make
#   mingw32-make            # 生成 tron_vanity_generator.exe
#
# 在 “MSYS2 MinGW x64” 终端里执行。

CXX      ?= g++
CXXFLAGS ?= -O3 -std=c++17 -march=native -pthread -Wall -Wextra
LDFLAGS  ?=
LDLIBS   ?= -lssl -lcrypto

# OpenCL 只需运行期的 OpenCL.dll（由显卡驱动提供），编译期用动态加载，无需链接。

SRC := $(wildcard src/*.cpp)
OBJ := $(SRC:.cpp=.o)
BIN := tron_vanity_generator.exe

.PHONY: all clean run

all: $(BIN)

$(BIN): $(OBJ)
	$(CXX) $(CXXFLAGS) $(OBJ) -o $@ $(LDFLAGS) $(LDLIBS)

src/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

run: $(BIN)
	./$(BIN) --list

clean:
	rm -f $(OBJ) $(BIN)
