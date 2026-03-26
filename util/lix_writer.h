// lix_writer.h  —  生成 .lix 索引（排序数组 + 二分）
// 小端格式：Header 12B = magic 'LIX1'(0x3149584C) + n_items(uint32) + entry_size(uint32=24)
// Entries[n]：每条 24B = hash64(8) | off(8) | len(4) | major(1) | pad(3)

#pragma once
#include <cstdint>
#include <cstdio>
#include <vector>
#include <string>
#include <algorithm>
#include <stdexcept>

struct LixEntryRaw {
    uint64_t h;     // hash64(pointer)
    uint64_t off;   // offset in .bin
    uint32_t len;   // length in .bin
    uint8_t  major; // CBOR major type (0..7)
    uint8_t  pad[3]{};
};
static_assert(sizeof(LixEntryRaw) == 24, "LixEntryRaw must be 24 bytes");

// 简单 FNV-1a 64bit（两端一致即可；若换 xxhash64/wyhash，改这里与 Python 对应实现）
inline uint64_t fnv1a64(const char* s, size_t n) {
    const uint64_t P = 1469598103934665603ull; // offset basis
    const uint64_t M = 1099511628211ull;       // FNV prime
    uint64_t h = P;
    for (size_t i = 0; i < n; ++i) { h ^= (unsigned char)s[i]; h *= M; }
    return h;
}
inline uint64_t fnv1a64(const std::string& s) { return fnv1a64(s.data(), s.size()); }

class LixBuilder {
public:
    void add(const std::string& pointer, uint64_t off, uint32_t len, uint8_t major) {
        LixEntryRaw e{};
        e.h = fnv1a64(pointer);
        e.off = off; e.len = len; e.major = major;
        entries_.push_back(e);
    }
    void write(const std::string& lix_path) {
        std::sort(entries_.begin(), entries_.end(),
                  [](const LixEntryRaw& a, const LixEntryRaw& b){ return a.h < b.h; });
        dedup_keep_last();  // 同 hash 取最后一条（同 pointer 多次采集或极小概率碰撞）
        FILE* f = std::fopen(lix_path.c_str(), "wb");
        if (!f) throw std::runtime_error("open lix failed: " + lix_path);
        uint32_t magic = 0x3149584Cu, n_items = (uint32_t)entries_.size(), esz = 24u;
        std::fwrite(&magic,   1, 4, f);
        std::fwrite(&n_items, 1, 4, f);
        std::fwrite(&esz,     1, 4, f);
        if (!entries_.empty()) {
            size_t n = std::fwrite(entries_.data(), sizeof(LixEntryRaw), entries_.size(), f);
            if (n != entries_.size()) { std::fclose(f); throw std::runtime_error("write lix entries failed"); }
        }
        std::fclose(f);
    }
    void clear() { entries_.clear(); }
    size_t size() const { return entries_.size(); }

private:
    std::vector<LixEntryRaw> entries_;
    void dedup_keep_last() {
        if (entries_.empty()) return;
        std::vector<LixEntryRaw> out; out.reserve(entries_.size());
        for (size_t i=0;i<entries_.size();) {
            size_t j=i+1; while (j<entries_.size() && entries_[j].h==entries_[i].h) ++j;
            out.push_back(entries_[j-1]); i=j;
        }
        entries_.swap(out);
    }
};
