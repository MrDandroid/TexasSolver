#pragma once
#include <string>

// 追加 JSON 字符串（含引号与转义）
inline void json_escape_append(const std::string& s, std::string& out){
    out.push_back('"');
    for (unsigned char c : s){
        switch (c){
        case '\"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (c < 0x20) {
                static const char* hex="0123456789ABCDEF";
                out += "\\u00";
                out.push_back(hex[(c>>4)&0xF]);
                out.push_back(hex[c & 0xF]);
            } else out.push_back((char)c);
        }
    }
    out.push_back('"');
}

// RFC6901 转义（~ → ~0，/ → ~1）
inline std::string rfc6901_escape(const std::string& s){
    std::string r; r.reserve(s.size()+2);
    for (char ch : s){
        if (ch=='~') r += "~0";
        else if (ch=='/') r += "~1";
        else r.push_back(ch);
    }
    return r;
}
