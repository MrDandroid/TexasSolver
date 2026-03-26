#pragma once
#include <string>
#include <cstdint>
#include <zlib.h>
#include <stdexcept>

class GzRawWriter {
public:
    explicit GzRawWriter(const std::string& path, int level = 6) {
        std::string mode = "wb";
        mode.push_back(char('0'+level));
        gf_ = gzopen(path.c_str(), mode.c_str());
        if (!gf_) throw std::runtime_error("gzopen failed: " + path);
    }
    ~GzRawWriter(){ if (gf_) gzclose(gf_); }

    void write(const void* p, size_t n){
        if (n==0) return;
        int wrote = gzwrite(gf_, p, (unsigned int)n);
        if (wrote == 0) throw std::runtime_error("gzwrite failed");
        off_ += (uint64_t)n; // 未压缩写入字节数
    }
    void write(const std::string& s){ write(s.data(), s.size()); }
    void put(char c){ write(&c, 1); }
    uint64_t tell_uncompressed() const { return off_; }
    // 新增 flush 方法
    void flush() {
        if (gzflush(gf_, Z_SYNC_FLUSH) != Z_OK) {
            throw std::runtime_error("gzflush failed");
        }
    }
    void close(){
        if (gf_) {
            if (gzclose(gf_) != Z_OK) throw std::runtime_error("gzclose failed");
            gf_ = nullptr;
        }
    }
private:
    gzFile   gf_{nullptr};
    uint64_t off_{0}; // 未压缩字节偏移
};
