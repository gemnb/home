#include "PacketParser.h"
#include "ABProtectSDK.h"
#include <sstream>
#include <iomanip>
#include <chrono>
#include <ctime>
#include <Windows.h>

// UTF8转GBK
std::string PacketParser::UTF8ToGBK(const std::string& utf8) {
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, NULL, 0);
    if (len == 0) return "";
    std::vector<wchar_t> wstr(len);
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wstr.data(), len);
    len = WideCharToMultiByte(CP_ACP, 0, wstr.data(), -1, NULL, 0, NULL, NULL);
    if (len == 0) return "";
    std::vector<char> str(len);
    WideCharToMultiByte(CP_ACP, 0, wstr.data(), -1, str.data(), len, NULL, NULL);
    return std::string(str.data());
}

// ===== 获取处理类型描述 =====
std::string PacketParser::GetProcessTypeDesc(PacketProcessType type) {
    switch (type) {
    case PACKET_NORMAL:     return "原包";
    case PACKET_FRAGMENTED: return "分包处理";
    case PACKET_MULTI:      return "粘包分离";
    case PACKET_BOTH:       return "分包+粘包";
    default:                return "未知";
    }
}

uint32_t PacketParser::ParseHeader(const std::vector<uint8_t>& data) {
    if (data.size() < 5) {
        return 0;
    }

    // 包头格式: [01命令][00 00保留][总长度2字节]
    uint32_t totalLength = (static_cast<uint32_t>(data[3]) << 8) |
        (static_cast<uint32_t>(data[4]));

    return totalLength;  // 返回包含包头的完整包长度
}


int PacketParser::FindPattern(const std::vector<uint8_t>& data,
    const std::vector<uint8_t>& pattern) {
    if (pattern.empty() || data.size() < pattern.size()) {
        return -1;
    }

    for (size_t i = 0; i <= data.size() - pattern.size(); ++i) {
        bool found = true;
        for (size_t j = 0; j < pattern.size(); ++j) {
            if (data[i + j] != pattern[j]) {
                found = false;
                break;
            }
        }
        if (found) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::string PacketParser::ExtractGameID(const std::vector<uint8_t>& data) {
    ABPROTECT_CFF_BEGIN;
    ABPROTECT_CHECK_INTEGRITY;
    // 查找 01 0a 00 23 特征码
    std::vector<uint8_t> pattern = { 0x01, 0x0a, 0x00, 0x23 };
    int pos = FindPattern(data, pattern);

    if (pos == -1) {
        return "";
    }

    // 从01前一位开始算第15位是游戏ID开头
    int gameIDStart = pos + 18;

    if (gameIDStart >= data.size()) {
        return "";
    }

    // 查找00结尾
    std::vector<uint8_t> gameIDBytes;
    for (int i = gameIDStart; i < data.size(); ++i) {
        if (data[i] == 0x00) {
            break;  // 不记录00
        }
        gameIDBytes.push_back(data[i]);
    }

    if (gameIDBytes.empty()) {
        return "";
    }

    // 转换为字符串（假设是ASCII）
    ABPROTECT_CFF_END;
    return std::string(gameIDBytes.begin(), gameIDBytes.end());
}

// ===== 修改：Extract_01_0a_00_23 =====
std::vector<uint8_t> PacketParser::Extract_01_0a_00_23(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> pattern = { 0x01, 0x0a, 0x00, 0x23 };
    int pos = FindPattern(data, pattern);

    if (pos == -1) {
        return {};
    }

    // 从01的前一位开始往前采集8字节
    int startPos = pos - 8;
    if (startPos < 0) {
        return {};
    }

    // 采集8字节 + 4字节特征码 = 12字节
    int endPos = pos + 4;
    if (endPos > data.size()) {
        return {};
    }

    return std::vector<uint8_t>(data.begin() + startPos, data.begin() + endPos);
}

// ===== 修改：Extract_01_0a_00_09 =====
std::vector<uint8_t> PacketParser::Extract_01_0a_00_09(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> pattern = { 0x01, 0x0a, 0x00, 0x09 };
    int pos = FindPattern(data, pattern);

    if (pos == -1) {
        return {};
    }

    // 从01的前一位开始往前采集8字节，然后到包尾
    int startPos = pos - 8;
    if (startPos < 0) {
        return {};
    }

    // 采集到数据包结尾
    return std::vector<uint8_t>(data.begin() + startPos, data.end());
}

// ===== 新增：Extract_01_0a_00_62 =====
std::vector<uint8_t> PacketParser::Extract_01_0a_00_62(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> pattern = { 0x01, 0x0a, 0x00, 0x62 };
    int pos = FindPattern(data, pattern);

    if (pos == -1) {
        return {};
    }

    // 从01的前一位开始往前采集8字节，然后到包尾（与09特征相同的采集方式）
    int startPos = pos - 8;
    if (startPos < 0) {
        return {};
    }

    // 采集到数据包结尾
    return std::vector<uint8_t>(data.begin() + startPos, data.end());
}

// ===== 新增：Extract_00_00_0a_92 =====
// 以 00 00 0A 92 为索引，往后偏移4位取4字节
std::vector<uint8_t> PacketParser::Extract_00_00_0a_92(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> pattern = { 0x00, 0x00, 0x0a, 0x92 };
    int pos = FindPattern(data, pattern);

    if (pos == -1) {
        return {};
    }

    // 往后偏移4位（跳过 00 00 0A 92 本身），取4字节
    int startPos = pos + 4;
    if (startPos + 4 > static_cast<int>(data.size())) {
        return {};
    }

    // 提取4字节数据
    return std::vector<uint8_t>(data.begin() + startPos, data.begin() + startPos + 4);
}

// ===== 新增：Find_16_86 =====
// 查找 16 86 特征的位置
int PacketParser::Find_16_86(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> pattern = { 0x16, 0x86 };
    return FindPattern(data, pattern);
}

// ===== 新增：Find_09_52 =====
// 查找 09 52 特征的位置
int PacketParser::Find_09_52(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> pattern = { 0x09, 0x52 };
    return FindPattern(data, pattern);
}

// ===== 新增：Extract_After_16_86 =====
// 提取 16 86 之后的所有数据（不包含 16 86 本身）
std::vector<uint8_t> PacketParser::Extract_After_16_86(const std::vector<uint8_t>& data) {
    int pos = Find_16_86(data);

    if (pos == -1) {
        return {};
    }

    // 从 16 86 之后开始（即 pos + 2）
    int startPos = pos + 2;
    if (startPos >= static_cast<int>(data.size())) {
        return {};
    }

    return std::vector<uint8_t>(data.begin() + startPos, data.end());
}

PacketInfo PacketParser::ParsePacket(const std::vector<uint8_t>& data) {
    PacketInfo info;
    info.rawData = data;
    info.dataLength = ParseHeader(data);
    info.gameID = ExtractGameID(data);
    info.pattern_01_0a_00_23 = Extract_01_0a_00_23(data);
    info.pattern_01_0a_00_09 = Extract_01_0a_00_09(data);
    info.pattern_01_0a_00_62 = Extract_01_0a_00_62(data);  // 新增：提取62特征
    info.pattern_00_00_0a_92 = Extract_00_00_0a_92(data);  // 新增：提取0A92特征

    // 新增：提取 16 86 和 09 52 相关数据
    info.pos_16_86 = Find_16_86(data);
    info.pos_09_52 = Find_09_52(data);
    info.pattern_after_16_86 = Extract_After_16_86(data);

    info.isComplete = (data.size() >= info.dataLength);

    // ===== 新增：检测包类型 =====
    info.packetType = DetectPacketType(data);

    // ===== 新增：计算游戏ID长度 =====
    info.gameIDLength = static_cast<int>(info.gameID.length());

    // 生成时间戳
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::stringstream ss;
    struct tm timeinfo;
    localtime_s(&timeinfo, &time);
    ss << std::put_time(&timeinfo, "%H:%M:%S");
    ss << '.' << std::setfill('0') << std::setw(3) << ms.count();
    info.timestamp = ss.str();

    return info;
}



std::string PacketParser::FormatHex(const std::vector<uint8_t>& data, int bytesPerLine) {
    std::stringstream ss;
    ss << std::hex << std::uppercase << std::setfill('0');

    for (size_t i = 0; i < data.size(); ++i) {
        ss << std::setw(2) << static_cast<int>(data[i]);

        if ((i + 1) % bytesPerLine == 0) {
            ss << "\n";
        }
        else {
            ss << " ";
        }
    }

    return ss.str();
}


// ===== 检测包类型（支持0x00-0xFF）=====
int PacketParser::DetectPacketType(const std::vector<uint8_t>& data, bool disableHeaderFilter) {
    if (data.size() < 5) {
        return -1;  // ✅ 修复：-1 表示数据不足，无法识别
    }

    uint8_t type = data[3];
    return static_cast<int>(type);  // 返回 0-255，包括 0x00
}




// ===== 获取包类型描述 =====
std::string PacketParser::GetPacketTypeDesc(int type) {
    if (type >= 0 && type <= 255) {
        char buffer[16];
        sprintf_s(buffer, "0x%02X包", type);
        return std::string(buffer);
    }
    return "未识别";
}





