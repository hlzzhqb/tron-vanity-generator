#include "rng.h"

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <bcrypt.h>
#  pragma comment(lib, "bcrypt.lib")

bool randBytes(unsigned char* buf, size_t len) {
    return BCryptGenRandom(nullptr, buf, static_cast<ULONG>(len),
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;  // STATUS_SUCCESS
}
#else
#  include <fstream>
bool randBytes(unsigned char* buf, size_t len) {
    std::ifstream f("/dev/urandom", std::ios::binary);
    return f && f.read(reinterpret_cast<char*>(buf), static_cast<std::streamsize>(len));
}
#endif
