// BinFormat.h
#pragma once
#include <cstdint>

#pragma pack(push,1)

// -------- HIDX: pointer -> (raw_off, raw_len) --------
struct HidxHeader {
    uint32_t magic;       // 'HIDX' 0x58444948
    uint32_t version;     // 1
    uint32_t chunk_size;  // 对 .bin/.jidx：原文块大小；对 .json.gz：填 0
    uint32_t reserved;    // 对齐/保留
    uint64_t rec_count;   // 记录条数
};

struct HidxRec {
    uint64_t raw_off;     // 未压缩原文偏移
    uint32_t raw_len;     // 未压缩原文长度
    uint32_t reserved;    // 对齐
};

// -------- JIDX: .bin 块级索引（压缩偏移/长度 + 原文偏移/长度）--------
struct JidxRec {
    uint64_t bin_off;     // 压缩块在 .bin 中的起始偏移
    uint64_t bin_len;     // 压缩块在 .bin 中的长度
    uint64_t raw_off;     // 该块对应的原文起始偏移
    uint64_t raw_len;     // 该块对应的原文长度
};

// -------- PIHF: pointer 哈希 -> hidx rec_id （256 桶）--------
struct PihfHeader {
    uint32_t magic;       // 'PIHF' 0x46484950
    uint32_t version;     // 1
    uint32_t bucket_cnt;  // 固定 256
    uint32_t reserved;    // 对齐
};

struct PihfBucket {
    uint64_t offset;      // 本桶 entries 文件偏移
    uint32_t count;       // 本桶 entries 数量
    uint32_t reserved;    // 对齐
};

struct PihfEntry {
    uint64_t hash;        // FNV-1a 64
    uint32_t rec_id;      // 指向 .hidx 的记录号
    uint16_t fp;          // 16-bit 指纹
    uint16_t reserved;    // 对齐（整条 16B）
};

#pragma pack(pop)

static inline uint32_t MAGIC_HIDX(){ return 0x58444948u; } // 'HIDX'
static inline uint32_t MAGIC_PIHF(){ return 0x46484950u; } // 'PIHF'
