/*
 * CustomEncoding - 自定义编码层
 * 在AES加密基础上增加混淆，提高破解难度
 */

#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace CustomEncoding {

// ========== 自定义Base64（打乱字符表）==========
// 标准Base64: ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/
// 自定义表：打乱顺序，增加识别难度
const char CUSTOM_BASE64_TABLE[] = 
    "3GHIJKLMNOPQRSTUb0FWXYZaBCDEVcdefghijklmnopq9rstuvwxyz1245678A+/";

std::string CustomBase64Encode(const std::vector<uint8_t>& data);
std::vector<uint8_t> CustomBase64Decode(const std::string& encoded);

// ========== 字节序混淆 ==========
// 对加密后的数据进行字节重排，破坏模式识别
void ShuffleBytes(std::vector<uint8_t>& data, uint32_t seed);
void UnshuffleBytes(std::vector<uint8_t>& data, uint32_t seed);

// ========== 异或混淆层 ==========
// 在AES加密前后增加一层简单异或，密钥从时间戳派生
void XorObfuscate(std::vector<uint8_t>& data, uint64_t timestamp);

// ========== 完整编码/解码 ==========
// 流程：明文 → AES加密 → 字节混淆 → XOR混淆 → 自定义Base64
std::string EncodeSecure(const std::string& plaintext, 
                         const uint8_t* aesKey, 
                         const uint8_t* aesIv,
                         uint64_t timestamp);

// 流程：自定义Base64 → XOR解混淆 → 字节解混淆 → AES解密 → 明文
std::string DecodeSecure(const std::string& encoded, 
                         const uint8_t* aesKey, 
                         const uint8_t* aesIv,
                         uint64_t timestamp);

} // namespace CustomEncoding
