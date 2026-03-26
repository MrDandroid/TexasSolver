#pragma once
#include <iostream>
#include <vector>
#include <cstdint>
#include <string>
#include <fstream>
#include <algorithm>
#include <stdexcept>

// XIDX v2（小端）：与 Python 脚本一致
// Header:  <I H H I Q Q>  = MAGIC('XIDX'), VER(2), RSV(0), blockN, totalN, dir_off
// Block :  base_h(ULEB64), base_off(ULEB64),
//          [ dh(ULEB64), do(SLEB64), len(ULEB32 as ULEB128), fp(1B) ] * cnt
// Dir   :  <Q Q I I I I>  = h0, file_off, comp_len, raw_len, cnt, flags
// 备注 :  本实现块未压缩，comp_len == raw_len
struct XidxV2Writer {
    struct Rec { uint64_t h; uint64_t off; uint32_t len; uint8_t fp; };

    explicit XidxV2Writer(const std::string& path, uint32_t block_entries = 8192)
        : path_(path), blockN_(block_entries) {}

    inline void add(uint64_t h, uint64_t off, uint32_t len, uint8_t fp) {
        items_.push_back(Rec{h, off, len, fp});
    }

    void finish() {
        std::ofstream wf(path_, std::ios::binary | std::ios::out);
        if (!wf) throw std::runtime_error("open xidx failed: " + path_);

        // ---- Header 占位 ----
        const uint32_t MAGIC = 0x58494458; // 'XIDX'
        const uint16_t VER   = 2;
        write_u32(wf, MAGIC);
        write_u16(wf, VER);
        write_u16(wf, 0);
        write_u32(wf, blockN_);
        write_u64(wf, (uint64_t)items_.size());
        const std::streamoff DIR_POS_PATCH = (std::streamoff)(4+2+2+4+8);
        write_u64(wf, 0); // dir_off 占位

        // ---- 排序：仅按 h 升序 ----
        std::sort(items_.begin(), items_.end(),
                  [](const Rec& a, const Rec& b){ return a.h < b.h; });

        // ---- 分块写 raw ----
        struct Dir { uint64_t h0; uint64_t foff; uint32_t clen; uint32_t rlen; uint32_t cnt; uint32_t flags; };
        std::vector<Dir> dirs;
        uint64_t pos = (uint64_t)wf.tellp();
        const size_t N = items_.size();
        size_t i = 0;

        while (i < N) {
            const size_t j = std::min(N, i + (size_t)blockN_);
            const Rec& base = items_[i];

            std::vector<uint8_t> raw; raw.reserve((j - i) * 20 + 32);

            // 块头：base_h/base_off = ULEB64
            put_uleb64(raw, base.h);
            put_uleb64(raw, base.off);

            // 记录：dh / do / len / fp
            const uint64_t base_h   = base.h;
            const uint64_t base_off = base.off;
            for (size_t k = i; k < j; ++k) {
                const uint64_t h   = items_[k].h;
                const uint64_t off = items_[k].off;
                const uint32_t len = items_[k].len;
                const int64_t  dh  = (int64_t)(h - base_h);     // ≥0
                const int64_t  dof = (int64_t)(off - base_off); // 允许负

                put_uleb64(raw, (uint64_t)dh);
                put_sleb64(raw, dof);
                put_uleb32_as_uleb(raw, len);
                raw.push_back(items_[k].fp);
            }

            const uint32_t clen = (uint32_t)raw.size();
            wf.write((const char*)raw.data(), (std::streamsize)raw.size());
            dirs.push_back(Dir{ base.h, pos, clen, clen, (uint32_t)(j - i), 0 });

            pos += raw.size();
            i = j;
        }

        // ---- 写目录 ----
        const uint64_t dir_off = pos;
        for (const auto& d : dirs) {
            write_u64(wf, d.h0);
            write_u64(wf, d.foff);
            write_u32(wf, d.clen);
            write_u32(wf, d.rlen);
            write_u32(wf, d.cnt);
            write_u32(wf, d.flags);
        }
        wf.flush();

        // ---- 回填 header.dir_off ----
        auto end_pos = wf.tellp();
        wf.seekp(DIR_POS_PATCH, std::ios::beg);
        write_u64(wf, dir_off);
        wf.seekp(end_pos, std::ios::beg);
        wf.flush();
        wf.close();
        std::ifstream file(path_, std::ios::binary | std::ios::ate);
        std::cout << "[DEBUG] Final .xidx file size: " << file.tellg() << " bytes." << std::endl;
    }

private:
    // 小端写
    static void write_u16(std::ofstream& f, uint16_t v){ f.write((char*)&v, 2); }
    static void write_u32(std::ofstream& f, uint32_t v){ f.write((char*)&v, 4); }
    static void write_u64(std::ofstream& f, uint64_t v){ f.write((char*)&v, 8); }

    // ULEB128
    static void put_uleb64(std::vector<uint8_t>& b, uint64_t v){
        while (v >= 0x80) { b.push_back(uint8_t(v | 0x80)); v >>= 7; }
        b.push_back(uint8_t(v));
    }
    static void put_uleb32_as_uleb(std::vector<uint8_t>& b, uint32_t v){
        uint64_t x = v;
        while (x >= 0x80) { b.push_back(uint8_t(x | 0x80)); x >>= 7; }
        b.push_back(uint8_t(x));
    }
    // SLEB128
    static void put_sleb64(std::vector<uint8_t>& b, int64_t v){
        bool more = true;
        while (more){
            uint8_t byte = (uint8_t)(v & 0x7F);
            v >>= 7;
            bool sign_done = ((v == 0) && ((byte & 0x40) == 0)) || ((v == -1) && ((byte & 0x40) != 0));
            if (!sign_done) byte |= 0x80;
            b.push_back(byte);
            more = !sign_done;
        }
    }

private:
    std::string      path_;
    uint32_t         blockN_;
    std::vector<Rec> items_;
};
