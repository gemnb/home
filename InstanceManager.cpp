#include "InstanceManager.h"
#include "AbStandaloneApi.h"
#include "AbInstanceInterop.h"
#include "SingleInstanceInterop.h"
#include "HttpApiServer.h"
#include "GlobalAntiCCCoordinator.h"
#include "Logger.h"
#include "WPEFilterIntegration.h"
#include "UserFilterManager.h"
#include "DisconnectRuleTypes.h"
#include <algorithm>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <json/json.h>

// 🔥 修复 nlohmann/json 在 MSVC 2022 中的兼容性问题
#if defined(_MSC_VER)
    #include <cmath>
    #define JSON_HAS_CPP_17
    #define JSON_HAS_CPP_14
    namespace std {
        inline bool isfinite(double x) { return ::isfinite(x); }
        inline bool isnan(double x) { return ::isnan(x); }
    }
#endif

#include "res/json.hpp"  // 🔥 nlohmann/json 用于流量过滤规则序列化

using json = nlohmann::json;

extern DatabaseManager* g_database;

namespace {
InstanceInfo BuildAbCollectorInfo() {
    InstanceInfo info;
    info.id = InstanceManager::kAbCollectorInstanceId;
    info.name = "ab采集";
    info.type = InstanceType::AbCollector;
    info.state = AbStandaloneApi::IsCollectorRunning() ? InstanceState::Running : InstanceState::Stopped;
    info.port = AbStandaloneApi::GetCollectorDesiredPort();
    info.currentConnections = AbStandaloneApi::GetCollectorConnections();
    info.totalPackets = AbStandaloneApi::GetCollectorTotalPackets();
    info.totalBytes = AbStandaloneApi::GetCollectorTotalBytes();
    info.bindToInstanceId = InstanceManager::kAbHeartbeatInstanceId;
    return info;
}

InstanceInfo BuildAbHeartbeatInfo() {
    InstanceInfo info;
    info.id = InstanceManager::kAbHeartbeatInstanceId;
    info.name = "ab伪心跳";
    info.type = InstanceType::AbHeartbeat;
    info.state = AbStandaloneApi::IsHeartbeatForwarderRunning() ? InstanceState::Running : InstanceState::Stopped;
    info.port = AbStandaloneApi::GetHeartbeatDesiredPort();
    info.currentConnections = AbStandaloneApi::GetHeartbeatConnections();
    info.totalPackets = AbStandaloneApi::GetHeartbeatTotalPackets();
    info.totalBytes = AbStandaloneApi::GetHeartbeatTotalBytes();
    info.bindToInstanceId = InstanceManager::kAbCollectorInstanceId;
    return info;
}
} // namespace

// ==================== CollectorInstance实现 ====================

CollectorInstance::CollectorInstance(const std::string& id, const InstanceConfig& config)
    : m_id(id)
    , m_config(config)
    , m_state(InstanceState::Stopped)
{
    // 创建PacketCollector
    m_collector = std::make_unique<PacketCollector>(config.port);

    // 每个单伪采集实例仍需独立数据库用于数据存储（dataDb/memory）
    m_database = std::make_unique<DatabaseManager>();

    // 配置键前缀
    const std::string cfgPrefix = "single_collector_" + m_id + "_";

    try {
        std::filesystem::create_directories("db/single_instances");
        const std::string dataDir = std::string("db/single_instances/") + m_id;
        m_database->SetDataDirectory(dataDir);

        // 从config.db加载关键采集选项（默认与独立项目一致）
        auto strToBool = [](const std::string& v, bool defV) {
            if (v.empty()) return defV;
            return (v == "1" || v == "true");
        };
        auto strToInt = [](const std::string& v, int defV) {
            if (v.empty()) return defV;
            try { return std::stoi(v); } catch (...) { return defV; }
        };
        auto getConfig = [&](const std::string& key, const std::string& def) {
            return g_database ? g_database->GetConfigValue(cfgPrefix + key, def) : def;
        };
        auto setConfig = [&](const std::string& key, const std::string& value) {
            if (g_database) g_database->SetConfigValue(cfgPrefix + key, value);
        };

        // 存储模式/自定义落盘目录（与原生单伪一致：按小时/按天/内存 + 可指定路径）
        {
            const std::string smRaw = getConfig("storage_mode", "");
            if (!smRaw.empty()) {
                const int sm = strToInt(smRaw, 0);
                if (sm == static_cast<int>(::StorageMode::DAILY)) m_config.storageMode = InstMgr::StorageMode::Daily;
                else if (sm == static_cast<int>(::StorageMode::MEMORY)) m_config.storageMode = InstMgr::StorageMode::Memory;
                else m_config.storageMode = InstMgr::StorageMode::Hourly;
            } else {
                // 新建实例：把创建时选择的模式写入配置库
                ::StorageMode sm = ::StorageMode::HOURLY;
                if (m_config.storageMode == InstMgr::StorageMode::Daily) sm = ::StorageMode::DAILY;
                else if (m_config.storageMode == InstMgr::StorageMode::Memory) sm = ::StorageMode::MEMORY;
                setConfig("storage_mode", std::to_string(static_cast<int>(sm)));
            }

            const std::string customDir = getConfig("custom_save_path", "");
            if (!customDir.empty()) {
                try { std::filesystem::create_directories(customDir); } catch (...) {}
                m_database->SetDataDirectory(customDir);
            }
        }

        m_disablePacketHeaderFilter = strToBool(getConfig("disable_packet_header_filter", "0"), false);
        m_enableCollect62Pattern = strToBool(getConfig("enable_collect_62_pattern", "0"), false);
        m_enableDisconnectAutoClear = strToBool(getConfig("enable_disconnect_auto_clear", "1"), true);

        // 特殊ID过滤持久化（与独立项目 key 对齐）
        m_config.allowCollect00ID = strToBool(getConfig("allow_collect_00id", m_config.allowCollect00ID ? "1" : "0"), m_config.allowCollect00ID);
        m_config.allowCollectOBID = strToBool(getConfig("allow_collect_obid", m_config.allowCollectOBID ? "1" : "0"), m_config.allowCollectOBID);

        // 包头类型（CSV）+ 每类型启用状态
        const std::string typesCsv = getConfig("collector_packet_types", "1,2,3,4");
        m_collectorPacketTypes.clear();
        m_collectorPacketTypeEnabled.clear();
        std::stringstream ss(typesCsv);
        std::string item;
        while (std::getline(ss, item, ',')) {
            int type = strToInt(item, -1);
            if (type < 0 || type > 255) continue;
            m_collectorPacketTypes.push_back(type);
            const std::string key = "collector_type_enabled_" + std::to_string(type);
            m_collectorPacketTypeEnabled[type] = strToBool(getConfig(key, "1"), true);
        }
        std::sort(m_collectorPacketTypes.begin(), m_collectorPacketTypes.end());
        m_collectorPacketTypes.erase(std::unique(m_collectorPacketTypes.begin(), m_collectorPacketTypes.end()), m_collectorPacketTypes.end());
        if (m_collectorPacketTypes.empty()) {
            m_collectorPacketTypes = { 1, 2, 3, 4 };
        }
        for (int t : m_collectorPacketTypes) {
            if (m_collectorPacketTypeEnabled.find(t) == m_collectorPacketTypeEnabled.end()) {
                m_collectorPacketTypeEnabled[t] = true;
            }
        }

        // 应用断开清理到采集器
        m_collector->SetDisconnectClearEnabled(m_enableDisconnectAutoClear);

        // 恢复过滤配置（与独立项目 key 对齐）
        const int filterType = strToInt(getConfig("filter_type", "0"), 0);
        const std::string filterValue = getConfig("filter_value", "");
        if (filterType != 0 && !filterValue.empty()) {
            m_collector->SetFilter(static_cast<FilterType>(filterType), filterValue);
        } else {
            m_collector->ClearFilter();
        }

        // 注意：二级代理配置已迁移到新的配置键，在后面的代码中加载（使用 MakeInstanceConfigKey）

        // 恢复采集白/黑名单（每实例持久化，JSON）
        {
            const std::string wl = getConfig("collector_whitelist_enabled", "");
            m_enableCollectorWhitelist = strToBool(!wl.empty() ? wl : getConfig("enable_collector_whitelist", "0"), false);

            const std::string bl = getConfig("collector_blacklist_enabled", "");
            m_enableCollectorBlacklist = strToBool(!bl.empty() ? bl : getConfig("enable_collector_blacklist", "0"), false);
        }

        auto loadRules = [&](const std::string& primaryKey, const std::string& fallbackKey, bool isWhitelist) {
            std::string jsonStr = getConfig(primaryKey, "");
            if (jsonStr.empty() && !fallbackKey.empty()) {
                jsonStr = getConfig(fallbackKey, "");
            }
            if (jsonStr.empty()) return;

            Json::Value root;
            Json::CharReaderBuilder b;
            std::string errs;
            std::unique_ptr<Json::CharReader> reader(b.newCharReader());
            if (!reader->parse(jsonStr.data(), jsonStr.data() + jsonStr.size(), &root, &errs)) {
                return;
            }
            if (!root.isArray()) return;

            for (const auto& it : root) {
                const int idv = it.get("id", 0).asInt();
                const std::string name = it.get("name", "").asString();

                // 兼容两套字段命名：
                // - 原生单伪：searchPattern/isEnabled/matchCount/usePatternSearch
                // - 旧版迁移：pattern/enabled/match_count/use_pattern_search
                const std::string pattern = it.isMember("searchPattern") ? it.get("searchPattern", "").asString()
                    : it.get("pattern", "").asString();
                const bool usePatternSearch = it.isMember("usePatternSearch") ? it.get("usePatternSearch", true).asBool()
                    : it.get("use_pattern_search", true).asBool();
                const bool enabled = it.isMember("isEnabled") ? it.get("isEnabled", true).asBool()
                    : it.get("enabled", true).asBool();
                const uint64_t matchCount = it.isMember("matchCount") ? it.get("matchCount", 0).asUInt64()
                    : it.get("match_count", 0).asUInt64();

                if (name.empty() || pattern.empty()) continue;

                if (isWhitelist) {
                    InstMgr::CollectorWhitelistRule r;
                    r.id = idv;
                    r.name = name;
                    r.searchPattern = pattern;
                    r.usePatternSearch = usePatternSearch;
                    r.isEnabled = enabled;
                    r.matchCount = matchCount;
                    AddCollectorWhitelistRule(r);
                } else {
                    InstMgr::CollectorBlacklistRule r;
                    r.id = idv;
                    r.name = name;
                    r.searchPattern = pattern;
                    r.usePatternSearch = usePatternSearch;
                    r.isEnabled = enabled;
                    r.matchCount = matchCount;
                    AddCollectorBlacklistRule(r);
                }
            }
        };

        loadRules("collector_whitelist_rules", "collector_whitelist_rules_json", true);
        loadRules("collector_blacklist_rules", "collector_blacklist_rules_json", false);
    }
    catch (...) {
        // 保持默认即可（不阻断实例创建）
    }

    // 记录创建时间
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    struct tm timeinfo;
    localtime_s(&timeinfo, &time);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    m_createTime = buf;

    AB_LOG_INFO("[实例管理] 创建采集实例: " + m_id + " (" + m_config.name + ") 端口:" + std::to_string(m_config.port));
}

void CollectorInstance::SetDisablePacketHeaderFilter(bool disable) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_disablePacketHeaderFilter = disable;
    if (g_database) {
        g_database->SetConfigValue("single_collector_" + m_id + "_disable_packet_header_filter", disable ? "1" : "0");
    }
}

bool CollectorInstance::GetDisablePacketHeaderFilter() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_disablePacketHeaderFilter;
}

void CollectorInstance::SetEnableCollect62Pattern(bool enable) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_enableCollect62Pattern = enable;
    if (g_database) {
        g_database->SetConfigValue("single_collector_" + m_id + "_enable_collect_62_pattern", enable ? "1" : "0");
    }
}

bool CollectorInstance::GetEnableCollect62Pattern() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_enableCollect62Pattern;
}

void CollectorInstance::SetCollectorPacketTypes(const std::vector<int>& types) {
    std::lock_guard<std::mutex> lock(m_mutex);

    std::vector<int> normalized;
    normalized.reserve(types.size());
    for (int t : types) {
        if (t < 0 || t > 255) continue;
        normalized.push_back(t);
    }
    std::sort(normalized.begin(), normalized.end());
    normalized.erase(std::unique(normalized.begin(), normalized.end()), normalized.end());
    if (normalized.empty()) {
        normalized = { 1, 2, 3, 4 };
    }

    // 保留已有 enable 状态；新类型默认启用
    std::map<int, bool> newEnabled;
    for (int t : normalized) {
        auto it = m_collectorPacketTypeEnabled.find(t);
        newEnabled[t] = (it == m_collectorPacketTypeEnabled.end()) ? true : it->second;
    }

    m_collectorPacketTypes = normalized;
    m_collectorPacketTypeEnabled = std::move(newEnabled);

    if (g_database) {
        const std::string prefix = "single_collector_" + m_id + "_";
        std::string csv;
        for (size_t i = 0; i < m_collectorPacketTypes.size(); i++) {
            if (i) csv += ",";
            csv += std::to_string(m_collectorPacketTypes[i]);
        }
        g_database->SetConfigValue(prefix + "collector_packet_types", csv);
        for (const auto& pair : m_collectorPacketTypeEnabled) {
            g_database->SetConfigValue(prefix + "collector_type_enabled_" + std::to_string(pair.first), pair.second ? "1" : "0");
        }
    }
}

std::vector<int> CollectorInstance::GetCollectorPacketTypes() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_collectorPacketTypes;
}

void CollectorInstance::SetCollectorPacketTypeEnabled(int type, bool enabled) {
    if (type < 0 || type > 255) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_collectorPacketTypeEnabled[type] = enabled;
    if (g_database) {
        g_database->SetConfigValue("single_collector_" + m_id + "_collector_type_enabled_" + std::to_string(type), enabled ? "1" : "0");
    }
}

bool CollectorInstance::GetCollectorPacketTypeEnabled(int type) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_collectorPacketTypeEnabled.find(type);
    if (it == m_collectorPacketTypeEnabled.end()) return false;
    return it->second;
}

std::map<int, bool> CollectorInstance::GetCollectorPacketTypeEnabledMap() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_collectorPacketTypeEnabled;
}

void CollectorInstance::SetDisconnectAutoClear(bool enable) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_enableDisconnectAutoClear = enable;
    if (m_collector) {
        m_collector->SetDisconnectClearEnabled(enable);
    }
    if (g_database) {
        g_database->SetConfigValue("single_collector_" + m_id + "_enable_disconnect_auto_clear", enable ? "1" : "0");
    }
}

bool CollectorInstance::GetDisconnectAutoClear() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_enableDisconnectAutoClear;
}

CollectorInstance::~CollectorInstance() {
    Stop();
    AB_LOG_INFO("[实例管理] 销毁采集实例: " + m_id);
}

bool CollectorInstance::Start() {
    // 注意：StartCollector 内部/回调会调用 instance 的 getter（会锁 m_mutex）。
    // 若这里持续持有 m_mutex，会触发同线程重复加锁 -> std::system_error(EDEADLK): resource deadlock would occur。
    int port = 0;
    ::StorageMode desiredMode = ::StorageMode::HOURLY;
    DatabaseManager* db = nullptr;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_state == InstanceState::Running) {
            m_lastError = "实例已在运行中";
            return false;
        }

        m_state = InstanceState::Starting;
        port = m_config.port;
        if (m_config.storageMode == InstMgr::StorageMode::Daily) desiredMode = ::StorageMode::DAILY;
        else if (m_config.storageMode == InstMgr::StorageMode::Memory) desiredMode = ::StorageMode::MEMORY;
        else desiredMode = ::StorageMode::HOURLY;

        db = m_database.get();
    }

    AB_LOG_INFO("[实例管理] 启动采集实例: " + m_id);

    bool ok = false;
    try {
        ok = SingleInstanceInterop::StartCollector(m_collector, this, m_id, port, db, desiredMode);
    }
    catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_state = InstanceState::Error;
        m_lastError = std::string("启动异常: ") + e.what();
        AB_LOG_ERROR("[实例管理] 采集实例启动异常: " + m_id + " - " + e.what());
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (ok) {
            m_state = InstanceState::Running;
            m_lastError.clear();

            // 🔥 加载用户滤镜配置
            if (g_database && m_collector) {
                std::string enableUserFilterModeStr = g_database->GetConfigValue(
                    "instance_" + m_id + "_enableUserFilterMode", "0");
                if (enableUserFilterModeStr == "1") {
                    m_collector->SetUserFilterMode(true);
                    AB_LOG_INFO("[实例管理] 已启用用户滤镜模式: " + m_id);
                }

                std::string httpPortStr = g_database->GetConfigValue(
                    "instance_" + m_id + "_userFilterHttpPort", "8080");
                int httpPort = std::stoi(httpPortStr);
                m_collector->SetUserFilterHttpPort(httpPort);
                AB_LOG_INFO("[实例管理] 用户滤镜HTTP端口: " + std::to_string(httpPort));

                // 如果启用了用户滤镜模式，自动启动HTTP服务器
                if (enableUserFilterModeStr == "1") {
                    if (m_collector->StartUserFilterHttpServer()) {
                        AB_LOG_INFO("[实例管理] 用户滤镜HTTP服务器已启动: " + m_id);
                    }
                }
            }

            AB_LOG_INFO("[实例管理] 采集实例启动成功: " + m_id);
            return true;
        }

        m_state = InstanceState::Error;
        {
            const std::string detail = m_collector ? m_collector->GetLastErrorMessage() : std::string();
            m_lastError = detail.empty() ? "启动失败" : detail;
        }
        AB_LOG_ERROR("[实例管理] 采集实例启动失败: " + m_id);
        return false;
    }
}

void CollectorInstance::Stop() {
    // 🔥 修复：先复制 unique_ptr，然后释放锁再调用 Stop，避免死锁
    std::unique_ptr<PacketCollector> collectorCopy;
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_state != InstanceState::Running) {
            return;
        }

        m_state = InstanceState::Stopping;
        AB_LOG_INFO("[实例管理] 停止采集实例: " + m_id);

        // 注意：这里不能真正移动 m_collector，因为其他方法还需要它
        // 所以我们改用不同的策略：设置状态为 Stopping，然后在锁外调用 Stop
    }

    // 🔥 在锁外调用 Stop，避免死锁
    try {
        // 🔥 停止用户滤镜HTTP服务器
        if (m_collector && m_collector->IsUserFilterHttpServerRunning()) {
            m_collector->StopUserFilterHttpServer();
            AB_LOG_INFO("[实例管理] 用户滤镜HTTP服务器已停止: " + m_id);
        }

        SingleInstanceInterop::StopCollector(m_collector);

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_state = InstanceState::Stopped;
        }
        AB_LOG_INFO("[实例管理] 采集实例已停止: " + m_id);
    }
    catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_state = InstanceState::Error;
        m_lastError = std::string("停止异常: ") + e.what();
        AB_LOG_ERROR("[实例管理] 采集实例停止异常: " + m_id + " - " + e.what());
    }
}

bool CollectorInstance::IsRunning() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state == InstanceState::Running && m_collector && m_collector->IsRunning();
}

InstanceInfo CollectorInstance::GetInfo() const {
    std::lock_guard<std::mutex> lock(m_mutex);

    InstanceInfo info;
    info.id = m_id;
    info.name = m_config.name;
    info.type = InstanceType::Collector;
    info.state = m_state;
    info.port = m_config.port;
    info.bindToInstanceId = m_boundHeartbeatId;
    info.createTime = m_createTime;
    info.lastError = m_lastError;
    info.storageMode = m_config.storageMode;
    info.memoryPoolCount = 0;

    if (m_collector) {
        info.currentConnections = m_collector->GetTotalConnections();
        info.totalPackets = m_collector->GetTotalPackets();
        info.totalBytes = m_collector->GetTotalBytes();
    }

    return info;
}

void CollectorInstance::SetPort(int port) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config.port = port;
}

void CollectorInstance::SetPacketCallback(PacketReceivedCallback callback) {
    if (m_collector) {
        m_collector->SetPacketCallback(callback);
    }
}

void CollectorInstance::BindToHeartbeat(const std::string& heartbeatId) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_boundHeartbeatId = heartbeatId;
    AB_LOG_INFO("[实例管理] 采集实例 " + m_id + " 绑定到伪心跳实例 " + heartbeatId);
}

void CollectorInstance::UnbindHeartbeat() {
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::string old = m_boundHeartbeatId;
    m_boundHeartbeatId.clear();
    if (!old.empty()) {
        AB_LOG_INFO("[实例管理] 采集实例 " + m_id + " 已解绑伪心跳实例 " + old);
    } else {
        AB_LOG_INFO("[实例管理] 采集实例 " + m_id + " 未绑定伪心跳实例");
    }
}

// ==================== CollectorInstance 采集白名单实现 ====================

void CollectorInstance::AddCollectorWhitelistRule(const InstMgr::CollectorWhitelistRule& rule) {
    std::lock_guard<std::mutex> lock(m_collectorWhitelistMutex);
    InstMgr::CollectorWhitelistRule newRule = rule;
    if (newRule.id <= 0) {
        newRule.id = m_nextCollectorWhitelistRuleId++;
    } else {
        m_nextCollectorWhitelistRuleId = (std::max)(m_nextCollectorWhitelistRuleId, newRule.id + 1);
    }
    m_collectorWhitelistRules.push_back(newRule);
    AB_LOG_INFO("[采集白名单] 添加规则: " + newRule.name + " (ID: " + std::to_string(newRule.id) + ")");
}

void CollectorInstance::UpdateCollectorWhitelistRule(int id, const InstMgr::CollectorWhitelistRule& rule) {
    std::lock_guard<std::mutex> lock(m_collectorWhitelistMutex);
    for (auto& r : m_collectorWhitelistRules) {
        if (r.id == id) {
            r.name = rule.name;
            r.searchPattern = rule.searchPattern;
            r.usePatternSearch = rule.usePatternSearch;
            r.isEnabled = rule.isEnabled;
            AB_LOG_INFO("[采集白名单] 更新规则: " + r.name + " (ID: " + std::to_string(id) + ")");
            return;
        }
    }
}

void CollectorInstance::RemoveCollectorWhitelistRule(int id) {
    std::lock_guard<std::mutex> lock(m_collectorWhitelistMutex);
    for (auto it = m_collectorWhitelistRules.begin(); it != m_collectorWhitelistRules.end(); ++it) {
        if (it->id == id) {
            AB_LOG_INFO("[采集白名单] 删除规则: " + it->name + " (ID: " + std::to_string(id) + ")");
            m_collectorWhitelistRules.erase(it);
            return;
        }
    }
}

void CollectorInstance::ClearCollectorWhitelistRules() {
    std::lock_guard<std::mutex> lock(m_collectorWhitelistMutex);
    m_collectorWhitelistRules.clear();
    AB_LOG_INFO("[采集白名单] 已清空所有规则");
}

std::vector<InstMgr::CollectorWhitelistRule> CollectorInstance::GetCollectorWhitelistRules() const {
    std::lock_guard<std::mutex> lock(m_collectorWhitelistMutex);
    return m_collectorWhitelistRules;
}

int CollectorInstance::GetNextCollectorWhitelistRuleId() {
    std::lock_guard<std::mutex> lock(m_collectorWhitelistMutex);
    return m_nextCollectorWhitelistRuleId;
}

// 解析模式搜索字符串，格式: "pos|hex,pos|hex,..."
static bool ParsePatternSearch(const std::string& pattern, std::vector<std::pair<int, std::vector<uint8_t>>>& result) {
    result.clear();
    if (pattern.empty()) return false;

    std::string remaining = pattern;
    while (!remaining.empty()) {
        // 找逗号分隔
        size_t commaPos = remaining.find(',');
        std::string segment = (commaPos != std::string::npos) ? remaining.substr(0, commaPos) : remaining;
        remaining = (commaPos != std::string::npos) ? remaining.substr(commaPos + 1) : "";

        // 去除空格
        while (!segment.empty() && segment[0] == ' ') segment = segment.substr(1);
        while (!segment.empty() && segment.back() == ' ') segment.pop_back();

        if (segment.empty()) continue;

        // 找竖线分隔
        size_t pipePos = segment.find('|');
        if (pipePos == std::string::npos) continue;

        std::string posStr = segment.substr(0, pipePos);
        std::string hexStr = segment.substr(pipePos + 1);

        // 解析位置
        int pos = 0;
        try {
            pos = std::stoi(posStr);
        } catch (...) {
            continue;
        }

        // 解析HEX字节
        std::vector<uint8_t> bytes;
        for (size_t i = 0; i + 1 < hexStr.size(); i += 2) {
            // 跳过空格
            while (i < hexStr.size() && hexStr[i] == ' ') i++;
            if (i + 1 >= hexStr.size()) break;

            std::string byteStr = hexStr.substr(i, 2);
            try {
                uint8_t byte = static_cast<uint8_t>(std::stoi(byteStr, nullptr, 16));
                bytes.push_back(byte);
            } catch (...) {
                break;
            }
        }

        if (!bytes.empty()) {
            result.push_back({ pos, bytes });
        }
    }

    return !result.empty();
}

namespace {
struct CollectorPatternItem {
    int relativePos = 0;
    uint8_t value = 0;
    bool isWildcard = false;
    bool isHighNibbleWildcard = false;
    bool isLowNibbleWildcard = false;
};

static bool CollectorParsePatternItems(const std::string& pattern, std::vector<CollectorPatternItem>& items, int& outMinPos, int& outMaxPos) {
    items.clear();
    outMinPos = INT_MAX;
    outMaxPos = INT_MIN;

    std::stringstream ss(pattern);
    std::string part;
    bool parseError = false;

    while (std::getline(ss, part, ',')) {
        part.erase(std::remove(part.begin(), part.end(), ' '), part.end());
        if (part.empty()) continue;

        size_t pipePos = part.find('|');
        if (pipePos == std::string::npos) continue;

        try {
            int pos = std::stoi(part.substr(0, pipePos));
            std::string hexStr = part.substr(pipePos + 1);
            for (char& c : hexStr) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

            CollectorPatternItem item;
            item.relativePos = pos;
            item.isWildcard = (hexStr == "??" || hexStr == "**");
            item.isHighNibbleWildcard = false;
            item.isLowNibbleWildcard = false;
            item.value = 0;

            if (!item.isWildcard) {
                if (hexStr.length() != 2) { parseError = true; break; }

                const char highChar = hexStr[0];
                const char lowChar = hexStr[1];

                auto nibble = [&](char ch, bool& isWild) -> int {
                    if (ch == '*') { isWild = true; return 0; }
                    if (ch >= '0' && ch <= '9') return ch - '0';
                    if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
                    parseError = true;
                    return 0;
                };

                int hi = nibble(highChar, item.isHighNibbleWildcard);
                int lo = nibble(lowChar, item.isLowNibbleWildcard);
                if (parseError) break;

                item.value = static_cast<uint8_t>((hi << 4) | lo);

                if (item.isHighNibbleWildcard && item.isLowNibbleWildcard) {
                    item.isWildcard = true;
                    item.isHighNibbleWildcard = false;
                    item.isLowNibbleWildcard = false;
                }
            }

            items.push_back(item);
            outMinPos = (std::min)(outMinPos, pos);
            outMaxPos = (std::max)(outMaxPos, pos);
        }
        catch (...) {
            parseError = true;
            break;
        }
    }

    if (parseError || items.empty()) return false;
    return true;
}

static bool CollectorMatchPatternItems(const std::vector<uint8_t>& data, const std::string& pattern, bool usePatternSearch) {
    std::vector<CollectorPatternItem> items;
    int minPos = INT_MAX, maxPos = INT_MIN;
    if (!CollectorParsePatternItems(pattern, items, minPos, maxPos)) return false;

    auto matchByte = [](uint8_t dataByte, const CollectorPatternItem& item) -> bool {
        if (item.isWildcard) return true;
        if (item.isHighNibbleWildcard) return (dataByte & 0x0F) == (item.value & 0x0F);
        if (item.isLowNibbleWildcard) return (dataByte & 0xF0) == (item.value & 0xF0);
        return dataByte == item.value;
    };

    if (usePatternSearch) {
        const int patternLength = maxPos - minPos + 1;
        if (patternLength <= 0) return false;
        if (static_cast<int>(data.size()) < patternLength) return false;

        for (int startPos = 0; startPos <= static_cast<int>(data.size()) - patternLength; ++startPos) {
            bool allMatch = true;
            for (const auto& item : items) {
                const int actualPos = startPos + (item.relativePos - minPos);
                if (actualPos < 0 || actualPos >= static_cast<int>(data.size())) { allMatch = false; break; }
                if (!matchByte(data[actualPos], item)) { allMatch = false; break; }
            }
            if (allMatch) return true;
        }
        return false;
    }

    for (const auto& item : items) {
        const int pos = item.relativePos;
        if (pos < 0 || pos >= static_cast<int>(data.size())) return false;
        if (!matchByte(data[pos], item)) return false;
    }
    return true;
}
} // namespace

bool CollectorInstance::CheckCollectorWhitelist(const std::vector<uint8_t>& data, int* matchedIndex, std::string* matchedRuleName) {
    if (!m_enableCollectorWhitelist) return false;

    std::lock_guard<std::mutex> lock(m_collectorWhitelistMutex);

    for (size_t i = 0; i < m_collectorWhitelistRules.size(); ++i) {
        auto& rule = m_collectorWhitelistRules[i];
        if (!rule.isEnabled) continue;

        // 对齐原生单伪：searchPattern="pos|hex,..."；usePatternSearch=true为“搜索模式”，false为“绝对位置模式”；支持 ??/** 通配与半字节通配(*F / F*)
        const bool matched = CollectorMatchPatternItems(data, rule.searchPattern, rule.usePatternSearch);

        if (matched) {
            rule.matchCount++;
            if (matchedIndex) *matchedIndex = static_cast<int>(i);
            if (matchedRuleName) *matchedRuleName = rule.name;
            return true;
        }
    }

    return false;
}

// ==================== CollectorInstance 采集黑名单实现 ====================

void CollectorInstance::AddCollectorBlacklistRule(const InstMgr::CollectorBlacklistRule& rule) {
    std::lock_guard<std::mutex> lock(m_collectorBlacklistMutex);
    InstMgr::CollectorBlacklistRule newRule = rule;
    if (newRule.id <= 0) {
        newRule.id = m_nextCollectorBlacklistRuleId++;
    } else {
        m_nextCollectorBlacklistRuleId = (std::max)(m_nextCollectorBlacklistRuleId, newRule.id + 1);
    }
    m_collectorBlacklistRules.push_back(newRule);
    AB_LOG_INFO("[采集黑名单] 添加规则: " + newRule.name + " (ID: " + std::to_string(newRule.id) + ")");
}

void CollectorInstance::UpdateCollectorBlacklistRule(int id, const InstMgr::CollectorBlacklistRule& rule) {
    std::lock_guard<std::mutex> lock(m_collectorBlacklistMutex);
    for (auto& r : m_collectorBlacklistRules) {
        if (r.id == id) {
            r.name = rule.name;
            r.searchPattern = rule.searchPattern;
            r.usePatternSearch = rule.usePatternSearch;
            r.isEnabled = rule.isEnabled;
            AB_LOG_INFO("[采集黑名单] 更新规则: " + r.name + " (ID: " + std::to_string(id) + ")");
            return;
        }
    }
}

void CollectorInstance::RemoveCollectorBlacklistRule(int id) {
    std::lock_guard<std::mutex> lock(m_collectorBlacklistMutex);
    for (auto it = m_collectorBlacklistRules.begin(); it != m_collectorBlacklistRules.end(); ++it) {
        if (it->id == id) {
            AB_LOG_INFO("[采集黑名单] 删除规则: " + it->name + " (ID: " + std::to_string(id) + ")");
            m_collectorBlacklistRules.erase(it);
            return;
        }
    }
}

void CollectorInstance::ClearCollectorBlacklistRules() {
    std::lock_guard<std::mutex> lock(m_collectorBlacklistMutex);
    m_collectorBlacklistRules.clear();
    AB_LOG_INFO("[采集黑名单] 已清空所有规则");
}

std::vector<InstMgr::CollectorBlacklistRule> CollectorInstance::GetCollectorBlacklistRules() const {
    std::lock_guard<std::mutex> lock(m_collectorBlacklistMutex);
    return m_collectorBlacklistRules;
}

int CollectorInstance::GetNextCollectorBlacklistRuleId() {
    std::lock_guard<std::mutex> lock(m_collectorBlacklistMutex);
    return m_nextCollectorBlacklistRuleId;
}

bool CollectorInstance::CheckCollectorBlacklist(const std::vector<uint8_t>& data, int* matchedIndex, std::string* matchedRuleName) {
    if (!m_enableCollectorBlacklist) return false;

    std::lock_guard<std::mutex> lock(m_collectorBlacklistMutex);

    for (size_t i = 0; i < m_collectorBlacklistRules.size(); ++i) {
        auto& rule = m_collectorBlacklistRules[i];
        if (!rule.isEnabled) continue;

        const bool matched = CollectorMatchPatternItems(data, rule.searchPattern, rule.usePatternSearch);

        if (matched) {
            rule.matchCount++;
            if (matchedIndex) *matchedIndex = static_cast<int>(i);
            if (matchedRuleName) *matchedRuleName = rule.name;
            return true;
        }
    }

    return false;
}

// ==================== HeartbeatInstance实现 ====================

HeartbeatInstance::HeartbeatInstance(const std::string& id, const InstanceConfig& config)
    : m_id(id)
    , m_config(config)
    , m_state(InstanceState::Stopped)
{
    // 创建HeartbeatCore
    m_core = std::make_unique<HeartbeatCore>();

    // 如果配置了端口，创建PacketCollector用于独立监听
    if (m_config.port > 0) {
        m_collector = std::make_unique<PacketCollector>(m_config.port);
        AB_LOG_INFO("[实例管理] 伪心跳实例创建PacketCollector, 端口: " + std::to_string(m_config.port));
    }

    // 记录创建时间
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    struct tm timeinfo;
    localtime_s(&timeinfo, &time);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    m_createTime = buf;

    // 尝试加载配置
    LoadConfig();

    // 注：实例配置现在统一保存到config.db，由InstanceManager::CreateHeartbeatInstance处理

    AB_LOG_INFO("[实例管理] 创建伪心跳实例: " + m_id + " (" + m_config.name + ")");
}

HeartbeatInstance::~HeartbeatInstance() {
    // 保存配置
    SaveConfig();
    Stop();
    AB_LOG_INFO("[实例管理] 销毁伪心跳实例: " + m_id);
}

bool HeartbeatInstance::Start() {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_state == InstanceState::Running) {
        m_lastError = "实例已在运行中";
        return false;
    }

    m_state = InstanceState::Starting;
    AB_LOG_INFO("[实例管理] 启动伪心跳实例: " + m_id);

    try {
        if (!m_collector) {
            m_collector = std::make_unique<PacketCollector>(m_config.port);
        }

        const bool useBoundCollectorDb = (m_config.poolSource == InstMgr::PoolSource::BoundCollector);
        if (!SingleInstanceInterop::StartHeartbeatForwarder(m_collector, this, m_id, m_config.port, &m_boundCollectorDb, useBoundCollectorDb, m_core.get())) {
            m_state = InstanceState::Error;
            {
                const std::string detail = m_collector ? m_collector->GetLastErrorMessage() : std::string();
                m_lastError = detail.empty() ? "PacketCollector启动失败" : detail;
            }
            AB_LOG_ERROR("[实例管理] 伪心跳实例PacketCollector启动失败: " + m_id);
            return false;
        }

        m_state = InstanceState::Running;
        m_lastError.clear();
        AB_LOG_INFO("[实例管理] 伪心跳实例启动成功: " + m_id);
        return true;
    }
    catch (const std::exception& e) {
        m_state = InstanceState::Error;
        m_lastError = std::string("启动异常: ") + e.what();
        AB_LOG_ERROR("[实例管理] 伪心跳实例启动异常: " + m_id + " - " + e.what());
        return false;
    }
}

void HeartbeatInstance::Stop() {
    // 🔥 修复：先设置状态，然后释放锁再调用 Stop，避免死锁
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_state != InstanceState::Running) {
            return;
        }

        m_state = InstanceState::Stopping;
        AB_LOG_INFO("[实例管理] 停止伪心跳实例: " + m_id);
    }

    // 🔥 在锁外调用 Stop，避免死锁
    try {
        SingleInstanceInterop::StopHeartbeatForwarder(m_collector);

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_state = InstanceState::Stopped;
        }
        AB_LOG_INFO("[实例管理] 伪心跳实例已停止: " + m_id);
    }
    catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_state = InstanceState::Error;
        m_lastError = std::string("停止异常: ") + e.what();
        AB_LOG_ERROR("[实例管理] 伪心跳实例停止异常: " + m_id + " - " + e.what());
    }
}

bool HeartbeatInstance::IsRunning() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state == InstanceState::Running;
}

InstanceInfo HeartbeatInstance::GetInfo() const {
    std::lock_guard<std::mutex> lock(m_mutex);

    InstanceInfo info;
    info.id = m_id;
    info.name = m_config.name;
    info.type = InstanceType::Heartbeat;
    info.state = m_state;
    info.port = m_config.port;
    info.bindToInstanceId = m_boundCollectorId;
    info.createTime = m_createTime;
    info.lastError = m_lastError;
    info.poolSource = m_config.poolSource;

    // 添加伪心跳统计信息
    if (m_core) {
        info.totalPackets = m_core->GetTotalProcessed();
    }

    // 添加连接数信息（如果有PacketCollector）
    if (m_collector) {
        info.currentConnections = m_collector->GetTotalConnections();
        info.totalBytes = m_collector->GetTotalBytes();
    }

    return info;
}

void HeartbeatInstance::SetPort(int port) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config.port = port;
}

void HeartbeatInstance::ProcessPacket(const PacketInfo& info, const std::vector<uint8_t>& data) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_state != InstanceState::Running) {
        return;
    }

    if (!m_core) {
        AB_LOG_WARNING("[实例管理] HeartbeatCore未初始化: " + m_id);
        return;
    }

    try {
        // 使用HeartbeatCore处理数据包
        std::vector<HBCore::LengthFieldAdjustment> adjustments;
        int originalIDLen = 0, replacedIDLen = 0, finalIDLen = 0;
        HeartbeatRecord poolData;

        auto result = m_core->ReplaceHeartbeatData(
            data,
            &adjustments,
            &originalIDLen,
            &replacedIDLen,
            &finalIDLen,
            &poolData,
            info.socksUsername,
            info.gameID
        );

        if (!result.empty() && result != data) {
            AB_LOG_INFO("[实例管理] 伪心跳实例 " + m_id + " 替换数据包, 原ID长度: " +
                        std::to_string(originalIDLen) + ", 替换后ID长度: " + std::to_string(finalIDLen));
        }

        // TODO: 将处理后的数据包转发到目标服务器

    }
    catch (const std::exception& e) {
        AB_LOG_ERROR("[实例管理] 伪心跳实例处理数据包异常: " + m_id + " - " + e.what());
    }
}

void HeartbeatInstance::BindToCollector(const std::string& collectorId) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_boundCollectorId = collectorId;
    AB_LOG_INFO("[实例管理] 伪心跳实例 " + m_id + " 绑定到采集实例 " + collectorId);
}

uint64_t HeartbeatInstance::GetTotalProcessed() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_core ? m_core->GetTotalProcessed() : 0;
}

uint64_t HeartbeatInstance::GetTotalReplaced() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_core ? m_core->GetTotalReplaced() : 0;
}

bool HeartbeatInstance::SaveConfig() {
    if (!m_core) return false;
    if (!g_database) return false;

    try {
        // 将HeartbeatCore配置序列化为JSON字符串存储到config.db
        Json::Value root = m_core->ToJson();
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";  // 紧凑格式
        std::string jsonStr = Json::writeString(builder, root);

        g_database->SetConfigValue("single_heartbeat_" + m_id + "_core_config", jsonStr);
        AB_LOG_INFO("[实例管理] 伪心跳实例配置已保存到config.db: " + m_id);
        return true;
    }
    catch (const std::exception& e) {
        AB_LOG_ERROR("[实例管理] 伪心跳实例配置保存失败: " + m_id + " - " + e.what());
        return false;
    }
}

bool HeartbeatInstance::LoadConfig() {
    if (!m_core) return false;
    if (!g_database) return false;

    try {
        // 从config.db加载HeartbeatCore配置
        std::string jsonStr = g_database->GetConfigValue("single_heartbeat_" + m_id + "_core_config", "");
        if (jsonStr.empty()) {
            AB_LOG_INFO("[实例管理] 伪心跳实例配置不存在，使用默认配置: " + m_id);
            return false;
        }

        Json::CharReaderBuilder builder;
        std::istringstream stream(jsonStr);
        Json::Value root;
        std::string errors;
        if (!Json::parseFromStream(builder, stream, &root, &errors)) {
            AB_LOG_ERROR("[实例管理] 伪心跳实例配置解析失败: " + m_id + " - " + errors);
            return false;
        }

        if (m_core->FromJson(root)) {
            AB_LOG_INFO("[实例管理] 伪心跳实例配置已从config.db加载: " + m_id);
            return true;
        }
        else {
            AB_LOG_ERROR("[实例管理] 伪心跳实例配置加载失败: " + m_id);
            return false;
        }
    }
    catch (const std::exception& e) {
        AB_LOG_ERROR("[实例管理] 伪心跳实例配置加载异常: " + m_id + " - " + e.what());
        return false;
    }
}

// ==================== AbCollectorInstance实现（ab采集多实例） ====================

AbCollectorInstance::AbCollectorInstance(const std::string& id, const InstanceConfig& config)
    : m_id(id)
    , m_config(config)
    , m_state(InstanceState::Stopped)
{
    // 预创建对象：用于"实例配置"里复用完整UI（RenderCollectorPage 依赖非空采集器指针）
    m_collector = std::make_unique<PacketCollector>(m_config.port);

    // 注：实例配置现在统一保存到config.db，由InstanceManager::CreateAbCollectorInstance处理

    // 记录创建时间
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    struct tm timeinfo;
    localtime_s(&timeinfo, &time);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    m_createTime = buf;

    AB_LOG_INFO("[实例管理] 创建ab采集实例: " + m_id + " (" + m_config.name + ") 端口:" + std::to_string(m_config.port));
}

AbCollectorInstance::~AbCollectorInstance() {
    Stop();
    AB_LOG_INFO("[实例管理] 销毁ab采集实例: " + m_id);
}

bool AbCollectorInstance::Start() {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_state == InstanceState::Running) {
        m_lastError = "实例已在运行中";
        return false;
    }

    m_state = InstanceState::Starting;
    AB_LOG_INFO("[实例管理] 启动ab采集实例: " + m_id);

    try {
        if (AbInstanceInterop::StartCollector(m_collector, m_id, m_config.port, &m_pool)) {
            m_state = InstanceState::Running;
            m_lastError.clear();
            return true;
        }

        m_state = InstanceState::Error;
        m_lastError = "启动失败";
        return false;
    }
    catch (const std::exception& e) {
        m_state = InstanceState::Error;
        m_lastError = std::string("启动异常: ") + e.what();
        return false;
    }
}

void AbCollectorInstance::Stop() {
    // 🔥 修复：先设置状态，然后释放锁再调用 Stop，避免死锁
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_state != InstanceState::Running) {
            return;
        }

        m_state = InstanceState::Stopping;
        AB_LOG_INFO("[实例管理] 停止ab采集实例: " + m_id);
    }

    // 🔥 在锁外调用 Stop，避免死锁
    try {
        AbInstanceInterop::StopCollector(m_collector);

        std::lock_guard<std::mutex> lock(m_mutex);
        m_state = InstanceState::Stopped;
    }
    catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_state = InstanceState::Error;
        m_lastError = std::string("停止异常: ") + e.what();
    }
}

bool AbCollectorInstance::IsRunning() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state == InstanceState::Running && m_collector && m_collector->IsRunning();
}

InstanceInfo AbCollectorInstance::GetInfo() const {
    std::lock_guard<std::mutex> lock(m_mutex);

    InstanceInfo info;
    info.id = m_id;
    info.name = m_config.name;
    info.type = InstanceType::AbCollector;
    info.state = m_state;
    info.port = m_config.port;
    info.bindToInstanceId = m_boundHeartbeatId;
    info.createTime = m_createTime;
    info.lastError = m_lastError;
    info.memoryPoolCount = m_pool.totalCount.load();

    if (m_collector) {
        info.currentConnections = m_collector->GetTotalConnections();
        info.totalPackets = m_collector->GetTotalPackets();
        info.totalBytes = m_collector->GetTotalBytes();
    }

    return info;
}

void AbCollectorInstance::SetPort(int port) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config.port = port;
}

void AbCollectorInstance::BindToHeartbeat(const std::string& heartbeatId) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_boundHeartbeatId = heartbeatId;
}

void AbCollectorInstance::UnbindHeartbeat() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_boundHeartbeatId.clear();
}

// ==================== AbHeartbeatInstance实现（ab伪心跳多实例） ====================

AbHeartbeatInstance::AbHeartbeatInstance(const std::string& id, const InstanceConfig& config)
    : m_id(id)
    , m_config(config)
    , m_state(InstanceState::Stopped)
{
    // 预创建对象：用于"实例配置"里复用完整UI（RenderHeartbeatPage 依赖非空转发器指针）
    m_forwarder = std::make_unique<PacketCollector>(m_config.port);

    // 注：实例配置现在统一保存到config.db，由InstanceManager::CreateAbHeartbeatInstance处理

    // 记录创建时间
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    struct tm timeinfo;
    localtime_s(&timeinfo, &time);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    m_createTime = buf;

    AB_LOG_INFO("[实例管理] 创建ab伪心跳实例: " + m_id + " (" + m_config.name + ") 端口:" + std::to_string(m_config.port));
}

AbHeartbeatInstance::~AbHeartbeatInstance() {
    Stop();
    AB_LOG_INFO("[实例管理] 销毁ab伪心跳实例: " + m_id);
}

bool AbHeartbeatInstance::Start() {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_state == InstanceState::Running) {
        m_lastError = "实例已在运行中";
        return false;
    }

    m_state = InstanceState::Starting;
    AB_LOG_INFO("[实例管理] 启动ab伪心跳实例: " + m_id);

    try {
        if (AbInstanceInterop::StartHeartbeatForwarder(m_forwarder, m_id, m_config.port, &m_boundCollectorPool)) {
            m_state = InstanceState::Running;
            m_lastError.clear();
            return true;
        }

        m_state = InstanceState::Error;
        m_lastError = "启动失败";
        return false;
    }
    catch (const std::exception& e) {
        m_state = InstanceState::Error;
        m_lastError = std::string("启动异常: ") + e.what();
        return false;
    }
}

void AbHeartbeatInstance::Stop() {
    // 🔥 修复：先设置状态，然后释放锁再调用 Stop，避免死锁
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_state != InstanceState::Running) {
            return;
        }

        m_state = InstanceState::Stopping;
        AB_LOG_INFO("[实例管理] 停止ab伪心跳实例: " + m_id);
    }

    // 🔥 在锁外调用 Stop，避免死锁
    try {
        AbInstanceInterop::StopHeartbeatForwarder(m_forwarder);

        std::lock_guard<std::mutex> lock(m_mutex);
        m_state = InstanceState::Stopped;
    }
    catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_state = InstanceState::Error;
        m_lastError = std::string("停止异常: ") + e.what();
    }
}

bool AbHeartbeatInstance::IsRunning() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state == InstanceState::Running && m_forwarder && m_forwarder->IsRunning();
}

InstanceInfo AbHeartbeatInstance::GetInfo() const {
    std::lock_guard<std::mutex> lock(m_mutex);

    InstanceInfo info;
    info.id = m_id;
    info.name = m_config.name;
    info.type = InstanceType::AbHeartbeat;
    info.state = m_state;
    info.port = m_config.port;
    info.bindToInstanceId = m_boundCollectorId;
    info.createTime = m_createTime;
    info.lastError = m_lastError;

    if (m_forwarder) {
        info.currentConnections = m_forwarder->GetTotalConnections();
        info.totalPackets = m_forwarder->GetTotalPackets();
        info.totalBytes = m_forwarder->GetTotalBytes();
    }

    return info;
}

void AbHeartbeatInstance::SetPort(int port) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config.port = port;
}

void AbHeartbeatInstance::BindToCollector(const std::string& collectorId) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_boundCollectorId = collectorId;
}

// ==================== InstanceManager实现 ====================

// 辅助函数：生成带实例ID前缀的配置键
std::string InstanceManager::MakeInstanceConfigKey(const std::string& instanceId, const std::string& key) {
    return "instance_" + instanceId + "_" + key;
}

InstanceManager::InstanceManager()
    : m_nextInstanceId(1), m_loaded(false)
{
    AB_LOG_INFO("[实例管理] 实例管理器已初始化");
}

void InstanceManager::LoadFromDatabase() {
    if (m_loaded) return;
    m_loaded = true;

    if (!g_database) {
        AB_LOG_ERROR("[实例管理] g_database未初始化，无法加载实例");
        return;
    }

    AB_LOG_INFO("[实例管理] 开始从数据库加载实例列表...");

    // 恢复实例ID计数器
    std::string nextIdStr = g_database->GetConfigValue("instance_next_id", "1");
    try {
        int savedNextId = std::stoi(nextIdStr);
        if (savedNextId > m_nextInstanceId) {
            m_nextInstanceId = savedNextId;
        }
    } catch (...) {}

    // 从config.db加载实例列表（JSON格式）
    try {
        const std::string instanceListJson = g_database->GetConfigValue("instance_list", "");
        if (!instanceListJson.empty()) {
            Json::Value root;
            Json::CharReaderBuilder builder;
            std::string errs;
            std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
            if (reader->parse(instanceListJson.data(), instanceListJson.data() + instanceListJson.size(), &root, &errs) && root.isArray()) {
                for (const auto& item : root) {
                    const std::string instanceId = item.get("id", "").asString();
                    const std::string typeStr = item.get("type", "").asString();
                    if (instanceId.empty() || typeStr.empty()) continue;

                    // 从config.db加载该实例的配置
                    InstanceConfig config;
                    config.name = g_database->GetConfigValue(InstanceManager::MakeInstanceConfigKey(instanceId, "name"), "");
                    config.port = std::atoi(g_database->GetConfigValue(InstanceManager::MakeInstanceConfigKey(instanceId, "port"), "1080").c_str());

                    // 更新nextInstanceId
                    if (instanceId.substr(0, 5) == "inst_") {
                        try {
                            int id = std::stoi(instanceId.substr(5));
                            if (id >= m_nextInstanceId) {
                                m_nextInstanceId = id + 1;
                            }
                        } catch (...) {}
                    }

                    // 根据类型创建实例
                    if (typeStr == "ab_collector") {
                        if (config.name.empty()) config.name = "AB采集实例";
                        auto instance = std::make_unique<AbCollectorInstance>(instanceId, config);
                        m_abCollectors[instanceId] = std::move(instance);
                        AB_LOG_INFO("[实例管理] 已加载AB采集实例: " + instanceId + " (" + config.name + ")");

                        // 检查是否需要自动启动
                        std::string autoStartStr = g_database->GetConfigValue(
                            InstanceManager::MakeInstanceConfigKey(instanceId, "autoStart"), "0");
                        if (autoStartStr == "1") {
                            m_autoStartInstances.push_back(instanceId);
                            AB_LOG_INFO("[实例管理] 实例 " + instanceId + " 标记为自动启动");
                        }
                    }
                    else if (typeStr == "ab_heartbeat") {
                        if (config.name.empty()) config.name = "AB伪心跳实例";
                        auto instance = std::make_unique<AbHeartbeatInstance>(instanceId, config);
                        m_abHeartbeats[instanceId] = std::move(instance);
                        AB_LOG_INFO("[实例管理] 已加载AB伪心跳实例: " + instanceId + " (" + config.name + ")");

                        // 检查是否需要自动启动
                        std::string autoStartStr = g_database->GetConfigValue(
                            InstanceManager::MakeInstanceConfigKey(instanceId, "autoStart"), "0");
                        if (autoStartStr == "1") {
                            m_autoStartInstances.push_back(instanceId);
                            AB_LOG_INFO("[实例管理] 实例 " + instanceId + " 标记为自动启动");
                        }
                    }
                    else if (typeStr == "single_collector") {
                        if (config.name.empty()) config.name = "单伪采集实例";
                        auto instance = std::make_unique<CollectorInstance>(instanceId, config);
                        m_collectors[instanceId] = std::move(instance);
                        AB_LOG_INFO("[实例管理] 已加载单伪采集实例: " + instanceId + " (" + config.name + ")");

                        // 检查是否需要自动启动
                        std::string autoStartStr = g_database->GetConfigValue(
                            InstanceManager::MakeInstanceConfigKey(instanceId, "autoStart"), "0");
                        if (autoStartStr == "1") {
                            m_autoStartInstances.push_back(instanceId);
                            AB_LOG_INFO("[实例管理] 实例 " + instanceId + " 标记为自动启动");
                        }
                    }
                    else if (typeStr == "single_heartbeat") {
                        if (config.name.empty()) config.name = "单伪伪心跳实例";
                        auto instance = std::make_unique<HeartbeatInstance>(instanceId, config);
                        m_heartbeats[instanceId] = std::move(instance);
                        AB_LOG_INFO("[实例管理] 已加载单伪伪心跳实例: " + instanceId + " (" + config.name + ")");

                        // 检查是否需要自动启动
                        std::string autoStartStr = g_database->GetConfigValue(
                            InstanceManager::MakeInstanceConfigKey(instanceId, "autoStart"), "0");
                        if (autoStartStr == "1") {
                            m_autoStartInstances.push_back(instanceId);
                            AB_LOG_INFO("[实例管理] 实例 " + instanceId + " 标记为自动启动");
                        }
                    }
                    else if (typeStr == "socks5_pool") {
                        if (config.name.empty()) config.name = "SOCKS5账号库";
                        auto instance = std::make_unique<Socks5PoolInstance>(instanceId, config);
                        // 自动恢复API服务（如果之前是启用的）
                        instance->AutoStartApiIfEnabled();
                        m_socks5Pools[instanceId] = std::move(instance);
                        AB_LOG_INFO("[实例管理] 已加载SOCKS5账号库实例: " + instanceId + " (" + config.name + ")");
                    }
                    else if (typeStr == "socks_forward") {
                        if (config.name.empty()) config.name = "Socks转发实例";
                        // 加载账号库ID配置
                        config.accountSourceInstanceId = g_database->GetConfigValue(InstanceManager::MakeInstanceConfigKey(instanceId, "accountSourceInstanceId"), "");
                        auto instance = std::make_unique<SocksForwardInstance>(instanceId, config);

                        // 恢复数据包记录设置
                        if (instance->GetCollector()) {
                            std::string enabledStr = g_database->GetConfigValue("proxydata_enabled_" + instanceId, "0");
                            std::string bufSizeStr = g_database->GetConfigValue("proxydata_buffersize_" + instanceId, "200");
                            instance->GetCollector()->SetProxyPacketRecordEnabled(enabledStr == "1");
                            instance->GetCollector()->SetProxyPacketBufferSize(static_cast<size_t>(std::stoi(bufSizeStr)));
                        }

                        m_socksForwards[instanceId] = std::move(instance);
                        AB_LOG_INFO("[实例管理] 已加载Socks转发实例: " + instanceId + " (" + config.name + ")");

                        // 检查是否需要自动启动
                        std::string autoStartStr = g_database->GetConfigValue(
                            InstanceManager::MakeInstanceConfigKey(instanceId, "autoStart"), "0");
                        if (autoStartStr == "1") {
                            m_autoStartInstances.push_back(instanceId);
                            AB_LOG_INFO("[实例管理] 实例 " + instanceId + " 标记为自动启动");
                        }
                    }
                }
                AB_LOG_INFO("[实例管理] 实例列表加载完成，共 " + std::to_string(
                    m_abCollectors.size() + m_abHeartbeats.size() + m_collectors.size() +
                    m_heartbeats.size() + m_socks5Pools.size() + m_socksForwards.size()) + " 个实例");
            }
        }
    } catch (const std::exception& e) {
        AB_LOG_ERROR("[实例管理] 从config.db加载实例列表失败: " + std::string(e.what()));
    }

    // 加载完所有实例后，恢复 Socks 转发实例的账号库绑定
    RestoreSocksForwardAccountBindings();

    // 账号库绑定恢复后，再按实例配置恢复用户滤镜 HTTP 服务
    RestoreSocksForwardUserFilterHttpServers();

    // 自动启动标记为自动启动的实例
    AutoStartMarkedInstances();
}

// 恢复 Socks 转发实例的账号库绑定
void InstanceManager::RestoreSocksForwardAccountBindings() {
    if (!g_database) return;

    for (auto& pair : m_socksForwards) {
        SocksForwardInstance* instance = pair.second.get();
        if (!instance) continue;

        std::string instanceId = instance->GetId();

        // 读取账号库ID配置
        std::string accountSourceId = g_database->GetConfigValue(
            InstanceManager::MakeInstanceConfigKey(instanceId, "accountSourceInstanceId"), "");

        if (accountSourceId.empty()) {
            continue;
        }

        AB_LOG_INFO("[调试] 恢复实例 " + instanceId + " 的账号库绑定: " + accountSourceId);

        // 查找账号库实例
        auto it = m_socks5Pools.find(accountSourceId);
        if (it != m_socks5Pools.end()) {
            Socks5PoolInstance* pool = it->second.get();
            if (pool) {
                PacketCollector* poolCollector = pool->GetInternalCollector();
                if (poolCollector) {
                    PacketCollector* forwardCollector = instance->GetCollector();
                    if (forwardCollector) {
                        forwardCollector->SetExternalAccountSource(poolCollector);
                        AB_LOG_INFO("[实例管理] Socks转发实例 " + instanceId + " 恢复账号库绑定: " + accountSourceId);
                    }
                } else {
                    AB_LOG_WARNING("[调试] 账号库实例 " + accountSourceId + " 的 collector 为空");
                }
            }
        } else {
            AB_LOG_WARNING("[调试] 未找到账号库实例: " + accountSourceId);
        }
    }
}

void InstanceManager::RestoreSocksForwardUserFilterHttpServers() {
    for (auto& pair : m_socksForwards) {
        SocksForwardInstance* instance = pair.second.get();
        if (!instance) continue;
        instance->SyncUserFilterHttpServerState(false);
    }
}

// 🔥 自动启动标记为自动启动的实例
void InstanceManager::AutoStartMarkedInstances() {
    if (m_autoStartInstances.empty()) {
        return;
    }

    AB_LOG_INFO("[实例管理] 开始自动启动实例，共 " + std::to_string(m_autoStartInstances.size()) + " 个");

    for (const std::string& instanceId : m_autoStartInstances) {
        bool found = false;

        // 尝试在AB采集实例中查找
        auto itAbCollector = m_abCollectors.find(instanceId);
        if (itAbCollector != m_abCollectors.end()) {
            AbCollectorInstance* instance = itAbCollector->second.get();
            if (instance && !instance->IsRunning()) {
                AB_LOG_INFO("[实例管理] 自动启动AB采集实例: " + instanceId);
                if (instance->Start()) {
                    AB_LOG_INFO("[实例管理] AB采集实例自动启动成功: " + instanceId);
                } else {
                    AB_LOG_WARNING("[实例管理] AB采集实例自动启动失败: " + instanceId);
                }
            }
            found = true;
        }

        // 尝试在AB伪心跳实例中查找
        auto itAbHeartbeat = m_abHeartbeats.find(instanceId);
        if (itAbHeartbeat != m_abHeartbeats.end()) {
            AbHeartbeatInstance* instance = itAbHeartbeat->second.get();
            if (instance && !instance->IsRunning()) {
                AB_LOG_INFO("[实例管理] 自动启动AB伪心跳实例: " + instanceId);
                if (instance->Start()) {
                    AB_LOG_INFO("[实例管理] AB伪心跳实例自动启动成功: " + instanceId);
                } else {
                    AB_LOG_WARNING("[实例管理] AB伪心跳实例自动启动失败: " + instanceId);
                }
            }
            found = true;
        }

        // 尝试在单伪采集实例中查找
        auto itCollector = m_collectors.find(instanceId);
        if (itCollector != m_collectors.end()) {
            CollectorInstance* instance = itCollector->second.get();
            if (instance && !instance->IsRunning()) {
                AB_LOG_INFO("[实例管理] 自动启动单伪采集实例: " + instanceId);
                if (instance->Start()) {
                    AB_LOG_INFO("[实例管理] 单伪采集实例自动启动成功: " + instanceId);
                } else {
                    AB_LOG_WARNING("[实例管理] 单伪采集实例自动启动失败: " + instanceId);
                }
            }
            found = true;
        }

        // 尝试在单伪伪心跳实例中查找
        auto itHeartbeat = m_heartbeats.find(instanceId);
        if (itHeartbeat != m_heartbeats.end()) {
            HeartbeatInstance* instance = itHeartbeat->second.get();
            if (instance && !instance->IsRunning()) {
                AB_LOG_INFO("[实例管理] 自动启动单伪伪心跳实例: " + instanceId);
                if (instance->Start()) {
                    AB_LOG_INFO("[实例管理] 单伪伪心跳实例自动启动成功: " + instanceId);
                } else {
                    AB_LOG_WARNING("[实例管理] 单伪伪心跳实例自动启动失败: " + instanceId);
                }
            }
            found = true;
        }

        // 尝试在Socks转发实例中查找
        auto itSocksForward = m_socksForwards.find(instanceId);
        if (itSocksForward != m_socksForwards.end()) {
            SocksForwardInstance* instance = itSocksForward->second.get();
            if (instance && !instance->IsRunning()) {
                AB_LOG_INFO("[实例管理] 自动启动Socks转发实例: " + instanceId);
                if (instance->Start()) {
                    AB_LOG_INFO("[实例管理] Socks转发实例自动启动成功: " + instanceId);
                } else {
                    AB_LOG_WARNING("[实例管理] Socks转发实例自动启动失败: " + instanceId);
                }
            }
            found = true;
        }

        if (!found) {
            AB_LOG_WARNING("[实例管理] 未找到需要自动启动的实例: " + instanceId);
        }
    }

    m_autoStartInstances.clear();
}

// 保存实例列表到config.db
void InstanceManager::SaveInstanceList() {
    if (!g_database) return;

    Json::Value root(Json::arrayValue);

    // 添加所有AB采集实例
    for (const auto& pair : m_abCollectors) {
        Json::Value item;
        item["id"] = pair.first;
        item["type"] = "ab_collector";
        root.append(item);
    }

    // 添加所有AB伪心跳实例
    for (const auto& pair : m_abHeartbeats) {
        Json::Value item;
        item["id"] = pair.first;
        item["type"] = "ab_heartbeat";
        root.append(item);
    }

    // 添加所有单伪采集实例
    for (const auto& pair : m_collectors) {
        Json::Value item;
        item["id"] = pair.first;
        item["type"] = "single_collector";
        root.append(item);
    }

    // 添加所有单伪伪心跳实例
    for (const auto& pair : m_heartbeats) {
        Json::Value item;
        item["id"] = pair.first;
        item["type"] = "single_heartbeat";
        root.append(item);
    }

    // 添加所有SOCKS5账号库实例
    for (const auto& pair : m_socks5Pools) {
        Json::Value item;
        item["id"] = pair.first;
        item["type"] = "socks5_pool";
        root.append(item);
    }

    // 添加所有Socks转发实例
    for (const auto& pair : m_socksForwards) {
        Json::Value item;
        item["id"] = pair.first;
        item["type"] = "socks_forward";
        root.append(item);
    }

    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    const std::string jsonStr = Json::writeString(writer, root);
    g_database->SetConfigValue("instance_list", jsonStr);

    // 持久化下一个实例ID计数器，防止重启后ID复用
    g_database->SetConfigValue("instance_next_id", std::to_string(m_nextInstanceId));
}

void InstanceManager::CleanupInstanceConfig(const std::string& instanceId) {
    if (!g_database) return;

    // 删除该实例的所有配置项
    const std::vector<std::string> configKeys = {
        "name", "port", "accountSourceInstanceId", "autoStart",
        "threadPoolMode", "whitelistPoolSize", "normalPoolSize",
        "iocpMaxWhitelist", "iocpMaxNormal",
        "enableSocks5Auth",
        "enableSecondaryProxy", "secondaryProxyHost", "secondaryProxyPort",
        "secondaryProxyUsername", "secondaryProxyPassword",
        "enablePacketSplit", "packetSplitPorts", "applyWpeOnNonSplitTraffic",
        "enableTrafficFilter", "enableSniSniffing", "trafficFilterRules",
        "enableSSLMitm", "sslMitmRules",
        "enableHttpLocalMap", "httpLocalMapRules",
        "enableUserFilterMode", "userFilterHttpPort",
        "anticc_enabled", "anticc_timeWindowSeconds", "anticc_maxRequestsInWindow",
        "anticc_banTimeSeconds", "anticc_maxConnections", "anticc_authFailBanTime",
        "anticc_noAuthBanTime", "anticc_whitelistDuration", "anticc_useBlacklist",
        "anticc_useWhitelist", "anticc_useFirewall", "anticc_rateLimitEnabled",
        "anticc_rateLimit", "anticc_rateTimeWindow", "anticc_blockNonSocks",
        "anticc_enableAuthPriorityAdmission", "anticc_authPriorityQueueLimit",
        "anticc_enableLowPriorityEviction", "anticc_lowPriorityEvictionThreshold",
        "anticc_enableCoordinator"
    };

    for (const auto& key : configKeys) {
        g_database->DeleteConfigValue(InstanceManager::MakeInstanceConfigKey(instanceId, key));
    }

    AB_LOG_INFO("[实例管理] 已清理实例 " + instanceId + " 的所有配置数据");
}

InstanceManager::~InstanceManager() {
    // 停止所有实例
    std::lock_guard<std::mutex> lock(m_mutex);

    // 🔥 先记录所有正在运行的 Socks 转发实例
    // 因为 Stop() 会将 autoStart 设为 "0"，但软件退出时不应清除自启动标记
    std::vector<std::string> runningSocksForwards;
    for (const auto& pair : m_socksForwards) {
        if (pair.second && pair.second->IsRunning()) {
            runningSocksForwards.push_back(pair.first);
        }
    }

    for (auto& pair : m_collectors) {
        pair.second->Stop();
    }

    for (auto& pair : m_heartbeats) {
        pair.second->Stop();
    }

    for (auto& pair : m_abCollectors) {
        pair.second->Stop();
    }

    for (auto& pair : m_abHeartbeats) {
        pair.second->Stop();
    }

    for (auto& pair : m_socksForwards) {
        pair.second->Stop();
    }

    // 🔥 恢复软件退出前正在运行的实例的 autoStart 标记
    // 这样下次启动软件时这些实例会自动启动
    if (g_database) {
        for (const auto& instanceId : runningSocksForwards) {
            g_database->SetConfigValue(
                InstanceManager::MakeInstanceConfigKey(instanceId, "autoStart"), "1");
            AB_LOG_INFO("[实例管理] 保留Socks转发实例自启动标记: " + instanceId);
        }
    }

    AB_LOG_INFO("[实例管理] 实例管理器已销毁");
}

InstanceManager& InstanceManager::GetInstance() {
    static InstanceManager instance;
    return instance;
}

std::string InstanceManager::GenerateInstanceId() {
    std::stringstream ss;
    ss << "inst_" << std::setw(6) << std::setfill('0') << m_nextInstanceId++;
    return ss.str();
}

std::string InstanceManager::GetCurrentTimeString() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    struct tm timeinfo;
    localtime_s(&timeinfo, &time);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    return buf;
}

std::string InstanceManager::CreateCollectorInstance(const InstanceConfig& config) {
    std::lock_guard<std::mutex> lock(m_mutex);

    std::string id = GenerateInstanceId();
    auto instance = std::make_unique<CollectorInstance>(id, config);
    m_collectors[id] = std::move(instance);

    // 保存实例配置到config.db
    if (g_database) {
        g_database->SetConfigValue(InstanceManager::MakeInstanceConfigKey(id, "name"), config.name);
        g_database->SetConfigValue(InstanceManager::MakeInstanceConfigKey(id, "port"), std::to_string(config.port));
    }
    SaveInstanceList();

    AB_LOG_INFO("[实例管理] 已创建采集实例: " + id + " (" + config.name + ")");
    return id;
}

std::string InstanceManager::CreateHeartbeatInstance(const InstanceConfig& config) {
    std::lock_guard<std::mutex> lock(m_mutex);

    std::string id = GenerateInstanceId();
    auto instance = std::make_unique<HeartbeatInstance>(id, config);
    m_heartbeats[id] = std::move(instance);

    // 保存实例配置到config.db
    if (g_database) {
        g_database->SetConfigValue(InstanceManager::MakeInstanceConfigKey(id, "name"), config.name);
        g_database->SetConfigValue(InstanceManager::MakeInstanceConfigKey(id, "port"), std::to_string(config.port));
    }
    SaveInstanceList();

    AB_LOG_INFO("[实例管理] 已创建伪心跳实例: " + id + " (" + config.name + ")");
    return id;
}

std::string InstanceManager::CreateAbCollectorInstance(const InstanceConfig& config) {
    std::lock_guard<std::mutex> lock(m_mutex);

    std::string id = GenerateInstanceId();
    auto instance = std::make_unique<AbCollectorInstance>(id, config);
    m_abCollectors[id] = std::move(instance);

    // 保存实例配置到config.db
    if (g_database) {
        g_database->SetConfigValue(InstanceManager::MakeInstanceConfigKey(id, "name"), config.name);
        g_database->SetConfigValue(InstanceManager::MakeInstanceConfigKey(id, "port"), std::to_string(config.port));
    }
    SaveInstanceList();

    AB_LOG_INFO("[实例管理] 已创建ab采集实例: " + id + " (" + config.name + ")");
    return id;
}

std::string InstanceManager::CreateAbHeartbeatInstance(const InstanceConfig& config) {
    std::lock_guard<std::mutex> lock(m_mutex);

    std::string id = GenerateInstanceId();
    auto instance = std::make_unique<AbHeartbeatInstance>(id, config);
    m_abHeartbeats[id] = std::move(instance);

    // 保存实例配置到config.db
    if (g_database) {
        g_database->SetConfigValue(InstanceManager::MakeInstanceConfigKey(id, "name"), config.name);
        g_database->SetConfigValue(InstanceManager::MakeInstanceConfigKey(id, "port"), std::to_string(config.port));
    }
    SaveInstanceList();

    AB_LOG_INFO("[实例管理] 已创建ab伪心跳实例: " + id + " (" + config.name + ")");
    return id;
}

std::string InstanceManager::CreateSocksForwardInstance(const InstanceConfig& config) {
    std::lock_guard<std::mutex> lock(m_mutex);

    std::string id = GenerateInstanceId();
    auto instance = std::make_unique<SocksForwardInstance>(id, config);
    m_socksForwards[id] = std::move(instance);

    // 保存实例配置到config.db
    if (g_database) {
        g_database->SetConfigValue(InstanceManager::MakeInstanceConfigKey(id, "name"), config.name);
        g_database->SetConfigValue(InstanceManager::MakeInstanceConfigKey(id, "port"), std::to_string(config.port));
        g_database->SetConfigValue(InstanceManager::MakeInstanceConfigKey(id, "accountSourceInstanceId"), config.accountSourceInstanceId);
    }
    SaveInstanceList();

    AB_LOG_INFO("[实例管理] 已创建Socks转发实例: " + id + " (" + config.name + ")");
    return id;
}

bool InstanceManager::DeleteInstance(const std::string& instanceId) {
    // 内置ab实例不允许删除
    if (instanceId == kAbCollectorInstanceId || instanceId == kAbHeartbeatInstanceId) {
        AB_LOG_WARNING("[实例管理] 内置ab实例不支持删除: " + instanceId);
        return false;
    }

    // 先解绑，避免悬空回调/修改器引用已删除实例
    UnbindInstance(instanceId);

    std::vector<SocksForwardInstance*> affectedSocksForwards;
    {
        std::lock_guard<std::mutex> prelock(m_mutex);
        auto socks5PoolIt = m_socks5Pools.find(instanceId);
        if (socks5PoolIt != m_socks5Pools.end()) {
            for (auto& pair : m_socksForwards) {
                if (!pair.second) continue;
                auto cfg = pair.second->GetCachedConfig();
                if (cfg.accountSourceInstanceId == instanceId) {
                    affectedSocksForwards.push_back(pair.second.get());
                }
            }
        }
    }

    for (auto* socksForward : affectedSocksForwards) {
        if (!socksForward) continue;
        if (socksForward->IsRunning()) {
            socksForward->Stop();
        }
        socksForward->ClearAccountSourceBinding();
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    bool deleted = false;

    // 尝试删除采集实例
    auto collectorIt = m_collectors.find(instanceId);
    if (collectorIt != m_collectors.end()) {
        collectorIt->second->Stop();
        m_collectors.erase(collectorIt);
        SingleInstanceInterop::DestroyCollectorContext(instanceId);
        AB_LOG_INFO("[实例管理] 已删除采集实例: " + instanceId);
        deleted = true;
    }

    // 尝试删除伪心跳实例
    if (!deleted) {
        auto heartbeatIt = m_heartbeats.find(instanceId);
        if (heartbeatIt != m_heartbeats.end()) {
            heartbeatIt->second->Stop();
            m_heartbeats.erase(heartbeatIt);
            SingleInstanceInterop::DestroyHeartbeatContext(instanceId);
            AB_LOG_INFO("[实例管理] 已删除伪心跳实例: " + instanceId);
            deleted = true;
        }
    }

    // 尝试删除ab采集实例
    if (!deleted) {
        auto abCollectorIt = m_abCollectors.find(instanceId);
        if (abCollectorIt != m_abCollectors.end()) {
            abCollectorIt->second->Stop();
            m_abCollectors.erase(abCollectorIt);
            AbInstanceInterop::DestroyCollectorContext(instanceId);
            AB_LOG_INFO("[实例管理] 已删除ab采集实例: " + instanceId);
            deleted = true;
        }
    }

    // 尝试删除ab伪心跳实例
    if (!deleted) {
        auto abHeartbeatIt = m_abHeartbeats.find(instanceId);
        if (abHeartbeatIt != m_abHeartbeats.end()) {
            abHeartbeatIt->second->Stop();
            m_abHeartbeats.erase(abHeartbeatIt);
            AbInstanceInterop::DestroyHeartbeatContext(instanceId);
            AB_LOG_INFO("[实例管理] 已删除ab伪心跳实例: " + instanceId);
            deleted = true;
        }
    }

    // 尝试删除SOCKS5账号库实例
    if (!deleted) {
        auto socks5PoolIt = m_socks5Pools.find(instanceId);
        if (socks5PoolIt != m_socks5Pools.end()) {
            m_socks5Pools.erase(socks5PoolIt);
            AB_LOG_INFO("[实例管理] 已删除SOCKS5账号库实例: " + instanceId);
            deleted = true;
        }
    }

    // 尝试删除Socks转发实例
    if (!deleted) {
        auto socksForwardIt = m_socksForwards.find(instanceId);
        if (socksForwardIt != m_socksForwards.end()) {
            socksForwardIt->second->Stop();
            m_socksForwards.erase(socksForwardIt);
            AB_LOG_INFO("[实例管理] 已删除Socks转发实例: " + instanceId);
            deleted = true;
        }
    }

    if (deleted) {
        // 从config.db删除该实例的所有配置项
        CleanupInstanceConfig(instanceId);
        SaveInstanceList();
        return true;
    }

    AB_LOG_WARNING("[实例管理] 实例不存在: " + instanceId);
    return false;
}

bool InstanceManager::StartInstance(const std::string& instanceId) {
    // 内置ab实例：直接调用独立逻辑（与独立UI一致）
    if (instanceId == kAbCollectorInstanceId) {
        return AbStandaloneApi::StartCollectorFromBuffer();
    }
    if (instanceId == kAbHeartbeatInstanceId) {
        return AbStandaloneApi::StartHeartbeatForwarderFromBuffer();
    }

    // 🔥 查找实例指针（在锁内），然后释放锁再调用 Start()，避免死锁
    CollectorInstance* collectorInstance = nullptr;
    HeartbeatInstance* heartbeatInstance = nullptr;
    AbCollectorInstance* abCollectorInstance = nullptr;
    AbHeartbeatInstance* abHeartbeatInstance = nullptr;
    SocksForwardInstance* socksForwardInstance = nullptr;

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        // 查找各类实例
        auto collectorIt = m_collectors.find(instanceId);
        if (collectorIt != m_collectors.end()) {
            collectorInstance = collectorIt->second.get();
        }

        auto heartbeatIt = m_heartbeats.find(instanceId);
        if (heartbeatIt != m_heartbeats.end()) {
            heartbeatInstance = heartbeatIt->second.get();
        }

        auto abCollectorIt = m_abCollectors.find(instanceId);
        if (abCollectorIt != m_abCollectors.end()) {
            abCollectorInstance = abCollectorIt->second.get();
        }

        auto abHeartbeatIt = m_abHeartbeats.find(instanceId);
        if (abHeartbeatIt != m_abHeartbeats.end()) {
            abHeartbeatInstance = abHeartbeatIt->second.get();
        }

        auto socksForwardIt = m_socksForwards.find(instanceId);
        if (socksForwardIt != m_socksForwards.end()) {
            socksForwardInstance = socksForwardIt->second.get();
        }
    } // 🔥 释放 InstanceManager::m_mutex

    // 🔥 在锁外调用 Start()，避免死锁
    if (collectorInstance) {
        return collectorInstance->Start();
    }

    if (heartbeatInstance) {
        return heartbeatInstance->Start();
    }

    if (abCollectorInstance) {
        const bool ok = abCollectorInstance->Start();
        if (ok) {
            // 与独立运行一致：采集端共享伪心跳端账号（绑定后生效）
            const std::string boundHeartbeatId = abCollectorInstance->GetInfo().bindToInstanceId;
            if (!boundHeartbeatId.empty()) {
                std::lock_guard<std::mutex> lock(m_mutex);
                auto hbIt = m_abHeartbeats.find(boundHeartbeatId);
                if (hbIt != m_abHeartbeats.end()) {
                    PacketCollector* collector = abCollectorInstance->GetCollector();
                    if (collector) {
                        collector->SetExternalAccountSource(hbIt->second->GetForwarder());
                    }
                }
            }
        }
        return ok;
    }

    if (abHeartbeatInstance) {
        // 若本次启动会"重建forwarder"（端口变化），先清空已绑定采集端的外部账号源，避免悬空指针
        const std::string boundCollectorId = abHeartbeatInstance->GetInfo().bindToInstanceId;
        if (!boundCollectorId.empty()) {
            PacketCollector* existingForwarder = abHeartbeatInstance->GetForwarder();
            const bool willRecreate = existingForwarder && (existingForwarder->GetListenPort() != abHeartbeatInstance->GetPort());
            if (willRecreate) {
                std::lock_guard<std::mutex> lock(m_mutex);
                auto cIt = m_abCollectors.find(boundCollectorId);
                if (cIt != m_abCollectors.end()) {
                    if (PacketCollector* collector = cIt->second->GetCollector()) {
                        collector->SetExternalAccountSource(nullptr);
                    }
                }
            }
        }

        const bool ok = abHeartbeatInstance->Start();
        if (ok) {
            // 与独立运行一致：伪心跳启动后，更新已绑定采集端的共享账号源
            if (!boundCollectorId.empty()) {
                std::lock_guard<std::mutex> lock(m_mutex);
                auto cIt = m_abCollectors.find(boundCollectorId);
                if (cIt != m_abCollectors.end()) {
                    PacketCollector* collector = cIt->second->GetCollector();
                    if (collector) {
                        collector->SetExternalAccountSource(abHeartbeatInstance->GetForwarder());
                    }
                }
            }
        }
        return ok;
    }

    if (socksForwardInstance) {
        return socksForwardInstance->Start();
    }

    AB_LOG_WARNING("[实例管理] 实例不存在: " + instanceId);
    return false;
}

bool InstanceManager::StopInstance(const std::string& instanceId) {
    // 内置ab实例：直接调用独立逻辑（与独立UI一致）
    if (instanceId == kAbCollectorInstanceId) {
        AbStandaloneApi::StopCollector();
        return true;
    }
    if (instanceId == kAbHeartbeatInstanceId) {
        AbStandaloneApi::StopHeartbeatForwarder();
        return true;
    }

    // 🔥 查找实例指针（在锁内），然后释放锁再调用 Stop()，避免死锁
    CollectorInstance* collectorInstance = nullptr;
    HeartbeatInstance* heartbeatInstance = nullptr;
    AbCollectorInstance* abCollectorInstance = nullptr;
    AbHeartbeatInstance* abHeartbeatInstance = nullptr;
    SocksForwardInstance* socksForwardInstance = nullptr;

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto collectorIt = m_collectors.find(instanceId);
        if (collectorIt != m_collectors.end()) {
            collectorInstance = collectorIt->second.get();
        }

        auto heartbeatIt = m_heartbeats.find(instanceId);
        if (heartbeatIt != m_heartbeats.end()) {
            heartbeatInstance = heartbeatIt->second.get();
        }

        auto abCollectorIt = m_abCollectors.find(instanceId);
        if (abCollectorIt != m_abCollectors.end()) {
            abCollectorInstance = abCollectorIt->second.get();
        }

        auto abHeartbeatIt = m_abHeartbeats.find(instanceId);
        if (abHeartbeatIt != m_abHeartbeats.end()) {
            abHeartbeatInstance = abHeartbeatIt->second.get();
        }

        auto socksForwardIt = m_socksForwards.find(instanceId);
        if (socksForwardIt != m_socksForwards.end()) {
            socksForwardInstance = socksForwardIt->second.get();
        }
    } // 🔥 释放 InstanceManager::m_mutex

    // 🔥 在锁外调用 Stop()，避免死锁
    if (collectorInstance) {
        collectorInstance->Stop();
        return true;
    }

    if (heartbeatInstance) {
        heartbeatInstance->Stop();
        return true;
    }

    if (abCollectorInstance) {
        abCollectorInstance->Stop();
        return true;
    }

    if (abHeartbeatInstance) {
        abHeartbeatInstance->Stop();
        return true;
    }

    if (socksForwardInstance) {
        socksForwardInstance->Stop();
        return true;
    }

    AB_LOG_WARNING("[实例管理] 实例不存在: " + instanceId);
    return false;
}

bool InstanceManager::BindInstances(const std::string& collectorId, const std::string& heartbeatId) {
    // 先解除旧绑定，避免遗留修改器/回调导致"配置未生效"或悬空指针
    UnbindInstance(collectorId);
    UnbindInstance(heartbeatId);

    std::lock_guard<std::mutex> lock(m_mutex);

    auto collectorIt = m_collectors.find(collectorId);
    auto heartbeatIt = m_heartbeats.find(heartbeatId);

    // ab多实例绑定：ab采集 <-> ab伪心跳
    auto abCollectorIt = m_abCollectors.find(collectorId);
    auto abHeartbeatIt = m_abHeartbeats.find(heartbeatId);

    const bool isRegularPair = (collectorIt != m_collectors.end()) && (heartbeatIt != m_heartbeats.end());
    const bool isAbPair = (abCollectorIt != m_abCollectors.end()) && (abHeartbeatIt != m_abHeartbeats.end());

    if (isAbPair) {
        auto* abCollector = abCollectorIt->second.get();
        auto* abHeartbeat = abHeartbeatIt->second.get();

        // 绑定关系（用于UI显示）
        abCollector->BindToHeartbeat(heartbeatId);
        abHeartbeat->BindToCollector(collectorId);

        // 绑定内存池指针：ab伪心跳始终从绑定的ab采集池读取（满足“绑定哪个采集就用哪个采集的数据”）
        abHeartbeat->SetBoundCollectorPool(abCollector->GetPool());

        // 与独立运行一致：采集端共享伪心跳端账号（若双方已启动则立即生效）
        if (PacketCollector* collector = abCollector->GetCollector()) {
            collector->SetExternalAccountSource(abHeartbeat->GetForwarder());
        }

        AB_LOG_INFO("[实例管理] 已绑定ab实例: " + collectorId + " -> " + heartbeatId);
        return true;
    }

    if (!isRegularPair) {
        if (collectorIt == m_collectors.end() && abCollectorIt == m_abCollectors.end()) {
            AB_LOG_WARNING("[实例管理] 采集实例不存在: " + collectorId);
        }
        if (heartbeatIt == m_heartbeats.end() && abHeartbeatIt == m_abHeartbeats.end()) {
            AB_LOG_WARNING("[实例管理] 伪心跳实例不存在: " + heartbeatId);
        }
        return false;
    }

    auto* collectorInstance = collectorIt->second.get();
    auto* heartbeat = heartbeatIt->second.get();
    PacketCollector* collector = collectorInstance ? collectorInstance->GetCollector() : nullptr;

    if (!collectorInstance || !heartbeat || !collector) {
        AB_LOG_ERROR("[实例管理] 绑定失败：实例未就绪 collectorId=" + collectorId + " heartbeatId=" + heartbeatId);
        return false;
    }

    // 绑定关系（用于UI显示）
    collectorInstance->BindToHeartbeat(heartbeatId);
    heartbeat->BindToCollector(collectorId);

    // 防护：若伪心跳选择“绑定采集(内存)”模式，则只能绑定到“内存存储”的采集实例，避免误用数据库模式数据
    if (heartbeat->GetPoolSource() == InstMgr::PoolSource::BoundCollector &&
        collectorInstance->GetStorageMode() != InstMgr::StorageMode::Memory) {
        AB_LOG_ERROR("[实例管理] 绑定失败：伪心跳为内存绑定模式，但采集实例不是内存存储 collectorId=" + collectorId + " heartbeatId=" + heartbeatId);
        // 回滚UI显示绑定关系
        collectorInstance->UnbindHeartbeat();
        heartbeat->BindToCollector("");
        return false;
    }

    // 单伪绑定：伪心跳实例绑定到采集实例的 DatabaseManager（保证“绑定哪个采集就用哪个采集的数据”）
    heartbeat->SetBoundCollectorDb(collectorInstance->GetDatabase());

    // 可选：采集端复用伪心跳端账号（与ab绑定保持一致的体验）
    collector->SetExternalAccountSource(heartbeat->GetCollector());

    AB_LOG_INFO("[实例管理] 已绑定单伪实例: " + collectorId + " -> " + heartbeatId);
    return true;
}

bool InstanceManager::UnbindInstance(const std::string& instanceId) {
    std::lock_guard<std::mutex> lock(m_mutex);

    // 尝试解绑采集实例
    auto collectorIt = m_collectors.find(instanceId);
    if (collectorIt != m_collectors.end()) {
        const std::string boundHeartbeatId = collectorIt->second->GetInfo().bindToInstanceId;

        // 清理绑定关系（UI显示）
        collectorIt->second->UnbindHeartbeat();
        if (PacketCollector* collector = collectorIt->second->GetCollector()) {
            collector->SetExternalAccountSource(nullptr);
        }
        if (!boundHeartbeatId.empty()) {
            auto hbIt = m_heartbeats.find(boundHeartbeatId);
            if (hbIt != m_heartbeats.end()) {
                hbIt->second->BindToCollector("");
                hbIt->second->SetBoundCollectorDb(nullptr);
            }
        }

        AB_LOG_INFO("[实例管理] 已解绑采集实例: " + instanceId + (boundHeartbeatId.empty() ? "" : (" -> " + boundHeartbeatId)));
        return true;
    }

    // 尝试解绑伪心跳实例
    auto heartbeatIt = m_heartbeats.find(instanceId);
    if (heartbeatIt != m_heartbeats.end()) {
        const std::string boundCollectorId = heartbeatIt->second->GetInfo().bindToInstanceId;
        heartbeatIt->second->BindToCollector("");
        heartbeatIt->second->SetBoundCollectorDb(nullptr);

        if (!boundCollectorId.empty()) {
            auto cIt = m_collectors.find(boundCollectorId);
            if (cIt != m_collectors.end()) {
                cIt->second->UnbindHeartbeat();
                if (PacketCollector* collector = cIt->second->GetCollector()) {
                    collector->SetExternalAccountSource(nullptr);
                }
            }
        }

        AB_LOG_INFO("[实例管理] 已解绑伪心跳实例: " + instanceId + (boundCollectorId.empty() ? "" : (" <- " + boundCollectorId)));
        return true;
    }

    // ==================== ab多实例解绑 ====================

    // 尝试解绑ab采集实例
    auto abCollectorIt = m_abCollectors.find(instanceId);
    if (abCollectorIt != m_abCollectors.end()) {
        const std::string boundHeartbeatId = abCollectorIt->second->GetInfo().bindToInstanceId;

        abCollectorIt->second->UnbindHeartbeat();
        if (PacketCollector* collector = abCollectorIt->second->GetCollector()) {
            collector->SetExternalAccountSource(nullptr);
        }

        if (!boundHeartbeatId.empty()) {
            auto hbIt = m_abHeartbeats.find(boundHeartbeatId);
            if (hbIt != m_abHeartbeats.end()) {
                hbIt->second->BindToCollector("");
                hbIt->second->SetBoundCollectorPool(nullptr);
            }
        }

        AB_LOG_INFO("[实例管理] 已解绑ab采集实例: " + instanceId + (boundHeartbeatId.empty() ? "" : (" -> " + boundHeartbeatId)));
        return true;
    }

    // 尝试解绑ab伪心跳实例
    auto abHeartbeatIt = m_abHeartbeats.find(instanceId);
    if (abHeartbeatIt != m_abHeartbeats.end()) {
        const std::string boundCollectorId = abHeartbeatIt->second->GetInfo().bindToInstanceId;

        abHeartbeatIt->second->BindToCollector("");
        abHeartbeatIt->second->SetBoundCollectorPool(nullptr);

        if (!boundCollectorId.empty()) {
            auto cIt = m_abCollectors.find(boundCollectorId);
            if (cIt != m_abCollectors.end()) {
                cIt->second->UnbindHeartbeat();
                if (PacketCollector* collector = cIt->second->GetCollector()) {
                    collector->SetExternalAccountSource(nullptr);
                }
            }
        }

        AB_LOG_INFO("[实例管理] 已解绑ab伪心跳实例: " + instanceId + (boundCollectorId.empty() ? "" : (" <- " + boundCollectorId)));
        return true;
    }

    auto socksForwardIt = m_socksForwards.find(instanceId);
    if (socksForwardIt != m_socksForwards.end()) {
        socksForwardIt->second->ClearAccountSourceBinding();
        AB_LOG_INFO("[实例管理] 已解绑Socks转发实例账号库: " + instanceId);
        return true;
    }

    AB_LOG_WARNING("[实例管理] 实例不存在: " + instanceId);
    return false;
}

std::vector<BindingInfo> InstanceManager::GetAllBindings() {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<BindingInfo> bindings;

    // 遍历单伪采集实例，查找有绑定关系的
    for (const auto& pair : m_collectors) {
        const auto& info = pair.second->GetInfo();
        if (!info.bindToInstanceId.empty()) {
            BindingInfo binding;
            binding.collectorId = info.id;
            binding.heartbeatId = info.bindToInstanceId;
            binding.collectorType = InstanceType::Collector;
            binding.heartbeatType = InstanceType::Heartbeat;
            binding.collectorRunning = (info.state == InstanceState::Running);
            // 查找伪心跳实例的运行状态
            auto hbIt = m_heartbeats.find(info.bindToInstanceId);
            if (hbIt != m_heartbeats.end()) {
                binding.heartbeatRunning = (hbIt->second->GetInfo().state == InstanceState::Running);
            }
            bindings.push_back(binding);
        }
    }

    // 遍历ab采集实例，查找有绑定关系的
    for (const auto& pair : m_abCollectors) {
        const auto& info = pair.second->GetInfo();
        if (!info.bindToInstanceId.empty()) {
            BindingInfo binding;
            binding.collectorId = info.id;
            binding.heartbeatId = info.bindToInstanceId;
            binding.collectorType = InstanceType::AbCollector;
            binding.heartbeatType = InstanceType::AbHeartbeat;
            binding.collectorRunning = (info.state == InstanceState::Running);
            // 查找ab伪心跳实例的运行状态
            auto hbIt = m_abHeartbeats.find(info.bindToInstanceId);
            if (hbIt != m_abHeartbeats.end()) {
                binding.heartbeatRunning = (hbIt->second->GetInfo().state == InstanceState::Running);
            }
            bindings.push_back(binding);
        }
    }

    return bindings;
}

InstanceInfo InstanceManager::GetInstanceInfo(const std::string& instanceId) {
    if (instanceId == kAbCollectorInstanceId) {
        return BuildAbCollectorInfo();
    }
    if (instanceId == kAbHeartbeatInstanceId) {
        return BuildAbHeartbeatInfo();
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    auto collectorIt = m_collectors.find(instanceId);
    if (collectorIt != m_collectors.end()) {
        return collectorIt->second->GetInfo();
    }

    auto heartbeatIt = m_heartbeats.find(instanceId);
    if (heartbeatIt != m_heartbeats.end()) {
        return heartbeatIt->second->GetInfo();
    }

    auto abCollectorIt = m_abCollectors.find(instanceId);
    if (abCollectorIt != m_abCollectors.end()) {
        return abCollectorIt->second->GetInfo();
    }

    auto abHeartbeatIt = m_abHeartbeats.find(instanceId);
    if (abHeartbeatIt != m_abHeartbeats.end()) {
        return abHeartbeatIt->second->GetInfo();
    }

    auto socksForwardIt = m_socksForwards.find(instanceId);
    if (socksForwardIt != m_socksForwards.end()) {
        return socksForwardIt->second->GetInfo();
    }

    return InstanceInfo();
}

std::vector<InstanceInfo> InstanceManager::GetAllInstances() {
    std::lock_guard<std::mutex> lock(m_mutex);

    std::vector<InstanceInfo> result;

    for (const auto& pair : m_collectors) {
        result.push_back(pair.second->GetInfo());
    }

    for (const auto& pair : m_heartbeats) {
        result.push_back(pair.second->GetInfo());
    }

    for (const auto& pair : m_abCollectors) {
        result.push_back(pair.second->GetInfo());
    }

    for (const auto& pair : m_abHeartbeats) {
        result.push_back(pair.second->GetInfo());
    }

    for (const auto& pair : m_socks5Pools) {
        result.push_back(pair.second->GetInfo());
    }

    for (const auto& pair : m_socksForwards) {
        result.push_back(pair.second->GetInfo());
    }

    return result;
}

std::vector<InstanceInfo> InstanceManager::GetInstancesByType(InstanceType type) {
    std::lock_guard<std::mutex> lock(m_mutex);

    std::vector<InstanceInfo> result;

    if (type == InstanceType::Collector) {
        for (const auto& pair : m_collectors) {
            result.push_back(pair.second->GetInfo());
        }
    } else if (type == InstanceType::Heartbeat) {
        for (const auto& pair : m_heartbeats) {
            result.push_back(pair.second->GetInfo());
        }
    } else if (type == InstanceType::AbCollector) {
        for (const auto& pair : m_abCollectors) {
            result.push_back(pair.second->GetInfo());
        }
    } else if (type == InstanceType::AbHeartbeat) {
        for (const auto& pair : m_abHeartbeats) {
            result.push_back(pair.second->GetInfo());
        }
    } else if (type == InstanceType::Socks5Pool) {
        for (const auto& pair : m_socks5Pools) {
            result.push_back(pair.second->GetInfo());
        }
    } else if (type == InstanceType::SocksForward) {
        for (const auto& pair : m_socksForwards) {
            result.push_back(pair.second->GetInfo());
        }
    }

    return result;
}

CollectorInstance* InstanceManager::GetCollectorInstance(const std::string& instanceId) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_collectors.find(instanceId);
    if (it != m_collectors.end()) {
        return it->second.get();
    }

    return nullptr;
}

HeartbeatInstance* InstanceManager::GetHeartbeatInstance(const std::string& instanceId) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_heartbeats.find(instanceId);
    if (it != m_heartbeats.end()) {
        return it->second.get();
    }

    return nullptr;
}

AbCollectorInstance* InstanceManager::GetAbCollectorInstance(const std::string& instanceId) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_abCollectors.find(instanceId);
    if (it != m_abCollectors.end()) {
        return it->second.get();
    }

    return nullptr;
}

AbHeartbeatInstance* InstanceManager::GetAbHeartbeatInstance(const std::string& instanceId) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_abHeartbeats.find(instanceId);
    if (it != m_abHeartbeats.end()) {
        return it->second.get();
    }

    return nullptr;
}

int InstanceManager::GetTotalInstances() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<int>(m_collectors.size() + m_heartbeats.size() + m_abCollectors.size() + m_abHeartbeats.size() + m_socks5Pools.size() + m_socksForwards.size() + 2);
}

int InstanceManager::GetRunningInstances() const {
    std::lock_guard<std::mutex> lock(m_mutex);

    int count = 0;

    if (AbStandaloneApi::IsCollectorRunning()) {
        count++;
    }
    if (AbStandaloneApi::IsHeartbeatForwarderRunning()) {
        count++;
    }

    for (const auto& pair : m_collectors) {
        if (pair.second->IsRunning()) {
            count++;
        }
    }

    for (const auto& pair : m_heartbeats) {
        if (pair.second->IsRunning()) {
            count++;
        }
    }

    for (const auto& pair : m_abCollectors) {
        if (pair.second->IsRunning()) {
            count++;
        }
    }

    for (const auto& pair : m_abHeartbeats) {
        if (pair.second->IsRunning()) {
            count++;
        }
    }

    for (const auto& pair : m_socksForwards) {
        if (pair.second->IsRunning()) {
            count++;
        }
    }

    return count;
}

// ==================== Socks5PoolInstance实现 ====================

Socks5PoolInstance::Socks5PoolInstance(const std::string& id, const InstanceConfig& config)
    : m_id(id)
    , m_config(config)
    , m_nextAccountId(1)
    , m_apiServer(nullptr)
    , m_apiPort(90)
    , m_apiUsername("admin")
    , m_apiPassword("admin")
{
    // 获取当前时间作为创建时间
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm tm_now;
    localtime_s(&tm_now, &time_t_now);
    std::ostringstream oss;
    oss << std::put_time(&tm_now, "%Y-%m-%d %H:%M:%S");
    m_createTime = oss.str();

    // 🔥 先创建内部PacketCollector（必须在LoadFromDatabase之前）
    m_internalCollector = std::make_unique<PacketCollector>(0);

    // 从config.db加载账号和API配置
    try {
        LoadFromDatabase();
    } catch (const std::exception& e) {
        AB_LOG_ERROR("[SOCKS5Pool] 初始化账号库失败 " + m_id + ": " + e.what());
    }
}

Socks5PoolInstance::~Socks5PoolInstance() {
    // 停止API服务（但不更新启用状态，保持原有配置）
    StopApiServer(false);

    // 保存账号到数据库
    SaveToDatabase();
}

InstanceInfo Socks5PoolInstance::GetInfo() const {
    std::lock_guard<std::mutex> lock(m_mutex);

    InstanceInfo info;
    info.id = m_id;
    info.name = m_config.name;
    info.type = InstanceType::Socks5Pool;
    info.state = InstanceState::Running;  // 账号库始终处于可用状态
    info.port = 0;  // 账号库不监听端口
    info.currentConnections = static_cast<int>(m_accounts.size());  // 用连接数字段显示账号数
    info.totalPackets = 0;
    info.totalBytes = 0;
    info.createTime = m_createTime;

    return info;
}

bool Socks5PoolInstance::AddAccount(const std::string& username, const std::string& password,
    const std::string& expireTime, int maxConnections) {
    std::lock_guard<std::mutex> lock(m_mutex);

    // 检查用户名是否已存在
    for (const auto& acc : m_accounts) {
        if (acc.username == username) {
            return false;
        }
    }

    Socks5Account account;
    account.id = m_nextAccountId++;
    account.username = username;
    account.password = password;
    account.expireTime = expireTime;
    account.maxConnections = maxConnections;
    account.currentConnections = 0;
    account.isEnabled = true;
    account.totalOnlineSeconds = 0;

    // 设置创建时间
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm tm_now;
    localtime_s(&tm_now, &time_t_now);
    std::ostringstream oss;
    oss << std::put_time(&tm_now, "%Y-%m-%d %H:%M:%S");
    account.createdAt = oss.str();

    m_accounts.push_back(account);
    SaveToDatabase();

    // 同步到内部PacketCollector（用于API服务）
    if (m_internalCollector) {
        m_internalCollector->AddAccount(username, password, expireTime, maxConnections);
    }

    // 同步到所有已绑定的运行中实例
    NotifyBoundCollectors();

    return true;
}

bool Socks5PoolInstance::RemoveAccount(const std::string& username) {
    std::lock_guard<std::mutex> lock(m_mutex);

    for (auto it = m_accounts.begin(); it != m_accounts.end(); ++it) {
        if (it->username == username) {
            m_accounts.erase(it);
            SaveToDatabase();

            // 同步到内部PacketCollector（用于API服务）
            if (m_internalCollector) {
                m_internalCollector->RemoveAccount(username);
            }

            // 同步到所有已绑定的运行中实例
            NotifyBoundCollectors();

            return true;
        }
    }

    return false;
}

bool Socks5PoolInstance::UpdateAccount(const std::string& username, const std::string& password,
    const std::string& expireTime, int maxConnections, bool isEnabled) {
    std::lock_guard<std::mutex> lock(m_mutex);

    for (auto& acc : m_accounts) {
        if (acc.username == username) {
            if (!password.empty()) {
                acc.password = password;
            }
            acc.expireTime = expireTime;
            acc.maxConnections = maxConnections;
            acc.isEnabled = isEnabled;
            SaveToDatabase();

            // 同步到内部PacketCollector（用于API服务）
            if (m_internalCollector) {
                m_internalCollector->UpdateAccount(username, password.empty() ? acc.password : password,
                    expireTime, maxConnections, isEnabled);
            }

            // 同步到所有已绑定的运行中实例
            NotifyBoundCollectors();

            return true;
        }
    }

    return false;
}

bool Socks5PoolInstance::AccountExists(const std::string& username) const {
    std::lock_guard<std::mutex> lock(m_mutex);

    for (const auto& acc : m_accounts) {
        if (acc.username == username) {
            return true;
        }
    }

    return false;
}

Socks5Account Socks5PoolInstance::GetAccount(const std::string& username) const {
    std::lock_guard<std::mutex> lock(m_mutex);

    for (const auto& acc : m_accounts) {
        if (acc.username == username) {
            return acc;
        }
    }

    return Socks5Account();
}

std::vector<Socks5Account> Socks5PoolInstance::GetAllAccounts() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_accounts;
}

int Socks5PoolInstance::GetAccountCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<int>(m_accounts.size());
}

Socks5PoolInstance::OnlineDevicePolicy Socks5PoolInstance::GetOnlineDevicePolicy() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_onlineDevicePolicy;
}

void Socks5PoolInstance::SetOnlineDevicePolicy(OnlineDevicePolicy policy) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_onlineDevicePolicy = policy;
    if (m_internalCollector) {
        m_internalCollector->SetSharedAccountSingleDeviceModeEnabled(
            m_onlineDevicePolicy == OnlineDevicePolicy::SingleDeviceSingleInstance);
    }
    SaveToDatabase();
    AB_LOG_INFO("[SOCKS5Pool " + m_id + "] 在线设备策略已更新: " +
        std::string(m_onlineDevicePolicy == OnlineDevicePolicy::SingleDeviceSingleInstance
            ? "单账号单设备单实例在线" : "不限制"));
}

bool Socks5PoolInstance::ValidateAccount(const std::string& username, const std::string& password) const {
    std::lock_guard<std::mutex> lock(m_mutex);

    AB_LOG_INFO("[调试] Socks5PoolInstance::ValidateAccount 开始验证: username=" + username + ", password=" + password);
    AB_LOG_INFO("[调试] 账号库中共有 " + std::to_string(m_accounts.size()) + " 个账号");

    for (const auto& acc : m_accounts) {
        AB_LOG_INFO("[调试] 检查账号: username=" + acc.username + ", password=" + acc.password +
                     ", isEnabled=" + std::to_string(acc.isEnabled) +
                     ", maxConnections=" + std::to_string(acc.maxConnections) +
                     ", currentConnections=" + std::to_string(acc.currentConnections) +
                     ", expireTime=" + acc.expireTime);

        if (acc.username == username && acc.password == password) {
            AB_LOG_INFO("[调试] 找到匹配的账号: " + username);

            // 检查是否启用
            if (!acc.isEnabled) {
                AB_LOG_INFO("[调试] 账号未启用: " + username);
                return false;
            }

            // 检查是否过期
            if (!acc.expireTime.empty()) {
                auto now = std::chrono::system_clock::now();
                auto time_t_now = std::chrono::system_clock::to_time_t(now);
                std::tm tm_now;
                localtime_s(&tm_now, &time_t_now);

                std::tm tm_expire = {};
                std::istringstream ss(acc.expireTime);
                ss >> std::get_time(&tm_expire, "%Y-%m-%d %H:%M:%S");

                if (!ss.fail()) {
                    auto expire_time_t = std::mktime(&tm_expire);
                    if (time_t_now > expire_time_t) {
                        AB_LOG_INFO("[调试] 账号已过期: " + username);
                        return false;  // 已过期
                    }
                }
            }

            // 检查连接数限制
            if (acc.maxConnections > 0 && acc.currentConnections >= acc.maxConnections) {
                AB_LOG_INFO("[调试] 账号连接数已达上限: " + username +
                             ", current=" + std::to_string(acc.currentConnections) +
                             ", max=" + std::to_string(acc.maxConnections));
                return false;  // 超过最大连接数
            }

            AB_LOG_INFO("[调试] 账号验证成功: " + username);
            return true;
        }
    }

    AB_LOG_INFO("[调试] 未找到匹配的账号: " + username);
    return false;
}

bool Socks5PoolInstance::ValidateAccountWithReason(const std::string& username, const std::string& password, std::string& failReason) const {
    std::lock_guard<std::mutex> lock(m_mutex);

    for (const auto& acc : m_accounts) {
        if (acc.username == username) {
            // 找到用户名，检查密码
            if (acc.password != password) {
                failReason = "密码错误";
                return false;
            }

            // 检查是否启用
            if (!acc.isEnabled) {
                failReason = "账号已被禁用";
                return false;
            }

            // 检查是否过期
            if (!acc.expireTime.empty()) {
                auto now = std::chrono::system_clock::now();
                auto time_t_now = std::chrono::system_clock::to_time_t(now);
                std::tm tm_now;
                localtime_s(&tm_now, &time_t_now);

                std::tm tm_expire = {};
                std::istringstream ss(acc.expireTime);
                ss >> std::get_time(&tm_expire, "%Y-%m-%d %H:%M:%S");

                if (!ss.fail()) {
                    auto expire_time_t = std::mktime(&tm_expire);
                    if (time_t_now > expire_time_t) {
                        failReason = "账号已过期 (到期时间: " + acc.expireTime + ")";
                        return false;
                    }
                }
            }

            // 检查连接数限制
            if (acc.maxConnections > 0 && acc.currentConnections >= acc.maxConnections) {
                failReason = "连接数已达上限 (" + std::to_string(acc.currentConnections) + "/" + std::to_string(acc.maxConnections) + ")";
                return false;
            }

            failReason = "";
            return true;
        }
    }

    failReason = "用户名不存在";
    return false;
}

void Socks5PoolInstance::SaveToDatabase() {
    if (!g_database) return;

    AB_LOG_INFO("[调试] 开始保存账号库 " + m_id + " 的账号数据，当前账号数: " + std::to_string(m_accounts.size()));

    // 使用JSON格式保存账号列表
    Json::Value root(Json::arrayValue);

    for (const auto& acc : m_accounts) {
        Json::Value item;
        item["id"] = acc.id;
        item["username"] = acc.username;
        item["password"] = acc.password;
        item["expireTime"] = acc.expireTime;
        item["maxConnections"] = acc.maxConnections;
        item["isEnabled"] = acc.isEnabled;
        item["createdAt"] = acc.createdAt;
        item["lastLoginTime"] = acc.lastLoginTime;
        item["lastLoginIP"] = acc.lastLoginIP;
        item["totalOnlineSeconds"] = static_cast<Json::UInt64>(acc.totalOnlineSeconds);
        root.append(item);

        AB_LOG_INFO("[调试] 保存账号: username=" + acc.username + ", password=" + acc.password);
    }

    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    std::string jsonStr = Json::writeString(builder, root);

    AB_LOG_INFO("[调试] 生成的账号JSON (前200字符): " + jsonStr.substr(0, 200));

    const std::string prefix = "socks5_pool_" + m_id + "_";
    g_database->SetConfigValue(prefix + "accounts", jsonStr);
    g_database->SetConfigValue(prefix + "next_account_id", std::to_string(m_nextAccountId));
    g_database->SetConfigValue(prefix + "pool_name", m_config.name);
    g_database->SetConfigValue(prefix + "create_time", m_createTime);

    AB_LOG_INFO("[调试] 账号数据已保存到数据库，配置键: " + prefix + "accounts");

    // 保存API配置
    g_database->SetConfigValue(prefix + "api_port", std::to_string(m_apiPort));
    g_database->SetConfigValue(prefix + "api_username", m_apiUsername);
    g_database->SetConfigValue(prefix + "api_password", m_apiPassword);
    g_database->SetConfigValue(prefix + "api_enabled", m_apiEnabled ? "1" : "0");
    g_database->SetConfigValue(prefix + "online_device_policy", std::to_string(static_cast<int>(m_onlineDevicePolicy)));
}

void Socks5PoolInstance::LoadFromDatabase() {
    if (!g_database) return;

    m_accounts.clear();

    const std::string prefix = "socks5_pool_" + m_id + "_";

    AB_LOG_INFO("[调试] 开始加载账号库 " + m_id + " 的账号数据");

    // 加载池名称和创建时间
    std::string poolName = g_database->GetConfigValue(prefix + "pool_name", "");
    if (!poolName.empty()) {
        m_config.name = poolName;
    }
    std::string createTime = g_database->GetConfigValue(prefix + "create_time", "");
    if (!createTime.empty()) {
        m_createTime = createTime;
    }

    // 加载下一个账号ID
    std::string nextIdStr = g_database->GetConfigValue(prefix + "next_account_id", "1");
    try {
        m_nextAccountId = std::stoi(nextIdStr);
    } catch (...) {
        m_nextAccountId = 1;
    }

    // 加载账号列表
    std::string jsonStr = g_database->GetConfigValue(prefix + "accounts", "");
    AB_LOG_INFO("[调试] 从数据库读取账号JSON: " + (jsonStr.empty() ? "(空)" : jsonStr.substr(0, 200)));
    if (jsonStr.empty()) {
        AB_LOG_INFO("[调试] 账号JSON为空，没有账号需要加载");
        return;
    }

    Json::CharReaderBuilder readerBuilder;
    std::unique_ptr<Json::CharReader> reader(readerBuilder.newCharReader());
    Json::Value root;
    std::string errors;

    if (!reader->parse(jsonStr.c_str(), jsonStr.c_str() + jsonStr.size(), &root, &errors)) {
        AB_LOG_ERROR("[SOCKS5Pool] 解析账号JSON失败: " + errors);
        return;
    }

    if (!root.isArray()) {
        AB_LOG_ERROR("[调试] 账号JSON不是数组格式");
        return;
    }

    AB_LOG_INFO("[调试] 开始解析账号，数组大小: " + std::to_string(root.size()));

    for (const auto& item : root) {
        Socks5Account acc;
        acc.id = item.get("id", 0).asInt();
        acc.username = item.get("username", "").asString();
        acc.password = item.get("password", "").asString();
        acc.expireTime = item.get("expireTime", "").asString();
        acc.maxConnections = item.get("maxConnections", 0).asInt();
        acc.currentConnections = 0;  // 运行时状态，不持久化
        acc.isEnabled = item.get("isEnabled", true).asBool();
        acc.createdAt = item.get("createdAt", "").asString();
        acc.lastLoginTime = item.get("lastLoginTime", "").asString();
        acc.lastLoginIP = item.get("lastLoginIP", "").asString();
        acc.totalOnlineSeconds = item.get("totalOnlineSeconds", 0).asUInt64();

        m_accounts.push_back(acc);
        AB_LOG_INFO("[调试] 加载账号: username=" + acc.username + ", password=" + acc.password +
                     ", enabled=" + (acc.isEnabled ? "true" : "false"));

        // 更新nextAccountId
        if (acc.id >= m_nextAccountId) {
            m_nextAccountId = acc.id + 1;
        }
    }

    AB_LOG_INFO("[调试] 账号加载完成，共加载 " + std::to_string(m_accounts.size()) + " 个账号");

    // 🔥 同步账号到 m_internalCollector
    if (m_internalCollector) {
        AB_LOG_INFO("[调试] 开始同步账号到 m_internalCollector");
        for (const auto& acc : m_accounts) {
            m_internalCollector->AddAccount(acc.username, acc.password, acc.expireTime, acc.maxConnections);
            AB_LOG_INFO("[调试] 同步账号到 m_internalCollector: username=" + acc.username);
        }
        AB_LOG_INFO("[调试] 账号同步完成，m_internalCollector 账号数: " +
                     std::to_string(m_internalCollector->GetAllAccounts().size()));
    }

    // 加载API配置
    std::string apiPortStr = g_database->GetConfigValue(prefix + "api_port", "90");
    try {
        m_apiPort = std::stoi(apiPortStr);
    } catch (...) {
        m_apiPort = 90;
    }
    m_apiUsername = g_database->GetConfigValue(prefix + "api_username", "admin");
    m_apiPassword = g_database->GetConfigValue(prefix + "api_password", "admin");

    // 加载API启用状态并自动启动
    std::string apiEnabledStr = g_database->GetConfigValue(prefix + "api_enabled", "0");
    m_apiEnabled = (apiEnabledStr == "1");
    std::string onlineDevicePolicyStr = g_database->GetConfigValue(prefix + "online_device_policy", "1");
    m_onlineDevicePolicy = (onlineDevicePolicyStr == "0")
        ? OnlineDevicePolicy::Unlimited
        : OnlineDevicePolicy::SingleDeviceSingleInstance;
    if (m_internalCollector) {
        m_internalCollector->SetSharedAccountSingleDeviceModeEnabled(
            m_onlineDevicePolicy == OnlineDevicePolicy::SingleDeviceSingleInstance);
    }
    if (m_apiEnabled) {
        // 延迟启动API服务器（需要先解锁mutex）
        // 这里只记录状态，实际启动在外部调用
    }
}

// ===== CCProxy API兼容服务方法 =====

bool Socks5PoolInstance::StartApiServer(int port, const std::string& username, const std::string& password) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_apiServer) {
        AB_LOG_WARNING("[SOCKS5Pool " + m_id + "] API服务已在运行");
        return false;
    }

    // 更新配置
    m_apiPort = port;
    m_apiUsername = username;
    m_apiPassword = password;

    // 同步账号到内部PacketCollector
    if (m_internalCollector) {
        // 清空现有账号
        auto existingAccounts = m_internalCollector->GetAllAccounts();
        for (const auto& acc : existingAccounts) {
            m_internalCollector->RemoveAccount(acc.username);
        }
        // 添加当前账号库的账号
        for (const auto& acc : m_accounts) {
            m_internalCollector->AddAccount(acc.username, acc.password, acc.expireTime, acc.maxConnections);
            Socks5Account updatedAcc = m_internalCollector->GetAccount(acc.username);
            updatedAcc.isEnabled = acc.isEnabled;
            m_internalCollector->UpdateAccountEx(updatedAcc);
        }
    }

    // 创建并启动API服务
    m_apiServer = new HttpApiServer(port, username, password);

    // 设置账号变更回调 - 当通过API修改账号时，同步回m_accounts并持久化
    m_apiServer->SetAccountChangedCallback([this]() {
        SyncAccountsFromInternalCollector();
    });

    if (m_apiServer->Start(m_internalCollector.get())) {
        AB_LOG_INFO("[SOCKS5Pool " + m_id + "] API服务已启动，端口: " + std::to_string(port));
        m_apiEnabled = true;  // 标记为启用
        SaveToDatabase();
        return true;
    } else {
        AB_LOG_ERROR("[SOCKS5Pool " + m_id + "] API服务启动失败");
        delete m_apiServer;
        m_apiServer = nullptr;
        return false;
    }
}

void Socks5PoolInstance::StopApiServer(bool updateEnabledState) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_apiServer) {
        m_apiServer->Stop();
        delete m_apiServer;
        m_apiServer = nullptr;

        // 🔥 只有在用户手动停止时才更新启用状态
        // 析构函数调用时不更新，保持原有的启用状态
        if (updateEnabledState) {
            m_apiEnabled = false;  // 标记为禁用
            SaveToDatabase();
        }

        AB_LOG_INFO("[SOCKS5Pool " + m_id + "] API服务已停止");
    }
}

bool Socks5PoolInstance::IsApiServerRunning() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_apiServer != nullptr && m_apiServer->IsRunning();
}

void Socks5PoolInstance::SetApiConfig(int port, const std::string& username, const std::string& password) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_apiPort = port;
    m_apiUsername = username;
    m_apiPassword = password;
    SaveToDatabase();
}

void Socks5PoolInstance::AutoStartApiIfEnabled() {
    // 如果API启用状态为true且当前未运行，则自动启动
    if (m_apiEnabled && !IsApiServerRunning()) {
        AB_LOG_INFO("[SOCKS5Pool " + m_id + "] 自动恢复API服务...");
        StartApiServer(m_apiPort, m_apiUsername, m_apiPassword);
    }
}

void Socks5PoolInstance::SyncAccountsFromInternalCollector() {
    // 注意：此方法由回调调用，不能加锁（调用者可能已持有锁）
    // 但我们需要保护m_accounts，所以使用try_lock或者确保调用链不会死锁

    if (!m_internalCollector) return;

    // 从内部PacketCollector获取最新账号列表
    auto collectorAccounts = m_internalCollector->GetAllAccounts();

    // 更新m_accounts（需要加锁）
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        m_accounts.clear();
        for (const auto& acc : collectorAccounts) {
            m_accounts.push_back(acc);

            // 更新nextAccountId
            if (acc.id >= m_nextAccountId) {
                m_nextAccountId = acc.id + 1;
            }
        }

        // 持久化到数据库
        SaveToDatabase();

        // 同步到所有已绑定的运行中实例（远程API变更也实时生效）
        NotifyBoundCollectors();
    }

    AB_LOG_INFO("[SOCKS5Pool " + m_id + "] 已从API同步 " + std::to_string(collectorAccounts.size()) + " 个账号");
}

void Socks5PoolInstance::RegisterBoundCollector(PacketCollector* collector) {
    if (!collector) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    // 避免重复注册
    for (const auto* c : m_boundCollectors) {
        if (c == collector) return;
    }
    m_boundCollectors.push_back(collector);
    AB_LOG_INFO("[SOCKS5Pool " + m_id + "] 注册绑定的Collector，当前绑定数: " + std::to_string(m_boundCollectors.size()));
    // 注意：调用方（SyncCollectorSocks5Accounts/SyncHeartbeatSocks5Accounts）已在注册前完成账号同步，
    // 此处不再重复同步，避免双重操作。
}

void Socks5PoolInstance::UnregisterBoundCollector(PacketCollector* collector) {
    if (!collector) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_boundCollectors.erase(
        std::remove(m_boundCollectors.begin(), m_boundCollectors.end(), collector),
        m_boundCollectors.end()
    );
    AB_LOG_INFO("[SOCKS5Pool " + m_id + "] 取消注册Collector，当前绑定数: " + std::to_string(m_boundCollectors.size()));
}

void Socks5PoolInstance::SyncAccountsToCollector(PacketCollector* collector) {
    // 注意：调用者已持有 m_mutex，此处不再加锁
    if (!collector) return;

    // 先清空 collector 中的所有账号
    auto existingAccounts = collector->GetAllAccounts();
    for (const auto& acc : existingAccounts) {
        collector->RemoveAccount(acc.username);
    }

    // 将当前账号库的所有账号同步到 collector
    for (const auto& acc : m_accounts) {
        if (collector->AddAccount(acc.username, acc.password, acc.expireTime, acc.maxConnections)) {
            Socks5Account updatedAcc = collector->GetAccount(acc.username);
            updatedAcc.isEnabled = acc.isEnabled;
            collector->UpdateAccountEx(updatedAcc);
        }
    }
}

void Socks5PoolInstance::NotifyBoundCollectors() {
    // 注意：调用者已持有 m_mutex
    for (auto* collector : m_boundCollectors) {
        if (collector) {
            SyncAccountsToCollector(collector);
        }
    }
    if (!m_boundCollectors.empty()) {
        AB_LOG_INFO("[SOCKS5Pool " + m_id + "] 账号变更已同步到 " + std::to_string(m_boundCollectors.size()) + " 个绑定实例");
    }
}

// ==================== InstanceManager的Socks5Pool管理方法 ====================

std::string InstanceManager::CreateSocks5PoolInstance(const InstanceConfig& config) {
    std::lock_guard<std::mutex> lock(m_mutex);

    std::string instanceId = GenerateInstanceId();

    auto instance = std::make_unique<Socks5PoolInstance>(instanceId, config);
    m_socks5Pools[instanceId] = std::move(instance);

    // 保存实例配置到config.db
    if (g_database) {
        g_database->SetConfigValue(InstanceManager::MakeInstanceConfigKey(instanceId, "name"), config.name);
        g_database->SetConfigValue(InstanceManager::MakeInstanceConfigKey(instanceId, "port"), std::to_string(config.port));
    }
    SaveInstanceList();

    AB_LOG_INFO("[实例管理] 创建SOCKS5账号库实例: " + instanceId + " (" + config.name + ")");

    return instanceId;
}

Socks5PoolInstance* InstanceManager::GetSocks5PoolInstance(const std::string& instanceId) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_socks5Pools.find(instanceId);
    if (it != m_socks5Pools.end()) {
        return it->second.get();
    }

    return nullptr;
}

std::vector<CollectorInstance*> InstanceManager::GetCollectorInstances() {
    std::lock_guard<std::mutex> lock(m_mutex);

    std::vector<CollectorInstance*> result;
    for (const auto& pair : m_collectors) {
        result.push_back(pair.second.get());
    }

    return result;
}

std::vector<HeartbeatInstance*> InstanceManager::GetHeartbeatInstances() {
    std::lock_guard<std::mutex> lock(m_mutex);

    std::vector<HeartbeatInstance*> result;
    for (const auto& pair : m_heartbeats) {
        result.push_back(pair.second.get());
    }

    return result;
}

std::vector<Socks5PoolInstance*> InstanceManager::GetSocks5PoolInstances() {
    std::lock_guard<std::mutex> lock(m_mutex);

    std::vector<Socks5PoolInstance*> result;
    for (const auto& pair : m_socks5Pools) {
        result.push_back(pair.second.get());
    }

    return result;
}

// ==================== SocksForwardInstance实现 ====================

SocksForwardInstance::SocksForwardInstance(const std::string& id, const InstanceConfig& config)
    : m_id(id)
    , m_config(config)
    , m_state(InstanceState::Stopped)
{
    // 创建PacketCollector，但不记录数据包
    m_collector = std::make_unique<PacketCollector>(config.port);

    // 设置实例ID（用于WPE滤镜判断）
    m_collector->SetInstanceId(id);

    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    struct tm timeinfo;
    localtime_s(&timeinfo, &time);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    m_createTime = buf;

    // 🔥 关键：构造时立即加载配置（确保配置在自启动前就绪）
    LoadConfigFromDatabase();

    AB_LOG_INFO("[实例管理] 创建Socks转发实例: " + m_id + " (端口:" + std::to_string(config.port) + ")");
}

SocksForwardInstance::~SocksForwardInstance() {
    Stop();
    AB_LOG_INFO("[实例管理] 销毁Socks转发实例: " + m_id);
}

// 🔥 从数据库加载配置到成员变量（构造时调用，确保配置始终可用）
void SocksForwardInstance::LoadConfigFromDatabase() {
    if (!g_database) {
        AB_LOG_WARNING("[实例管理] g_database未初始化，无法加载Socks转发实例配置: " + m_id);
        return;
    }

    try {
        auto getConfig = [this](const std::string& key, const std::string& defaultValue) -> std::string {
            return g_database->GetConfigValue(
                InstanceManager::MakeInstanceConfigKey(m_id, key), defaultValue);
        };

        auto getInt = [&getConfig](const std::string& key, int defaultValue) -> int {
            try {
                std::string val = getConfig(key, std::to_string(defaultValue));
                return val.empty() ? defaultValue : std::stoi(val);
            } catch (...) {
                return defaultValue;
            }
        };

        auto getBool = [&getConfig](const std::string& key, bool defaultValue) -> bool {
            std::string val = getConfig(key, defaultValue ? "1" : "0");
            return (val == "1" || val == "true");
        };

        // 加载线程模型配置
        m_cachedConfig.threadPoolMode = getInt("threadPoolMode", 0);
        m_cachedConfig.whitelistPoolSize = getInt("whitelistPoolSize", 10);
        m_cachedConfig.normalPoolSize = getInt("normalPoolSize", 50);
        m_cachedConfig.iocpMaxWhitelist = getInt("iocpMaxWhitelist", 2000);
        m_cachedConfig.iocpMaxNormal = getInt("iocpMaxNormal", 500);

        // 加载 SOCKS5 认证配置
        m_cachedConfig.enableSocks5Auth = getBool("enableSocks5Auth", false);
        m_cachedConfig.accountSourceInstanceId = getConfig("accountSourceInstanceId", "");
        m_cachedConfig.instanceDeviceLimit = getInt("instanceDeviceLimit", 0);

        // 加载二级代理配置
        m_cachedConfig.enableSecondaryProxy = getBool("enableSecondaryProxy", false);
        m_cachedConfig.secondaryProxyHost = getConfig("secondaryProxyHost", "127.0.0.1");
        m_cachedConfig.secondaryProxyPort = getInt("secondaryProxyPort", 1080);
        m_cachedConfig.secondaryProxyUsername = getConfig("secondaryProxyUsername", "");
        m_cachedConfig.secondaryProxyPassword = getConfig("secondaryProxyPassword", "");

        // 加载分包处理配置
        m_cachedConfig.enablePacketSplit = getBool("enablePacketSplit", true);
        m_cachedConfig.packetSplitPorts = getConfig("packetSplitPorts", "");
        m_cachedConfig.applyWpeOnNonSplitTraffic = getBool("applyWpeOnNonSplitTraffic", false);

        // 加载流量过滤配置
        m_cachedConfig.enableTrafficFilter = getBool("enableTrafficFilter", false);
        m_cachedConfig.enableSniSniffing = getBool("enableSniSniffing", false);
        m_cachedConfig.trafficFilterRules = getConfig("trafficFilterRules", "");
        m_cachedConfig.enableSSLMitm = getBool("enableSSLMitm", false);
        m_cachedConfig.sslMitmRules = getConfig("sslMitmRules", "[]");
        m_cachedConfig.enableHttpLocalMap = getBool("enableHttpLocalMap", false);
        m_cachedConfig.httpLocalMapRules = getConfig("httpLocalMapRules", "[]");
        m_cachedConfig.disconnectRulesJson = getConfig("disconnectRulesJson", "[]");

        // 加载用户滤镜配置
        m_cachedConfig.enableUserFilterMode = getBool("enableUserFilterMode", false);
        m_cachedConfig.userFilterHttpPort = getInt("userFilterHttpPort", 8080);

        // 加载防CC配置
        m_cachedConfig.antiCC.enabled = getBool("anticc_enabled", false);
        m_cachedConfig.antiCC.timeWindowSeconds = getInt("anticc_timeWindowSeconds", 10);
        m_cachedConfig.antiCC.maxRequestsInWindow = getInt("anticc_maxRequestsInWindow", 20);
        m_cachedConfig.antiCC.banTimeSeconds = getInt("anticc_banTimeSeconds", 300);
        m_cachedConfig.antiCC.maxConnections = getInt("anticc_maxConnections", 100);
        m_cachedConfig.antiCC.authFailBanTime = getInt("anticc_authFailBanTime", 60);
        m_cachedConfig.antiCC.noAuthBanTime = getInt("anticc_noAuthBanTime", 30);
        m_cachedConfig.antiCC.whitelistDuration = getInt("anticc_whitelistDuration", 3600);
        m_cachedConfig.antiCC.useBlacklist = getBool("anticc_useBlacklist", true);
        m_cachedConfig.antiCC.useWhitelist = getBool("anticc_useWhitelist", true);
        m_cachedConfig.antiCC.useFirewall = getBool("anticc_useFirewall", false);
        m_cachedConfig.antiCC.rateLimitEnabled = getBool("anticc_rateLimitEnabled", false);
        m_cachedConfig.antiCC.rateLimit = getInt("anticc_rateLimit", 100);
        m_cachedConfig.antiCC.rateTimeWindow = getInt("anticc_rateTimeWindow", 1);
        m_cachedConfig.antiCC.blockNonSocks = getBool("anticc_blockNonSocks", false);
        m_cachedConfig.antiCC.enableAuthPriorityAdmission = getBool("anticc_enableAuthPriorityAdmission", true);
        m_cachedConfig.antiCC.authPriorityQueueLimit = getInt("anticc_authPriorityQueueLimit", 128);
        m_cachedConfig.antiCC.enableLowPriorityEviction = getBool("anticc_enableLowPriorityEviction", true);
        m_cachedConfig.antiCC.lowPriorityEvictionThreshold = getInt("anticc_lowPriorityEvictionThreshold", 80);
        m_cachedConfig.antiCC.enableCoordinator = getBool("anticc_enableCoordinator", true);

        AB_LOG_INFO("[实例管理] Socks转发实例配置加载完成: " + m_id);
    }
    catch (const std::exception& e) {
        AB_LOG_ERROR("[实例管理] 加载Socks转发实例配置异常: " + m_id + " - " + e.what());
    }
}

// 🔥 将缓存的配置保存到数据库
void SocksForwardInstance::SaveConfigToDatabase() {
    if (!g_database) {
        AB_LOG_WARNING("[实例管理] g_database未初始化，无法保存Socks转发实例配置: " + m_id);
        return;
    }

    try {
        auto setConfig = [this](const std::string& key, const std::string& value) {
            g_database->SetConfigValue(InstanceManager::MakeInstanceConfigKey(m_id, key), value);
        };

        // 保存线程模型配置
        setConfig("threadPoolMode", std::to_string(m_cachedConfig.threadPoolMode));
        setConfig("whitelistPoolSize", std::to_string(m_cachedConfig.whitelistPoolSize));
        setConfig("normalPoolSize", std::to_string(m_cachedConfig.normalPoolSize));
        setConfig("iocpMaxWhitelist", std::to_string(m_cachedConfig.iocpMaxWhitelist));
        setConfig("iocpMaxNormal", std::to_string(m_cachedConfig.iocpMaxNormal));

        // 保存 SOCKS5 认证配置
        setConfig("enableSocks5Auth", m_cachedConfig.enableSocks5Auth ? "1" : "0");
        setConfig("accountSourceInstanceId", m_cachedConfig.accountSourceInstanceId);
        setConfig("instanceDeviceLimit", std::to_string(m_cachedConfig.instanceDeviceLimit));

        // 保存二级代理配置
        setConfig("enableSecondaryProxy", m_cachedConfig.enableSecondaryProxy ? "1" : "0");
        setConfig("secondaryProxyHost", m_cachedConfig.secondaryProxyHost);
        setConfig("secondaryProxyPort", std::to_string(m_cachedConfig.secondaryProxyPort));
        setConfig("secondaryProxyUsername", m_cachedConfig.secondaryProxyUsername);
        setConfig("secondaryProxyPassword", m_cachedConfig.secondaryProxyPassword);

        // 保存分包处理配置
        setConfig("enablePacketSplit", m_cachedConfig.enablePacketSplit ? "1" : "0");
        setConfig("packetSplitPorts", m_cachedConfig.packetSplitPorts);
        setConfig("applyWpeOnNonSplitTraffic", m_cachedConfig.applyWpeOnNonSplitTraffic ? "1" : "0");

        // 保存流量过滤配置
        setConfig("enableTrafficFilter", m_cachedConfig.enableTrafficFilter ? "1" : "0");
        setConfig("enableSniSniffing", m_cachedConfig.enableSniSniffing ? "1" : "0");
        setConfig("trafficFilterRules", m_cachedConfig.trafficFilterRules);
        setConfig("enableSSLMitm", m_cachedConfig.enableSSLMitm ? "1" : "0");
        setConfig("sslMitmRules", m_cachedConfig.sslMitmRules);
        setConfig("enableHttpLocalMap", m_cachedConfig.enableHttpLocalMap ? "1" : "0");
        setConfig("httpLocalMapRules", m_cachedConfig.httpLocalMapRules);
        setConfig("disconnectRulesJson", m_cachedConfig.disconnectRulesJson);

        // 保存用户滤镜配置
        setConfig("enableUserFilterMode", m_cachedConfig.enableUserFilterMode ? "1" : "0");
        setConfig("userFilterHttpPort", std::to_string(m_cachedConfig.userFilterHttpPort));

        // 保存防CC配置
        setConfig("anticc_enabled", m_cachedConfig.antiCC.enabled ? "1" : "0");
        setConfig("anticc_timeWindowSeconds", std::to_string(m_cachedConfig.antiCC.timeWindowSeconds));
        setConfig("anticc_maxRequestsInWindow", std::to_string(m_cachedConfig.antiCC.maxRequestsInWindow));
        setConfig("anticc_banTimeSeconds", std::to_string(m_cachedConfig.antiCC.banTimeSeconds));
        setConfig("anticc_maxConnections", std::to_string(m_cachedConfig.antiCC.maxConnections));
        setConfig("anticc_authFailBanTime", std::to_string(m_cachedConfig.antiCC.authFailBanTime));
        setConfig("anticc_noAuthBanTime", std::to_string(m_cachedConfig.antiCC.noAuthBanTime));
        setConfig("anticc_whitelistDuration", std::to_string(m_cachedConfig.antiCC.whitelistDuration));
        setConfig("anticc_useBlacklist", m_cachedConfig.antiCC.useBlacklist ? "1" : "0");
        setConfig("anticc_useWhitelist", m_cachedConfig.antiCC.useWhitelist ? "1" : "0");
        setConfig("anticc_useFirewall", m_cachedConfig.antiCC.useFirewall ? "1" : "0");
        setConfig("anticc_rateLimitEnabled", m_cachedConfig.antiCC.rateLimitEnabled ? "1" : "0");
        setConfig("anticc_rateLimit", std::to_string(m_cachedConfig.antiCC.rateLimit));
        setConfig("anticc_rateTimeWindow", std::to_string(m_cachedConfig.antiCC.rateTimeWindow));
        setConfig("anticc_blockNonSocks", m_cachedConfig.antiCC.blockNonSocks ? "1" : "0");
        setConfig("anticc_enableAuthPriorityAdmission", m_cachedConfig.antiCC.enableAuthPriorityAdmission ? "1" : "0");
        setConfig("anticc_authPriorityQueueLimit", std::to_string(m_cachedConfig.antiCC.authPriorityQueueLimit));
        setConfig("anticc_enableLowPriorityEviction", m_cachedConfig.antiCC.enableLowPriorityEviction ? "1" : "0");
        setConfig("anticc_lowPriorityEvictionThreshold", std::to_string(m_cachedConfig.antiCC.lowPriorityEvictionThreshold));
        setConfig("anticc_enableCoordinator", m_cachedConfig.antiCC.enableCoordinator ? "1" : "0");

        AB_LOG_INFO("[实例管理] Socks转发实例配置保存完成: " + m_id);
    }
    catch (const std::exception& e) {
        AB_LOG_ERROR("[实例管理] 保存Socks转发实例配置异常: " + m_id + " - " + e.what());
    }
}

// 🔥 将缓存的配置应用到 PacketCollector（启动时调用）
void SocksForwardInstance::ApplyConfigToCollector() {
    if (!m_collector) {
        AB_LOG_ERROR("[实例管理] PacketCollector未初始化: " + m_id);
        return;
    }

    try {
        AB_LOG_INFO("[实例管理] 开始应用Socks转发实例配置: " + m_id);

        // 1. 应用线程模型配置
        PacketCollector::ThreadPoolMode mode = PacketCollector::ThreadPoolMode::TRADITIONAL;
        if (m_cachedConfig.threadPoolMode == 1) {
            mode = PacketCollector::ThreadPoolMode::BLOCKING;
        } else if (m_cachedConfig.threadPoolMode == 2) {
            mode = PacketCollector::ThreadPoolMode::IOCP;
        }
        m_collector->SetThreadPoolMode(mode);

        if (m_cachedConfig.threadPoolMode == 1) {
            // 阻塞式线程池
            int wlPool = (std::max)(1, m_cachedConfig.whitelistPoolSize);
            int nlPool = (std::max)(1, m_cachedConfig.normalPoolSize);
            m_collector->SetThreadPoolSizes(wlPool, nlPool);
            AB_LOG_INFO("[实例管理] 应用线程池配置 - 白名单:" + std::to_string(wlPool) +
                        " 普通:" + std::to_string(nlPool));
        } else if (m_cachedConfig.threadPoolMode == 2) {
            // IOCP模式
            int iocpWl = (std::max)(1, m_cachedConfig.iocpMaxWhitelist);
            int iocpNl = (std::max)(1, m_cachedConfig.iocpMaxNormal);
            m_collector->SetIOCPMaxConnections(iocpWl, iocpNl);
            AB_LOG_INFO("[实例管理] 应用IOCP配置 - 白名单:" + std::to_string(iocpWl) +
                        " 普通:" + std::to_string(iocpNl));
        }

        // 2. 应用 SOCKS5 认证配置
        m_collector->SetSocks5Auth(m_cachedConfig.enableSocks5Auth);
        m_collector->SetInstanceDeviceLimit(0);
        AB_LOG_INFO("[实例管理] 实例级设备数限制已停用，设备策略由账号库侧控制");

        if (m_cachedConfig.enableSocks5Auth && !m_cachedConfig.accountSourceInstanceId.empty()) {
            AB_LOG_INFO("[实例管理] 启用SOCKS5认证，账号库: " + m_cachedConfig.accountSourceInstanceId);

            try {
            auto& instMgr = InstanceManager::GetInstance();
            auto* poolInstance = instMgr.GetSocks5PoolInstance(m_cachedConfig.accountSourceInstanceId);

            if (poolInstance) {
                PacketCollector* poolCollector = poolInstance->GetInternalCollector();
                if (poolCollector) {
                    // 🔥 使用 SetExternalAccountSource 绑定账号库
                    m_collector->SetExternalAccountSource(poolCollector);
                    AB_LOG_INFO("[实例管理] 已绑定账号库: " + m_cachedConfig.accountSourceInstanceId);
                } else {
                    m_collector->SetExternalAccountSource(nullptr);
                    AB_LOG_WARNING("[实例管理] 账号库实例的 collector 为空: " + m_cachedConfig.accountSourceInstanceId);
                }
            } else {
                m_collector->SetExternalAccountSource(nullptr);
                AB_LOG_WARNING("[实例管理] 未找到账号库实例: " + m_cachedConfig.accountSourceInstanceId);
            }
            } catch (const std::system_error& e) {
                m_collector->SetExternalAccountSource(nullptr);
                AB_LOG_WARNING("[实例管理] 绑定账号库时发生锁冲突: " + std::string(e.what()));
            }
        } else {
            // 清除账号库绑定
            m_collector->SetExternalAccountSource(nullptr);
        }

        // 3. 应用二级代理配置
        AB_LOG_INFO("[实例管理] 正在应用二级代理配置: enable=" +
            std::to_string(m_cachedConfig.enableSecondaryProxy) +
            ", host=" + m_cachedConfig.secondaryProxyHost +
            ", port=" + std::to_string(m_cachedConfig.secondaryProxyPort));

        m_collector->SetSecondaryProxy(
            m_cachedConfig.enableSecondaryProxy,
            m_cachedConfig.secondaryProxyHost,
            m_cachedConfig.secondaryProxyPort,
            m_cachedConfig.secondaryProxyUsername,
            m_cachedConfig.secondaryProxyPassword
        );

        if (m_cachedConfig.enableSecondaryProxy) {
            AB_LOG_INFO("[实例管理] 启用二级代理: " + m_cachedConfig.secondaryProxyHost +
                        ":" + std::to_string(m_cachedConfig.secondaryProxyPort));
        } else {
            AB_LOG_INFO("[实例管理] 二级代理已禁用");
        }

        // 4. 应用分包处理配置
        m_collector->SetPacketSplitEnabled(m_cachedConfig.enablePacketSplit);

        if (m_cachedConfig.enablePacketSplit && !m_cachedConfig.packetSplitPorts.empty()) {
            std::vector<int> ports;
            std::stringstream ss(m_cachedConfig.packetSplitPorts);
            std::string item;

            while (std::getline(ss, item, ',')) {
                try {
                    int port = std::stoi(item);
                    if (port > 0 && port <= 65535) {
                        ports.push_back(port);
                    }
                } catch (...) {}
            }

            if (!ports.empty()) {
                m_collector->SetPacketSplitPorts(ports);
                AB_LOG_INFO("[实例管理] 分包端口: " + m_cachedConfig.packetSplitPorts);
            }
        }

        m_collector->SetApplyWpeOnNonSplitTraffic(m_cachedConfig.applyWpeOnNonSplitTraffic);

        // 5. 应用流量过滤配置
        m_collector->SetTrafficFilterEnabled(m_cachedConfig.enableTrafficFilter);
        m_collector->SetSNISniffingEnabled(m_cachedConfig.enableSniSniffing);

        if (m_cachedConfig.enableTrafficFilter && !m_cachedConfig.trafficFilterRules.empty() &&
            m_cachedConfig.trafficFilterRules != "[]") {
            try {
                json rulesJson = json::parse(m_cachedConfig.trafficFilterRules);

                // 先清空现有规则
                m_collector->ClearAllTrafficRules();

                if (rulesJson.is_array()) {
                    int loadedCount = 0;
                    for (const auto& ruleJson : rulesJson) {
                        TrafficFilterRule rule;
                        rule.id = ruleJson.value("id", 0);
                        rule.type = static_cast<TrafficRuleType>(ruleJson.value("type", 0));
                        rule.value1 = ruleJson.value("value1", "");
                        rule.value2 = ruleJson.value("value2", "");
                        rule.enabled = ruleJson.value("enabled", true);
                        rule.description = ruleJson.value("description", "");

                        m_collector->AddTrafficRule(rule);
                        loadedCount++;
                    }
                    AB_LOG_INFO("[实例管理] 已加载 " + std::to_string(loadedCount) + " 条流量过滤规则");
                }
            } catch (const json::exception& e) {
                AB_LOG_WARNING("[实例管理] 流量过滤规则JSON解析失败: " + std::string(e.what()));
            } catch (...) {
                AB_LOG_WARNING("[实例管理] 流量过滤规则解析失败");
            }
        }

        // 6. 应用 SSL MITM 配置
        m_collector->SetSSLMitmEnabled(m_cachedConfig.enableSSLMitm);
        m_collector->ClearSSLMitmRules();

        if (m_cachedConfig.enableSSLMitm) {
            if (!PacketCollector::InitSSLMitmCA()) {
                const std::string detail = PacketCollector::GetSSLMitmLastErrorDetail();
                AB_LOG_WARNING("[实例管理] SSL MITM CA 初始化失败" +
                    (detail.empty() ? std::string() : (": " + detail)));
            }
        }

        if (!m_cachedConfig.sslMitmRules.empty() && m_cachedConfig.sslMitmRules != "[]") {
            try {
                json mitmRulesJson = json::parse(m_cachedConfig.sslMitmRules);
                if (mitmRulesJson.is_array()) {
                    int loadedCount = 0;
                    for (const auto& ruleJson : mitmRulesJson) {
                        const bool matchByPort = ruleJson.value("matchByPort", true);
                        if (matchByPort) {
                            const int port = ruleJson.value("port", 0);
                            if (port > 0 && port <= 65535) {
                                m_collector->AddSSLMitmPortRule(port);
                                loadedCount++;
                            }
                        } else {
                            const std::string domain = ruleJson.value("domain", "");
                            if (!domain.empty()) {
                                m_collector->AddSSLMitmDomainRule(domain);
                                loadedCount++;
                            }
                        }
                    }
                    AB_LOG_INFO("[实例管理] 已加载 " + std::to_string(loadedCount) + " 条 SSL MITM 规则");
                }
            } catch (const json::exception& e) {
                AB_LOG_WARNING("[实例管理] SSL MITM 规则JSON解析失败: " + std::string(e.what()));
            } catch (...) {
                AB_LOG_WARNING("[实例管理] SSL MITM 规则解析失败");
            }
        }

        // 7. 应用 HTTP 本地映射配置
        m_collector->SetHttpLocalMapEnabled(m_cachedConfig.enableHttpLocalMap);
        m_collector->ClearHttpLocalMapRules();

        if (!m_cachedConfig.httpLocalMapRules.empty() && m_cachedConfig.httpLocalMapRules != "[]") {
            try {
                json localMapRulesJson = json::parse(m_cachedConfig.httpLocalMapRules);
                if (localMapRulesJson.is_array()) {
                    std::vector<HttpLocalMapRule> rules;
                    for (const auto& ruleJson : localMapRulesJson) {
                        HttpLocalMapRule rule;
                        rule.id = ruleJson.value("id", 0);
                        rule.enabled = ruleJson.value("enabled", true);
                        rule.scheme = ruleJson.value("scheme", "*");
                        rule.hostPattern = ruleJson.value("hostPattern", "*");
                        rule.pathPattern = ruleJson.value("pathPattern", "/");
                        rule.method = ruleJson.value("method", "*");
                        rule.localFilePath = ruleJson.value("localFilePath", "");
                        rule.contentType = ruleJson.value("contentType", "");

                        if (!rule.localFilePath.empty()) {
                            rules.push_back(std::move(rule));
                        }
                    }

                    m_collector->SetHttpLocalMapRules(rules);
                    AB_LOG_INFO("[实例管理] 已加载 " + std::to_string(rules.size()) + " 条 HTTP 本地映射规则");
                }
            } catch (const json::exception& e) {
                AB_LOG_WARNING("[实例管理] HTTP 本地映射规则JSON解析失败: " + std::string(e.what()));
            } catch (...) {
                AB_LOG_WARNING("[实例管理] HTTP 本地映射规则解析失败");
            }
        }

        // 8. 应用断网规则配置
        m_collector->ClearDisconnectRules();
        if (!m_cachedConfig.disconnectRulesJson.empty() && m_cachedConfig.disconnectRulesJson != "[]") {
            try {
                std::vector<DisconnectRule> rules;
                std::string error;
                if (DisconnectRuleCodec::DeserializeRulesFromJson(m_cachedConfig.disconnectRulesJson, rules, &error)) {
                    std::string applyError;
                    if (m_collector->SetDisconnectRules(rules, &applyError)) {
                        AB_LOG_INFO("[实例管理] 已加载 " + std::to_string(rules.size()) + " 条断网规则");
                    } else {
                        AB_LOG_WARNING("[实例管理] 断网规则应用失败: " + applyError);
                    }
                } else {
                    AB_LOG_WARNING("[实例管理] 断网规则JSON解析失败: " + error);
                }
            } catch (const std::exception& e) {
                AB_LOG_WARNING("[实例管理] 断网规则加载异常: " + std::string(e.what()));
            } catch (...) {
                AB_LOG_WARNING("[实例管理] 断网规则加载失败");
            }
        }

        // 7. 应用用户滤镜模式配置
        m_collector->SetUserFilterMode(m_cachedConfig.enableUserFilterMode);
        AB_LOG_INFO("[实例管理] 用户滤镜模式: " + std::string(m_cachedConfig.enableUserFilterMode ? "启用" : "禁用"));

        // 8. 应用防CC配置
        AntiCCConfig antiCCConfig;
        antiCCConfig.enabled = m_cachedConfig.antiCC.enabled;
        antiCCConfig.timeWindowSeconds = m_cachedConfig.antiCC.timeWindowSeconds;
        antiCCConfig.maxRequestsInWindow = m_cachedConfig.antiCC.maxRequestsInWindow;
        antiCCConfig.banTimeSeconds = m_cachedConfig.antiCC.banTimeSeconds;
        antiCCConfig.maxConnections = m_cachedConfig.antiCC.maxConnections;
        antiCCConfig.useBlacklist = m_cachedConfig.antiCC.useBlacklist;
        antiCCConfig.useWhitelist = m_cachedConfig.antiCC.useWhitelist;
        antiCCConfig.whitelistDuration = m_cachedConfig.antiCC.whitelistDuration;
        antiCCConfig.authFailBanTime = m_cachedConfig.antiCC.authFailBanTime;
        antiCCConfig.noAuthBanTime = m_cachedConfig.antiCC.noAuthBanTime;
        antiCCConfig.useFirewall = m_cachedConfig.antiCC.useFirewall;
        antiCCConfig.rateLimitEnabled = m_cachedConfig.antiCC.rateLimitEnabled;
        antiCCConfig.rateLimit = m_cachedConfig.antiCC.rateLimit;
        antiCCConfig.rateTimeWindow = m_cachedConfig.antiCC.rateTimeWindow;
        antiCCConfig.listenPort = m_config.port;
        antiCCConfig.blockNonSocks = m_cachedConfig.antiCC.blockNonSocks;

        m_collector->SetAntiCCDatabaseManager(g_database);
        m_collector->SetAntiCCConfig(antiCCConfig);
        m_collector->SetAntiCCEnabled(m_cachedConfig.antiCC.enabled);
        m_collector->SetAntiCCAuthPriorityAdmissionEnabled(m_cachedConfig.antiCC.enableAuthPriorityAdmission);
        m_collector->SetAntiCCAuthPriorityQueueLimit(m_cachedConfig.antiCC.authPriorityQueueLimit);
        m_collector->SetAntiCCLowPriorityEvictionEnabled(m_cachedConfig.antiCC.enableLowPriorityEviction);
        m_collector->SetAntiCCLowPriorityEvictionThreshold(m_cachedConfig.antiCC.lowPriorityEvictionThreshold);
        m_collector->LoadAntiCCWhitelistFromDB();

        GlobalAntiCCCoordinator::GetInstance().SetDatabaseManager(g_database);
        GlobalAntiCCCoordinator::GetInstance().UpdateInstanceConfig(
            m_id,
            m_config.port,
            m_cachedConfig.antiCC.maxConnections,
            m_cachedConfig.antiCC.useFirewall);

        AB_LOG_INFO("[实例管理] Socks转发实例配置应用完成: " + m_id);
    }
    catch (const std::exception& e) {
        AB_LOG_ERROR("[实例管理] 应用Socks转发实例配置异常: " + m_id + " - " + e.what());
    }
}

// 🔥 获取缓存的配置（供UI读取）
SocksForwardInstance::CachedConfig SocksForwardInstance::GetCachedConfig() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_cachedConfig;
}

void SocksForwardInstance::SyncUserFilterHttpServerState(bool forceRestart) {
    PacketCollector* collector = nullptr;
    bool enableUserFilterMode = false;
    bool enableSocks5Auth = false;
    int httpPort = 0;
    std::string accountSourceInstanceId;
    std::string instanceId;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        collector = m_collector.get();
        enableUserFilterMode = m_cachedConfig.enableUserFilterMode;
        enableSocks5Auth = m_cachedConfig.enableSocks5Auth;
        httpPort = m_cachedConfig.userFilterHttpPort;
        accountSourceInstanceId = m_cachedConfig.accountSourceInstanceId;
        instanceId = m_id;
    }

    if (!collector) {
        return;
    }

    collector->SetSocks5Auth(enableSocks5Auth);
    if (enableSocks5Auth && !accountSourceInstanceId.empty()) {
        auto* poolInstance = InstanceManager::GetInstance().GetSocks5PoolInstance(accountSourceInstanceId);
        if (poolInstance && poolInstance->GetInternalCollector()) {
            collector->SetExternalAccountSource(poolInstance->GetInternalCollector());
        } else {
            collector->SetExternalAccountSource(nullptr);
            AB_LOG_WARNING("[实例管理] 用户滤镜HTTP服务恢复时未找到账号库实例: " + accountSourceInstanceId);
        }
    } else {
        collector->SetExternalAccountSource(nullptr);
    }

    collector->SetUserFilterMode(enableUserFilterMode);
    collector->SetUserFilterHttpPort(httpPort);

    if (g_userFilterManager) {
        g_userFilterManager->LoadInstanceConfig(instanceId);
    }

    if (!enableUserFilterMode) {
        if (collector->IsUserFilterHttpServerRunning()) {
            collector->StopUserFilterHttpServer();
            AB_LOG_INFO("[实例管理] 用户滤镜HTTP服务器已按配置停止: " + instanceId);
        }
        return;
    }

    if (forceRestart && collector->IsUserFilterHttpServerRunning()) {
        collector->StopUserFilterHttpServer();
        AB_LOG_INFO("[实例管理] 用户滤镜HTTP服务器按配置重启: " + instanceId);
    }

    if (!collector->IsUserFilterHttpServerRunning()) {
        if (collector->StartUserFilterHttpServer()) {
            AB_LOG_INFO("[实例管理] 用户滤镜HTTP服务器已恢复/启动: " + instanceId +
                " 端口: " + std::to_string(httpPort));
        } else {
            AB_LOG_WARNING("[实例管理] 用户滤镜HTTP服务器启动失败: " + instanceId);
        }
    }
}

// 🔥 更新配置（供UI修改，会同步到数据库，如果实例运行中会立即应用）
void SocksForwardInstance::UpdateConfig(const CachedConfig& newConfig) {
    bool isRunning = false;
    bool forceRestartUserFilterHttpServer = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        AB_LOG_INFO("[实例管理] 更新Socks转发实例配置: " + m_id);

        forceRestartUserFilterHttpServer =
            (m_cachedConfig.userFilterHttpPort != newConfig.userFilterHttpPort);

        // 更新缓存
        m_cachedConfig = newConfig;

        // 保存到数据库
        SaveConfigToDatabase();
        isRunning = (m_state == InstanceState::Running);
    }

    // 如果实例正在运行，立即应用配置
    if (isRunning) {
        AB_LOG_INFO("[实例管理] 实例运行中，立即应用新配置: " + m_id);
        ApplyConfigToCollector();
    }

    // 用户滤镜 HTTP 服务独立于实例运行态，配置变更后始终同步
    SyncUserFilterHttpServerState(forceRestartUserFilterHttpServer);

    if (!isRunning) {
        AB_LOG_INFO("[实例管理] 实例未运行，用户滤镜HTTP服务已按配置独立同步: " + m_id);
    }
}

void SocksForwardInstance::ClearAccountSourceBinding() {
    bool hadBinding = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        hadBinding = !m_cachedConfig.accountSourceInstanceId.empty();
        if (hadBinding) {
            AB_LOG_INFO("[实例管理] 清理Socks转发实例账号库绑定: " + m_id +
                " <- " + m_cachedConfig.accountSourceInstanceId);
            m_cachedConfig.accountSourceInstanceId.clear();
            SaveConfigToDatabase();
        }
        if (m_collector) {
            m_collector->SetExternalAccountSource(nullptr);
        }
    }

    if (hadBinding) {
        SyncUserFilterHttpServerState(false);
    }
}

bool SocksForwardInstance::Start() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_state == InstanceState::Running) {
            m_lastError = "实例已在运行中";
            return false;
        }

        m_state = InstanceState::Starting;
        AB_LOG_INFO("[实例管理] 启动Socks转发实例: " + m_id);

        try {
            // 🔥 标记为自动启动（下次软件启动时会自动启动）
            if (g_database) {
                g_database->SetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(m_id, "autoStart"), "1");
            }
        } catch (const std::exception& e) {
            m_state = InstanceState::Error;
            m_lastError = std::string("启动异常: ") + e.what();
            AB_LOG_ERROR("[实例管理] Socks转发实例启动异常: " + m_id + " - " + e.what());
            return false;
        }
    } // 🔥 释放 m_mutex 锁，避免在调用 ApplyConfigToCollector() 时死锁

    // 🔥 在锁外应用配置到 Collector（避免死锁）
    // ApplyConfigToCollector() 内部会调用 InstanceManager::GetSocks5PoolInstance()
    // 该方法会获取 InstanceManager::m_mutex，如果在持有 m_mutex 时调用会导致死锁
    try {
        ApplyConfigToCollector();

        // 🔥 设置数据包修改器（WPE滤镜）- 请求方向
        m_collector->SetPacketModifier([this](const PacketInfo& packet, const std::vector<uint8_t>& originalData) -> PacketTransformResult {
            PacketTransformResult result;
            result.forwardedData = originalData;
            result.callbackData = originalData;
            result.intercepted = false;

            // 原始请求包已在底层连接处理路径中统一记录，这里不再重复记包，
            // 避免 Raw/Charles 视图出现重复记录且丢失连接上下文。

            // 应用WPE滤镜
            if (g_wpeFilterManager) {
                std::vector<uint8_t> modifiedData = originalData;

                // 🔥 获取用户启用的滤镜列表（如果启用了用户滤镜模式）
                std::vector<int> userEnabledFilters;
                const std::vector<int>* pUserEnabledFilters = nullptr;
                if (m_collector && m_collector->IsUserFilterModeEnabled() && !packet.socksUsername.empty()) {
                    // 🔥 使用 GetEffectiveUserFilters：如果用户未配置，返回默认配置
                    auto filterSet = g_userFilterManager->GetEffectiveUserFilters(m_id, packet.socksUsername);
                    userEnabledFilters.assign(filterSet.begin(), filterSet.end());
                    pUserEnabledFilters = &userEnabledFilters;

                    // 🔥 调试日志
                    std::ostringstream oss;
                    oss << "[调试-用户滤镜] 用户=" << packet.socksUsername
                        << ", 启用滤镜数=" << userEnabledFilters.size() << ", ID列表=[";
                    for (size_t i = 0; i < userEnabledFilters.size(); ++i) {
                        if (i > 0) oss << ",";
                        oss << userEnabledFilters[i];
                    }
                    oss << "]";
                    AB_LOG_INFO(oss.str());
                } else {
                    // 🔥 调试日志：为什么没有启用用户滤镜模式
                    AB_LOG_INFO("[调试-用户滤镜] 未启用用户滤镜模式: collector=" +
                                std::string(m_collector ? "有效" : "无效") +
                                ", IsUserFilterModeEnabled=" +
                                std::string(m_collector && m_collector->IsUserFilterModeEnabled() ? "true" : "false") +
                                ", username=" + packet.socksUsername);
                }

                auto filterResult = g_wpeFilterManager->ProcessPacket(
                    modifiedData,
                    m_id,  // 实例ID
                    true,  // isRequest (客户端→服务器)
                    false, // isCollector (SOCKS转发实例不是采集端)
                    WPEFilter::FilterPriority::BeforeHeartbeat,  // 先执行滤镜
                    packet.socksUsername,  // 账号名
                    pUserEnabledFilters    // 用户启用的滤镜列表
                );

                if (filterResult.intercepted) {
                    result.forwardedData.clear();
                    result.intercepted = true;
                    return result;
                }

                if (filterResult.modified) {
                    result.forwardedData = modifiedData;
                }
            }

            return result;
        });

        // 🔥 设置响应包修改器（WPE滤镜）- 响应方向
        m_collector->SetResponseModifier([this](const PacketInfo& packet, const std::vector<uint8_t>& originalData) -> PacketTransformResult {
            PacketTransformResult result;
            result.forwardedData = originalData;
            result.callbackData = originalData;
            result.intercepted = false;

            // 原始响应包已在底层连接处理路径中统一记录，这里不再重复记包，
            // 避免 Raw/Charles 视图出现重复记录且丢失连接上下文。

            // 应用WPE滤镜
            if (g_wpeFilterManager) {
                std::vector<uint8_t> modifiedData = originalData;

                // 🔥 获取用户启用的滤镜列表（如果启用了用户滤镜模式）
                std::vector<int> userEnabledFilters;
                const std::vector<int>* pUserEnabledFilters = nullptr;
                if (m_collector && m_collector->IsUserFilterModeEnabled() && !packet.socksUsername.empty()) {
                    // 🔥 使用 GetEffectiveUserFilters：如果用户未配置，返回默认配置
                    auto filterSet = g_userFilterManager->GetEffectiveUserFilters(m_id, packet.socksUsername);
                    userEnabledFilters.assign(filterSet.begin(), filterSet.end());
                    pUserEnabledFilters = &userEnabledFilters;

                    // 🔥 调试日志
                    std::ostringstream oss;
                    oss << "[调试-用户滤镜] 用户=" << packet.socksUsername
                        << ", 启用滤镜数=" << userEnabledFilters.size() << ", ID列表=[";
                    for (size_t i = 0; i < userEnabledFilters.size(); ++i) {
                        if (i > 0) oss << ",";
                        oss << userEnabledFilters[i];
                    }
                    oss << "]";
                    AB_LOG_INFO(oss.str());
                } else {
                    // 🔥 调试日志：为什么没有启用用户滤镜模式
                    AB_LOG_INFO("[调试-用户滤镜] 未启用用户滤镜模式: collector=" +
                                std::string(m_collector ? "有效" : "无效") +
                                ", IsUserFilterModeEnabled=" +
                                std::string(m_collector && m_collector->IsUserFilterModeEnabled() ? "true" : "false") +
                                ", username=" + packet.socksUsername);
                }

                auto filterResult = g_wpeFilterManager->ProcessPacket(
                    modifiedData,
                    m_id,  // 实例ID
                    false, // isRequest (服务器→客户端，响应包)
                    false, // isCollector (SOCKS转发实例不是采集端)
                    WPEFilter::FilterPriority::BeforeHeartbeat,  // 先执行滤镜
                    packet.socksUsername,  // 账号名
                    pUserEnabledFilters    // 用户启用的滤镜列表
                );

                if (filterResult.intercepted) {
                    result.forwardedData.clear();
                    result.intercepted = true;
                    return result;
                }

                if (filterResult.modified) {
                    result.forwardedData = modifiedData;
                }
            }

            return result;
        });

        // 🔥 账号库绑定已在上面的认证状态恢复中处理，这里不再重复
        // 启动PacketCollector，但不设置数据包记录回调
        // 这样就只转发数据，不记录数据包
        if (m_collector->Start()) {
            m_state = InstanceState::Running;
            m_lastError.clear();
            AB_LOG_INFO("[实例管理] Socks转发实例启动成功: " + m_id);

            GlobalAntiCCCoordinator::GetInstance().RegisterInstance({
                m_id,
                m_config.port,
                m_cachedConfig.antiCC.maxConnections,
                m_cachedConfig.antiCC.useFirewall,
                [collector = m_collector.get()](const std::string& ip) {
                    if (collector) {
                        collector->DisconnectIP(ip);
                    }
                },
                [collector = m_collector.get()](const std::string& ip) {
                    if (collector) {
                        collector->AddToWhitelist(ip);
                    }
                },
                [collector = m_collector.get()](const std::string& ip) {
                    if (collector) {
                        collector->RemoveFromWhitelist(ip);
                    }
                },
                [collector = m_collector.get()](const std::string& ip) {
                    if (collector) {
                        collector->AddToBlacklist(ip);
                    }
                },
                [collector = m_collector.get()](const std::string& ip) {
                    if (collector) {
                        collector->RemoveFromBlacklist(ip);
                    }
                }
            });

            // 🔥 实例启动成功后，设置用户滤镜配置（此时 iocpPool 已创建）
            if (m_cachedConfig.enableUserFilterMode && m_collector) {
                m_collector->SetUserFilterMode(true);
                m_collector->SetUserFilterHttpPort(m_cachedConfig.userFilterHttpPort);
                AB_LOG_INFO("[实例管理] 已启用用户滤镜模式: " + m_id);

                // 🔥 加载用户滤镜配置到内存
                if (g_userFilterManager) {
                    g_userFilterManager->LoadInstanceConfig(m_id);
                    AB_LOG_INFO("[实例管理] 已加载用户滤镜配置: " + m_id);
                }

                // 🔥 启动HTTP服务器
                AB_LOG_INFO("[调试] 准备启动用户滤镜HTTP服务器");
                if (m_collector->StartUserFilterHttpServer()) {
                    AB_LOG_INFO("[实例管理] 用户滤镜HTTP服务器已启动: " + m_id);
                } else {
                    AB_LOG_WARNING("[实例管理] 用户滤镜HTTP服务器启动失败: " + m_id);
                }
            }

            // 启动成功
        } else {
            m_state = InstanceState::Error;
            m_lastError = "启动失败";
            AB_LOG_ERROR("[实例管理] Socks转发实例启动失败: " + m_id);
            return false;
        }
    } catch (const std::exception& e) {
        m_state = InstanceState::Error;
        m_lastError = std::string("启动异常: ") + e.what();
        AB_LOG_ERROR("[实例管理] Socks转发实例启动异常: " + m_id + " - " + e.what());
        return false;
    }

    return true;
}

void SocksForwardInstance::Stop() {
    // 🔥 修复：先获取 collector 指针，然后释放锁再调用 Stop，避免死锁
    PacketCollector* collector = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_state != InstanceState::Running) {
            return;
        }

        m_state = InstanceState::Stopping;
        AB_LOG_INFO("[实例管理] 停止Socks转发实例: " + m_id);

        // 🔥 保存停止状态到数据库
        if (g_database) {
            g_database->SetConfigValue(
                InstanceManager::MakeInstanceConfigKey(m_id, "autoStart"), "0");
        }

        collector = m_collector.get();
    }

    // 🔥 在锁外调用 Stop，避免死锁
    if (collector) {
        collector->Stop();
    }

    GlobalAntiCCCoordinator::GetInstance().UnregisterInstance(m_id);

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_state = InstanceState::Stopped;
        AB_LOG_INFO("[实例管理] Socks转发实例已停止: " + m_id);
    }
}

bool SocksForwardInstance::IsRunning() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state == InstanceState::Running;
}

InstanceInfo SocksForwardInstance::GetInfo() const {
    std::lock_guard<std::mutex> lock(m_mutex);

    InstanceInfo info;
    info.id = m_id;
    info.name = m_config.name;
    info.type = InstanceType::SocksForward;
    info.state = m_state;
    info.port = m_config.port;
    info.createTime = m_createTime;
    info.lastError = m_lastError;

    if (m_collector) {
        info.currentConnections = m_collector->GetCurrentConnections();
        info.totalPackets = m_collector->GetTotalPackets();
        info.totalBytes = m_collector->GetTotalBytes();
    }

    return info;
}

void SocksForwardInstance::SetPort(int port) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_state == InstanceState::Running) {
        AB_LOG_WARNING("[实例管理] 无法在运行时修改端口: " + m_id);
        return;
    }

    m_config.port = port;
    if (m_collector) {
        m_collector->SetListenPort(port);
    }

    // 保存到config.db
    if (g_database) {
        g_database->SetConfigValue("inst_" + m_id + "_port", std::to_string(port));
    }
}

SocksForwardInstance* InstanceManager::GetSocksForwardInstance(const std::string& instanceId) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_socksForwards.find(instanceId);
    if (it != m_socksForwards.end()) {
        return it->second.get();
    }

    return nullptr;
}

std::vector<SocksForwardInstance*> InstanceManager::GetSocksForwardInstances() {
    std::lock_guard<std::mutex> lock(m_mutex);

    std::vector<SocksForwardInstance*> result;
    for (const auto& pair : m_socksForwards) {
        result.push_back(pair.second.get());
    }

    return result;
}
