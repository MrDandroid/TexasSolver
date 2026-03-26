// src/solver/Leb128.h
#pragma once
#include <cstdint>
#include <vector>

inline void put_u64_leb128(std::vector<uint8_t>& out, uint64_t v){
    while (v >= 0x80){ out.push_back(uint8_t((v & 0x7Fu) | 0x80u)); v >>= 7; }
    out.push_back(uint8_t(v));
}
inline void put_u32_leb128(std::vector<uint8_t>& out, uint32_t v){
    while (v >= 0x80){ out.push_back(uint8_t((v & 0x7u) | 0x80u)); v >>= 7; }
    out.push_back(uint8_t(v));
}
// SLEB64（v2 关键：do 用这个）
inline void put_i64_leb128(std::vector<uint8_t>& out, int64_t v){
    bool more = true;
    while (more){
        uint8_t byte = uint8_t(v & 0x7F);
        v >>= 7;
        bool sign = (byte & 0x40) != 0;
        bool done = ((v == 0) && !sign) || ((v == -1) && sign);
        if (!done) byte |= 0x80;
        out.push_back(byte);
        more = !done;
    }
}
