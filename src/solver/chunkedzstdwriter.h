// ChunkedZstdWriter.h
#pragma once
#include <vector>
#include <fstream>
#include <algorithm>
#include <stdexcept>
#include <zstd.h>
#include "BinFormat.h"

// 负责边写原文（紧凑 JSON 文本），边按 chunk_size 压缩落盘到 .bin，
// 并把每个压缩块的 (bin_off,bin_len, raw_off,raw_len) 写到 .jidx。
class ChunkedZstdWriter {
public:
    ChunkedZstdWriter(std::ofstream& bin, std::ofstream& jidx, uint32_t chunk_size)
        : bin_(bin), jidx_(jidx), chunk_size_(chunk_size) {
        buf_.reserve(chunk_size_ * 2);
    }

    // 当前“原文”累计字节数（已刷新的 + 缓冲区里未刷新的）
    inline uint64_t tell_raw() const {
        return raw_total_off_ + static_cast<uint64_t>(buf_.size());
    }

    inline uint32_t chunk_size() const { return chunk_size_; }

    // 逐字节写
    inline void put(char c) {
        buf_.push_back(c);
        if (buf_.size() >= chunk_size_) flush_one_chunk_();
    }

    // 批量写
    inline void write(const char* s, size_t n) {
        size_t i = 0;
        while (i < n) {
            size_t room = (buf_.size() < chunk_size_) ? (chunk_size_ - buf_.size()) : 0;
            size_t take = std::min(room, n - i);
            if (take > 0) {
                buf_.insert(buf_.end(), s + i, s + i + take);
                i += take;
                if (buf_.size() >= chunk_size_) flush_one_chunk_();
            } else {
                // 当前块已满，先冲刷
                flush_one_chunk_();
            }
        }
    }

    // 冲刷尾块（不足 chunk_size_ 的最后一块）
    void finish() {
        if (buf_.empty()) return;
        const size_t raw_len = buf_.size();
        size_t bound = ZSTD_compressBound(raw_len);
        std::vector<char> tmp(bound);
        size_t comp = ZSTD_compress(tmp.data(), bound, buf_.data(), raw_len, ZSTD_CLEVEL_DEFAULT);
        if (ZSTD_isError(comp)) throw std::runtime_error("Zstd compress tail failed");

        // jidx 记录（压缩后偏移与长度 + 原文偏移与长度）
        JidxRec rec;
        rec.bin_off = bin_pos_;
        rec.bin_len = comp;
        rec.raw_off = raw_total_off_;
        rec.raw_len = raw_len;
        jidx_.write(reinterpret_cast<const char*>(&rec), sizeof(rec));

        // 写 .bin
        bin_.write(tmp.data(), comp);
        bin_pos_       += comp;
        raw_total_off_ += raw_len;

        buf_.clear();
    }

private:
    void flush_one_chunk_() {
        if (buf_.size() < chunk_size_) return;

        const size_t raw_len = chunk_size_;
        size_t bound = ZSTD_compressBound(raw_len);
        std::vector<char> tmp(bound);
        size_t comp = ZSTD_compress(tmp.data(), bound, buf_.data(), raw_len, ZSTD_CLEVEL_DEFAULT);
        if (ZSTD_isError(comp)) throw std::runtime_error("Zstd compress failed");

        // jidx
        JidxRec rec;
        rec.bin_off = bin_pos_;
        rec.bin_len = comp;
        rec.raw_off = raw_total_off_;
        rec.raw_len = raw_len;
        jidx_.write(reinterpret_cast<const char*>(&rec), sizeof(rec));

        // bin
        bin_.write(tmp.data(), comp);
        bin_pos_       += comp;
        raw_total_off_ += raw_len;

        // 去掉已写 portion
        buf_.erase(buf_.begin(), buf_.begin() + raw_len);
    }

private:
    std::ofstream& bin_;
    std::ofstream& jidx_;
    uint32_t chunk_size_;
    std::vector<char> buf_;

    uint64_t raw_total_off_ = 0; // 已完全刷新的原文字节数
    uint64_t bin_pos_       = 0; // 已写 .bin 的压缩偏移
};
