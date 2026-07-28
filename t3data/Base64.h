#include <iostream>
#include <string>

class Base64 {
private:
    static const int BASE = 3;
    static const int EXPAND = 4;
    static const int SIGN = -128;
    static const char END = '=';

public:
    static std::string encrypt(const std::string& plaintext, const std::string& str) {
        std::string result;
        const char* data = plaintext.c_str();
        int len = plaintext.length();
        int overBytes = len % BASE;
        int lenBytes = len / BASE;

        int encodeBytesLen = (overBytes == 0) ? lenBytes * EXPAND : (lenBytes + 1) * EXPAND;
        result.resize(encodeBytesLen);

        int step = 0;
        int encodeIndex = 0;
        char b1, b2, b3, m0, m1;

        for (int i = 0; i < lenBytes; i++) {
            step = i * BASE;
            b1 = data[step];
            b2 = data[step + 1];
            b3 = data[step + 2];

            m0 = static_cast<char>(b1 & 0x03);
            m1 = static_cast<char>(b2 & 0x0f);

            char b1Index = static_cast<char>((b1 & SIGN) == 0 ? b1 >> 2 : (b1 >> 2) ^ 0xc0);
            char b2Index = static_cast<char>((b2 & SIGN) == 0 ? b2 >> 4 : (b2 >> 4) ^ 0xf0);
            char b3Index = static_cast<char>((b3 & SIGN) == 0 ? b3 >> 6 : (b3 >> 6) ^ 0xfc);

            result[encodeIndex] = str[b1Index];
            result[encodeIndex + 1] = str[b2Index | (m0 << 4)];
            result[encodeIndex + 2] = str[b3Index | (m1 << 2)];
            result[encodeIndex + 3] = str[b3 & 0x3f];

            encodeIndex += 4;
        }

        if (overBytes == 1) {
            b1 = data[len - 1];
            char b1Index = static_cast<char>((b1 & SIGN) == 0 ? b1 >> 2 : (b1 >> 2) ^ 0xc0);
            m0 = static_cast<char>(b1 & 0x03);
            result[lenBytes * EXPAND] = str[b1Index];
            result[lenBytes * EXPAND + 1] = str[m0 << 4];
            result[lenBytes * EXPAND + 2] = END;
            result[lenBytes * EXPAND + 3] = END;
        } else if (overBytes == 2) {
            b1 = data[len - 2];
            b2 = data[len - 1];
            char b1Index = static_cast<char>((b1 & SIGN) == 0 ? b1 >> 2 : (b1 >> 2) ^ 0xc0);
            char b2Index = static_cast<char>((b2 & SIGN) == 0 ? b2 >> 4 : (b2 >> 4) ^ 0xf0);
            m0 = static_cast<char>(b1 & 0x03);
            m1 = static_cast<char>(b2 & 0x0f);

            result[lenBytes * EXPAND] = str[b1Index];
            result[lenBytes * EXPAND + 1] = str[b2Index | (m0 << 4)];
            result[lenBytes * EXPAND + 2] = str[m1 << 2];
            result[lenBytes * EXPAND + 3] = END;
        }

        return result;
    }

    static std::string decrypt(const std::string& ciphertext, const std::string& str, int charset) {
        std::string result;
        const char* data = ciphertext.c_str();

        if (ciphertext.empty()) {
            return "";
        }

        int len = 0;
        if ((len = ciphertext.length() % EXPAND) != 0) {
            throw std::invalid_argument("data is not base 64 bytes");
        }

        len = ciphertext.length() / EXPAND;
        result.resize(len * BASE);

        int byteLen = 0;
        int decodeIndex = 0;
        char b1, b2, b3, b4;

        for (int i = 0; i < len; i++) {
            decodeIndex = i * EXPAND;
            b1 = data[decodeIndex];
            b2 = data[decodeIndex + 1];
            b3 = data[decodeIndex + 2];
            b4 = data[decodeIndex + 3];

            if (b3 != END && b4 != END) {
                b1 = static_cast<char>(str.find(b1));
                b2 = static_cast<char>(str.find(b2));
                b3 = static_cast<char>(str.find(b3));
                b4 = static_cast<char>(str.find(b4));

                result[byteLen++] = static_cast<char>(b1 << 2 | b2 >> 4);
                result[byteLen++] = static_cast<char>(((b2 & 0xf) << 4) | ((b3 >> 2) & 0xf));
                result[byteLen++] = static_cast<char>(b3 << 6 | b4);
            } else if (b3 == END) {
                b1 = static_cast<char>(str.find(b1));
                b2 = static_cast<char>(str.find(b2));
                result[byteLen++] = static_cast<char>(b1 << 2 | b2 >> 4);
            } else if (b4 == END) {
                b1 = static_cast<char>(str.find(b1));
                b2 = static_cast<char>(str.find(b2));
                b3 = static_cast<char>(str.find(b3));
                result[byteLen++] = static_cast<char>(b1 << 2 | b2 >> 4);
                result[byteLen++] = static_cast<char>(((b2 & 0xf) << 4) | ((b3 >> 2) & 0xf));
            }
        }

        result.resize(byteLen);
        return result;
    }
};