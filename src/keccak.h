#pragma once
#include <cstddef>

// Keccak-256 (Ethereum/Tron 变体，pad 0x01)
void keccak256(const unsigned char* data, size_t len, unsigned char output[32]);
