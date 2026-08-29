#pragma once
#include <cstddef>

// 用操作系统 CSPRNG 填充 buf；失败返回 false。
bool randBytes(unsigned char* buf, size_t len);
