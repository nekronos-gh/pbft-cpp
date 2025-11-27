#pragma once
#include "salticidae/stream.h"
#include <vector>
#include <set>
#include <string>

using salticidae::DataStream;

// Deserialize strings
inline DataStream& operator>>(DataStream &s, std::string &str) {
    uint32_t len;
    s >> len;
    const uint8_t* ptr = s.get_data_inplace(len);
    str.assign((const char*)ptr, len);
    return s;
}

// Serialize vectors
template<typename T>
inline DataStream& operator<<(DataStream &s, const std::vector<T> &v) {
    s << (uint32_t)v.size();
    for (const auto &e : v) s << e;
    return s;
}

// Deserialize vectors
template<typename T>
inline DataStream& operator>>(DataStream &s, std::vector<T> &v) {
    uint32_t size;
    s >> size;
    v.resize(size);
    for (uint32_t i = 0; i < size; i++) s >> v[i];
    return s;
}

// Serialize sets
template<typename T>
inline DataStream& operator<<(DataStream &s, const std::set<T> &v) {
    s << (uint32_t)v.size();
    for (const auto &e : v) s << e;
    return s;
}

// Deserialize sets
template<typename T>
inline DataStream& operator>>(DataStream &s, std::set<T> &v) {
    uint32_t size;
    s >> size;
    for (uint32_t i = 0; i < size; i++) {
        T tmp;
        s >> tmp;
        v.insert(tmp);
    }
    return s;
}
