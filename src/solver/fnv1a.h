#pragma once
#include <cstdint>
#include <cstddef>

struct Fnv1a {
    static constexpr uint64_t kOffset = 1469598103934665603ull;
    static constexpr uint64_t kPrime  = 1099511628211ull;

    static inline uint64_t update_byte(uint64_t h, uint8_t b){
        h ^= b; h *= kPrime; return h;
    }
    static inline uint64_t update_bytes(uint64_t h, const void* p, size_t n){
        const uint8_t* s = (const uint8_t*)p;
        for (size_t i=0;i<n;++i) h = update_byte(h, s[i]);
        return h;
    }
};
