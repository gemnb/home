#include "CustomEncoding.h"
#include "TinyAES.h"
#include "ABProtectSDK.h"
#include <algorithm>
#include <sstream>

namespace CustomEncoding {

// ========== 自定义Base64 ==========

std::string CustomBase64Encode(const std::vector<uint8_t>& data) {
    std::string result;
    result.reserve(((data.size() + 2) / 3) * 4);

    for (size_t i = 0; i < data.size(); i += 3) {
        uint32_t val = (uint32_t)data[i] << 16;
        if (i + 1 < data.size()) val |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < data.size()) val |= (uint32_t)data[i + 2];

        result += CUSTOM_BASE64_TABLE[(val >> 18) & 0x3F];
        result += CUSTOM_BASE64_TABLE[(val >> 12) & 0x3F];
        result += (i + 1 < data.size()) ? CUSTOM_BASE64_TABLE[(val >> 6) & 0x3F] : '=';
        result += (i + 2 < data.size()) ? CUSTOM_BASE64_TABLE[val & 0x3F] : '=';
    }
    return result;
}

std::vector<uint8_t> CustomBase64Decode(const std::string& encoded) {
    // 构建反向查找表
    static uint8_t reverseTable[256] = { 0 };
    static bool initialized = false;
    if (!initialized) {
        for (int i = 0; i < 64; i++) {
            reverseTable[(uint8_t)CUSTOM_BASE64_TABLE[i]] = i;
        }
        initialized = true;
    }

    std::vector<uint8_t> result;
    result.reserve((encoded.size() / 4) * 3);

    for (size_t i = 0; i < encoded.size(); i += 4) {
        if (i + 3 >= encoded.size()) break;

        uint32_t val = 0;
        val |= (uint32_t)reverseTable[(uint8_t)encoded[i]] << 18;
        val |= (uint32_t)reverseTable[(uint8_t)encoded[i + 1]] << 12;
        if (encoded[i + 2] != '=') val |= (uint32_t)reverseTable[(uint8_t)encoded[i + 2]] << 6;
        if (encoded[i + 3] != '=') val |= (uint32_t)reverseTable[(uint8_t)encoded[i + 3]];

        result.push_back((val >> 16) & 0xFF);
        if (encoded[i + 2] != '=') result.push_back((val >> 8) & 0xFF);
        if (encoded[i + 3] != '=') result.push_back(val & 0xFF);
    }
    return result;
}

// ========== 字节序混淆 ==========

void ShuffleBytes(std::vector<uint8_t>& data, uint32_t seed) {
    if (data.size() < 2) return;

    // 使用seed生成伪随机序列
    uint32_t state = seed;
    for (size_t i = data.size() - 1; i > 0; i--) {
        // 简单的LCG随机数生成器
        state = state * 1103515245 + 12345;
        size_t j = state % (i + 1);
        std::swap(data[i], data[j]);
    }
}

void UnshuffleBytes(std::vector<uint8_t>& data, uint32_t seed) {
    if (data.size() < 2) return;

    // 生成相同的随机序列，但逆向应用
    std::vector<size_t> indices;
    indices.reserve(data.size());
    
    uint32_t state = seed;
    for (size_t i = data.size() - 1; i > 0; i--) {
        state = state * 1103515245 + 12345;
        indices.push_back(state % (i + 1));
    }

    // 逆向交换
    for (size_t i = 1; i < data.size(); i++) {
        size_t j = indices[data.size() - 1 - i];
        std::swap(data[i], data[j]);
    }
}

// ========== 异或混淆 ==========

void XorObfuscate(std::vector<uint8_t>& data, uint64_t timestamp) {
    // 从时间戳派生8字节密钥
    uint8_t xorKey[8];
    for (int i = 0; i < 8; i++) {
        xorKey[i] = (uint8_t)((timestamp >> (i * 8)) & 0xFF);
    }

    // 循环异或
    for (size_t i = 0; i < data.size(); i++) {
        data[i] ^= xorKey[i % 8];
    }
}

// ========== 完整编码/解码 ==========

std::string EncodeSecure(const std::string& plaintext,
                         const uint8_t* aesKey,
                         const uint8_t* aesIv,
                         uint64_t timestamp) {
    ABPROTECT_SUBLEQ_BEGIN;
    // 1. 准备数据（PKCS7填充）
    std::vector<uint8_t> data(plaintext.begin(), plaintext.end());
    size_t paddedLen = ((data.size() + 15) / 16) * 16 + 16;  // 预留填充空间
    data.resize(paddedLen);
    
    size_t finalLen = TinyAES::PKCS7_Pad(data.data(), plaintext.size(), paddedLen);
    if (finalLen == 0) return "";  // 填充失败
    data.resize(finalLen);

    // 2. AES-256-CBC加密
    TinyAES::AES_ctx ctx;
    TinyAES::AES_init_ctx_iv(&ctx, aesKey, aesIv);
    TinyAES::AES_CBC_encrypt_buffer(&ctx, data.data(), data.size());

    // 3. 字节混淆（使用时间戳低32位作为seed）
    ShuffleBytes(data, (uint32_t)(timestamp & 0xFFFFFFFF));

    // 4. XOR混淆
    XorObfuscate(data, timestamp);

    // 5. 自定义Base64编码
    ABPROTECT_SUBLEQ_END;
    return CustomBase64Encode(data);
}

std::string DecodeSecure(const std::string& encoded,
                         const uint8_t* aesKey,
                         const uint8_t* aesIv,
                         uint64_t timestamp) {
    ABPROTECT_SUBLEQ_BEGIN;
    // 1. 自定义Base64解码
    std::vector<uint8_t> data = CustomBase64Decode(encoded);
    if (data.empty()) return "";

    // 2. XOR解混淆
    XorObfuscate(data, timestamp);  // XOR是对称的

    // 3. 字节解混淆
    UnshuffleBytes(data, (uint32_t)(timestamp & 0xFFFFFFFF));

    // 4. AES-256-CBC解密
    TinyAES::AES_ctx ctx;
    TinyAES::AES_init_ctx_iv(&ctx, aesKey, aesIv);
    TinyAES::AES_CBC_decrypt_buffer(&ctx, data.data(), data.size());

    // 5. 去填充
    size_t plainLen = TinyAES::PKCS7_Unpad(data.data(), data.size());
    if (plainLen == 0) return "";  // 去填充失败

    ABPROTECT_SUBLEQ_END;
    return std::string(data.begin(), data.begin() + plainLen);
}

} // namespace CustomEncoding
