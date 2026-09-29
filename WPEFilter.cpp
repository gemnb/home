#include "WPEFilter.h"
#include "CloudIntegration.h"
#include "DatabaseManager.h"
#include "Logger.h"
#include <fstream>
#include <cctype>
#include <random>
#include <json/json.h>

#define NOMINMAX
#include <Windows.h>

extern DatabaseManager* g_database;

namespace WPEFilter {

    namespace {
        uint8_t GetRandomByte() {
            static thread_local std::mt19937 generator(std::random_device{}());
            static thread_local std::uniform_int_distribution<int> distribution(0, 255);
            return static_cast<uint8_t>(distribution(generator));
        }
    }

    // ==================== 宸ュ叿鍑芥暟瀹炵幇 ====================

    std::vector<uint8_t> HexStringToBytes(const std::string& hex) {
        std::vector<uint8_t> result;
        std::string cleanHex;

        // 娓呯悊杈撳叆锛氱Щ闄ょ┖鏍煎拰鍏朵粬闈炲崄鍏繘鍒跺瓧绗?
        for (char c : hex) {
            if (std::isxdigit(c)) {
                cleanHex += std::toupper(c);
            }
        }

        // 姣忎袱涓瓧绗﹁浆鎹负涓€涓瓧鑺?
        for (size_t i = 0; i + 1 < cleanHex.length(); i += 2) {
            uint8_t byte = 0;
            if (ParseHexByte(cleanHex.substr(i, 2), byte)) {
                result.push_back(byte);
            }
        }

        return result;
    }

    std::string BytesToHexString(const std::vector<uint8_t>& bytes) {
        std::stringstream ss;
        for (size_t i = 0; i < bytes.size(); i++) {
            ss << std::setw(2) << std::setfill('0') << std::hex << std::uppercase
               << static_cast<int>(bytes[i]);
            if (i < bytes.size() - 1) {
                ss << " ";
            }
        }
        return ss.str();
    }

    bool ParseHexByte(const std::string& hex, uint8_t& result) {
        if (hex.length() != 2) return false;

        int high = -1, low = -1;

        char c1 = std::toupper(hex[0]);
        char c2 = std::toupper(hex[1]);

        if (c1 >= '0' && c1 <= '9') high = c1 - '0';
        else if (c1 >= 'A' && c1 <= 'F') high = 10 + (c1 - 'A');
        else return false;

        if (c2 >= '0' && c2 <= '9') low = c2 - '0';
        else if (c2 >= 'A' && c2 <= 'F') low = 10 + (c2 - 'A');
        else return false;

        result = static_cast<uint8_t>((high << 4) | low);
        return true;
    }

    // 瑙ｆ瀽甯﹂€氶厤绗︾殑鍗佸叚杩涘埗瀛楄妭锛堟敮鎸?*8, 8*, **, ??锛?
    // 杩斿洖鍊硷細value=鍖归厤鍊硷紝mask=鎺╃爜锛?xFF=瀹屾暣鍖归厤锛?x0F=鍙尮閰嶄綆4浣嶏紝0xF0=鍙尮閰嶉珮4浣嶏紝0x00=瀹屽叏閫氶厤锛?
    bool ParseHexByteWithWildcard(const std::string& hex, uint8_t& value, uint8_t& mask) {
        if (hex.length() != 2) return false;

        char c1 = std::toupper(hex[0]);
        char c2 = std::toupper(hex[1]);

        // 妫€鏌ユ槸鍚︽槸瀹屾暣閫氶厤绗??? 鎴?**
        if ((c1 == '?' && c2 == '?') || (c1 == '*' && c2 == '*')) {
            value = 0x00;
            mask = 0x00;  // 瀹屽叏涓嶅尮閰嶏紙浠绘剰鍊硷級
            return true;
        }

        // 妫€鏌ユ槸鍚︽槸鍗婂瓧鑺傞€氶厤绗?
        bool highWildcard = (c1 == '*' || c1 == '?');
        bool lowWildcard = (c2 == '*' || c2 == '?');

        int high = 0, low = 0;

        // 瑙ｆ瀽楂?浣?
        if (highWildcard) {
            high = 0;  // 閫氶厤绗﹂粯璁や负0
        } else {
            if (c1 >= '0' && c1 <= '9') high = c1 - '0';
            else if (c1 >= 'A' && c1 <= 'F') high = 10 + (c1 - 'A');
            else return false;
        }

        // 瑙ｆ瀽浣?浣?
        if (lowWildcard) {
            low = 0;  // 閫氶厤绗﹂粯璁や负0
        } else {
            if (c2 >= '0' && c2 <= '9') low = c2 - '0';
            else if (c2 >= 'A' && c2 <= 'F') low = 10 + (c2 - 'A');
            else return false;
        }

        // 缁勫悎瀛楄妭鍊?
        value = static_cast<uint8_t>((high << 4) | low);

        // 璁＄畻鎺╃爜
        if (highWildcard && lowWildcard) {
            mask = 0x00;  // **锛氬畬鍏ㄩ€氶厤
        } else if (highWildcard && !lowWildcard) {
            mask = 0x0F;  // *8锛氬彧鍖归厤浣?浣?
        } else if (!highWildcard && lowWildcard) {
            mask = 0xF0;  // 8*锛氬彧鍖归厤楂?浣?
        } else {
            mask = 0xFF;  // AB锛氬畬鍏ㄥ尮閰?
        }

        return true;
    }

    std::string GetFilterModeName(FilterMode mode) {
        switch (mode) {
            case FilterMode::Normal: return "Normal";
            case FilterMode::Advanced: return "Advanced";
            default: return "Unknown";
        }
    }

    std::string GetFilterActionName(FilterAction action) {
        switch (action) {
            case FilterAction::Replace: return "Replace";
            case FilterAction::Intercept: return "Intercept";
            case FilterAction::NoModify_Display: return "NoModify(Show)";
            case FilterAction::NoModify_NoDisplay: return "NoModify(Hide)";
            case FilterAction::Change: return "Change";
            case FilterAction::None: return "None";
            default: return "Unknown";
        }
    }

    std::string GetFilterTargetName(const FilterTarget& target) {
        std::string result;
        if (target.applyToCollector) result += "Collector";
        if (target.applyToCollector && target.applyToHeartbeat) result += "+";
        if (target.applyToHeartbeat) result += "Heartbeat";
        if (result.empty()) result = "None";
        return result;
    }

    bool ValidateSearchPattern(const std::string& pattern, std::string& errorMsg) {
        if (pattern.empty()) {
            errorMsg = "Search pattern cannot be empty";
            return false;
        }

        std::stringstream ss(pattern);
        std::string part;

        while (std::getline(ss, part, ',')) {
            // Trim whitespace
            size_t start = part.find_first_not_of(' ');
            size_t end = part.find_last_not_of(' ');
            if (start == std::string::npos) continue;
            part = part.substr(start, end - start + 1);

            // Check format: position|hex
            size_t pipePos = part.find('|');
            if (pipePos == std::string::npos) {
                errorMsg = "Format error: missing '|' separator at: " + part;
                return false;
            }

            std::string posStr = part.substr(0, pipePos);
            std::string hexStr = part.substr(pipePos + 1);

            // Validate position is a number
            try {
                int pos = std::stoi(posStr);
                if (pos < 0) {
                    errorMsg = "Position cannot be negative: " + posStr;
                    return false;
                }
            } catch (...) {
                errorMsg = "Invalid position format: " + posStr;
                return false;
            }

            // Validate hex
            if (hexStr.length() != 2) {
                errorMsg = "Hex must be 2 digits: " + hexStr;
                return false;
            }
            for (char c : hexStr) {
                if (!std::isxdigit(c)) {
                    errorMsg = "Invalid hex character: " + hexStr;
                    return false;
                }
            }
        }

        return true;
    }

    bool ValidateModifyPattern(const std::string& pattern, std::string& errorMsg) {
        // Modify pattern can be empty (means no modification)
        if (pattern.empty()) return true;

        // Use same validation as search pattern
        return ValidateSearchPattern(pattern, errorMsg);
    }

    // ==================== FilterManager 瀹炵幇 ====================

    int FilterManager::AddFilter(const FilterInfo& filter) {
        if (!CloudIntegration::IsLoggedIn()) {
            AB_LOG_ERROR("[WPE] AddFilter denied: not logged in");
            return -1;
        }
        {
            std::string denyReason;
            if (!CloudIntegration::Checkpoint(3110, "{\"op\":\"WPE.AddFilter\"}", denyReason)) {
                AB_LOG_ERROR("[WPE] AddFilter denied: " + denyReason);
                return -1;
            }
        }

        std::lock_guard<std::mutex> lock(m_mutex);

        FilterInfo newFilter = filter;
        newFilter.id = m_nextId++;
        newFilter.executionCount = 0;

        m_filters.push_back(newFilter);

        // 馃敟 璁板綍鎴愬姛娣诲姞
        AB_LOG_INFO("[WPE婊ら暅] 娣诲姞婊ら暅鎴愬姛: " + newFilter.name + " (ID: " + std::to_string(newFilter.id) + ")");

        return newFilter.id;
    }

    bool FilterManager::UpdateFilter(int id, const FilterInfo& filter) {
        if (!CloudIntegration::IsLoggedIn()) {
            AB_LOG_ERROR("[WPE] UpdateFilter denied: not logged in");
            return false;
        }
        {
            std::string denyReason;
            std::string contextJson = std::string("{\"op\":\"WPE.UpdateFilter\",\"id\":") + std::to_string(id) + "}";
            if (!CloudIntegration::Checkpoint(3111, contextJson, denyReason)) {
                AB_LOG_ERROR("[WPE] UpdateFilter denied: " + denyReason);
                return false;
            }
        }

        std::lock_guard<std::mutex> lock(m_mutex);

        for (auto& f : m_filters) {
            if (f.id == id) {
                int oldId = f.id;
                int oldCount = f.executionCount;
                std::string oldCreateTime = f.createTime;

                f = filter;
                f.id = oldId;
                f.executionCount = oldCount;
                f.createTime = oldCreateTime;

                // 鏇存柊淇敼鏃堕棿
                auto now = std::chrono::system_clock::now();
                auto time = std::chrono::system_clock::to_time_t(now);
                struct tm timeinfo;
                localtime_s(&timeinfo, &time);
                char buf[64];
                strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
                f.lastModifyTime = buf;

                // 馃敟 璁板綍鎴愬姛鏇存柊
                AB_LOG_INFO("[WPE婊ら暅] 鏇存柊婊ら暅鎴愬姛: " + f.name + " (ID: " + std::to_string(id) + ")");

                return true;
            }
        }
        // 馃敟 璁板綍鎵句笉鍒版护闀?
        AB_LOG_WARNING("[WPE婊ら暅] 鏇存柊澶辫触锛屾壘涓嶅埌婊ら暅 ID: " + std::to_string(id));
        return false;
    }

    bool FilterManager::RemoveFilter(int id) {
        if (!CloudIntegration::IsLoggedIn()) {
            AB_LOG_ERROR("[WPE] RemoveFilter denied: not logged in");
            return false;
        }
        {
            std::string denyReason;
            std::string contextJson = std::string("{\"op\":\"WPE.RemoveFilter\",\"id\":") + std::to_string(id) + "}";
            if (!CloudIntegration::Checkpoint(3112, contextJson, denyReason)) {
                AB_LOG_ERROR("[WPE] RemoveFilter denied: " + denyReason);
                return false;
            }
        }

        std::lock_guard<std::mutex> lock(m_mutex);

        for (auto it = m_filters.begin(); it != m_filters.end(); ++it) {
            if (it->id == id) {
                m_filters.erase(it);
                // 馃敟 璁板綍鎴愬姛鍒犻櫎
                AB_LOG_INFO("[WPE婊ら暅] 鍒犻櫎婊ら暅鎴愬姛 (ID: " + std::to_string(id) + ")");
                return true;
            }
        }
        // 馃敟 璁板綍鎵句笉鍒版护闀?
        AB_LOG_WARNING("[WPE婊ら暅] 鍒犻櫎澶辫触锛屾壘涓嶅埌婊ら暅 ID: " + std::to_string(id));
        return false;
    }

    bool FilterManager::EnableFilter(int id, bool enable) {
        if (!CloudIntegration::IsLoggedIn()) {
            return false;
        }
        {
            std::string denyReason;
            std::string contextJson = std::string("{\"op\":\"WPE.EnableFilter\",\"id\":") + std::to_string(id) +
                ",\"enable\":" + (enable ? "true" : "false") + "}";
            if (!CloudIntegration::Checkpoint(3113, contextJson, denyReason)) {
                return false;
            }
        }

        std::lock_guard<std::mutex> lock(m_mutex);

        for (auto& f : m_filters) {
            if (f.id == id) {
                f.isEnabled = enable;
                return true;
            }
        }
        return false;
    }

    void FilterManager::ClearAllFilters() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_filters.clear();
    }

    FilterInfo* FilterManager::GetFilter(int id) {
        std::lock_guard<std::mutex> lock(m_mutex);

        for (auto& f : m_filters) {
            if (f.id == id) {
                return &f;
            }
        }
        return nullptr;
    }

    std::vector<FilterInfo> FilterManager::GetAllFilters() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_filters;
    }

    int FilterManager::GetFilterCount() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return static_cast<int>(m_filters.size());
    }

    bool FilterManager::MoveFilterUp(int id, std::string* outError) {
        if (outError) outError->clear();

        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_filters.empty()) {
            if (outError) {
                *outError = "当前没有可移动的WPE滤镜";
            }
            return false;
        }

        if (!m_filters.empty() && m_filters.front().id == id) {
            if (outError) {
                *outError = "当前滤镜已经在最上方";
            }
            return false;
        }

        for (size_t i = 1; i < m_filters.size(); i++) {
            if (m_filters[i].id == id) {
                std::swap(m_filters[i], m_filters[i - 1]);
                return true;
            }
        }
        if (outError) {
            *outError = "未找到要上移的WPE滤镜";
        }
        return false;
    }

    bool FilterManager::MoveFilterDown(int id, std::string* outError) {
        if (outError) outError->clear();

        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_filters.empty()) {
            if (outError) {
                *outError = "当前没有可移动的WPE滤镜";
            }
            return false;
        }

        if (!m_filters.empty() && m_filters.back().id == id) {
            if (outError) {
                *outError = "当前滤镜已经在最下方";
            }
            return false;
        }

        for (size_t i = 0; i + 1 < m_filters.size(); i++) {
            if (m_filters[i].id == id) {
                std::swap(m_filters[i], m_filters[i + 1]);
                return true;
            }
        }
        if (outError) {
            *outError = "未找到要下移的WPE滤镜";
        }
        return false;
    }

    bool FilterManager::MoveFilterToTop(int id) {
        std::lock_guard<std::mutex> lock(m_mutex);

        for (size_t i = 0; i < m_filters.size(); i++) {
            if (m_filters[i].id == id) {
                FilterInfo temp = m_filters[i];
                m_filters.erase(m_filters.begin() + i);
                m_filters.insert(m_filters.begin(), temp);
                return true;
            }
        }
        return false;
    }

    bool FilterManager::MoveFilterToBottom(int id) {
        std::lock_guard<std::mutex> lock(m_mutex);

        for (size_t i = 0; i < m_filters.size(); i++) {
            if (m_filters[i].id == id) {
                FilterInfo temp = m_filters[i];
                m_filters.erase(m_filters.begin() + i);
                m_filters.push_back(temp);
                return true;
            }
        }
        return false;
    }

    void FilterManager::ResetStatistics() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_totalExecutions = 0;
        for (auto& f : m_filters) {
            f.executionCount = 0;
            f.progression.executedCount = 0;
            f.progression.isDone = false;
        }
    }

    // ===== Parse functions =====

    std::vector<SearchCondition> FilterManager::ParseSearchPattern(const std::string& pattern) {
        std::vector<SearchCondition> conditions;

        std::stringstream ss(pattern);
        std::string part;

        while (std::getline(ss, part, ',')) {
            // Remove spaces
            part.erase(std::remove(part.begin(), part.end(), ' '), part.end());
            if (part.empty()) continue;

            size_t pipePos = part.find('|');
            if (pipePos == std::string::npos) continue;

            try {
                int pos = std::stoi(part.substr(0, pipePos));
                std::string hexStr = part.substr(pipePos + 1);

                // 浣跨敤鏂扮殑甯﹂€氶厤绗﹁В鏋愬嚱鏁帮紙鏀寔 *8, 8*, **, ??锛?
                uint8_t value, mask;
                if (ParseHexByteWithWildcard(hexStr, value, mask)) {
                    conditions.push_back(SearchCondition(pos, value, mask));
                }
            } catch (...) {
                continue;
            }
        }

        return conditions;
    }

    std::vector<Modification> FilterManager::ParseModifyPattern(const std::string& pattern) {
        std::vector<Modification> modifications;

        std::stringstream ss(pattern);
        std::string part;

        while (std::getline(ss, part, ',')) {
            part.erase(std::remove(part.begin(), part.end(), ' '), part.end());
            if (part.empty()) continue;

            size_t pipePos = part.find('|');
            if (pipePos == std::string::npos) continue;

            try {
                int pos = std::stoi(part.substr(0, pipePos));
                std::string valueText = part.substr(pipePos + 1);
                std::transform(valueText.begin(), valueText.end(), valueText.begin(), [](unsigned char c) {
                    return static_cast<char>(std::toupper(c));
                });
                if (valueText == "RR") {
                    modifications.push_back(Modification(pos, 0, true));
                    continue;
                }
                uint8_t value;
                if (ParseHexByte(valueText, value)) {
                    modifications.push_back(Modification(pos, value));
                }
            } catch (...) {
                continue;
            }
        }

        return modifications;
    }

    std::vector<int> FilterManager::ParsePositions(const std::string& positions) {
        std::vector<int> result;

        std::stringstream ss(positions);
        std::string part;

        while (std::getline(ss, part, ',')) {
            part.erase(std::remove(part.begin(), part.end(), ' '), part.end());
            if (part.empty()) continue;

            try {
                result.push_back(std::stoi(part));
            } catch (...) {
                continue;
            }
        }

        return result;
    }

    // ===== 鏉′欢妫€鏌?=====

    bool FilterManager::CheckFilterConditions(const FilterInfo& filter, const std::vector<uint8_t>& data, bool isRequest) {
        // 妫€鏌ュ惎鐢ㄧ姸鎬?
        if (!filter.isEnabled) {
            AB_LOG_INFO("[WPE璋冭瘯] 婊ら暅ID=" + std::to_string(filter.id) + " 鏈惎鐢紝璺宠繃");
            return false;
        }

        // 妫€鏌ユ暟鎹柟鍚?
        if (isRequest && !filter.direction.applyToRequest) {
            AB_LOG_INFO("[WPE调试] 滤镜ID=" + std::to_string(filter.id) + " 不对请求生效，跳过");
            return false;
        }
        if (!isRequest && !filter.direction.applyToResponse) {
            AB_LOG_INFO("[WPE璋冭瘯] 婊ら暅ID=" + std::to_string(filter.id) +
                         " 不对响应生效(applyToResponse=" + std::string(filter.direction.applyToResponse ? "true" : "false") + ")，跳过");
            return false;
        }

        // 妫€鏌ュ寘澶?
        if (filter.appointHeader && !CheckHeaderMatch(filter, data)) {
            AB_LOG_INFO("[WPE璋冭瘯] 婊ら暅ID=" + std::to_string(filter.id) + " 鍖呭ご涓嶅尮閰嶏紝璺宠繃");
            return false;
        }

        // 妫€鏌ラ暱搴?
        if (filter.appointLength && !CheckLengthMatch(filter, data)) {
            AB_LOG_INFO("[WPE璋冭瘯] 婊ら暅ID=" + std::to_string(filter.id) + " 闀垮害涓嶅尮閰嶏紝璺宠繃");
            return false;
        }

        AB_LOG_INFO("[WPE璋冭瘯] 婊ら暅ID=" + std::to_string(filter.id) + " 鏉′欢妫€鏌ラ€氳繃");
        return true;
    }

    bool FilterManager::CheckHeaderMatch(const FilterInfo& filter, const std::vector<uint8_t>& data) {
        if (filter.headerContent.empty()) return false;

        std::vector<uint8_t> headerBytes = HexStringToBytes(filter.headerContent);
        if (headerBytes.empty()) return false;

        if (data.size() < headerBytes.size()) return false;

        for (size_t i = 0; i < headerBytes.size(); i++) {
            if (data[i] != headerBytes[i]) return false;
        }

        return true;
    }

    bool FilterManager::CheckLengthMatch(const FilterInfo& filter, const std::vector<uint8_t>& data) {
        int len = static_cast<int>(data.size());
        return len >= filter.minLength && len <= filter.maxLength;
    }

    // ===== Match checking =====

    FilterMatchResult FilterManager::CheckFilterMatch_Normal(const FilterInfo& filter, const std::vector<uint8_t>& data) {
        FilterMatchResult result;

        std::vector<SearchCondition> conditions = ParseSearchPattern(filter.searchPattern);
        if (conditions.empty()) {
            result.matched = true;  // No search condition means match
            result.matchPositions.push_back(0);
            return result;
        }

        // Normal mode: check each condition (absolute position)
        for (const auto& cond : conditions) {
            if (cond.position < 0 || cond.position >= static_cast<int>(data.size())) {
                return result;  // Position out of range, no match
            }
            // 浣跨敤鎺╃爜姣旇緝锛氬鏋渕ask=0x00鍒欏畬鍏ㄩ€氶厤锛宮ask=0x0F鍙瘮杈冧綆4浣嶏紝mask=0xF0鍙瘮杈冮珮4浣嶏紝mask=0xFF瀹屽叏鍖归厤
            if ((data[cond.position] & cond.mask) != (cond.value & cond.mask)) {
                return result;  // Value mismatch
            }
        }

        result.matched = true;
        result.matchPositions.push_back(0);
        result.action = filter.action;
        return result;
    }

    FilterMatchResult FilterManager::CheckFilterMatch_Advanced(const FilterInfo& filter, const std::vector<uint8_t>& data) {
        FilterMatchResult result;

        std::vector<SearchCondition> conditions = ParseSearchPattern(filter.searchPattern);
        if (conditions.empty()) {
            result.matched = true;
            result.matchPositions.push_back(0);
            return result;
        }

        // Find first non-wildcard condition for base matching
        int firstNonWildcardIdx = -1;
        for (size_t i = 0; i < conditions.size(); i++) {
            if (conditions[i].mask != 0x00) {  // 涓嶆槸瀹屽叏閫氶厤绗?
                firstNonWildcardIdx = static_cast<int>(i);
                break;
            }
        }

        // If all conditions are wildcards, match from position 0
        if (firstNonWildcardIdx == -1) {
            result.matched = true;
            result.matchPositions.push_back(0);
            result.action = filter.action;
            return result;
        }

        const SearchCondition& firstCond = conditions[firstNonWildcardIdx];
        int basePosition = firstCond.position;

        // Advanced mode: search relative position matching
        for (size_t i = 0; i < data.size(); i++) {
            // 浣跨敤鎺╃爜姣旇緝绗竴涓潪閫氶厤绗︽潯浠?
            if ((data[i] & firstCond.mask) != (firstCond.value & firstCond.mask)) continue;

            bool allMatch = true;
            int lastCheckedIndex = static_cast<int>(i);

            for (size_t j = 0; j < conditions.size(); j++) {
                if (static_cast<int>(j) == firstNonWildcardIdx) continue;  // Already checked

                int checkIndex = static_cast<int>(i) + conditions[j].position - basePosition;

                if (checkIndex < 0 || checkIndex >= static_cast<int>(data.size())) {
                    allMatch = false;
                    break;
                }

                // 浣跨敤鎺╃爜姣旇緝锛氭敮鎸佸畬鏁撮€氶厤銆佸崐瀛楄妭閫氶厤鍜屽畬鍏ㄥ尮閰?
                if ((data[checkIndex] & conditions[j].mask) != (conditions[j].value & conditions[j].mask)) {
                    allMatch = false;
                    break;
                }

                lastCheckedIndex = std::max(lastCheckedIndex, checkIndex);
            }

            if (allMatch) {
                result.matched = true;
                result.matchPositions.push_back(static_cast<int>(i));

                // If starting from head, only find first match
                if (filter.startFrom == FilterStartFrom::Head) {
                    break;
                }

                // Continue search from match position
                i = lastCheckedIndex;
            }
        }

        if (result.matched) {
            result.action = filter.action;
        }

        return result;
    }

    // ===== 閫掕繘澶勭悊 =====

    uint8_t FilterManager::GetStepByte(uint8_t currentValue, int step, int& carryOut) {
        int newValue = currentValue + step;
        carryOut = 0;

        while (newValue > 255) {
            newValue -= 256;
            carryOut++;
        }
        while (newValue < 0) {
            newValue += 256;
            carryOut--;
        }

        return static_cast<uint8_t>(newValue);
    }

    // ===== 鏇挎崲澶勭悊 =====

    bool FilterManager::ApplyModifications(FilterInfo& filter, std::vector<uint8_t>& data, int baseOffset) {
        bool modified = false;
        std::vector<Modification> mods = ParseModifyPattern(filter.modifyPattern);

        for (const auto& mod : mods) {
            int actualPos = mod.position;

            // 濡傛灉浠庝綅缃紑濮嬶紝鍔犱笂鍩虹鍋忕Щ
            if (filter.startFrom == FilterStartFrom::Position) {
                actualPos += baseOffset;
            }

            if (actualPos >= 0 && actualPos < static_cast<int>(data.size())) {
                const uint8_t nextValue = mod.isRandom ? GetRandomByte() : mod.value;
                if (data[actualPos] != nextValue) {
                    data[actualPos] = nextValue;
                    modified = true;
                }
            }
        }

        return modified;
    }

    bool FilterManager::ApplyProgressions(FilterInfo& filter, std::vector<uint8_t>& data, int baseOffset) {
        if (!filter.progression.isEnabled) return false;
        if (filter.progression.positions.empty()) return false;

        bool modified = false;
        std::vector<int> positions = ParsePositions(filter.progression.positions);
        int step = filter.progression.step;

        for (int pos : positions) {
            int actualPos = pos;

            if (filter.startFrom == FilterStartFrom::Position) {
                actualPos += baseOffset;
            }

            if (actualPos < 0 || actualPos >= static_cast<int>(data.size())) continue;

            int carryCount = 0;
            uint8_t newValue = GetStepByte(data[actualPos],
                step * (filter.progression.executedCount + 1), carryCount);

            data[actualPos] = newValue;
            modified = true;
            filter.progression.isDone = true;

            // 澶勭悊杩涗綅
            if (filter.progression.enableCarry && carryCount > 0) {
                for (int i = 0; i < filter.progression.carryDigits; i++) {
                    int prevIndex = actualPos - (i + 1);
                    if (prevIndex < 0) break;

                    uint8_t prevValue = data[prevIndex];
                    prevValue = GetStepByte(prevValue, carryCount, carryCount);
                    data[prevIndex] = prevValue;

                    if (carryCount == 0) break;
                }
            }
        }

        return modified;
    }

    bool FilterManager::ApplyReplace_Normal(FilterInfo& filter, std::vector<uint8_t>& data) {
        bool modified = false;

        // 搴旂敤淇敼
        if (!filter.modifyPattern.empty()) {
            modified |= ApplyModifications(filter, data, 0);
        }

        // 搴旂敤閫掕繘
        if (filter.progression.isEnabled) {
            modified |= ApplyProgressions(filter, data, 0);
        }

        return modified;
    }

    bool FilterManager::ApplyReplace_Advanced(FilterInfo& filter, std::vector<uint8_t>& data, const std::vector<int>& matchPositions) {
        bool modified = false;

        for (int matchPos : matchPositions) {
            // 搴旂敤淇敼
            if (!filter.modifyPattern.empty()) {
                modified |= ApplyModifications(filter, data, matchPos);
            }

            // 搴旂敤閫掕繘
            if (filter.progression.isEnabled) {
                modified |= ApplyProgressions(filter, data, matchPos);
            }
        }

        return modified;
    }

    std::vector<uint8_t> FilterManager::GenerateChangePacket(const FilterInfo& filter) {
        std::vector<uint8_t> result;

        std::vector<Modification> mods = ParseModifyPattern(filter.modifyPattern);
        if (mods.empty()) return result;

        // 鎵惧埌鏈€澶т綅缃?
        int maxPos = 0;
        for (const auto& mod : mods) {
            maxPos = std::max(maxPos, mod.position);
        }

        result.resize(maxPos + 1, 0);

        // 搴旂敤淇敼
        for (const auto& mod : mods) {
            if (mod.position >= 0 && mod.position < static_cast<int>(result.size())) {
                result[mod.position] = mod.isRandom ? GetRandomByte() : mod.value;
            }
        }

        return result;
    }

    // ===== 涓诲鐞嗗嚱鏁?=====

    FilterProcessResult FilterManager::ProcessPacket(
        std::vector<uint8_t>& data,
        const std::string& instanceId,
        bool isRequest,
        bool isCollector,
        FilterPriority currentPhase,
        const std::string& username,
        const std::vector<int>* userEnabledFilters)
    {
        FilterProcessResult result;
        std::vector<FilterInfo> filterSnapshot;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            filterSnapshot = m_filters;
        }

        // 馃敟 璋冭瘯鏃ュ織锛氳褰昉rocessPacket璋冪敤
//         AB_LOG_INFO("[WPE璋冭瘯] ProcessPacket琚皟鐢? isRequest=" + std::string(isRequest ? "true" : "false") +
//                      ", 婊ら暅鎬绘暟=" + std::to_string(filterSnapshot.size()) +
//                      ", 鏁版嵁闀垮害=" + std::to_string(data.size()));

        for (auto& filter : filterSnapshot) {
            // 馃敟 鐢ㄦ埛婊ら暅妯″紡锛氭鏌ュ綋鍓嶆护闀滄槸鍚﹀湪鐢ㄦ埛鍚敤鍒楄〃涓?
            if (userEnabledFilters != nullptr && !username.empty()) {
                // 鍚敤浜嗙敤鎴锋护闀滄ā寮忥紝妫€鏌ュ綋鍓嶆护闀淚D鏄惁鍦ㄧ敤鎴峰惎鐢ㄥ垪琛ㄤ腑
                bool isFilterEnabled = std::find(userEnabledFilters->begin(), userEnabledFilters->end(), filter.id) != userEnabledFilters->end();
                if (!isFilterEnabled) {
                    continue;  // 鐢ㄦ埛鏈惎鐢ㄦ婊ら暅锛岃烦杩?
                }
            }

            // 妫€鏌ョ洰鏍囧疄渚?
            if (!filter.target.applyToAllInstances) {
                // 濡傛灉涓嶆槸瀵规墍鏈夊疄渚嬬敓鏁堬紝妫€鏌ユ槸鍚﹀湪鐩爣瀹炰緥鍒楄〃涓?
                if (filter.target.targetInstanceIds.empty()) {
                    // 鍏煎鏃х増锛氬鏋滄病鏈夎缃畉argetInstanceIds锛屼娇鐢ㄦ棫鐨勯€昏緫
                    if (isCollector && !filter.target.applyToCollector) continue;
                    if (!isCollector && !filter.target.applyToHeartbeat) continue;
                } else {
                    // 鏂扮増锛氭锟斤拷瀹炰緥ID鏄惁鍦ㄧ洰鏍囧垪琛ㄤ腑
                    bool found = false;
                    for (const auto& targetId : filter.target.targetInstanceIds) {
                        if (targetId == instanceId) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) continue;
                }
            }
            // 濡傛灉 applyToAllInstances 涓?true锛屽垯璺宠繃瀹炰緥妫€鏌ワ紝瀵规墍鏈夊疄渚嬬敓鏁?

            // 妫€鏌ユ墽琛岄樁娈?
            const FilterPriority expectedPhase = isCollector ? filter.collectorPriority : filter.priority;
            if (expectedPhase != currentPhase) continue;

            // ===== 楂樼骇璁剧疆锛氭鏌ュ紑鍚?鍏抽棴瑙﹀彂鏉′欢锛堝湪鏁版嵁鏂瑰悜妫€鏌ヤ箣鍓嶏級=====
            // 馃敟 杩欐牱鍙互璁╄Е鍙戞潯浠剁洃鍚墍鏈夋暟鎹寘锛堣姹?鍝嶅簲锛夛紝鑰屾护闀滄湰韬彧瀵圭壒瀹氭柟鍚戠敓鏁?
            if (filter.advancedToggle.isEnabled && !username.empty()) {
                // 鑾峰彇褰撳墠璐﹀彿鐨勬护闀滅姸鎬侊紙閬垮厤姝婚攣锛岀洿鎺ュ湪杩欓噷鏌ヨ锛?
                std::string stateKey = instanceId + "|" + username + "|" + std::to_string(filter.id);
                bool accountFilterEnabled = filter.advancedToggle.defaultState;  // 榛樿鐘舵€?
                bool isEnableTriggerPacket = false;   // 鏄惁鏄Е鍙戝紑鍚殑鏁版嵁鍖?
                bool isDisableTriggerPacket = false;  // 鏄惁鏄Е鍙戝叧闂殑鏁版嵁鍖?

                {
                    std::lock_guard<std::mutex> stateLock(m_stateMutex);
                    auto it = m_accountFilterStates.find(stateKey);
                    if (it != m_accountFilterStates.end()) {
                        accountFilterEnabled = it->second;
                    } else {
                        // 棣栨璁块棶锛屽垵濮嬪寲涓洪粯璁ょ姸鎬?
                        m_accountFilterStates[stateKey] = accountFilterEnabled;
                    }
                }

                // 妫€鏌ユ槸鍚﹀尮閰嶅紑鍚Е鍙戞潯浠?
                if (filter.advancedToggle.enableTriggerEnabled &&
                    !filter.advancedToggle.enablePattern.empty() &&
                    CheckTogglePattern(filter.advancedToggle.enablePattern, data)) {
                    // 鍖归厤鍒板紑鍚Е鍙戞潯浠?
                    isEnableTriggerPacket = true;
                    if (!accountFilterEnabled) {
                        // 褰撳墠鏄叧闂姸鎬侊紝寮€鍚护闀?
                        std::lock_guard<std::mutex> stateLock(m_stateMutex);
                        m_accountFilterStates[stateKey] = true;
                        accountFilterEnabled = true;
                        AB_LOG_INFO("[WPE瑙﹀彂] 婊ら暅ID=" + std::to_string(filter.id) + " 琚Е鍙戝紑鍚?(璐﹀彿:" + username + ")");
                    }
                }

                // 妫€鏌ユ槸鍚﹀尮閰嶅叧闂Е鍙戞潯浠?
                if (filter.advancedToggle.disableTriggerEnabled &&
                    !filter.advancedToggle.disablePattern.empty() &&
                    CheckTogglePattern(filter.advancedToggle.disablePattern, data)) {
                    // 鍖归厤鍒板叧闂Е鍙戞潯浠?
                    isDisableTriggerPacket = true;
                    if (accountFilterEnabled) {
                        // 褰撳墠鏄紑鍚姸鎬侊紝鍏抽棴婊ら暅
                        std::lock_guard<std::mutex> stateLock(m_stateMutex);
                        m_accountFilterStates[stateKey] = false;
                        accountFilterEnabled = false;
                        AB_LOG_INFO("[WPE瑙﹀彂] 婊ら暅ID=" + std::to_string(filter.id) + " 琚Е鍙戝叧闂?(璐﹀彿:" + username + ")");
                    }
                }

                // 馃敟 濡傛灉鏄Е鍙戝寘锛屾牴鎹厤缃喅瀹氭槸鍚﹁烦杩囧悗缁鐞?
                if (isEnableTriggerPacket) {
                    if (!filter.advancedToggle.applyOnEnableTrigger) {
                        continue;  // 瑙﹀彂寮€鍚椂涓嶅瑙﹀彂鍖呮湰韬墽琛屾护闀?
                    }
                    // 鍚﹀垯缁х画鎵ц锛屽瑙﹀彂鍖呬篃搴旂敤婊ら暅
                }

                if (isDisableTriggerPacket) {
                    if (!filter.advancedToggle.applyOnDisableTrigger) {
                        continue;  // 瑙﹀彂鍏抽棴鏃朵笉瀵硅Е鍙戝寘鏈韩鎵ц婊ら暅
                    }
                    // 鍚﹀垯缁х画鎵ц锛屽瑙﹀彂鍖呬篃搴旂敤婊ら暅
                }

                // 馃敟 濡傛灉涓嶆槸瑙﹀彂鍖咃紝妫€鏌ヨ处鍙风骇鍒殑婊ら暅鐘舵€?
                if (!isEnableTriggerPacket && !isDisableTriggerPacket && !accountFilterEnabled) {
                    continue;  // 璇ヨ处鍙风殑婊ら暅澶勪簬鍏抽棴鐘舵€侊紝璺宠繃
                }
            }

            // 妫€鏌ユ潯浠讹紙鍖呮嫭鏁版嵁鏂瑰悜銆佸寘澶淬€侀暱搴︾瓑锛?
            if (!CheckFilterConditions(filter, data, isRequest)) continue;

            // 鎵ц鍖归厤妫€鏌?
            FilterMatchResult matchResult;
            if (filter.mode == FilterMode::Normal) {
                matchResult = CheckFilterMatch_Normal(filter, data);
            } else {
                matchResult = CheckFilterMatch_Advanced(filter, data);
            }

            if (!matchResult.matched) continue;

            // 鏍规嵁鍔ㄤ綔鎵ц
            switch (filter.action) {
                case FilterAction::Replace:
                {
                    filter.progression.isDone = false;

                    bool modified = false;
                    if (filter.mode == FilterMode::Normal) {
                        modified = ApplyReplace_Normal(filter, data);
                    } else {
                        modified = ApplyReplace_Advanced(filter, data, matchResult.matchPositions);
                    }

                    if (modified) {
                        result.modified = true;
                        result.modifiedData = data;
                    }

                    if (filter.progression.isDone && filter.progression.isContinuous) {
                        filter.progression.executedCount++;
                    }
                    break;
                }

                case FilterAction::Change:
                {
                    filter.progression.isDone = false;

                    std::vector<uint8_t> newData = GenerateChangePacket(filter);
                    if (!newData.empty()) {
                        data = newData;
                        result.modified = true;
                        result.modifiedData = data;
                    }

                    if (filter.progression.isDone && filter.progression.isContinuous) {
                        filter.progression.executedCount++;
                    }
                    break;
                }

                case FilterAction::Intercept:
                    result.intercepted = true;
                    result.shouldDisplay = false;
                    break;

                case FilterAction::NoModify_Display:
                    result.shouldDisplay = true;
                    break;

                case FilterAction::NoModify_NoDisplay:
                    result.shouldDisplay = false;
                    break;

                default:
                    break;
            }

            // 鏇存柊缁熻
            filter.executionCount++;
            m_totalExecutions.fetch_add(1, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                auto liveIt = std::find_if(m_filters.begin(), m_filters.end(),
                    [&filter](const FilterInfo& liveFilter) {
                        return liveFilter.id == filter.id;
                    });
                if (liveIt != m_filters.end()) {
                    liveIt->executionCount++;
                    if (filter.progression.isDone && filter.progression.isContinuous) {
                        liveIt->progression.executedCount++;
                    }
                }
            }
            result.processed = true;
            result.filterName = filter.name;

            // Persist per-user execution counts for the remote user filter console.
            if (userEnabledFilters != nullptr && !username.empty() && !instanceId.empty() && g_database) {
                g_database->IncrementFilterExecutionCount(instanceId, username, filter.id);
            }

            // Generate log message
            std::stringstream ss;
            ss << "[WPE Filter] " << GetFilterActionName(filter.action) << ": " << filter.name
               << " | Mode: " << GetFilterModeName(filter.mode)
               << " | DataLen: " << data.size();
            if (!matchResult.matchPositions.empty()) {
                ss << " | MatchPos: " << matchResult.matchPositions.size();
            }
            result.logMessage = ss.str();

            // 濡傛灉鏄嫤鎴姩浣滐紝绔嬪嵆杩斿洖
            if (filter.action == FilterAction::Intercept) {
                result.intercepted = true;
                return result;
            }
            if (filter.action == FilterAction::NoModify_Display ||
                filter.action == FilterAction::NoModify_NoDisplay) {
                return result;
            }
        }

        return result;
    }

    // ===== 璐﹀彿绾у埆鐘舵€佺鐞?=====

    std::string FilterManager::MakeStateKey(const std::string& instanceId, const std::string& username, int filterId) {
        return instanceId + "|" + username + "|" + std::to_string(filterId);
    }

    bool FilterManager::GetAccountFilterState(const std::string& instanceId, const std::string& username, int filterId) {
        std::lock_guard<std::mutex> lock(m_stateMutex);

        std::string key = MakeStateKey(instanceId, username, filterId);
        auto it = m_accountFilterStates.find(key);

        if (it != m_accountFilterStates.end()) {
            return it->second;
        }

        // 濡傛灉娌℃湁鎵惧埌鐘舵€佽褰曪紝浣跨敤榛樿鐘舵€?
        FilterInfo* filter = const_cast<FilterManager*>(this)->GetFilter(filterId);
        if (filter && filter->advancedToggle.isEnabled) {
            // 鍒濆鍖栦负榛樿鐘舵€?
            bool defaultState = filter->advancedToggle.defaultState;
            m_accountFilterStates[key] = defaultState;
            return defaultState;
        }

        return true;  // 濡傛灉娌℃湁鍚敤楂樼骇璁剧疆锛岄粯璁や负寮€鍚?
    }

    void FilterManager::SetAccountFilterState(const std::string& instanceId, const std::string& username, int filterId, bool state) {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        std::string key = MakeStateKey(instanceId, username, filterId);
        m_accountFilterStates[key] = state;
    }

    void FilterManager::ResetAccountFilterStates() {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_accountFilterStates.clear();
    }

    void FilterManager::ResetUserFilterStates(const std::string& instanceId, const std::string& username) {
        std::lock_guard<std::mutex> stateLock(m_stateMutex);

        // 鏋勫缓璇ョ敤鎴风殑鐘舵€侀敭鍓嶇紑锛歩nstanceId|username|
        std::string keyPrefix = instanceId + "|" + username + "|";

        // 閬嶅巻鎵€鏈夌姸鎬侊紝鍒犻櫎鍖归厤璇ョ敤鎴风殑鐘舵€?
        for (auto it = m_accountFilterStates.begin(); it != m_accountFilterStates.end(); ) {
            if (it->first.find(keyPrefix) == 0) {
                // 鎵惧埌璇ョ敤鎴风殑鐘舵€侊紝鍒犻櫎
                it = m_accountFilterStates.erase(it);
            } else {
                ++it;
            }
        }

        if (Logger::IsEnabled()) {
            OutputDebugStringA(("[WPE] 閲嶇疆鐢ㄦ埛婊ら暅鐘舵€? " + instanceId + " | " + username + "\n").c_str());
        }
    }

    // ===== 楂樼骇璁剧疆鐩稿叧 =====

    bool FilterManager::CheckTogglePattern(const std::string& pattern, const std::vector<uint8_t>& data) {
        if (pattern.empty()) return false;

        std::vector<SearchCondition> conditions = ParseSearchPattern(pattern);
        if (conditions.empty()) return false;

        // 馃敟 鏂伴€昏緫锛氭敮鎸佽嚜閫傚簲浣嶇疆鎼滅储
        // 濡傛灉鎵€鏈夋潯浠剁殑浣嶇疆閮芥槸杩炵画鐨勶紙0,1,2,3...锛夛紝鍒欒涓烘槸绠€鍗曟ā寮忥紝鏀寔鑷€傚簲鎼滅储
        // 鍚﹀垯浣跨敤鍥哄畾浣嶇疆鍖归厤锛堝吋瀹规棫閫昏緫锛?

        bool isSimplePattern = true;
        for (size_t i = 0; i < conditions.size(); ++i) {
            if (conditions[i].position != static_cast<int>(i)) {
                isSimplePattern = false;
                break;
            }
        }

        if (isSimplePattern) {
            // 馃敟 绠€鍗曟ā寮忥細鍦ㄦ暣涓暟鎹寘涓悳绱㈠尮閰嶏紙鑷€傚簲浣嶇疆锛?
            size_t patternLen = conditions.size();
            if (patternLen > data.size()) return false;

            // 閬嶅巻鏁版嵁鍖呯殑姣忎釜鍙兘鐨勮捣濮嬩綅缃?
            for (size_t startPos = 0; startPos <= data.size() - patternLen; ++startPos) {
                bool matched = true;

                // 妫€鏌ヤ粠褰撳墠浣嶇疆寮€濮嬫槸鍚﹀尮閰嶆暣涓ā寮?
                for (size_t i = 0; i < patternLen; ++i) {
                    const auto& cond = conditions[i];
                    uint8_t dataByte = data[startPos + i];

                    if (cond.mask == 0x00) {
                        // 瀹屽叏閫氶厤绗︼紝璺宠繃
                        continue;
                    } else if (cond.mask == 0xFF) {
                        // 瀹屾暣鍖归厤
                        if (dataByte != cond.value) {
                            matched = false;
                            break;
                        }
                    } else {
                        // 閮ㄥ垎鍖归厤
                        if ((dataByte & cond.mask) != (cond.value & cond.mask)) {
                            matched = false;
                            break;
                        }
                    }
                }

                if (matched) {
                    return true;  // 鎵惧埌鍖归厤
                }
            }

            return false;  // 娌℃湁鎵惧埌鍖归厤
        } else {
            // 馃敟 澶嶆潅妯″紡锛氫娇鐢ㄥ浐瀹氫綅缃尮閰嶏紙鍏煎鏃ч€昏緫锛?
            for (const auto& cond : conditions) {
                if (cond.position < 0 || cond.position >= static_cast<int>(data.size())) {
                    return false;
                }

                uint8_t dataByte = data[cond.position];

                if (cond.mask == 0x00) {
                    // 瀹屽叏閫氶厤绗︼紝璺宠繃
                    continue;
                } else if (cond.mask == 0xFF) {
                    // 瀹屾暣鍖归厤
                    if (dataByte != cond.value) return false;
                } else {
                    // 閮ㄥ垎鍖归厤
                    if ((dataByte & cond.mask) != (cond.value & cond.mask)) return false;
                }
            }

            return true;
        }
    }

    // ===== 瀵煎叆瀵煎嚭 =====

    std::string FilterManager::ExportToJson() {

        std::lock_guard<std::mutex> lock(m_mutex);

        Json::Value root(Json::objectValue);
        root["version"] = 2;
        Json::Value filters(Json::arrayValue);

        for (const auto& f : m_filters) {
            Json::Value item(Json::objectValue);
            item["id"] = f.id;
            item["name"] = f.name;
            item["webDisplayName"] = f.webDisplayName;
            item["isEnabled"] = f.isEnabled;
            item["executionCount"] = f.executionCount;

            item["mode"] = static_cast<int>(f.mode);
            item["action"] = static_cast<int>(f.action);
            item["startFrom"] = static_cast<int>(f.startFrom);

            // 浼績璺崇锛氭浛鎹㈠墠/鍚?
            item["priority"] = static_cast<int>(f.priority);
            // 閲囬泦绔細閲囬泦鍓?鍚?
            item["collectorPriority"] = static_cast<int>(f.collectorPriority);

            item["appointHeader"] = f.appointHeader;
            item["headerContent"] = f.headerContent;
            item["appointLength"] = f.appointLength;
            item["minLength"] = f.minLength;
            item["maxLength"] = f.maxLength;

            item["appointPort"] = f.appointPort;
            item["portContent"] = f.portContent;

            item["searchPattern"] = f.searchPattern;
            item["modifyPattern"] = f.modifyPattern;
            item["modifyRangeMode"] = f.useCustomModifyRange ? "custom" : "standard";
            item["modifyRangeMin"] = f.modifyRangeMin;
            item["modifyRangeMax"] = f.modifyRangeMax;

            item["applyToCollector"] = f.target.applyToCollector;
            item["applyToHeartbeat"] = f.target.applyToHeartbeat;

            item["applyToRequest"] = f.direction.applyToRequest;
            item["applyToResponse"] = f.direction.applyToResponse;

            item["progressionEnabled"] = f.progression.isEnabled;
            item["progressionContinuous"] = f.progression.isContinuous;
            item["progressionStep"] = f.progression.step;
            item["progressionEnableCarry"] = f.progression.enableCarry;
            item["progressionCarryDigits"] = f.progression.carryDigits;
            item["progressionPositions"] = f.progression.positions;

            // 楂樼骇璁剧疆锛氳处鍙风骇鍒姩鎬佸紑鍏?
            item["advancedToggleEnabled"] = f.advancedToggle.isEnabled;
            item["advancedToggleDefaultState"] = f.advancedToggle.defaultState;
            item["advancedToggleEnableTriggerEnabled"] = f.advancedToggle.enableTriggerEnabled;
            item["advancedToggleEnablePattern"] = f.advancedToggle.enablePattern;
            item["advancedToggleDisableTriggerEnabled"] = f.advancedToggle.disableTriggerEnabled;
            item["advancedToggleDisablePattern"] = f.advancedToggle.disablePattern;
            item["advancedToggleApplyOnEnableTrigger"] = f.advancedToggle.applyOnEnableTrigger;
            item["advancedToggleApplyOnDisableTrigger"] = f.advancedToggle.applyOnDisableTrigger;

            // 鏂扮増锛氱洰鏍囧疄渚嬪垪琛?
            Json::Value targetInstanceIds(Json::arrayValue);
            for (const auto& instanceId : f.target.targetInstanceIds) {
                targetInstanceIds.append(instanceId);
            }
            item["targetInstanceIds"] = targetInstanceIds;
            item["applyToAllInstances"] = f.target.applyToAllInstances;

            item["createTime"] = f.createTime;
            item["lastModifyTime"] = f.lastModifyTime;

            filters.append(item);
        }

        root["filters"] = filters;

        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        return Json::writeString(builder, root);
    }

    std::string FilterManager::ExportToJsonForLocalSave() {
        std::lock_guard<std::mutex> lock(m_mutex);

        Json::Value root(Json::objectValue);
        root["version"] = 2;
        Json::Value filters(Json::arrayValue);

        for (const auto& f : m_filters) {
            Json::Value item(Json::objectValue);
            item["id"] = f.id;
            item["name"] = f.name;
            item["webDisplayName"] = f.webDisplayName;
            item["isEnabled"] = f.isEnabled;
            item["executionCount"] = f.executionCount;

            item["mode"] = static_cast<int>(f.mode);
            item["action"] = static_cast<int>(f.action);
            item["startFrom"] = static_cast<int>(f.startFrom);
            item["priority"] = static_cast<int>(f.priority);
            item["collectorPriority"] = static_cast<int>(f.collectorPriority);

            item["appointHeader"] = f.appointHeader;
            item["headerContent"] = f.headerContent;
            item["appointLength"] = f.appointLength;
            item["minLength"] = f.minLength;
            item["maxLength"] = f.maxLength;
            item["appointPort"] = f.appointPort;
            item["portContent"] = f.portContent;
            item["searchPattern"] = f.searchPattern;
            item["modifyPattern"] = f.modifyPattern;
            item["modifyRangeMode"] = f.useCustomModifyRange ? "custom" : "standard";
            item["modifyRangeMin"] = f.modifyRangeMin;
            item["modifyRangeMax"] = f.modifyRangeMax;

            item["applyToCollector"] = f.target.applyToCollector;
            item["applyToHeartbeat"] = f.target.applyToHeartbeat;
            item["applyToRequest"] = f.direction.applyToRequest;
            item["applyToResponse"] = f.direction.applyToResponse;

            item["progressionEnabled"] = f.progression.isEnabled;
            item["progressionContinuous"] = f.progression.isContinuous;
            item["progressionStep"] = f.progression.step;
            item["progressionEnableCarry"] = f.progression.enableCarry;
            item["progressionCarryDigits"] = f.progression.carryDigits;
            item["progressionPositions"] = f.progression.positions;

            item["advancedToggleEnabled"] = f.advancedToggle.isEnabled;
            item["advancedToggleDefaultState"] = f.advancedToggle.defaultState;
            item["advancedToggleEnableTriggerEnabled"] = f.advancedToggle.enableTriggerEnabled;
            item["advancedToggleEnablePattern"] = f.advancedToggle.enablePattern;
            item["advancedToggleDisableTriggerEnabled"] = f.advancedToggle.disableTriggerEnabled;
            item["advancedToggleDisablePattern"] = f.advancedToggle.disablePattern;
            item["advancedToggleApplyOnEnableTrigger"] = f.advancedToggle.applyOnEnableTrigger;
            item["advancedToggleApplyOnDisableTrigger"] = f.advancedToggle.applyOnDisableTrigger;

            Json::Value targetInstanceIds(Json::arrayValue);
            for (const auto& instanceId : f.target.targetInstanceIds) {
                targetInstanceIds.append(instanceId);
            }
            item["targetInstanceIds"] = targetInstanceIds;
            item["applyToAllInstances"] = f.target.applyToAllInstances;
            item["createTime"] = f.createTime;
            item["lastModifyTime"] = f.lastModifyTime;

            filters.append(item);
        }

        root["filters"] = filters;

        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        return Json::writeString(builder, root);
    }

    bool FilterManager::ExportFilters(const std::string& filePath) {
        try {
            std::ofstream file(filePath);
            if (!file.is_open()) return false;

            file << ExportToJson();
            file.close();
            return true;
        } catch (...) {
            return false;
        }
    }

    bool FilterManager::ImportFilters(const std::string& filePath) {
        try {
            std::ifstream file(filePath);
            if (!file.is_open()) return false;

            std::stringstream buffer;
            buffer << file.rdbuf();
            file.close();

            return ImportFromJson(buffer.str());
        } catch (...) {
            return false;
        }
    }

    bool FilterManager::ImportFiltersMerge(const std::string& filePath) {
        try {
            std::ifstream file(filePath);
            if (!file.is_open()) return false;

            std::stringstream buffer;
            buffer << file.rdbuf();
            file.close();

            return ImportFromJsonMerge(buffer.str());
        } catch (...) {
            return false;
        }
    }

    bool FilterManager::ImportFiltersOverwrite(const std::string& filePath) {
        try {
            std::ifstream file(filePath);
            if (!file.is_open()) return false;

            std::stringstream buffer;
            buffer << file.rdbuf();
            file.close();

            return ImportFromJsonOverwrite(buffer.str());
        } catch (...) {
            return false;
        }
    }

    bool FilterManager::ImportFromJson(const std::string& json) {
        Json::CharReaderBuilder builder;
        Json::Value root;
        std::string errs;
        std::istringstream iss(json);

        if (!Json::parseFromStream(builder, iss, &root, &errs)) {
            return false;
        }

        const Json::Value filters = (root.isObject() && root.isMember("filters")) ? root["filters"] : Json::Value(Json::arrayValue);
        if (!filters.isArray()) {
            return false;
        }

        std::lock_guard<std::mutex> lock(m_mutex);

        m_filters.clear();
        int maxId = 0;

        for (const auto& item : filters) {
            if (!item.isObject()) continue;

            FilterInfo f;

            // 鍩烘湰灞炴€?
            f.id = item.get("id", 0).asInt();
            maxId = std::max(maxId, f.id);
            f.name = item.get("name", "").asString();
            f.webDisplayName = item.get("webDisplayName", "").asString();
            f.isEnabled = item.get("isEnabled", true).asBool();
            f.executionCount = item.get("executionCount", 0).asInt();

            // 閰嶇疆
            f.mode = static_cast<FilterMode>(item.get("mode", static_cast<int>(FilterMode::Normal)).asInt());
            f.action = static_cast<FilterAction>(item.get("action", static_cast<int>(FilterAction::Replace)).asInt());
            f.startFrom = static_cast<FilterStartFrom>(item.get("startFrom", static_cast<int>(FilterStartFrom::Head)).asInt());

            // 浼樺厛绾?
            f.priority = static_cast<FilterPriority>(item.get("priority", static_cast<int>(FilterPriority::BeforeHeartbeat)).asInt());
            f.collectorPriority = static_cast<FilterPriority>(item.get("collectorPriority", static_cast<int>(FilterPriority::AfterHeartbeat)).asInt());

            // 鏉′欢
            f.appointHeader = item.get("appointHeader", false).asBool();
            f.headerContent = item.get("headerContent", "").asString();
            f.appointLength = item.get("appointLength", false).asBool();
            f.minLength = item.get("minLength", 0).asInt();
            f.maxLength = item.get("maxLength", 65535).asInt();

            f.appointPort = item.get("appointPort", false).asBool();
            f.portContent = item.get("portContent", 0).asInt();

            f.searchPattern = item.get("searchPattern", "").asString();
            f.modifyPattern = item.get("modifyPattern", "").asString();
            {
                const std::string mode = item.get("modifyRangeMode", "standard").asString();
                f.useCustomModifyRange = (mode == "custom");
                f.modifyRangeMin = item.get("modifyRangeMin", kDefaultModifyRangeMin).asInt();
                f.modifyRangeMax = item.get("modifyRangeMax", kDefaultModifyRangeMax).asInt();
                if (f.modifyRangeMin >= 0) f.modifyRangeMin = kDefaultModifyRangeMin;
                if (f.modifyRangeMax <= 0) f.modifyRangeMax = kDefaultModifyRangeMax;
                if (f.modifyRangeMin >= f.modifyRangeMax) {
                    f.modifyRangeMin = kDefaultModifyRangeMin;
                    f.modifyRangeMax = kDefaultModifyRangeMax;
                    f.useCustomModifyRange = false;
                }
            }

            // 鐩爣涓庢柟鍚?
            f.target.applyToCollector = item.get("applyToCollector", false).asBool();
            f.target.applyToHeartbeat = item.get("applyToHeartbeat", true).asBool();
            f.direction.applyToRequest = item.get("applyToRequest", true).asBool();
            f.direction.applyToResponse = item.get("applyToResponse", false).asBool();

            // 閫掕繘
            f.progression.isEnabled = item.get("progressionEnabled", false).asBool();
            f.progression.isContinuous = item.get("progressionContinuous", false).asBool();
            f.progression.step = item.get("progressionStep", 1).asInt();
            f.progression.enableCarry = item.get("progressionEnableCarry", false).asBool();
            f.progression.carryDigits = item.get("progressionCarryDigits", 1).asInt();
            f.progression.positions = item.get("progressionPositions", "").asString();
            f.progression.executedCount = 0;
            f.progression.isDone = false;

            // 楂樼骇璁剧疆锛氳处鍙风骇鍒姩鎬佸紑鍏?
            f.advancedToggle.isEnabled = item.get("advancedToggleEnabled", false).asBool();
            f.advancedToggle.defaultState = item.get("advancedToggleDefaultState", true).asBool();
            f.advancedToggle.enableTriggerEnabled = item.get("advancedToggleEnableTriggerEnabled", false).asBool();
            f.advancedToggle.enablePattern = item.get("advancedToggleEnablePattern", "").asString();
            f.advancedToggle.disableTriggerEnabled = item.get("advancedToggleDisableTriggerEnabled", false).asBool();
            f.advancedToggle.disablePattern = item.get("advancedToggleDisablePattern", "").asString();
            f.advancedToggle.applyOnEnableTrigger = item.get("advancedToggleApplyOnEnableTrigger", false).asBool();
            f.advancedToggle.applyOnDisableTrigger = item.get("advancedToggleApplyOnDisableTrigger", true).asBool();

            // 鏂扮増锛氱洰鏍囧疄渚嬪垪琛?
            if (item.isMember("targetInstanceIds") && item["targetInstanceIds"].isArray()) {
                for (const auto& instanceId : item["targetInstanceIds"]) {
                    if (instanceId.isString()) {
                        f.target.targetInstanceIds.push_back(instanceId.asString());
                    }
                }
            }
            f.target.applyToAllInstances = item.get("applyToAllInstances", false).asBool();

            // 鏃堕棿鎴筹紙鍙€夛級
            if (item.isMember("createTime")) f.createTime = item["createTime"].asString();
            if (item.isMember("lastModifyTime")) f.lastModifyTime = item["lastModifyTime"].asString();

            // 濡傛灉缂哄皯/闈炴硶ID锛屽垯鎸夊綋鍓嶆渶澶D閫掑鐢熸垚锛岀‘淇漊I鍙敤
            if (f.id <= 0) {
                f.id = ++maxId;
            }

            m_filters.push_back(f);
        }

        // 鏇存柊涓嬩竴涓狪D
        int nextId = maxId + 1;
        if (nextId < 1) nextId = 1;
        m_nextId.store(nextId);

        // 缁熻鍙噸缃紙涓嶈法浼氳瘽淇濆瓨锛?
        m_totalExecutions.store(0);

        return true;
    }

    bool FilterManager::ImportFromJsonMerge(const std::string& json) {
        Json::CharReaderBuilder builder;
        Json::Value root;
        std::string errs;
        std::istringstream iss(json);

        if (!Json::parseFromStream(builder, iss, &root, &errs)) {
            return false;
        }

        const Json::Value filters = (root.isObject() && root.isMember("filters")) ? root["filters"] : Json::Value(Json::arrayValue);
        if (!filters.isArray()) {
            return false;
        }

        std::vector<FilterInfo> imported;
        imported.reserve(filters.size());

        for (const auto& item : filters) {
            if (!item.isObject()) continue;

            FilterInfo f;

            f.id = item.get("id", 0).asInt();
            f.name = item.get("name", "").asString();
            f.webDisplayName = item.get("webDisplayName", "").asString();
            f.isEnabled = item.get("isEnabled", true).asBool();
            f.executionCount = item.get("executionCount", 0).asInt();

            f.mode = static_cast<FilterMode>(item.get("mode", static_cast<int>(FilterMode::Normal)).asInt());
            f.action = static_cast<FilterAction>(item.get("action", static_cast<int>(FilterAction::Replace)).asInt());
            f.startFrom = static_cast<FilterStartFrom>(item.get("startFrom", static_cast<int>(FilterStartFrom::Head)).asInt());

            f.priority = static_cast<FilterPriority>(item.get("priority", static_cast<int>(FilterPriority::BeforeHeartbeat)).asInt());
            f.collectorPriority = static_cast<FilterPriority>(item.get("collectorPriority", static_cast<int>(FilterPriority::AfterHeartbeat)).asInt());

            f.appointHeader = item.get("appointHeader", false).asBool();
            f.headerContent = item.get("headerContent", "").asString();
            f.appointLength = item.get("appointLength", false).asBool();
            f.minLength = item.get("minLength", 0).asInt();
            f.maxLength = item.get("maxLength", 65535).asInt();

            f.appointPort = item.get("appointPort", false).asBool();
            f.portContent = item.get("portContent", 0).asInt();

            f.searchPattern = item.get("searchPattern", "").asString();
            f.modifyPattern = item.get("modifyPattern", "").asString();
            {
                const std::string mode = item.get("modifyRangeMode", "standard").asString();
                f.useCustomModifyRange = (mode == "custom");
                f.modifyRangeMin = item.get("modifyRangeMin", kDefaultModifyRangeMin).asInt();
                f.modifyRangeMax = item.get("modifyRangeMax", kDefaultModifyRangeMax).asInt();
                if (f.modifyRangeMin >= 0) f.modifyRangeMin = kDefaultModifyRangeMin;
                if (f.modifyRangeMax <= 0) f.modifyRangeMax = kDefaultModifyRangeMax;
                if (f.modifyRangeMin >= f.modifyRangeMax) {
                    f.modifyRangeMin = kDefaultModifyRangeMin;
                    f.modifyRangeMax = kDefaultModifyRangeMax;
                    f.useCustomModifyRange = false;
                }
            }

            f.target.applyToCollector = item.get("applyToCollector", false).asBool();
            f.target.applyToHeartbeat = item.get("applyToHeartbeat", true).asBool();
            f.direction.applyToRequest = item.get("applyToRequest", true).asBool();
            f.direction.applyToResponse = item.get("applyToResponse", false).asBool();

            f.progression.isEnabled = item.get("progressionEnabled", false).asBool();
            f.progression.isContinuous = item.get("progressionContinuous", false).asBool();
            f.progression.step = item.get("progressionStep", 1).asInt();
            f.progression.enableCarry = item.get("progressionEnableCarry", false).asBool();
            f.progression.carryDigits = item.get("progressionCarryDigits", 1).asInt();
            f.progression.positions = item.get("progressionPositions", "").asString();

            // 楂樼骇璁剧疆锛氳处鍙风骇鍒姩鎬佸紑鍏?
            f.advancedToggle.isEnabled = item.get("advancedToggleEnabled", false).asBool();
            f.advancedToggle.defaultState = item.get("advancedToggleDefaultState", true).asBool();
            f.advancedToggle.enableTriggerEnabled = item.get("advancedToggleEnableTriggerEnabled", false).asBool();
            f.advancedToggle.enablePattern = item.get("advancedToggleEnablePattern", "").asString();
            f.advancedToggle.disableTriggerEnabled = item.get("advancedToggleDisableTriggerEnabled", false).asBool();
            f.advancedToggle.disablePattern = item.get("advancedToggleDisablePattern", "").asString();
            f.advancedToggle.applyOnEnableTrigger = item.get("advancedToggleApplyOnEnableTrigger", false).asBool();
            f.advancedToggle.applyOnDisableTrigger = item.get("advancedToggleApplyOnDisableTrigger", true).asBool();

            // 鏂扮増锛氱洰鏍囧疄渚嬪垪琛?
            if (item.isMember("targetInstanceIds") && item["targetInstanceIds"].isArray()) {
                for (const auto& instanceId : item["targetInstanceIds"]) {
                    if (instanceId.isString()) {
                        f.target.targetInstanceIds.push_back(instanceId.asString());
                    }
                }
            }
            f.target.applyToAllInstances = item.get("applyToAllInstances", false).asBool();

            f.createTime = item.get("createTime", "").asString();
            f.lastModifyTime = item.get("lastModifyTime", "").asString();

            imported.push_back(std::move(f));
        }

        if (imported.empty()) return false;

        std::lock_guard<std::mutex> lock(m_mutex);

        for (auto& f : imported) {
            // 鍚堝苟杩藉姞锛氶噸鏂板垎閰岻D锛岄伩鍏嶈鐩?鍐茬獊
            f.id = m_nextId++;
            // 馃敟 淇濈暀鎵ц娆℃暟锛堜笉鍐嶉噸缃负0锛?
            // f.executionCount = 0;  // 宸叉敞閲婏細淇濈暀瀵煎叆鐨勬墽琛屾鏁?
            m_filters.push_back(f);
        }

        return true;
    }

    bool FilterManager::ImportFromJsonOverwrite(const std::string& json) {
        Json::CharReaderBuilder builder;
        Json::Value root;
        std::string errs;
        std::istringstream iss(json);

        if (!Json::parseFromStream(builder, iss, &root, &errs)) {
            return false;
        }

        const Json::Value filters = (root.isObject() && root.isMember("filters")) ? root["filters"] : Json::Value(Json::arrayValue);
        if (!filters.isArray()) {
            return false;
        }

        std::vector<FilterInfo> imported;
        imported.reserve(filters.size());

        for (const auto& item : filters) {
            if (!item.isObject()) continue;

            FilterInfo f;

            // 鍩烘湰灞炴€?- 淇濈暀鍘熷ID鍜屾墽琛屾鏁?
            f.id = item.get("id", 0).asInt();
            f.name = item.get("name", "").asString();
            f.webDisplayName = item.get("webDisplayName", "").asString();
            f.isEnabled = item.get("isEnabled", true).asBool();
            f.executionCount = item.get("executionCount", 0).asInt();

            f.mode = static_cast<FilterMode>(item.get("mode", static_cast<int>(FilterMode::Normal)).asInt());
            f.action = static_cast<FilterAction>(item.get("action", static_cast<int>(FilterAction::Replace)).asInt());
            f.startFrom = static_cast<FilterStartFrom>(item.get("startFrom", static_cast<int>(FilterStartFrom::Head)).asInt());

            f.priority = static_cast<FilterPriority>(item.get("priority", static_cast<int>(FilterPriority::BeforeHeartbeat)).asInt());
            f.collectorPriority = static_cast<FilterPriority>(item.get("collectorPriority", static_cast<int>(FilterPriority::AfterHeartbeat)).asInt());

            f.appointHeader = item.get("appointHeader", false).asBool();
            f.headerContent = item.get("headerContent", "").asString();
            f.appointLength = item.get("appointLength", false).asBool();
            f.minLength = item.get("minLength", 0).asInt();
            f.maxLength = item.get("maxLength", 65535).asInt();

            f.appointPort = item.get("appointPort", false).asBool();
            f.portContent = item.get("portContent", 0).asInt();

            f.searchPattern = item.get("searchPattern", "").asString();
            f.modifyPattern = item.get("modifyPattern", "").asString();
            {
                const std::string mode = item.get("modifyRangeMode", "standard").asString();
                f.useCustomModifyRange = (mode == "custom");
                f.modifyRangeMin = item.get("modifyRangeMin", kDefaultModifyRangeMin).asInt();
                f.modifyRangeMax = item.get("modifyRangeMax", kDefaultModifyRangeMax).asInt();
                if (f.modifyRangeMin >= 0) f.modifyRangeMin = kDefaultModifyRangeMin;
                if (f.modifyRangeMax <= 0) f.modifyRangeMax = kDefaultModifyRangeMax;
                if (f.modifyRangeMin >= f.modifyRangeMax) {
                    f.modifyRangeMin = kDefaultModifyRangeMin;
                    f.modifyRangeMax = kDefaultModifyRangeMax;
                    f.useCustomModifyRange = false;
                }
            }

            f.target.applyToCollector = item.get("applyToCollector", false).asBool();
            f.target.applyToHeartbeat = item.get("applyToHeartbeat", true).asBool();
            f.direction.applyToRequest = item.get("applyToRequest", true).asBool();
            f.direction.applyToResponse = item.get("applyToResponse", false).asBool();

            f.progression.isEnabled = item.get("progressionEnabled", false).asBool();
            f.progression.isContinuous = item.get("progressionContinuous", false).asBool();
            f.progression.step = item.get("progressionStep", 1).asInt();
            f.progression.enableCarry = item.get("progressionEnableCarry", false).asBool();
            f.progression.carryDigits = item.get("progressionCarryDigits", 1).asInt();
            f.progression.positions = item.get("progressionPositions", "").asString();
            f.progression.executedCount = 0;
            f.progression.isDone = false;

            f.advancedToggle.isEnabled = item.get("advancedToggleEnabled", false).asBool();
            f.advancedToggle.defaultState = item.get("advancedToggleDefaultState", true).asBool();
            f.advancedToggle.enableTriggerEnabled = item.get("advancedToggleEnableTriggerEnabled", false).asBool();
            f.advancedToggle.enablePattern = item.get("advancedToggleEnablePattern", "").asString();
            f.advancedToggle.disableTriggerEnabled = item.get("advancedToggleDisableTriggerEnabled", false).asBool();
            f.advancedToggle.disablePattern = item.get("advancedToggleDisablePattern", "").asString();
            f.advancedToggle.applyOnEnableTrigger = item.get("advancedToggleApplyOnEnableTrigger", false).asBool();
            f.advancedToggle.applyOnDisableTrigger = item.get("advancedToggleApplyOnDisableTrigger", true).asBool();

            // 鏂扮増锛氱洰鏍囧疄渚嬪垪琛?
            if (item.isMember("targetInstanceIds") && item["targetInstanceIds"].isArray()) {
                for (const auto& instanceId : item["targetInstanceIds"]) {
                    if (instanceId.isString()) {
                        f.target.targetInstanceIds.push_back(instanceId.asString());
                    }
                }
            }
            f.target.applyToAllInstances = item.get("applyToAllInstances", false).asBool();

            f.createTime = item.get("createTime", "").asString();
            f.lastModifyTime = item.get("lastModifyTime", "").asString();

            imported.push_back(std::move(f));
        }

        if (imported.empty()) return false;

        std::lock_guard<std::mutex> lock(m_mutex);

        // 馃敟 瑕嗙洊瀵煎叆锛氭竻绌虹幇鏈夋护闀滐紝浣跨敤瀵煎叆鐨処D鍜屾墽琛屾鏁?
        m_filters.clear();

        int maxId = 0;
        for (auto& f : imported) {
            // 淇濈暀鍘熷ID鍜屾墽琛屾鏁?
            if (f.id > 0) {
                maxId = std::max(maxId, f.id);
            } else {
                // 濡傛灉ID鏃犳晥锛屽垎閰嶆柊ID
                f.id = ++maxId;
            }
            m_filters.push_back(f);
        }

        // 鏇存柊涓嬩竴涓狪D
        int nextId = maxId + 1;
        if (nextId < 1) nextId = 1;
        m_nextId.store(nextId);

        return true;
    }

} // namespace WPEFilter
