#pragma once

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <atomic>
#include <functional>
#include <cstdint>
#include "DatabaseManager.h"

// ==================== 前向声明 ====================
namespace Json {
    class Value;
}

// 注意: HeartbeatRecord 已在 DatabaseManager.h 中定义

// ==================== HBCore命名空间 ====================
// 使用命名空间避免与新伪心跳.cpp中的同名类型冲突
namespace HBCore {

// ==================== 长度字段调整记录 ====================
struct LengthFieldAdjustment {
    int position;       // 长度字段位置
    int originalValue;  // 原始值
    int newValue;       // 新值
    int fieldSize;      // 字段大小（1或2字节）
};

// ==================== Pattern23模式枚举 ====================
enum class Pattern23Mode {
    STATIC = 0,   // 静态替换：直接替换12字节
    DYNAMIC = 1   // 动态计算：根据偏移位置计算长度
};

// ==================== 替换模式枚举 ====================
enum class ReplaceMode {
    RANDOM = 0,      // 随机：从数据池随机选择
    SEQUENTIAL = 1,  // 顺序：按顺序循环选择
    FIXED = 2        // 固定：使用指定的固定包
};

// ==================== 到达后策略枚举 ====================
enum class PostLimitStrategy {
    SendOriginal = 0,           // 到达后只发原包
    CycleFakeThenOriginal = 1   // 到达后按比例循环（N个伪包+M个原包）
};

// ==================== 数据包顺序模式枚举 ====================
enum class DataOrderMode {
    Ascending = 0,      // 升序（FIFO，先进先出，取最旧的数据）
    Descending = 1      // 降序（LIFO，后进先出，取最新的数据）
};

// ==================== Pattern23偏移替换规则 ====================
struct Pattern23OffsetRule {
    int id = 0;
    std::string name;
    int offset = 0;         // 相对于23特征位置的偏移
    int length = 12;        // 替换长度
    bool enabled = true;

    bool operator==(const Pattern23OffsetRule& other) const {
        return id == other.id && name == other.name &&
               offset == other.offset && length == other.length &&
               enabled == other.enabled;
    }
};

// ==================== Pattern09偏移替换规则 ====================
struct Pattern09OffsetRule {
    int id = 0;
    std::string name;
    int offset = 0;         // 相对于09特征位置的偏移
    int length = 12;        // 替换长度
    bool enabled = true;

    bool operator==(const Pattern09OffsetRule& other) const {
        return id == other.id && name == other.name &&
               offset == other.offset && length == other.length &&
               enabled == other.enabled;
    }
};

// ==================== Pattern62偏移替换规则 ====================
struct Pattern62OffsetRule {
    int id = 0;
    std::string name;
    int offset = 0;         // 相对于62特征位置的偏移
    int length = 12;        // 替换长度
    bool enabled = true;

    bool operator==(const Pattern62OffsetRule& other) const {
        return id == other.id && name == other.name &&
               offset == other.offset && length == other.length &&
               enabled == other.enabled;
    }
};

// ==================== 伪心跳白名单规则 ====================
struct WhitelistRule {
    int id = 0;
    std::string name;
    std::string searchPattern;  // 搜索模式字符串，格式: "pos|hex,pos|hex,..."
    bool usePatternSearch = true;  // true=模式搜索, false=绝对位置匹配
    bool isEnabled = true;
    uint64_t matchCount = 0;    // 命中次数统计

    bool operator==(const WhitelistRule& other) const {
        return id == other.id && name == other.name &&
               searchPattern == other.searchPattern &&
               usePatternSearch == other.usePatternSearch &&
               isEnabled == other.isEnabled;
    }
};

// ==================== 伪心跳黑名单规则 ====================
struct BlacklistRule {
    int id = 0;
    std::string name;
    std::string searchPattern;  // 搜索模式字符串
    bool usePatternSearch = true;
    bool isEnabled = true;
    uint64_t matchCount = 0;

    bool operator==(const BlacklistRule& other) const {
        return id == other.id && name == other.name &&
               searchPattern == other.searchPattern &&
               usePatternSearch == other.usePatternSearch &&
               isEnabled == other.isEnabled;
    }
};

// ==================== 替换数量规则 ====================
struct ReplaceCountRule {
    int id = 0;
    bool enabled = true;
    std::string name;

    // 触发阈值：成功替换(发伪包)达到 N 次后进入"到达后策略"
    int replaceLimit = 1;

    PostLimitStrategy postLimitStrategy = PostLimitStrategy::SendOriginal;

    // 到达阈值时是否清理数据池中该账号/ID的数据（只执行一次）
    bool clearPoolOnReach = false;

    // 到达后"原包段"是否仍执行 WPE 滤镜
    bool applyWpeOnOriginalSegment = true;

    // 断开连接后是否重置计数
    bool resetCountOnDisconnect = true;

    // 仅当 postLimitStrategy == CycleFakeThenOriginal 生效
    int postLimitFakeN = 1;      // 循环中伪包数量
    int postLimitOriginalN = 1;  // 循环中原包数量
};

// ==================== 替换运行时状态 ====================
struct ReplaceRuntimeState {
    int ruleId = 0;
    uint64_t rulesVersion = 0;

    int replacementsDone = 0;      // 已成功替换(发伪包)次数
    bool reached = false;          // 是否已到达阈值
    bool reachCleanupDone = false; // 到达时清理是否已执行

    // post-limit cycle 状态（仅 CycleFakeThenOriginal 使用）
    bool cycleSendFake = true;
    int cycleRemaining = 0;
};

// ==================== 固定包选择信息 ====================
struct FixedPacketSelection {
    int packetId = -1;          // 选中的包ID
    std::string gameID;         // 游戏ID
    std::string displayInfo;    // 显示信息
};

// ==================== 顺序模式配置 ====================
struct SequentialConfig {
    int startId = 1;            // 起始ID
    int endId = 1000;           // 结束ID
    bool enabled = true;        // 是否启用
    int currentCount = 0;       // 当前该长度的数据量（只读，用于显示）

    SequentialConfig() : startId(1), endId(1000), enabled(true), currentCount(0) {}
    SequentialConfig(int start, int end, bool en = true) : startId(start), endId(end), enabled(en), currentCount(0) {}
};

} // namespace HBCore

// 清理数据池回调类型：参数为(gameID, username, isSameAlgorithm)
using ClearPoolCallback = std::function<void(const std::string&, const std::string&, bool)>;

// ==================== HeartbeatCore类 ====================
class HeartbeatCore {
public:
    HeartbeatCore();
    ~HeartbeatCore();

    // ===== 数据库设置 =====
    void SetDatabase(DatabaseManager* db);
    DatabaseManager* GetDatabase() const { return m_database; }

    // ===== 清理数据池回调 =====
    void SetClearPoolCallback(ClearPoolCallback callback) { m_clearPoolCallback = callback; }

    // ===== 核心替换算法 =====
    // 主替换函数：处理完整的伪心跳替换逻辑
    std::vector<uint8_t> ReplaceHeartbeatData(
        const std::vector<uint8_t>& originalData,
        std::vector<HBCore::LengthFieldAdjustment>* outAdjustments = nullptr,
        int* outOriginalIDLen = nullptr,
        int* outReplacedIDLen = nullptr,
        int* outFinalIDLen = nullptr,
        HeartbeatRecord* outPoolData = nullptr,
        const std::string& username = "",
        const std::string& gameID = ""
    );

    // ===== 特征检测 =====
    int FindPattern23(const std::vector<uint8_t>& data, int startPos = 0);
    int FindPattern09(const std::vector<uint8_t>& data, int startPos = 0);
    int FindPattern62(const std::vector<uint8_t>& data, int startPos = 0);

    // ===== 偏移替换 =====
    bool ApplyPattern23OffsetReplace(std::vector<uint8_t>& data, const HeartbeatRecord& poolData, const std::string& gameID);
    bool ApplyPattern09OffsetReplace(std::vector<uint8_t>& data, const HeartbeatRecord& poolData, const std::string& gameID);
    bool ApplyPattern62OffsetReplace(std::vector<uint8_t>& data, const HeartbeatRecord& poolData, const std::string& gameID);

    // ===== 白名单/黑名单检查 =====
    bool CheckWhitelist(const std::vector<uint8_t>& data, int* matchedIndex = nullptr, std::string* matchedRuleName = nullptr);
    bool CheckBlacklist(const std::vector<uint8_t>& data, int* matchedIndex = nullptr, std::string* matchedRuleName = nullptr);

    // ===== 替换数量规则 =====
    bool ShouldAttemptReplace(const std::string& key, const std::string& gameID, const std::string& username);
    void OnReplacementSuccess(const std::string& key, const std::string& gameID, const std::string& username);
    void OnOriginalSentAfterReach(const std::string& key);
    void ResetRuntimeStateForKey(const std::string& key);
    std::string MakeReplaceRuleKey(bool isSameAlgorithm, const std::string& gameID, const std::string& username);
    HBCore::ReplaceCountRule* GetFirstEnabledReplaceRule();

    // ===== CRC32计算 =====
    static uint32_t CalculateCRC32(const uint8_t* data, size_t length);
    void CalculateCRC32Bytes(const std::vector<uint8_t>& data, int startPos, int endPos,
                             uint8_t& crc0, uint8_t& crc1, uint8_t& crc2, uint8_t& crc3);

    // ===== 数据池获取 =====
    HeartbeatRecord GetRandomHeartbeatData();
    HeartbeatRecord GetRandomHeartbeatDataByLength(int length);
    HeartbeatRecord GetSequentialHeartbeatData(const std::string& gameID, int length, bool isStaticMode);
    HeartbeatRecord GetSingleHeartbeatPacketByLength(int length);
    HeartbeatRecord GetHeartbeatDataByMode(const std::string& gameID, int length, const std::string& username);

    // ===== Pattern23偏移规则管理 =====
    void AddPattern23OffsetRule(const HBCore::Pattern23OffsetRule& rule);
    void UpdatePattern23OffsetRule(int id, const HBCore::Pattern23OffsetRule& rule);
    void RemovePattern23OffsetRule(int id);
    void ClearPattern23OffsetRules();
    std::vector<HBCore::Pattern23OffsetRule> GetPattern23OffsetRules() const;
    int GetNextPattern23OffsetRuleId();

    // ===== Pattern09偏移规则管理 =====
    void AddPattern09OffsetRule(const HBCore::Pattern09OffsetRule& rule);
    void UpdatePattern09OffsetRule(int id, const HBCore::Pattern09OffsetRule& rule);
    void RemovePattern09OffsetRule(int id);
    void ClearPattern09OffsetRules();
    std::vector<HBCore::Pattern09OffsetRule> GetPattern09OffsetRules() const;
    int GetNextPattern09OffsetRuleId();

    // ===== Pattern62偏移规则管理 =====
    void AddPattern62OffsetRule(const HBCore::Pattern62OffsetRule& rule);
    void UpdatePattern62OffsetRule(int id, const HBCore::Pattern62OffsetRule& rule);
    void RemovePattern62OffsetRule(int id);
    void ClearPattern62OffsetRules();
    std::vector<HBCore::Pattern62OffsetRule> GetPattern62OffsetRules() const;
    int GetNextPattern62OffsetRuleId();

    // ===== 白名单规则管理 =====
    void AddWhitelistRule(const HBCore::WhitelistRule& rule);
    void UpdateWhitelistRule(int id, const HBCore::WhitelistRule& rule);
    void RemoveWhitelistRule(int id);
    void ClearWhitelistRules();
    std::vector<HBCore::WhitelistRule> GetWhitelistRules() const;
    int GetNextWhitelistRuleId();

    // ===== 黑名单规则管理 =====
    void AddBlacklistRule(const HBCore::BlacklistRule& rule);
    void UpdateBlacklistRule(int id, const HBCore::BlacklistRule& rule);
    void RemoveBlacklistRule(int id);
    void ClearBlacklistRules();
    std::vector<HBCore::BlacklistRule> GetBlacklistRules() const;
    int GetNextBlacklistRuleId();

    // ===== 替换数量规则管理 =====
    void AddReplaceCountRule(const HBCore::ReplaceCountRule& rule);
    void UpdateReplaceCountRule(int id, const HBCore::ReplaceCountRule& rule);
    void RemoveReplaceCountRule(int id);
    void ClearReplaceCountRules();
    std::vector<HBCore::ReplaceCountRule> GetReplaceCountRules() const;
    int GetNextReplaceCountRuleId();
    void BumpReplaceRulesVersionAndResetRuntime();

    // ===== 固定包选择管理 =====
    void SetFixedPacketByLength(int length, int packetId, const std::string& gameID);
    HBCore::FixedPacketSelection GetFixedPacketByLength(int length) const;
    std::map<int, HBCore::FixedPacketSelection> GetAllFixedPacketSelections() const;
    void ClearFixedPacketSelections();

    // ===== 顺序模式配置管理 =====
    void SetSequentialConfig(int length, const HBCore::SequentialConfig& config);
    HBCore::SequentialConfig GetSequentialConfig(int length) const;
    std::map<int, HBCore::SequentialConfig> GetAllSequentialConfigs() const;
    void RemoveSequentialConfig(int length);
    void ClearSequentialConfigs();
    void UpdateSequentialConfigCounts();  // 更新各长度的数据量统计

    // ===== 配置 Getter/Setter =====
    void SetPattern23Mode(HBCore::Pattern23Mode mode);
    HBCore::Pattern23Mode GetPattern23Mode() const { return m_pattern23Mode; }

    void SetReplaceMode(HBCore::ReplaceMode mode);
    HBCore::ReplaceMode GetReplaceMode() const { return m_replaceMode; }

    void SetPattern23DynamicOffset(int offset);
    int GetPattern23DynamicOffset() const { return m_pattern23DynamicOffset; }

    void SetEnable62Pattern(bool enable);
    bool GetEnable62Pattern() const { return m_enable62Pattern; }

    void SetEnableCRC32AutoCalc(bool enable);
    bool GetEnableCRC32AutoCalc() const { return m_enableCRC32AutoCalc; }

    void SetUseSingleHeartbeatPacket(bool use);
    bool GetUseSingleHeartbeatPacket() const { return m_useSingleHeartbeatPacket; }

    void SetEnableWhitelist(bool enable);
    bool GetEnableWhitelist() const { return m_enableWhitelist; }

    void SetEnableBlacklist(bool enable);
    bool GetEnableBlacklist() const { return m_enableBlacklist; }

    void SetEnable09_62Complement(bool enable);
    bool GetEnable09_62Complement() const { return m_enable09_62Complement; }

    void SetSameAlgorithmMode(bool isSameAlgorithm);
    bool GetSameAlgorithmMode() const { return m_isSameAlgorithm; }

    void SetDataOrderMode(HBCore::DataOrderMode mode);
    HBCore::DataOrderMode GetDataOrderMode() const { return m_dataOrderMode; }

    // ===== 配置持久化 =====
    bool SaveConfig(const std::string& filePath);
    bool LoadConfig(const std::string& filePath);
    Json::Value ToJson() const;
    bool FromJson(const Json::Value& json);

    // ===== 统计信息 =====
    uint64_t GetTotalProcessed() const { return m_totalProcessed.load(); }
    uint64_t GetTotalReplaced() const { return m_totalReplaced.load(); }
    uint64_t GetTotalSkipped() const { return m_totalSkipped.load(); }
    void ResetStatistics();

private:
    // ===== 内部辅助函数 =====
    bool MatchPattern(const std::vector<uint8_t>& data, const std::string& pattern, bool usePatternSearch);
    int ExtractIDLength(const std::vector<uint8_t>& data, int pattern23Pos);
    void UpdateLengthFields(std::vector<uint8_t>& data, int oldLen, int newLen, int pattern23Pos);

    // ===== 数据库 =====
    DatabaseManager* m_database = nullptr;

    // ===== 清理数据池回调 =====
    ClearPoolCallback m_clearPoolCallback;

    // ===== 基础配置 =====
    HBCore::Pattern23Mode m_pattern23Mode = HBCore::Pattern23Mode::STATIC;
    HBCore::ReplaceMode m_replaceMode = HBCore::ReplaceMode::RANDOM;
    int m_pattern23DynamicOffset = 0;

    // ===== 特征开关 =====
    bool m_enable62Pattern = false;
    bool m_enableCRC32AutoCalc = false;
    bool m_useSingleHeartbeatPacket = false;
    bool m_enable09_62Complement = false;
    bool m_isSameAlgorithm = true;
    HBCore::DataOrderMode m_dataOrderMode = HBCore::DataOrderMode::Ascending;

    // ===== 偏移替换规则 =====
    std::vector<HBCore::Pattern23OffsetRule> m_pattern23OffsetRules;
    std::vector<HBCore::Pattern09OffsetRule> m_pattern09OffsetRules;
    std::vector<HBCore::Pattern62OffsetRule> m_pattern62OffsetRules;
    mutable std::mutex m_pattern23OffsetMutex;
    mutable std::mutex m_pattern09OffsetMutex;
    mutable std::mutex m_pattern62OffsetMutex;
    int m_nextPattern23OffsetRuleId = 1;
    int m_nextPattern09OffsetRuleId = 1;
    int m_nextPattern62OffsetRuleId = 1;

    // ===== 白名单/黑名单 =====
    std::vector<HBCore::WhitelistRule> m_whitelistRules;
    std::vector<HBCore::BlacklistRule> m_blacklistRules;
    mutable std::mutex m_whitelistMutex;
    mutable std::mutex m_blacklistMutex;
    bool m_enableWhitelist = false;
    bool m_enableBlacklist = false;
    int m_nextWhitelistRuleId = 1;
    int m_nextBlacklistRuleId = 1;

    // ===== 替换数量规则 =====
    std::vector<HBCore::ReplaceCountRule> m_replaceCountRules;
    std::map<std::string, HBCore::ReplaceRuntimeState> m_replaceRuntimeStates;
    mutable std::mutex m_replaceRulesMutex;
    mutable std::mutex m_replaceRuntimeMutex;
    std::atomic<uint64_t> m_replaceRulesVersion{1};
    int m_nextReplaceCountRuleId = 1;

    // ===== 顺序替换索引 =====
    std::map<int, int> m_sequentialIndexByLength;
    int m_sequentialIndexAll = 0;
    mutable std::mutex m_sequentialMutex;

    // ===== 固定包选择 =====
    std::map<int, HBCore::FixedPacketSelection> m_fixedPacketByLength;
    mutable std::mutex m_fixedPacketMutex;

    // ===== 顺序模式配置 =====
    std::map<int, HBCore::SequentialConfig> m_sequentialConfigs;  // ID长度 -> 配置
    mutable std::mutex m_sequentialConfigMutex;

    // ===== 统计信息 =====
    std::atomic<uint64_t> m_totalProcessed{0};
    std::atomic<uint64_t> m_totalReplaced{0};
    std::atomic<uint64_t> m_totalSkipped{0};

    // ===== CRC32查找表 =====
    static const uint32_t s_crc32Table[256];
    static bool s_crc32TableInitialized;
    static void InitCRC32Table();
};
