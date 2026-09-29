// HeartbeatCore.cpp
// 伪心跳核心算法实现

#include "HeartbeatCore.h"
#include "Logger.h"
#include <json/json.h>
#include <fstream>
#include <random>
#include <algorithm>
#include <sstream>
#include <iomanip>

// ==================== CRC32查找表 ====================
const uint32_t HeartbeatCore::s_crc32Table[256] = {
    0x00000000, 0x77073096, 0xEE0E612C, 0x990951BA, 0x076DC419, 0x706AF48F, 0xE963A535, 0x9E6495A3,
    0x0EDB8832, 0x79DCB8A4, 0xE0D5E91E, 0x97D2D988, 0x09B64C2B, 0x7EB17CBD, 0xE7B82D07, 0x90BF1D91,
    0x1DB71064, 0x6AB020F2, 0xF3B97148, 0x84BE41DE, 0x1ADAD47D, 0x6DDDE4EB, 0xF4D4B551, 0x83D385C7,
    0x136C9856, 0x646BA8C0, 0xFD62F97A, 0x8A65C9EC, 0x14015C4F, 0x63066CD9, 0xFA0F3D63, 0x8D080DF5,
    0x3B6E20C8, 0x4C69105E, 0xD56041E4, 0xA2677172, 0x3C03E4D1, 0x4B04D447, 0xD20D85FD, 0xA50AB56B,
    0x35B5A8FA, 0x42B2986C, 0xDBBBBBD6, 0xACBCCB40, 0x32D86CE3, 0x45DF5C75, 0xDCD60DCF, 0xABD13D59,
    0x26D930AC, 0x51DE003A, 0xC8D75180, 0xBFD06116, 0x21B4F4B5, 0x56B3C423, 0xCFBA9599, 0xB8BDA50F,
    0x2802B89E, 0x5F058808, 0xC60CD9B2, 0xB10BE924, 0x2F6F7C87, 0x58684C11, 0xC1611DAB, 0xB6662D3D,
    0x76DC4190, 0x01DB7106, 0x98D220BC, 0xEFD5102A, 0x71B18589, 0x06B6B51F, 0x9FBFE4A5, 0xE8B8D433,
    0x7807C9A2, 0x0F00F934, 0x9609A88E, 0xE10E9818, 0x7F6A0DBB, 0x086D3D2D, 0x91646C97, 0xE6635C01,
    0x6B6B51F4, 0x1C6C6162, 0x856530D8, 0xF262004E, 0x6C0695ED, 0x1B01A57B, 0x8208F4C1, 0xF50FC457,
    0x65B0D9C6, 0x12B7E950, 0x8BBEB8EA, 0xFCB9887C, 0x62DD1DDF, 0x15DA2D49, 0x8CD37CF3, 0xFBD44C65,
    0x4DB26158, 0x3AB551CE, 0xA3BC0074, 0xD4BB30E2, 0x4ADFA541, 0x3DD895D7, 0xA4D1C46D, 0xD3D6F4FB,
    0x4369E96A, 0x346ED9FC, 0xAD678846, 0xDA60B8D0, 0x44042D73, 0x33031DE5, 0xAA0A4C5F, 0xDD0D7D49,
    0x5005713C, 0x270241AA, 0xBE0B1010, 0xC90C2086, 0x5768B525, 0x206F85B3, 0xB966D409, 0xCE61E49F,
    0x5EDEF90E, 0x29D9C998, 0xB0D09822, 0xC7D7A8B4, 0x59B33D17, 0x2EB40D81, 0xB7BD5C3B, 0xC0BA6CAD,
    0xEDB88320, 0x9ABFB3B6, 0x03B6E20C, 0x74B1D29A, 0xEAD54739, 0x9DD277AF, 0x04DB2615, 0x73DC1683,
    0xE3630B12, 0x94643B84, 0x0D6D6A3E, 0x7A6A5AA8, 0xE40ECF0B, 0x9309FF9D, 0x0A00AE27, 0x7D079EB1,
    0xF00F9344, 0x8708A3D2, 0x1E01F268, 0x6906C2FE, 0xF762575D, 0x806567CB, 0x196C3671, 0x6E6B06E7,
    0xFED41B76, 0x89D32BE0, 0x10DA7A5A, 0x67DD4ACC, 0xF9B9DF6F, 0x8EBEEFF9, 0x17B7BE43, 0x60B08ED5,
    0xD6D6A3E8, 0xA1D1937E, 0x38D8C2C4, 0x4FDFF252, 0xD1BB67F1, 0xA6BC5767, 0x3FB506DD, 0x48B2364B,
    0xD80D2BDA, 0xAF0A1B4C, 0x36034AF6, 0x41047A60, 0xDF60EFC3, 0xA867DF55, 0x316E8EEF, 0x4669BE79,
    0xCB61B38C, 0xBC66831A, 0x256FD2A0, 0x5268E236, 0xCC0C7795, 0xBB0B4703, 0x220216B9, 0x5505262F,
    0xC5BA3BBE, 0xB2BD0B28, 0x2BB45A92, 0x5CB36A04, 0xC2D7FFA7, 0xB5D0CF31, 0x2CD99E8B, 0x5BDEAE1D,
    0x9B64C2B0, 0xEC63F226, 0x756AA39C, 0x026D930A, 0x9C0906A9, 0xEB0E363F, 0x72076785, 0x05005713,
    0x95BF4A82, 0xE2B87A14, 0x7BB12BAE, 0x0CB61B38, 0x92D28E9B, 0xE5D5BE0D, 0x7CDCEFB7, 0x0BDBDF21,
    0x86D3D2D4, 0xF1D4E242, 0x68DDB3F8, 0x1FDA836E, 0x81BE16CD, 0xF6B9265B, 0x6FB077E1, 0x18B74777,
    0x88085AE6, 0xFF0F6A70, 0x66063BCA, 0x11010B5C, 0x8F659EFF, 0xF862AE69, 0x616BFFD3, 0x166CCF45,
    0xA00AE278, 0xD70DD2EE, 0x4E048354, 0x3903B3C2, 0xA7672661, 0xD06016F7, 0x4969474D, 0x3E6E77DB,
    0xAED16A4A, 0xD9D65ADC, 0x40DF0B66, 0x37D83BF0, 0xA9BCAE53, 0xDEBBBEC5, 0x47B2CF7F, 0x30B5FFE9,
    0xBDBDF21C, 0xCABAC28A, 0x53B39330, 0x24B4A3A6, 0xBAD03605, 0xCDD706B3, 0x54DE5729, 0x23D967BF,
    0xB3667A2E, 0xC4614AB8, 0x5D681B02, 0x2A6F2B94, 0xB40BBE37, 0xC30C8EA1, 0x5A05DF1B, 0x2D02EF8D
};

bool HeartbeatCore::s_crc32TableInitialized = true;

void HeartbeatCore::InitCRC32Table() {
    // 表已经静态初始化
}

// ==================== 构造函数/析构函数 ====================
HeartbeatCore::HeartbeatCore()
    : m_database(nullptr)
    , m_pattern23Mode(HBCore::Pattern23Mode::STATIC)
    , m_replaceMode(HBCore::ReplaceMode::RANDOM)
    , m_pattern23DynamicOffset(0)
    , m_enable62Pattern(false)
    , m_enableCRC32AutoCalc(false)
    , m_useSingleHeartbeatPacket(false)
    , m_enable09_62Complement(false)
    , m_isSameAlgorithm(true)
    , m_enableWhitelist(false)
    , m_enableBlacklist(false)
    , m_nextPattern23OffsetRuleId(1)
    , m_nextPattern09OffsetRuleId(1)
    , m_nextWhitelistRuleId(1)
    , m_nextBlacklistRuleId(1)
    , m_nextReplaceCountRuleId(1)
    , m_sequentialIndexAll(0)
{
}

HeartbeatCore::~HeartbeatCore() {
}

// ==================== 数据库设置 ====================
void HeartbeatCore::SetDatabase(DatabaseManager* db) {
    m_database = db;
}

// ==================== CRC32计算 ====================
uint32_t HeartbeatCore::CalculateCRC32(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; i++) {
        crc = s_crc32Table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

void HeartbeatCore::CalculateCRC32Bytes(const std::vector<uint8_t>& data, int startPos, int endPos,
                                        uint8_t& crc0, uint8_t& crc1, uint8_t& crc2, uint8_t& crc3) {
    if (startPos < 0 || endPos > static_cast<int>(data.size()) || startPos >= endPos) {
        crc0 = crc1 = crc2 = crc3 = 0;
        return;
    }

    uint32_t crc = CalculateCRC32(&data[startPos], endPos - startPos);
    crc0 = static_cast<uint8_t>(crc & 0xFF);
    crc1 = static_cast<uint8_t>((crc >> 8) & 0xFF);
    crc2 = static_cast<uint8_t>((crc >> 16) & 0xFF);
    crc3 = static_cast<uint8_t>((crc >> 24) & 0xFF);
}

// ==================== 特征检测 ====================
int HeartbeatCore::FindPattern23(const std::vector<uint8_t>& data, int startPos) {
    // 查找 01 0A 00 23
    const uint8_t pattern[] = { 0x01, 0x0A, 0x00, 0x23 };
    for (size_t i = startPos; i + 4 <= data.size(); i++) {
        if (data[i] == pattern[0] && data[i + 1] == pattern[1] &&
            data[i + 2] == pattern[2] && data[i + 3] == pattern[3]) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int HeartbeatCore::FindPattern09(const std::vector<uint8_t>& data, int startPos) {
    // 查找 01 0A 00 09
    const uint8_t pattern[] = { 0x01, 0x0A, 0x00, 0x09 };
    for (size_t i = startPos; i + 4 <= data.size(); i++) {
        if (data[i] == pattern[0] && data[i + 1] == pattern[1] &&
            data[i + 2] == pattern[2] && data[i + 3] == pattern[3]) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int HeartbeatCore::FindPattern62(const std::vector<uint8_t>& data, int startPos) {
    // 查找 01 0A 00 62
    const uint8_t pattern[] = { 0x01, 0x0A, 0x00, 0x62 };
    for (size_t i = startPos; i + 4 <= data.size(); i++) {
        if (data[i] == pattern[0] && data[i + 1] == pattern[1] &&
            data[i + 2] == pattern[2] && data[i + 3] == pattern[3]) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// ==================== 内部辅助函数 ====================
int HeartbeatCore::ExtractIDLength(const std::vector<uint8_t>& data, int pattern23Pos) {
    // 从23特征位置提取ID长度
    // 通常ID长度在特征后的某个位置
    if (pattern23Pos + 5 >= static_cast<int>(data.size())) {
        return 12; // 默认长度
    }

    // 根据动态偏移计算
    int lengthPos = pattern23Pos + 4 + m_pattern23DynamicOffset;
    if (lengthPos >= 0 && lengthPos < static_cast<int>(data.size())) {
        return data[lengthPos];
    }
    return 12;
}

void HeartbeatCore::UpdateLengthFields(std::vector<uint8_t>& data, int oldLen, int newLen, int pattern23Pos) {
    // 更新长度字段
    int diff = newLen - oldLen;
    if (diff == 0) return;

    // 更新包头长度（通常在位置2-3）
    if (data.size() >= 4) {
        int currentLen = data[2] | (data[3] << 8);
        int newPacketLen = currentLen + diff;
        data[2] = static_cast<uint8_t>(newPacketLen & 0xFF);
        data[3] = static_cast<uint8_t>((newPacketLen >> 8) & 0xFF);
    }
}

bool HeartbeatCore::MatchPattern(const std::vector<uint8_t>& data, const std::string& pattern, bool usePatternSearch) {
    // 解析模式字符串: "pos|hex,pos|hex,..."
    std::stringstream ss(pattern);
    std::string segment;

    while (std::getline(ss, segment, ',')) {
        size_t delimPos = segment.find('|');
        if (delimPos == std::string::npos) continue;

        std::string posStr = segment.substr(0, delimPos);
        std::string hexStr = segment.substr(delimPos + 1);

        int pos = std::stoi(posStr);

        // 解析hex字符串
        std::vector<uint8_t> hexBytes;
        std::stringstream hexSS(hexStr);
        std::string byteStr;
        while (hexSS >> byteStr) {
            hexBytes.push_back(static_cast<uint8_t>(std::stoul(byteStr, nullptr, 16)));
        }

        // 检查匹配
        if (pos < 0 || pos + static_cast<int>(hexBytes.size()) > static_cast<int>(data.size())) {
            return false;
        }

        for (size_t i = 0; i < hexBytes.size(); i++) {
            if (data[pos + i] != hexBytes[i]) {
                return false;
            }
        }
    }

    return true;
}

// ==================== 白名单/黑名单检查 ====================
bool HeartbeatCore::CheckWhitelist(const std::vector<uint8_t>& data, int* matchedIndex, std::string* matchedRuleName) {
    if (!m_enableWhitelist) { return true; }

    std::lock_guard<std::mutex> lock(m_whitelistMutex);

    for (size_t i = 0; i < m_whitelistRules.size(); i++) {
        auto& rule = m_whitelistRules[i];
        if (!rule.isEnabled) continue;

        if (MatchPattern(data, rule.searchPattern, rule.usePatternSearch)) {
            rule.matchCount++;
            if (matchedIndex) *matchedIndex = static_cast<int>(i);
            if (matchedRuleName) *matchedRuleName = rule.name;
            return true;
        }
    }

    return false;
}

bool HeartbeatCore::CheckBlacklist(const std::vector<uint8_t>& data, int* matchedIndex, std::string* matchedRuleName) {
    if (!m_enableBlacklist) { return false; }

    std::lock_guard<std::mutex> lock(m_blacklistMutex);

    for (size_t i = 0; i < m_blacklistRules.size(); i++) {
        auto& rule = m_blacklistRules[i];
        if (!rule.isEnabled) continue;

        if (MatchPattern(data, rule.searchPattern, rule.usePatternSearch)) {
            rule.matchCount++;
            if (matchedIndex) *matchedIndex = static_cast<int>(i);
            if (matchedRuleName) *matchedRuleName = rule.name;
            return true;
        }
    }

    return false;
}

// ==================== 替换数量规则 ====================
std::string HeartbeatCore::MakeReplaceRuleKey(bool isSameAlgorithm, const std::string& gameID, const std::string& username) {
    return isSameAlgorithm ? gameID : username;
}

HBCore::ReplaceCountRule* HeartbeatCore::GetFirstEnabledReplaceRule() {
    std::lock_guard<std::mutex> lock(m_replaceRulesMutex);
    for (auto& rule : m_replaceCountRules) {
        if (rule.enabled) {
            return &rule;
        }
    }
    return nullptr;
}

bool HeartbeatCore::ShouldAttemptReplace(const std::string& key, const std::string& gameID, const std::string& username) {
    HBCore::ReplaceCountRule* rule = GetFirstEnabledReplaceRule();
    if (!rule) return true; // 没有规则，允许替换

    std::lock_guard<std::mutex> lock(m_replaceRuntimeMutex);

    auto& state = m_replaceRuntimeStates[key];

    // 检查版本是否匹配
    if (state.rulesVersion != m_replaceRulesVersion.load()) {
        state = HBCore::ReplaceRuntimeState();
        state.ruleId = rule->id;
        state.rulesVersion = m_replaceRulesVersion.load();
    }

    if (!state.reached) {
        return true; // 未到达阈值，允许替换
    }

    // 已到达阈值，根据策略决定
    if (rule->postLimitStrategy == HBCore::PostLimitStrategy::SendOriginal) {
        return false; // 只发原包
    }

    // CycleFakeThenOriginal策略
    if (state.cycleRemaining <= 0) {
        state.cycleSendFake = !state.cycleSendFake;
        state.cycleRemaining = state.cycleSendFake ? rule->postLimitFakeN : rule->postLimitOriginalN;
    }

    state.cycleRemaining--;
    return state.cycleSendFake;
}

void HeartbeatCore::OnReplacementSuccess(const std::string& key, const std::string& gameID, const std::string& username) {
    HBCore::ReplaceCountRule* rule = GetFirstEnabledReplaceRule();
    if (!rule) return;

    std::lock_guard<std::mutex> lock(m_replaceRuntimeMutex);

    auto& state = m_replaceRuntimeStates[key];
    state.replacementsDone++;

    if (!state.reached && state.replacementsDone >= rule->replaceLimit) {
        state.reached = true;
        state.cycleSendFake = true;
        state.cycleRemaining = rule->postLimitFakeN;

        // 到达阈值时清理数据池（只执行一次）
        if (rule->clearPoolOnReach && !state.reachCleanupDone) {
            state.reachCleanupDone = true;
            if (m_clearPoolCallback) {
                m_clearPoolCallback(gameID, username, m_isSameAlgorithm);
            }
        }
    }
}

void HeartbeatCore::OnOriginalSentAfterReach(const std::string& key) {
    // 可用于统计
}

void HeartbeatCore::ResetRuntimeStateForKey(const std::string& key) {
    std::lock_guard<std::mutex> lock(m_replaceRuntimeMutex);
    m_replaceRuntimeStates.erase(key);
}

// ==================== 数据池获取 ====================
HeartbeatRecord HeartbeatCore::GetRandomHeartbeatData() {
    if (!m_database) return HeartbeatRecord();

    auto records = m_database->GetHeartbeatsFromId(0, 1000);
    if (records.empty()) return HeartbeatRecord();

    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, static_cast<int>(records.size()) - 1);

    return records[dis(gen)];
}

HeartbeatRecord HeartbeatCore::GetRandomHeartbeatDataByLength(int length) {
    if (!m_database) return HeartbeatRecord();

    auto records = m_database->GetHeartbeatsFromId(0, 5000);
    std::vector<HeartbeatRecord> filtered;

    for (auto& rec : records) {
        if (rec.gameIDLength == length) {
            filtered.push_back(rec);
        }
    }

    if (filtered.empty()) return HeartbeatRecord();

    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, static_cast<int>(filtered.size()) - 1);

    return filtered[dis(gen)];
}

HeartbeatRecord HeartbeatCore::GetSequentialHeartbeatData(const std::string& gameID, int length, bool isStaticMode) {
    if (!m_database) { return HeartbeatRecord(); }

    auto records = m_database->GetHeartbeatsFromId(0, 5000);
    std::vector<HeartbeatRecord> filtered;

    for (auto& rec : records) {
        if (rec.gameIDLength == length) {
            filtered.push_back(rec);
        }
    }

    if (filtered.empty()) { return HeartbeatRecord(); }

    std::lock_guard<std::mutex> lock(m_sequentialMutex);

    int& index = m_sequentialIndexByLength[length];

    // 根据数据顺序模式决定获取方向
    if (m_dataOrderMode == HBCore::DataOrderMode::Ascending) {
        // 升序模式（FIFO）：从头开始顺序获取
        if (index >= static_cast<int>(filtered.size())) {
            index = 0;
        }
        return filtered[index++];
    } else {
        // 降序模式（LIFO）：从尾开始逆序获取
        if (index >= static_cast<int>(filtered.size())) {
            index = 0;
        }
        int reverseIndex = static_cast<int>(filtered.size()) - 1 - index;
        index++;
        return filtered[reverseIndex];
    }
}

HeartbeatRecord HeartbeatCore::GetSingleHeartbeatPacketByLength(int length) {
    std::lock_guard<std::mutex> lock(m_fixedPacketMutex);

    auto it = m_fixedPacketByLength.find(length);
    if (it == m_fixedPacketByLength.end() || it->second.packetId < 0) {
        return HeartbeatRecord();
    }

    // 从数据库获取指定ID的记录
    if (!m_database) return HeartbeatRecord();

    auto records = m_database->GetHeartbeatsFromId(it->second.packetId, 1);
    if (!records.empty() && records[0].id == it->second.packetId) {
        return records[0];
    }

    return HeartbeatRecord();
}

HeartbeatRecord HeartbeatCore::GetHeartbeatDataByMode(const std::string& gameID, int length, const std::string& username) {
    HeartbeatRecord rec;
    switch (m_replaceMode) {
    case HBCore::ReplaceMode::RANDOM:
        rec = GetRandomHeartbeatDataByLength(length);
        break;
    case HBCore::ReplaceMode::SEQUENTIAL:
        rec = GetSequentialHeartbeatData(gameID, length, m_pattern23Mode == HBCore::Pattern23Mode::STATIC);
        break;
    case HBCore::ReplaceMode::FIXED:
        rec = GetSingleHeartbeatPacketByLength(length);
        break;
    default:
        rec = GetRandomHeartbeatDataByLength(length);
        break;
    }
    return rec;
}

// ==================== 偏移替换 ====================
bool HeartbeatCore::ApplyPattern23OffsetReplace(std::vector<uint8_t>& data, const HeartbeatRecord& poolData, const std::string& gameID) {
    std::lock_guard<std::mutex> lock(m_pattern23OffsetMutex);

    bool anyReplaced = false;

    for (const auto& rule : m_pattern23OffsetRules) {
        if (!rule.enabled) continue;

        int pos = FindPattern23(data, 0);
        if (pos < 0) continue;

        int replaceStart = pos + 4 + rule.offset;
        int replaceLen = rule.length;

        if (replaceStart < 0 || replaceStart + replaceLen > static_cast<int>(data.size())) {
            continue;
        }

        // 从池数据获取替换内容
        if (poolData.pattern23Data.size() >= static_cast<size_t>(replaceLen)) {
            for (int i = 0; i < replaceLen; i++) {
                data[replaceStart + i] = poolData.pattern23Data[i];
            }
            anyReplaced = true;
        }
    }

    return anyReplaced;
}

bool HeartbeatCore::ApplyPattern09OffsetReplace(std::vector<uint8_t>& data, const HeartbeatRecord& poolData, const std::string& gameID) {
    std::lock_guard<std::mutex> lock(m_pattern09OffsetMutex);

    bool anyReplaced = false;

    for (const auto& rule : m_pattern09OffsetRules) {
        if (!rule.enabled) continue;

        int pos = FindPattern09(data, 0);
        if (pos < 0) continue;

        int replaceStart = pos + 4 + rule.offset;
        int replaceLen = rule.length;

        if (replaceStart < 0 || replaceStart + replaceLen > static_cast<int>(data.size())) {
            continue;
        }

        // 从池数据获取替换内容
        if (poolData.pattern09Data.size() >= static_cast<size_t>(replaceLen)) {
            for (int i = 0; i < replaceLen; i++) {
                data[replaceStart + i] = poolData.pattern09Data[i];
            }
            anyReplaced = true;
        }
    }

    return anyReplaced;
}

bool HeartbeatCore::ApplyPattern62OffsetReplace(std::vector<uint8_t>& data, const HeartbeatRecord& poolData, const std::string& gameID) {
    std::lock_guard<std::mutex> lock(m_pattern62OffsetMutex);

    bool anyReplaced = false;

    for (const auto& rule : m_pattern62OffsetRules) {
        if (!rule.enabled) continue;

        int pos = FindPattern62(data, 0);
        if (pos < 0) continue;

        int replaceStart = pos + 4 + rule.offset;
        int replaceLen = rule.length;

        if (replaceStart < 0 || replaceStart + replaceLen > static_cast<int>(data.size())) {
            continue;
        }

        // 从池数据获取替换内容（优先使用62数据，如果没有则使用09数据作为互补）
        const std::vector<uint8_t>& sourceData = !poolData.pattern62Data.empty() ?
            poolData.pattern62Data : poolData.pattern09Data;

        if (sourceData.size() >= static_cast<size_t>(replaceLen)) {
            for (int i = 0; i < replaceLen; i++) {
                data[replaceStart + i] = sourceData[i];
            }
            anyReplaced = true;
        }
    }

    return anyReplaced;
}

// ==================== 主替换函数 ====================
std::vector<uint8_t> HeartbeatCore::ReplaceHeartbeatData(
    const std::vector<uint8_t>& originalData,
    std::vector<HBCore::LengthFieldAdjustment>* outAdjustments,
    int* outOriginalIDLen,
    int* outReplacedIDLen,
    int* outFinalIDLen,
    HeartbeatRecord* outPoolData,
    const std::string& username,
    const std::string& gameID)
{

    m_totalProcessed++;

    // 检查黑名单
    if (CheckBlacklist(originalData, nullptr, nullptr)) {
        m_totalSkipped++;
        return originalData; // 返回原包
    }

    // 检查白名单
    if (m_enableWhitelist && !CheckWhitelist(originalData, nullptr, nullptr)) {
        m_totalSkipped++;
        return originalData; // 不在白名单中，返回原包
    }

    // 查找23特征
    int pattern23Pos = FindPattern23(originalData, 0);
    if (pattern23Pos < 0) {
        m_totalSkipped++;
        return originalData; // 没有23特征，返回原包
    }

    // 检查09和62特征
    bool has09Pattern = FindPattern09(originalData, 0) >= 0;
    bool has62Pattern = FindPattern62(originalData, 0) >= 0;

    // 提取原始ID长度
    int originalIDLen = 12; // 默认
    if (m_pattern23Mode == HBCore::Pattern23Mode::DYNAMIC) {
        originalIDLen = ExtractIDLength(originalData, pattern23Pos);
    }

    if (outOriginalIDLen) *outOriginalIDLen = originalIDLen;

    // 检查替换数量规则
    std::string ruleKey = MakeReplaceRuleKey(m_isSameAlgorithm, gameID, username);
    if (!ShouldAttemptReplace(ruleKey, gameID, username)) {
        m_totalSkipped++;
        return originalData; // 达到替换限制，返回原包
    }

    // 获取替换数据
    HeartbeatRecord poolData = GetHeartbeatDataByMode(gameID, originalIDLen, username);
    if (poolData.pattern23Data.empty()) {
        m_totalSkipped++;
        return originalData; // 没有可用的替换数据
    }

    // 09和62特征互补逻辑
    if (m_enable09_62Complement) {
        // 如果池数据中09为空但62不为空，用62填充09
        if (poolData.pattern09Data.empty() && !poolData.pattern62Data.empty()) {
            poolData.pattern09Data = poolData.pattern62Data;
        }
        // 如果池数据中62为空但09不为空，用09填充62
        if (poolData.pattern62Data.empty() && !poolData.pattern09Data.empty()) {
            poolData.pattern62Data = poolData.pattern09Data;
        }
    }

    if (outPoolData) *outPoolData = poolData;

    // 复制数据进行修改
    std::vector<uint8_t> result = originalData;

    // 执行23特征替换
    int replaceStart = pattern23Pos + 4;
    int replaceLen = originalIDLen;

    if (replaceStart + replaceLen <= static_cast<int>(result.size()) &&
        poolData.pattern23Data.size() >= static_cast<size_t>(replaceLen)) {
        for (int i = 0; i < replaceLen; i++) {
            result[replaceStart + i] = poolData.pattern23Data[i];
        }
    }

    // 应用23偏移规则
    ApplyPattern23OffsetReplace(result, poolData, gameID);

    // 应用09偏移规则
    ApplyPattern09OffsetReplace(result, poolData, gameID);

    // 62特征处理
    if (m_enable62Pattern) {
        // 如果启用了09/62互补且有09特征，则跳过62的基础替换（但仍执行偏移替换）
        bool skip62BasicReplace = m_enable09_62Complement && has09Pattern;

        // 62特征基础替换
        if (!skip62BasicReplace && has62Pattern && !poolData.pattern62Data.empty()) {
            int pattern62Pos = FindPattern62(result, 0);
            if (pattern62Pos >= 0) {
                int pos62Start = pattern62Pos + 4;
                size_t len62 = std::min(poolData.pattern62Data.size(), result.size() - pos62Start);
                for (size_t i = 0; i < len62; i++) {
                    result[pos62Start + i] = poolData.pattern62Data[i];
                }
            }
        }

        // 应用62偏移规则（无论是否跳过基础替换，偏移规则都执行）
        ApplyPattern62OffsetReplace(result, poolData, gameID);
    }

    // CRC32自动计算
    if (m_enableCRC32AutoCalc && result.size() >= 8) {
        uint8_t crc0, crc1, crc2, crc3;
        CalculateCRC32Bytes(result, 4, static_cast<int>(result.size()) - 4, crc0, crc1, crc2, crc3);
        // 写入CRC32到包尾
        result[result.size() - 4] = crc0;
        result[result.size() - 3] = crc1;
        result[result.size() - 2] = crc2;
        result[result.size() - 1] = crc3;
    }

    if (outReplacedIDLen) *outReplacedIDLen = static_cast<int>(poolData.pattern23Data.size());
    if (outFinalIDLen) *outFinalIDLen = replaceLen;

    // 记录替换成功
    OnReplacementSuccess(ruleKey, gameID, username);
    m_totalReplaced++;

    return result;
}

// ==================== 统计信息 ====================
void HeartbeatCore::ResetStatistics() {
    m_totalProcessed = 0;
    m_totalReplaced = 0;
    m_totalSkipped = 0;
}

// ==================== Pattern23偏移规则管理 ====================
void HeartbeatCore::AddPattern23OffsetRule(const HBCore::Pattern23OffsetRule& rule) {
    std::lock_guard<std::mutex> lock(m_pattern23OffsetMutex);
    HBCore::Pattern23OffsetRule newRule = rule;
    newRule.id = m_nextPattern23OffsetRuleId++;
    m_pattern23OffsetRules.push_back(newRule);
}

void HeartbeatCore::UpdatePattern23OffsetRule(int id, const HBCore::Pattern23OffsetRule& rule) {
    std::lock_guard<std::mutex> lock(m_pattern23OffsetMutex);
    for (auto& r : m_pattern23OffsetRules) {
        if (r.id == id) {
            r = rule;
            r.id = id;
            break;
        }
    }
}

void HeartbeatCore::RemovePattern23OffsetRule(int id) {
    std::lock_guard<std::mutex> lock(m_pattern23OffsetMutex);
    m_pattern23OffsetRules.erase(
        std::remove_if(m_pattern23OffsetRules.begin(), m_pattern23OffsetRules.end(),
            [id](const HBCore::Pattern23OffsetRule& r) { return r.id == id; }),
        m_pattern23OffsetRules.end());
}

void HeartbeatCore::ClearPattern23OffsetRules() {
    std::lock_guard<std::mutex> lock(m_pattern23OffsetMutex);
    m_pattern23OffsetRules.clear();
}

std::vector<HBCore::Pattern23OffsetRule> HeartbeatCore::GetPattern23OffsetRules() const {
    std::lock_guard<std::mutex> lock(m_pattern23OffsetMutex);
    return m_pattern23OffsetRules;
}

int HeartbeatCore::GetNextPattern23OffsetRuleId() {
    return m_nextPattern23OffsetRuleId;
}

// ==================== Pattern09偏移规则管理 ====================
void HeartbeatCore::AddPattern09OffsetRule(const HBCore::Pattern09OffsetRule& rule) {
    std::lock_guard<std::mutex> lock(m_pattern09OffsetMutex);
    HBCore::Pattern09OffsetRule newRule = rule;
    newRule.id = m_nextPattern09OffsetRuleId++;
    m_pattern09OffsetRules.push_back(newRule);
}

void HeartbeatCore::UpdatePattern09OffsetRule(int id, const HBCore::Pattern09OffsetRule& rule) {
    std::lock_guard<std::mutex> lock(m_pattern09OffsetMutex);
    for (auto& r : m_pattern09OffsetRules) {
        if (r.id == id) {
            r = rule;
            r.id = id;
            break;
        }
    }
}

void HeartbeatCore::RemovePattern09OffsetRule(int id) {
    std::lock_guard<std::mutex> lock(m_pattern09OffsetMutex);
    m_pattern09OffsetRules.erase(
        std::remove_if(m_pattern09OffsetRules.begin(), m_pattern09OffsetRules.end(),
            [id](const HBCore::Pattern09OffsetRule& r) { return r.id == id; }),
        m_pattern09OffsetRules.end());
}

void HeartbeatCore::ClearPattern09OffsetRules() {
    std::lock_guard<std::mutex> lock(m_pattern09OffsetMutex);
    m_pattern09OffsetRules.clear();
}

std::vector<HBCore::Pattern09OffsetRule> HeartbeatCore::GetPattern09OffsetRules() const {
    std::lock_guard<std::mutex> lock(m_pattern09OffsetMutex);
    return m_pattern09OffsetRules;
}

int HeartbeatCore::GetNextPattern09OffsetRuleId() {
    return m_nextPattern09OffsetRuleId;
}

// ==================== Pattern62偏移规则管理 ====================
void HeartbeatCore::AddPattern62OffsetRule(const HBCore::Pattern62OffsetRule& rule) {
    std::lock_guard<std::mutex> lock(m_pattern62OffsetMutex);
    HBCore::Pattern62OffsetRule newRule = rule;
    newRule.id = m_nextPattern62OffsetRuleId++;
    m_pattern62OffsetRules.push_back(newRule);
}

void HeartbeatCore::UpdatePattern62OffsetRule(int id, const HBCore::Pattern62OffsetRule& rule) {
    std::lock_guard<std::mutex> lock(m_pattern62OffsetMutex);
    for (auto& r : m_pattern62OffsetRules) {
        if (r.id == id) {
            r = rule;
            r.id = id;
            break;
        }
    }
}

void HeartbeatCore::RemovePattern62OffsetRule(int id) {
    std::lock_guard<std::mutex> lock(m_pattern62OffsetMutex);
    m_pattern62OffsetRules.erase(
        std::remove_if(m_pattern62OffsetRules.begin(), m_pattern62OffsetRules.end(),
            [id](const HBCore::Pattern62OffsetRule& r) { return r.id == id; }),
        m_pattern62OffsetRules.end());
}

void HeartbeatCore::ClearPattern62OffsetRules() {
    std::lock_guard<std::mutex> lock(m_pattern62OffsetMutex);
    m_pattern62OffsetRules.clear();
}

std::vector<HBCore::Pattern62OffsetRule> HeartbeatCore::GetPattern62OffsetRules() const {
    std::lock_guard<std::mutex> lock(m_pattern62OffsetMutex);
    return m_pattern62OffsetRules;
}

int HeartbeatCore::GetNextPattern62OffsetRuleId() {
    return m_nextPattern62OffsetRuleId;
}

// ==================== 白名单规则管理 ====================
void HeartbeatCore::AddWhitelistRule(const HBCore::WhitelistRule& rule) {
    std::lock_guard<std::mutex> lock(m_whitelistMutex);
    HBCore::WhitelistRule newRule = rule;
    newRule.id = m_nextWhitelistRuleId++;
    m_whitelistRules.push_back(newRule);
}

void HeartbeatCore::UpdateWhitelistRule(int id, const HBCore::WhitelistRule& rule) {
    std::lock_guard<std::mutex> lock(m_whitelistMutex);
    for (auto& r : m_whitelistRules) {
        if (r.id == id) {
            r = rule;
            r.id = id;
            break;
        }
    }
}

void HeartbeatCore::RemoveWhitelistRule(int id) {
    std::lock_guard<std::mutex> lock(m_whitelistMutex);
    m_whitelistRules.erase(
        std::remove_if(m_whitelistRules.begin(), m_whitelistRules.end(),
            [id](const HBCore::WhitelistRule& r) { return r.id == id; }),
        m_whitelistRules.end());
}

void HeartbeatCore::ClearWhitelistRules() {
    std::lock_guard<std::mutex> lock(m_whitelistMutex);
    m_whitelistRules.clear();
}

std::vector<HBCore::WhitelistRule> HeartbeatCore::GetWhitelistRules() const {
    std::lock_guard<std::mutex> lock(m_whitelistMutex);
    return m_whitelistRules;
}

int HeartbeatCore::GetNextWhitelistRuleId() {
    return m_nextWhitelistRuleId;
}

// ==================== 黑名单规则管理 ====================
void HeartbeatCore::AddBlacklistRule(const HBCore::BlacklistRule& rule) {
    std::lock_guard<std::mutex> lock(m_blacklistMutex);
    HBCore::BlacklistRule newRule = rule;
    newRule.id = m_nextBlacklistRuleId++;
    m_blacklistRules.push_back(newRule);
}

void HeartbeatCore::UpdateBlacklistRule(int id, const HBCore::BlacklistRule& rule) {
    std::lock_guard<std::mutex> lock(m_blacklistMutex);
    for (auto& r : m_blacklistRules) {
        if (r.id == id) {
            r = rule;
            r.id = id;
            break;
        }
    }
}

void HeartbeatCore::RemoveBlacklistRule(int id) {
    std::lock_guard<std::mutex> lock(m_blacklistMutex);
    m_blacklistRules.erase(
        std::remove_if(m_blacklistRules.begin(), m_blacklistRules.end(),
            [id](const HBCore::BlacklistRule& r) { return r.id == id; }),
        m_blacklistRules.end());
}

void HeartbeatCore::ClearBlacklistRules() {
    std::lock_guard<std::mutex> lock(m_blacklistMutex);
    m_blacklistRules.clear();
}

std::vector<HBCore::BlacklistRule> HeartbeatCore::GetBlacklistRules() const {
    std::lock_guard<std::mutex> lock(m_blacklistMutex);
    return m_blacklistRules;
}

int HeartbeatCore::GetNextBlacklistRuleId() {
    return m_nextBlacklistRuleId;
}

// ==================== 替换数量规则管理 ====================
void HeartbeatCore::AddReplaceCountRule(const HBCore::ReplaceCountRule& rule) {
    std::lock_guard<std::mutex> lock(m_replaceRulesMutex);
    HBCore::ReplaceCountRule newRule = rule;
    newRule.id = m_nextReplaceCountRuleId++;
    m_replaceCountRules.push_back(newRule);
    BumpReplaceRulesVersionAndResetRuntime();
}

void HeartbeatCore::UpdateReplaceCountRule(int id, const HBCore::ReplaceCountRule& rule) {
    std::lock_guard<std::mutex> lock(m_replaceRulesMutex);
    for (auto& r : m_replaceCountRules) {
        if (r.id == id) {
            r = rule;
            r.id = id;
            break;
        }
    }
    BumpReplaceRulesVersionAndResetRuntime();
}

void HeartbeatCore::RemoveReplaceCountRule(int id) {
    std::lock_guard<std::mutex> lock(m_replaceRulesMutex);
    m_replaceCountRules.erase(
        std::remove_if(m_replaceCountRules.begin(), m_replaceCountRules.end(),
            [id](const HBCore::ReplaceCountRule& r) { return r.id == id; }),
        m_replaceCountRules.end());
    BumpReplaceRulesVersionAndResetRuntime();
}

void HeartbeatCore::ClearReplaceCountRules() {
    std::lock_guard<std::mutex> lock(m_replaceRulesMutex);
    m_replaceCountRules.clear();
    BumpReplaceRulesVersionAndResetRuntime();
}

std::vector<HBCore::ReplaceCountRule> HeartbeatCore::GetReplaceCountRules() const {
    std::lock_guard<std::mutex> lock(m_replaceRulesMutex);
    return m_replaceCountRules;
}

int HeartbeatCore::GetNextReplaceCountRuleId() {
    return m_nextReplaceCountRuleId;
}

void HeartbeatCore::BumpReplaceRulesVersionAndResetRuntime() {
    m_replaceRulesVersion++;
    std::lock_guard<std::mutex> lock(m_replaceRuntimeMutex);
    m_replaceRuntimeStates.clear();
}

// ==================== 固定包选择管理 ====================
void HeartbeatCore::SetFixedPacketByLength(int length, int packetId, const std::string& gameID) {
    std::lock_guard<std::mutex> lock(m_fixedPacketMutex);
    if (packetId < 0) {
        m_fixedPacketByLength.erase(length);
    } else {
        HBCore::FixedPacketSelection selection;
        selection.packetId = packetId;
        selection.gameID = gameID;
        m_fixedPacketByLength[length] = selection;
    }
}

HBCore::FixedPacketSelection HeartbeatCore::GetFixedPacketByLength(int length) const {
    std::lock_guard<std::mutex> lock(m_fixedPacketMutex);
    auto it = m_fixedPacketByLength.find(length);
    if (it != m_fixedPacketByLength.end()) {
        return it->second;
    }
    return HBCore::FixedPacketSelection();
}

std::map<int, HBCore::FixedPacketSelection> HeartbeatCore::GetAllFixedPacketSelections() const {
    std::lock_guard<std::mutex> lock(m_fixedPacketMutex);
    return m_fixedPacketByLength;
}

void HeartbeatCore::ClearFixedPacketSelections() {
    std::lock_guard<std::mutex> lock(m_fixedPacketMutex);
    m_fixedPacketByLength.clear();
}

// ==================== 顺序模式配置管理 ====================
void HeartbeatCore::SetSequentialConfig(int length, const HBCore::SequentialConfig& config) {
    std::lock_guard<std::mutex> lock(m_sequentialConfigMutex);
    m_sequentialConfigs[length] = config;
    AB_LOG_INFO("[HeartbeatCore] 设置长度 " + std::to_string(length) + " 的顺序配置: startId=" +
                 std::to_string(config.startId) + ", endId=" + std::to_string(config.endId) +
                 ", enabled=" + (config.enabled ? "true" : "false"));
}

HBCore::SequentialConfig HeartbeatCore::GetSequentialConfig(int length) const {
    std::lock_guard<std::mutex> lock(m_sequentialConfigMutex);
    auto it = m_sequentialConfigs.find(length);
    if (it != m_sequentialConfigs.end()) {
        return it->second;
    }
    // 返回默认配置
    return HBCore::SequentialConfig();
}

std::map<int, HBCore::SequentialConfig> HeartbeatCore::GetAllSequentialConfigs() const {
    std::lock_guard<std::mutex> lock(m_sequentialConfigMutex);
    return m_sequentialConfigs;
}

void HeartbeatCore::RemoveSequentialConfig(int length) {
    std::lock_guard<std::mutex> lock(m_sequentialConfigMutex);
    m_sequentialConfigs.erase(length);
    AB_LOG_INFO("[HeartbeatCore] 移除长度 " + std::to_string(length) + " 的顺序配置");
}

void HeartbeatCore::ClearSequentialConfigs() {
    std::lock_guard<std::mutex> lock(m_sequentialConfigMutex);
    m_sequentialConfigs.clear();
    AB_LOG_INFO("[HeartbeatCore] 清空所有顺序配置");
}

void HeartbeatCore::UpdateSequentialConfigCounts() {
    if (!m_database) return;

    std::lock_guard<std::mutex> lock(m_sequentialConfigMutex);
    for (auto& pair : m_sequentialConfigs) {
        int length = pair.first;
        // 查询该长度的数据量
        int count = m_database->GetHeartbeatCountByLength(length);
        pair.second.currentCount = count;
    }
}

// ==================== 配置 Setter ====================
void HeartbeatCore::SetPattern23Mode(HBCore::Pattern23Mode mode) {
    m_pattern23Mode = mode;
}

void HeartbeatCore::SetReplaceMode(HBCore::ReplaceMode mode) {
    m_replaceMode = mode;
}

void HeartbeatCore::SetPattern23DynamicOffset(int offset) {
    m_pattern23DynamicOffset = offset;
}

void HeartbeatCore::SetEnable62Pattern(bool enable) {
    m_enable62Pattern = enable;
}

void HeartbeatCore::SetEnableCRC32AutoCalc(bool enable) {
    m_enableCRC32AutoCalc = enable;
}

void HeartbeatCore::SetUseSingleHeartbeatPacket(bool use) {
    m_useSingleHeartbeatPacket = use;
}

void HeartbeatCore::SetEnableWhitelist(bool enable) {
    m_enableWhitelist = enable;
}

void HeartbeatCore::SetEnableBlacklist(bool enable) {
    m_enableBlacklist = enable;
}

void HeartbeatCore::SetEnable09_62Complement(bool enable) {
    m_enable09_62Complement = enable;
}

void HeartbeatCore::SetSameAlgorithmMode(bool isSameAlgorithm) {
    m_isSameAlgorithm = isSameAlgorithm;
}

void HeartbeatCore::SetDataOrderMode(HBCore::DataOrderMode mode) {
    m_dataOrderMode = mode;
}

// ==================== JSON序列化 ====================
Json::Value HeartbeatCore::ToJson() const {
    Json::Value root;

    // 基础配置
    root["pattern23Mode"] = static_cast<int>(m_pattern23Mode);
    root["replaceMode"] = static_cast<int>(m_replaceMode);
    root["pattern23DynamicOffset"] = m_pattern23DynamicOffset;
    root["enable62Pattern"] = m_enable62Pattern;
    root["enableCRC32AutoCalc"] = m_enableCRC32AutoCalc;
    root["useSingleHeartbeatPacket"] = m_useSingleHeartbeatPacket;
    root["enable09_62Complement"] = m_enable09_62Complement;
    root["isSameAlgorithm"] = m_isSameAlgorithm;
    root["enableWhitelist"] = m_enableWhitelist;
    root["enableBlacklist"] = m_enableBlacklist;

    // Pattern23偏移规则
    Json::Value p23Rules(Json::arrayValue);
    for (const auto& rule : m_pattern23OffsetRules) {
        Json::Value r;
        r["id"] = rule.id;
        r["name"] = rule.name;
        r["offset"] = rule.offset;
        r["length"] = rule.length;
        r["enabled"] = rule.enabled;
        p23Rules.append(r);
    }
    root["pattern23OffsetRules"] = p23Rules;

    // Pattern09偏移规则
    Json::Value p09Rules(Json::arrayValue);
    for (const auto& rule : m_pattern09OffsetRules) {
        Json::Value r;
        r["id"] = rule.id;
        r["name"] = rule.name;
        r["offset"] = rule.offset;
        r["length"] = rule.length;
        r["enabled"] = rule.enabled;
        p09Rules.append(r);
    }
    root["pattern09OffsetRules"] = p09Rules;

    // 白名单规则
    Json::Value wlRules(Json::arrayValue);
    for (const auto& rule : m_whitelistRules) {
        Json::Value r;
        r["id"] = rule.id;
        r["name"] = rule.name;
        r["searchPattern"] = rule.searchPattern;
        r["usePatternSearch"] = rule.usePatternSearch;
        r["isEnabled"] = rule.isEnabled;
        wlRules.append(r);
    }
    root["whitelistRules"] = wlRules;

    // 黑名单规则
    Json::Value blRules(Json::arrayValue);
    for (const auto& rule : m_blacklistRules) {
        Json::Value r;
        r["id"] = rule.id;
        r["name"] = rule.name;
        r["searchPattern"] = rule.searchPattern;
        r["usePatternSearch"] = rule.usePatternSearch;
        r["isEnabled"] = rule.isEnabled;
        blRules.append(r);
    }
    root["blacklistRules"] = blRules;

    // 替换数量规则
    Json::Value rcRules(Json::arrayValue);
    for (const auto& rule : m_replaceCountRules) {
        Json::Value r;
        r["id"] = rule.id;
        r["name"] = rule.name;
        r["enabled"] = rule.enabled;
        r["replaceLimit"] = rule.replaceLimit;
        r["postLimitStrategy"] = static_cast<int>(rule.postLimitStrategy);
        r["clearPoolOnReach"] = rule.clearPoolOnReach;
        r["applyWpeOnOriginalSegment"] = rule.applyWpeOnOriginalSegment;
        r["resetCountOnDisconnect"] = rule.resetCountOnDisconnect;
        r["postLimitFakeN"] = rule.postLimitFakeN;
        r["postLimitOriginalN"] = rule.postLimitOriginalN;
        rcRules.append(r);
    }
    root["replaceCountRules"] = rcRules;

    // 固定包选择
    Json::Value fpSelections(Json::objectValue);
    for (const auto& [length, selection] : m_fixedPacketByLength) {
        Json::Value s;
        s["packetId"] = selection.packetId;
        s["gameID"] = selection.gameID;
        fpSelections[std::to_string(length)] = s;
    }
    root["fixedPacketSelections"] = fpSelections;

    return root;
}

bool HeartbeatCore::FromJson(const Json::Value& root) {
    try {
        // 基础配置
        if (root.isMember("pattern23Mode"))
            m_pattern23Mode = static_cast<HBCore::Pattern23Mode>(root["pattern23Mode"].asInt());
        if (root.isMember("replaceMode"))
            m_replaceMode = static_cast<HBCore::ReplaceMode>(root["replaceMode"].asInt());
        if (root.isMember("pattern23DynamicOffset"))
            m_pattern23DynamicOffset = root["pattern23DynamicOffset"].asInt();
        if (root.isMember("enable62Pattern"))
            m_enable62Pattern = root["enable62Pattern"].asBool();
        if (root.isMember("enableCRC32AutoCalc"))
            m_enableCRC32AutoCalc = root["enableCRC32AutoCalc"].asBool();
        if (root.isMember("useSingleHeartbeatPacket"))
            m_useSingleHeartbeatPacket = root["useSingleHeartbeatPacket"].asBool();
        if (root.isMember("enable09_62Complement"))
            m_enable09_62Complement = root["enable09_62Complement"].asBool();
        if (root.isMember("isSameAlgorithm"))
            m_isSameAlgorithm = root["isSameAlgorithm"].asBool();
        if (root.isMember("enableWhitelist"))
            m_enableWhitelist = root["enableWhitelist"].asBool();
        if (root.isMember("enableBlacklist"))
            m_enableBlacklist = root["enableBlacklist"].asBool();

        // Pattern23偏移规则
        if (root.isMember("pattern23OffsetRules")) {
            m_pattern23OffsetRules.clear();
            const Json::Value& rules = root["pattern23OffsetRules"];
            for (const auto& r : rules) {
                HBCore::Pattern23OffsetRule rule;
                rule.id = r["id"].asInt();
                rule.name = r["name"].asString();
                rule.offset = r["offset"].asInt();
                rule.length = r["length"].asInt();
                rule.enabled = r["enabled"].asBool();
                m_pattern23OffsetRules.push_back(rule);
                if (rule.id >= m_nextPattern23OffsetRuleId)
                    m_nextPattern23OffsetRuleId = rule.id + 1;
            }
        }

        // Pattern09偏移规则
        if (root.isMember("pattern09OffsetRules")) {
            m_pattern09OffsetRules.clear();
            const Json::Value& rules = root["pattern09OffsetRules"];
            for (const auto& r : rules) {
                HBCore::Pattern09OffsetRule rule;
                rule.id = r["id"].asInt();
                rule.name = r["name"].asString();
                rule.offset = r["offset"].asInt();
                rule.length = r["length"].asInt();
                rule.enabled = r["enabled"].asBool();
                m_pattern09OffsetRules.push_back(rule);
                if (rule.id >= m_nextPattern09OffsetRuleId)
                    m_nextPattern09OffsetRuleId = rule.id + 1;
            }
        }

        // 白名单规则
        if (root.isMember("whitelistRules")) {
            m_whitelistRules.clear();
            const Json::Value& rules = root["whitelistRules"];
            for (const auto& r : rules) {
                HBCore::WhitelistRule rule;
                rule.id = r["id"].asInt();
                rule.name = r["name"].asString();
                rule.searchPattern = r["searchPattern"].asString();
                rule.usePatternSearch = r["usePatternSearch"].asBool();
                rule.isEnabled = r["isEnabled"].asBool();
                m_whitelistRules.push_back(rule);
                if (rule.id >= m_nextWhitelistRuleId)
                    m_nextWhitelistRuleId = rule.id + 1;
            }
        }

        // 黑名单规则
        if (root.isMember("blacklistRules")) {
            m_blacklistRules.clear();
            const Json::Value& rules = root["blacklistRules"];
            for (const auto& r : rules) {
                HBCore::BlacklistRule rule;
                rule.id = r["id"].asInt();
                rule.name = r["name"].asString();
                rule.searchPattern = r["searchPattern"].asString();
                rule.usePatternSearch = r["usePatternSearch"].asBool();
                rule.isEnabled = r["isEnabled"].asBool();
                m_blacklistRules.push_back(rule);
                if (rule.id >= m_nextBlacklistRuleId)
                    m_nextBlacklistRuleId = rule.id + 1;
            }
        }

        // 替换数量规则
        if (root.isMember("replaceCountRules")) {
            m_replaceCountRules.clear();
            const Json::Value& rules = root["replaceCountRules"];
            for (const auto& r : rules) {
                HBCore::ReplaceCountRule rule;
                rule.id = r["id"].asInt();
                rule.name = r["name"].asString();
                rule.enabled = r["enabled"].asBool();
                rule.replaceLimit = r["replaceLimit"].asInt();
                rule.postLimitStrategy = static_cast<HBCore::PostLimitStrategy>(r["postLimitStrategy"].asInt());
                rule.clearPoolOnReach = r["clearPoolOnReach"].asBool();
                rule.applyWpeOnOriginalSegment = r["applyWpeOnOriginalSegment"].asBool();
                rule.resetCountOnDisconnect = r["resetCountOnDisconnect"].asBool();
                rule.postLimitFakeN = r["postLimitFakeN"].asInt();
                rule.postLimitOriginalN = r["postLimitOriginalN"].asInt();
                m_replaceCountRules.push_back(rule);
                if (rule.id >= m_nextReplaceCountRuleId)
                    m_nextReplaceCountRuleId = rule.id + 1;
            }
        }

        // 固定包选择
        if (root.isMember("fixedPacketSelections")) {
            m_fixedPacketByLength.clear();
            const Json::Value& selections = root["fixedPacketSelections"];
            for (const auto& key : selections.getMemberNames()) {
                int length = std::stoi(key);
                HBCore::FixedPacketSelection selection;
                selection.packetId = selections[key]["packetId"].asInt();
                selection.gameID = selections[key]["gameID"].asString();
                m_fixedPacketByLength[length] = selection;
            }
        }

        return true;
    }
    catch (const std::exception& e) {
        AB_LOG_ERROR("[HeartbeatCore] FromJson异常: " + std::string(e.what()));
        return false;
    }
}

bool HeartbeatCore::SaveConfig(const std::string& filePath) {
    try {
        Json::Value root = ToJson();

        Json::StreamWriterBuilder builder;
        builder["indentation"] = "  ";
        std::string jsonStr = Json::writeString(builder, root);

        std::ofstream file(filePath);
        if (!file.is_open()) {
            AB_LOG_ERROR("[HeartbeatCore] 无法打开配置文件进行写入: " + filePath);
            return false;
        }

        file << jsonStr;
        file.close();

        AB_LOG_INFO("[HeartbeatCore] 配置已保存: " + filePath);
        return true;
    }
    catch (const std::exception& e) {
        AB_LOG_ERROR("[HeartbeatCore] SaveConfig异常: " + std::string(e.what()));
        return false;
    }
}

bool HeartbeatCore::LoadConfig(const std::string& filePath) {
    try {
        std::ifstream file(filePath);
        if (!file.is_open()) {
            AB_LOG_WARNING("[HeartbeatCore] 配置文件不存在: " + filePath);
            return false;
        }

        Json::Value root;
        Json::CharReaderBuilder builder;
        std::string errors;

        if (!Json::parseFromStream(builder, file, &root, &errors)) {
            AB_LOG_ERROR("[HeartbeatCore] JSON解析失败: " + errors);
            return false;
        }

        file.close();

        if (!FromJson(root)) {
            return false;
        }

        AB_LOG_INFO("[HeartbeatCore] 配置已加载: " + filePath);
        return true;
    }
    catch (const std::exception& e) {
        AB_LOG_ERROR("[HeartbeatCore] LoadConfig异常: " + std::string(e.what()));
        return false;
    }
}
