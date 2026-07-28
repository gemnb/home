#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <set>

enum PacketProcessType {
    PACKET_NORMAL = 0,
    PACKET_FRAGMENTED = 1,
    PACKET_MULTI = 2,
    PACKET_BOTH = 3
};

struct PacketInfo {
    std::vector<uint8_t> rawData;
    std::string gameID;
    std::string socksUsername;  // 🔥 新增：当前连接的SOCKS5账号用户名（用于按账号查找替换数据）
    std::vector<uint8_t> pattern_01_0a_00_23;
    std::vector<uint8_t> pattern_01_0a_00_09;
    std::vector<uint8_t> pattern_01_0a_00_62;  // 新增：62特征
    std::vector<uint8_t> pattern_00_00_0a_92;  // 新增：0A92特征（4字节）
    std::vector<uint8_t> pattern_after_16_86;  // 新增：16 86之后的所有数据
    int pos_16_86;    // 新增：16 86的位置（-1表示不存在）
    int pos_09_52;    // 新增：09 52的位置（-1表示不存在）
    uint32_t dataLength;
    std::string timestamp;
    bool isComplete;
    PacketProcessType processType;
    int fragmentCount;

    // ===== 新增字段 =====
    int packetType;      // 包类型
    int gameIDLength;    // 游戏ID长度

    // 🔥 用户自定义滤镜模式：用户启用的滤镜ID列表
    std::set<int> userEnabledFilters;

    PacketInfo() : dataLength(0), isComplete(false),
        processType(PACKET_NORMAL), fragmentCount(0),
        packetType(-1), gameIDLength(0), pos_16_86(-1), pos_09_52(-1) {}
};


class PacketParser {
public:
    static uint32_t ParseHeader(const std::vector<uint8_t>& data);
    static int FindPattern(const std::vector<uint8_t>& data,
        const std::vector<uint8_t>& pattern);
    static std::string ExtractGameID(const std::vector<uint8_t>& data);
    static std::vector<uint8_t> Extract_01_0a_00_23(const std::vector<uint8_t>& data);
    static std::vector<uint8_t> Extract_01_0a_00_09(const std::vector<uint8_t>& data);
    static std::vector<uint8_t> Extract_01_0a_00_62(const std::vector<uint8_t>& data);  // 新增：提取62特征
    static std::vector<uint8_t> Extract_00_00_0a_92(const std::vector<uint8_t>& data);  // 新增：提取0A92特征

    // 新增：16 86 和 09 52 特征相关函数
    static int Find_16_86(const std::vector<uint8_t>& data);  // 查找16 86的位置
    static int Find_09_52(const std::vector<uint8_t>& data);  // 查找09 52的位置
    static std::vector<uint8_t> Extract_After_16_86(const std::vector<uint8_t>& data);  // 提取16 86之后的所有数据

    static PacketInfo ParsePacket(const std::vector<uint8_t>& data);
    static std::string FormatHex(const std::vector<uint8_t>& data, int bytesPerLine = 16);
    static std::string UTF8ToGBK(const std::string& utf8);
    static std::string GetProcessTypeDesc(PacketProcessType type);


    static int DetectPacketType(const std::vector<uint8_t>& data, bool disableHeaderFilter = false);
    static std::string GetPacketTypeDesc(int type);
};
