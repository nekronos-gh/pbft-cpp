#pragma once
#include <string>
#include <memory>
#include <openssl/evp.h>

namespace pbft {

inline std::string compute_digest(const void *data, size_t len) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;
    
    // Use unique_ptr with custom deleter for automatic cleanup
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(
        EVP_MD_CTX_new(), EVP_MD_CTX_free
    );
    
    if (!ctx) {
        return "";  
    }
    
    if (EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(ctx.get(), data, len) != 1 ||
        EVP_DigestFinal_ex(ctx.get(), hash, &hash_len) != 1) {
        return "";  
    }
    
    static const char *hex = "0123456789abcdef";
    std::string out;

    out.resize(hash_len * 2);
    for (unsigned int i = 0; i < hash_len; ++i) {
        out[2 * i]     = hex[(hash[i] >> 4) & 0xF];
        out[2 * i + 1] = hex[hash[i] & 0xF];
    }
    return out;
}

// Overload for std::string.
inline std::string compute_digest(const std::string &s) {
    return compute_digest(s.data(), s.size());
}

} // namespace pbft
