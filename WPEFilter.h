#pragma once

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <atomic>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <regex>
#include <cstdint>

namespace WPEFilter {

    enum class FilterMode {
        Normal = 0,
        Advanced = 1
    };

    enum class FilterAction {
        Replace = 0,
        Intercept = 1,
        NoModify_Display = 2,
        NoModify_NoDisplay = 3,
        None = 4,
        Change = 5
    };

    enum class FilterStartFrom {
        Head = 0,
        Position = 1
    };

    struct FilterTarget {
        bool applyToCollector;
        bool applyToHeartbeat;
        std::vector<std::string> targetInstanceIds;
        bool applyToAllInstances;

        FilterTarget()
            : applyToCollector(false), applyToHeartbeat(true), applyToAllInstances(false) {
        }
    };

    struct FilterDirection {
        bool applyToRequest;
        bool applyToResponse;

        FilterDirection() : applyToRequest(true), applyToResponse(true) {
        }
    };

    enum class FilterPriority {
        BeforeHeartbeat = 0,
        AfterHeartbeat = 1
    };

    static constexpr int kDefaultModifyRangeMin = -500;
    static constexpr int kDefaultModifyRangeMax = 500;

    struct SearchCondition {
        int position;
        uint8_t value;
        bool isWildcard;
        uint8_t mask;

        SearchCondition() : position(0), value(0), isWildcard(false), mask(0xFF) {
        }
        SearchCondition(int pos, uint8_t val) : position(pos), value(val), isWildcard(false), mask(0xFF) {
        }
        SearchCondition(int pos, uint8_t val, bool wild) : position(pos), value(val), isWildcard(wild), mask(wild ? 0x00 : 0xFF) {
        }
        SearchCondition(int pos, uint8_t val, uint8_t msk) : position(pos), value(val), isWildcard(msk == 0x00), mask(msk) {
        }
    };

    struct Modification {
        int position;
        uint8_t value;
        bool isRandom;

        Modification() : position(0), value(0), isRandom(false) {
        }
        Modification(int pos, uint8_t val) : position(pos), value(val), isRandom(false) {
        }
        Modification(int pos, uint8_t val, bool randomFlag) : position(pos), value(val), isRandom(randomFlag) {
        }
    };

    struct ProgressionConfig {
        bool isEnabled;
        bool isContinuous;
        int step;
        bool enableCarry;
        int carryDigits;
        std::string positions;
        int executedCount;
        bool isDone;

        ProgressionConfig()
            : isEnabled(false), isContinuous(false), step(1), enableCarry(false), carryDigits(1), positions(), executedCount(0), isDone(false) {
        }
    };

    struct AdvancedToggleConfig {
        bool isEnabled;
        bool enableTriggerEnabled;
        bool defaultState;
        std::string enablePattern;
        bool disableTriggerEnabled;
        std::string disablePattern;
        bool applyOnEnableTrigger;
        bool applyOnDisableTrigger;

        AdvancedToggleConfig()
            : isEnabled(false), enableTriggerEnabled(false), defaultState(true), disableTriggerEnabled(false), enablePattern(), disablePattern(), applyOnEnableTrigger(false), applyOnDisableTrigger(true) {
        }
    };

    struct FilterInfoData {
        bool isEnabled;
        int id;
        std::string name;
        std::string webDisplayName;
        int executionCount;

        bool appointHeader;
        std::string headerContent;
        bool appointLength;
        int minLength;
        int maxLength;
        bool appointPort;
        int portContent;

        FilterMode mode;
        FilterAction action;
        FilterStartFrom startFrom;
        FilterTarget target;
        FilterDirection direction;
        FilterPriority priority;
        FilterPriority collectorPriority;

        std::string searchPattern;
        std::string modifyPattern;
        bool useCustomModifyRange;
        int modifyRangeMin;
        int modifyRangeMax;
        ProgressionConfig progression;
        AdvancedToggleConfig advancedToggle;

        std::string createTime;
        std::string lastModifyTime;

        FilterInfoData()
            : isEnabled(false), id(0), name(), webDisplayName(), executionCount(0),
              appointHeader(false), headerContent(), appointLength(false), minLength(0), maxLength(65535), appointPort(false), portContent(0),
              mode(FilterMode::Normal), action(FilterAction::Replace), startFrom(FilterStartFrom::Head),
              target(), direction(), priority(FilterPriority::BeforeHeartbeat), collectorPriority(FilterPriority::AfterHeartbeat),
              searchPattern(), modifyPattern(), useCustomModifyRange(false),
              modifyRangeMin(kDefaultModifyRangeMin), modifyRangeMax(kDefaultModifyRangeMax),
              progression(), advancedToggle(), createTime(), lastModifyTime() {
            auto now = std::chrono::system_clock::now();
            auto time = std::chrono::system_clock::to_time_t(now);
            struct tm timeinfo;
            localtime_s(&timeinfo, &time);
            char buf[64];
            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
            createTime = buf;
            lastModifyTime = buf;
        }
    };

    using FilterInfo = FilterInfoData;

    struct FilterMatchResult {
        bool matched;
        std::vector<int> matchPositions;
        FilterAction action;

        FilterMatchResult() : matched(false), matchPositions(), action(FilterAction::None) {
        }
    };

    struct FilterProcessResult {
        bool processed;
        bool modified;
        bool intercepted;
        bool shouldDisplay;
        std::vector<uint8_t> modifiedData;
        std::string filterName;
        std::string logMessage;

        FilterProcessResult()
            : processed(false), modified(false), intercepted(false), shouldDisplay(true), modifiedData(), filterName(), logMessage() {
        }
    };

    class FilterManager {
    private:
        std::vector<FilterInfo> m_filters;
        std::mutex m_mutex;
        std::atomic<int> m_nextId;
        std::atomic<int> m_totalExecutions;
        std::map<std::string, bool> m_accountFilterStates;
        std::mutex m_stateMutex;

    public:
        FilterManager() : m_nextId(1), m_totalExecutions(0) {
        }

        int AddFilter(const FilterInfo& filter);
        bool UpdateFilter(int id, const FilterInfo& filter);
        bool RemoveFilter(int id);
        bool EnableFilter(int id, bool enable);
        void ClearAllFilters();
        FilterInfo* GetFilter(int id);
        std::vector<FilterInfo> GetAllFilters();
        int GetFilterCount();

        bool MoveFilterUp(int id, std::string* outError = nullptr);
        bool MoveFilterDown(int id, std::string* outError = nullptr);
        bool MoveFilterToTop(int id);
        bool MoveFilterToBottom(int id);

        FilterProcessResult ProcessPacket(
            std::vector<uint8_t>& data,
            const std::string& instanceId,
            bool isRequest,
            bool isCollector,
            FilterPriority currentPhase,
            const std::string& username = "",
            const std::vector<int>* userEnabledFilters = nullptr
        );

        bool GetAccountFilterState(const std::string& instanceId, const std::string& username, int filterId);
        void SetAccountFilterState(const std::string& instanceId, const std::string& username, int filterId, bool state);
        void ResetAccountFilterStates();
        void ResetUserFilterStates(const std::string& instanceId, const std::string& username);

        bool ExportFilters(const std::string& filePath);
        bool ImportFilters(const std::string& filePath);
        std::string ExportToJson();
        std::string ExportToJsonForLocalSave();
        bool ImportFiltersMerge(const std::string& filePath);
        bool ImportFiltersOverwrite(const std::string& filePath);
        bool ImportFromJson(const std::string& json);
        bool ImportFromJsonMerge(const std::string& json);
        bool ImportFromJsonOverwrite(const std::string& json);

        int GetTotalExecutions() { return m_totalExecutions.load(); }
        void ResetStatistics();

    private:
        bool CheckFilterConditions(const FilterInfo& filter, const std::vector<uint8_t>& data, bool isRequest);
        bool CheckHeaderMatch(const FilterInfo& filter, const std::vector<uint8_t>& data);
        bool CheckLengthMatch(const FilterInfo& filter, const std::vector<uint8_t>& data);

        FilterMatchResult CheckFilterMatch_Normal(const FilterInfo& filter, const std::vector<uint8_t>& data);
        FilterMatchResult CheckFilterMatch_Advanced(const FilterInfo& filter, const std::vector<uint8_t>& data);

        bool ApplyReplace_Normal(FilterInfo& filter, std::vector<uint8_t>& data);
        bool ApplyReplace_Advanced(FilterInfo& filter, std::vector<uint8_t>& data, const std::vector<int>& matchPositions);
        std::vector<uint8_t> GenerateChangePacket(const FilterInfo& filter);

        bool ApplyModifications(FilterInfo& filter, std::vector<uint8_t>& data, int baseOffset);
        bool ApplyProgressions(FilterInfo& filter, std::vector<uint8_t>& data, int baseOffset);

        std::vector<SearchCondition> ParseSearchPattern(const std::string& pattern);
        std::vector<Modification> ParseModifyPattern(const std::string& pattern);
        std::vector<int> ParsePositions(const std::string& positions);

        uint8_t GetStepByte(uint8_t currentValue, int step, int& carryOut);
        bool CheckTogglePattern(const std::string& pattern, const std::vector<uint8_t>& data);
        std::string MakeStateKey(const std::string& instanceId, const std::string& username, int filterId);
    };

    std::vector<uint8_t> HexStringToBytes(const std::string& hex);
    std::string BytesToHexString(const std::vector<uint8_t>& bytes);
    bool ParseHexByte(const std::string& hex, uint8_t& result);
    std::string GetFilterModeName(FilterMode mode);
    std::string GetFilterActionName(FilterAction action);
    std::string GetFilterTargetName(const FilterTarget& target);
    bool ValidateSearchPattern(const std::string& pattern, std::string& errorMsg);
    bool ValidateModifyPattern(const std::string& pattern, std::string& errorMsg);

} // namespace WPEFilter
