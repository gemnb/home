/*
 * TinyAES - 轻量级AES-256-CBC实现
 * 基于tiny-AES-c项目，针对AB项目定制
 * 优点：无需OpenSSL依赖，代码可控，便于混淆
 */

#pragma once

#include <cstdint>
#include <cstring>

namespace TinyAES {

// AES-256密钥长度
constexpr int AES_KEYLEN = 32;  // 256位
constexpr int AES_BLOCKLEN = 16; // 128位块

// AES上下文
struct AES_ctx {
    uint8_t RoundKey[240];
    uint8_t Iv[AES_BLOCKLEN];
};

// 初始化AES上下文（CBC模式）
void AES_init_ctx_iv(struct AES_ctx* ctx, const uint8_t* key, const uint8_t* iv);

// CBC加密（输入必须是16字节的倍数）
void AES_CBC_encrypt_buffer(struct AES_ctx* ctx, uint8_t* buf, size_t length);

// CBC解密
void AES_CBC_decrypt_buffer(struct AES_ctx* ctx, uint8_t* buf, size_t length);

// PKCS7填充
size_t PKCS7_Pad(uint8_t* data, size_t dataLen, size_t maxLen);

// PKCS7去填充
size_t PKCS7_Unpad(const uint8_t* data, size_t dataLen);

} // namespace TinyAES
