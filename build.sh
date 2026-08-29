#!/usr/bin/env bash
# 在 “MSYS2 MinGW x64” 终端中运行： ./build.sh
set -e

if ! command -v g++ >/dev/null 2>&1; then
  echo "未找到 g++。请先安装工具链："
  echo "  pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-openssl mingw-w64-x86_64-make"
  exit 1
fi

if ! pkg-config --exists openssl 2>/dev/null; then
  echo "提示：未检测到 openssl 的 pkg-config，将使用默认 -lssl -lcrypto。"
  echo "  如缺失请： pacman -S mingw-w64-x86_64-openssl"
fi

CXXFLAGS="-O3 -std=c++17 -march=native -pthread -Wall -Wextra"
LDLIBS="-lssl -lcrypto"

echo "编译中..."
g++ $CXXFLAGS src/*.cpp -o tron_vanity_generator.exe $LDLIBS
echo "完成： ./tron_vanity_generator.exe"
echo
./tron_vanity_generator.exe --list
