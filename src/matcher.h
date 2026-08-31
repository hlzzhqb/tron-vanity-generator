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

// 字符类别：1=数字 2=小写 3=大写 0=其它
inline int charClass(char c) {
    if (c >= '0' && c <= '9') return 1;
    if (c >= 'a' && c <= 'z') return 2;
    if (c >= 'A' && c <= 'Z') return 3;
    return 0;
}

// 结尾相同字符的连续长度
inline int trailingRepeatLen(const std::string& a) {
    if (a.empty()) return 0;
    int n = static_cast<int>(a.size());
    int len = 1;
    for (int i = n - 2; i >= 0 && a[i] == a[n - 1]; --i) ++len;
    return len;
}

// 结尾「连续号码/字母」的长度：按字符本身的 ASCII 值 ±1 递增或递减，
// 且整段必须落在同一类别内（全数字 / 全小写 / 全大写）。
// 例：12345、abcde、WXYZ、54321、edcba 命中；
//     89123（跨过缺失的 0）、xyzabc（z→a 回绕）、9abc（数字跨字母）、FGHJ（跳过缺失的 I）不命中。
inline int trailingSequenceLen(const std::string& a) {
    int n = static_cast<int>(a.size());
    if (n < 2) return n;
    int cls = charClass(a[n - 1]);
    if (cls == 0 || charClass(a[n - 2]) != cls) return 1;
    int step = static_cast<int>(a[n - 1]) - static_cast<int>(a[n - 2]);
    if (step != 1 && step != -1) return 1;
    int len = 2;
    for (int i = n - 3; i >= 0; --i) {
        if (charClass(a[i]) != cls) break;
        if (static_cast<int>(a[i + 1]) - static_cast<int>(a[i]) != step) break;
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
