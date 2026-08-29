#pragma once
#include <cstdlib>
#include <cstring>
#include <string>

#include "crypto.h"

struct MatchResult {
    bool matched = false;
    const char* kind = "";   // "相同" / "连续"
    int runLen = 0;
    std::string tail;
};

inline int base58Pos(char c) {
    const char* p = std::strchr(kBase58, c);
    return p ? static_cast<int>(p - kBase58) : -1;
}

// 结尾相同字符的连续长度
inline int trailingRepeatLen(const std::string& a) {
    if (a.empty()) return 0;
    int n = static_cast<int>(a.size());
    int len = 1;
    for (int i = n - 2; i >= 0 && a[i] == a[n - 1]; --i) ++len;
    return len;
}

// 结尾按 base58 表递增/递减的连续号码长度 (如 12345 / 54321)
inline int trailingSequenceLen(const std::string& a) {
    int n = static_cast<int>(a.size());
    if (n < 2) return n;
    int p1 = base58Pos(a[n - 1]);
    int p0 = base58Pos(a[n - 2]);
    if (p0 < 0 || p1 < 0 || std::abs(p1 - p0) != 1) return 1;
    int step = p1 - p0;
    int len = 2;
    for (int i = n - 3; i >= 0; --i) {
        int cur = base58Pos(a[i]);
        int nxt = base58Pos(a[i + 1]);
        if (cur < 0 || nxt < 0 || nxt - cur != step) break;
        ++len;
    }
    return len;
}

inline MatchResult evaluateAddress(const std::string& addr, int minLen) {
    MatchResult r;
    int rep = trailingRepeatLen(addr);
    int seq = trailingSequenceLen(addr);
    if (rep >= minLen) {
        r.matched = true; r.kind = "相同"; r.runLen = rep;
        r.tail = addr.substr(addr.size() - rep);
    }
    if (seq >= minLen && seq > r.runLen) {
        r.matched = true; r.kind = "连续"; r.runLen = seq;
        r.tail = addr.substr(addr.size() - seq);
    }
    return r;
}
