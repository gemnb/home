#include "GlobalAntiCCCoordinator.h"

#include "DatabaseManager.h"
#include "Logger.h"
#include "res/json.hpp"

#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <sstream>

using json = nlohmann::json;

namespace {
constexpr const char* kGlobalBlacklistDbKey = "global_anticc_blacklist_json";
constexpr const char* kScopedWhitelistDbKey = "global_anticc_scoped_whitelist_json";
constexpr const char* kFirewallRulePrefix = "AB_GlobalAntiCC_";

int64_t CurrentUnixSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string MakeFirewallRuleName(const std::string& clientIP) {
    return std::string(kFirewallRulePrefix) + clientIP;
}

std::string ExecutePowerShell(const std::string& command) {
    try {
        std::string fullCommand = "powershell.exe -ExecutionPolicy Bypass -NoProfile -Command \"" + command + "\"";

        SECURITY_ATTRIBUTES sa;
        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = NULL;
        sa.bInheritHandle = TRUE;

        HANDLE hStdOutRead = NULL;
        HANDLE hStdOutWrite = NULL;
        if (!CreatePipe(&hStdOutRead, &hStdOutWrite, &sa, 0)) {
            return "";
        }

        SetHandleInformation(hStdOutRead, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOA si = {};
        si.cb = sizeof(si);
        si.hStdError = hStdOutWrite;
        si.hStdOutput = hStdOutWrite;
        si.dwFlags |= STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;

        PROCESS_INFORMATION pi = {};
        if (!CreateProcessA(NULL, (LPSTR)fullCommand.c_str(), NULL, NULL, TRUE,
            CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            CloseHandle(hStdOutRead);
            CloseHandle(hStdOutWrite);
            return "";
        }

        CloseHandle(hStdOutWrite);

        std::string output;
        char buffer[128];
        DWORD bytesRead = 0;
        while (ReadFile(hStdOutRead, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
            buffer[bytesRead] = '\0';
            output += buffer;
        }

        CloseHandle(hStdOutRead);
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return output;
    }
    catch (...) {
        return "";
    }
}
}

GlobalAntiCCCoordinator& GlobalAntiCCCoordinator::GetInstance() {
    static GlobalAntiCCCoordinator instance;
    return instance;
}

void GlobalAntiCCCoordinator::SetDatabaseManager(DatabaseManager* db) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    dbManager_ = db;
}

void GlobalAntiCCCoordinator::EnsureLoaded() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (loaded_) {
        return;
    }
    LoadFromDatabaseLocked();
    loaded_ = true;
}

void GlobalAntiCCCoordinator::RegisterInstance(const InstanceRegistration& registration) {
    EnsureLoaded();

    std::unordered_set<std::string> blacklistSnapshot;
    std::unordered_set<std::string> whitelistSnapshot;

    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        auto& state = instances_[registration.instanceId];
        state.listenPort = registration.listenPort;
        state.maxConnections = registration.maxConnections;
        state.useFirewall = registration.useFirewall;
        state.disconnectIpCallback = registration.disconnectIpCallback;
        state.addWhitelistCallback = registration.addWhitelistCallback;
        state.removeWhitelistCallback = registration.removeWhitelistCallback;
        state.addBlacklistCallback = registration.addBlacklistCallback;
        state.removeBlacklistCallback = registration.removeBlacklistCallback;

        const int64_t nowUnix = CurrentUnixSeconds();
        CleanupExpiredEntriesLocked();
        blacklistSnapshot = globalBlacklist_;
        for (const auto& pair : transientBanUntilUnix_) {
            if (pair.second > nowUnix) {
                blacklistSnapshot.insert(pair.first);
            }
        }
        whitelistSnapshot = GetApplicableWhitelistSetLocked(registration.instanceId, nowUnix);
    }

    for (const auto& ip : blacklistSnapshot) {
        if (registration.addBlacklistCallback) {
            registration.addBlacklistCallback(ip);
        }
    }

    for (const auto& ip : whitelistSnapshot) {
        if (registration.addWhitelistCallback) {
            registration.addWhitelistCallback(ip);
        }
    }

    AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[GlobalAntiCC] Registered instance: " + registration.instanceId +
        " port=" + std::to_string(registration.listenPort));
}

void GlobalAntiCCCoordinator::UnregisterInstance(const std::string& instanceId) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    instances_.erase(instanceId);
    AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[GlobalAntiCC] Unregistered instance: " + instanceId);
}

void GlobalAntiCCCoordinator::UpdateInstanceConfig(const std::string& instanceId, int listenPort, int maxConnections, bool useFirewall) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto it = instances_.find(instanceId);
    if (it == instances_.end()) {
        return;
    }
    it->second.listenPort = listenPort;
    it->second.maxConnections = maxConnections;
    it->second.useFirewall = useFirewall;
}

GlobalAntiCCCoordinator::PreCheckResult GlobalAntiCCCoordinator::PreCheck(const std::string& instanceId, const std::string& clientIP) {
    EnsureLoaded();

    std::vector<std::string> expiredTransientBans;
    PreCheckResult result;
    const int64_t nowUnix = CurrentUnixSeconds();

    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        CleanupExpiredEntriesLocked(&expiredTransientBans);

        result.instanceWhitelisted = IsWhitelistedForInstanceLocked(instanceId, clientIP, nowUnix);
        result.globalPressure = ComputeGlobalPressureLocked();
        result.instancePressure = ComputeInstancePressureLocked(instanceId);
        result.allowed = !IsBlacklistedLocked(clientIP, nowUnix);
        result.globallyBlocked = !result.allowed;

        auto metricsIt = globalIpMetrics_.find(clientIP);
        if (metricsIt != globalIpMetrics_.end()) {
            result.globalScore = metricsIt->second.globalScore;
        }
        result.effectiveScore = GetEffectiveScoreLocked(instanceId, clientIP, result.instanceWhitelisted);
    }

    if (!expiredTransientBans.empty()) {
        std::unordered_set<std::string> currentBlacklist;
        std::vector<InstanceRegistration> callbacks;
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            currentBlacklist = globalBlacklist_;
            for (const auto& pair : instances_) {
                InstanceRegistration item;
                item.instanceId = pair.first;
                item.removeBlacklistCallback = pair.second.removeBlacklistCallback;
                callbacks.push_back(item);
            }
        }

        for (const auto& ip : expiredTransientBans) {
            if (currentBlacklist.find(ip) == currentBlacklist.end()) {
                RemoveGlobalFirewallRule(ip);
                for (const auto& callback : callbacks) {
                    if (callback.removeBlacklistCallback) {
                        callback.removeBlacklistCallback(ip);
                    }
                }
                AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[GlobalAntiCC] Expired transient ban removed: " + ip);
            }
        }
    }

    return result;
}

void GlobalAntiCCCoordinator::RecordAuthHint(const std::string& instanceId, const std::string& clientIP) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto& metrics = globalIpMetrics_[clientIP];
    metrics.recentAuthHints++;
    if (!instanceId.empty()) {
        auto& perInstance = perIpPerInstanceConnections_[clientIP];
        if (perInstance.find(instanceId) == perInstance.end()) {
            perInstance[instanceId] = 0;
        }
    }
}

void GlobalAntiCCCoordinator::RecordAuthSuccess(const std::string& instanceId, const std::string& clientIP) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto& metrics = globalIpMetrics_[clientIP];
    metrics.recentAuthSuccess++;
    metrics.globalScore = std::min(100.0f, metrics.globalScore + 8.0f);
    AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[GlobalAntiCC] Auth success: " + clientIP + " instance=" + instanceId);
}

void GlobalAntiCCCoordinator::RecordAuthFailure(const std::string& instanceId, const std::string& clientIP) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto& metrics = globalIpMetrics_[clientIP];
    metrics.recentAuthFailures++;
    metrics.globalScore = std::max(0.0f, metrics.globalScore - 10.0f);
    AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[GlobalAntiCC] Auth failure: " + clientIP + " instance=" + instanceId);
}

void GlobalAntiCCCoordinator::RecordProtocolViolation(const std::string& instanceId, const std::string& clientIP, int banSeconds) {
    EnsureLoaded();

    std::vector<InstanceRegistration> callbacks;
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        transientBanUntilUnix_[clientIP] = CurrentUnixSeconds() + std::max(30, banSeconds);
        auto& metrics = globalIpMetrics_[clientIP];
        metrics.recentBlocks++;
        metrics.globalScore = 0.0f;
        for (const auto& pair : instances_) {
            InstanceRegistration item;
            item.instanceId = pair.first;
            item.disconnectIpCallback = pair.second.disconnectIpCallback;
            item.addBlacklistCallback = pair.second.addBlacklistCallback;
            callbacks.push_back(item);
        }
    }

    for (const auto& callback : callbacks) {
        if (callback.addBlacklistCallback) {
            callback.addBlacklistCallback(clientIP);
        }
        if (callback.disconnectIpCallback) {
            callback.disconnectIpCallback(clientIP);
        }
    }
    AddGlobalFirewallRule(clientIP);
    AB_LOG_WARNING_CAT(LOG_CAT_ANTICC, "[GlobalAntiCC] Protocol violation banned globally: " + clientIP +
        " instance=" + instanceId + " ban=" + std::to_string(std::max(30, banSeconds)) + "s");
}

void GlobalAntiCCCoordinator::RecordConnectionOpened(const std::string& instanceId, const std::string& clientIP) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    globalConnectionCount_++;
    auto& instance = instances_[instanceId];
    instance.activeConnections++;
    auto& metrics = globalIpMetrics_[clientIP];
    metrics.totalActiveConnections++;
    metrics.recentConnections++;
    perIpPerInstanceConnections_[clientIP][instanceId]++;
}

void GlobalAntiCCCoordinator::RecordConnectionClosed(const std::string& instanceId, const std::string& clientIP) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    globalConnectionCount_ = std::max(0, globalConnectionCount_ - 1);

    auto instIt = instances_.find(instanceId);
    if (instIt != instances_.end()) {
        instIt->second.activeConnections = std::max(0, instIt->second.activeConnections - 1);
    }

    auto metricsIt = globalIpMetrics_.find(clientIP);
    if (metricsIt != globalIpMetrics_.end()) {
        metricsIt->second.totalActiveConnections = std::max(0, metricsIt->second.totalActiveConnections - 1);
    }

    auto perIpIt = perIpPerInstanceConnections_.find(clientIP);
    if (perIpIt != perIpPerInstanceConnections_.end()) {
        auto perInstIt = perIpIt->second.find(instanceId);
        if (perInstIt != perIpIt->second.end()) {
            perInstIt->second = std::max(0, perInstIt->second - 1);
            if (perInstIt->second == 0) {
                perIpIt->second.erase(perInstIt);
            }
        }
        if (perIpIt->second.empty()) {
            perIpPerInstanceConnections_.erase(perIpIt);
        }
    }
}

bool GlobalAntiCCCoordinator::IsBlacklisted(const std::string& clientIP) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return IsBlacklistedLocked(clientIP, CurrentUnixSeconds());
}

bool GlobalAntiCCCoordinator::IsWhitelistedForInstance(const std::string& instanceId, const std::string& clientIP) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return IsWhitelistedForInstanceLocked(instanceId, clientIP, CurrentUnixSeconds());
}

int GlobalAntiCCCoordinator::GetGlobalConnectionCount() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return globalConnectionCount_;
}

int GlobalAntiCCCoordinator::GetInstanceConnectionCount(const std::string& instanceId) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = instances_.find(instanceId);
    return it != instances_.end() ? it->second.activeConnections : 0;
}

int GlobalAntiCCCoordinator::GetIpGlobalConnectionCount(const std::string& clientIP) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = globalIpMetrics_.find(clientIP);
    return it != globalIpMetrics_.end() ? it->second.totalActiveConnections : 0;
}

AntiCCPressureState GlobalAntiCCCoordinator::GetGlobalPressure() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return ComputeGlobalPressureLocked();
}

AntiCCPressureState GlobalAntiCCCoordinator::GetInstancePressure(const std::string& instanceId) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return ComputeInstancePressureLocked(instanceId);
}

float GlobalAntiCCCoordinator::GetEffectiveScoreForInstance(const std::string& instanceId, const std::string& clientIP) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return GetEffectiveScoreLocked(instanceId, clientIP, IsWhitelistedForInstanceLocked(instanceId, clientIP, CurrentUnixSeconds()));
}

float GlobalAntiCCCoordinator::GetGlobalScore(const std::string& clientIP) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = globalIpMetrics_.find(clientIP);
    return it != globalIpMetrics_.end() ? it->second.globalScore : 50.0f;
}

std::vector<std::string> GlobalAntiCCCoordinator::GetGlobalBlacklist() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::unordered_set<std::string> merged = globalBlacklist_;
    const int64_t nowUnix = CurrentUnixSeconds();
    for (const auto& pair : transientBanUntilUnix_) {
        if (pair.second > nowUnix) {
            merged.insert(pair.first);
        }
    }
    std::vector<std::string> result(merged.begin(), merged.end());
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<ScopedWhitelistEntry> GlobalAntiCCCoordinator::GetScopedWhitelistEntries() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<ScopedWhitelistEntry> result;
    const int64_t nowUnix = CurrentUnixSeconds();
    for (const auto& entry : scopedWhitelistEntries_) {
        if (entry.expireUnixSeconds == 0 || entry.expireUnixSeconds > nowUnix) {
            result.push_back(entry);
        }
    }
    return result;
}

std::vector<ScopedWhitelistEntry> GlobalAntiCCCoordinator::GetScopedWhitelistEntriesForInstance(const std::string& instanceId) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<ScopedWhitelistEntry> result;
    const int64_t nowUnix = CurrentUnixSeconds();
    for (const auto& entry : scopedWhitelistEntries_) {
        if (entry.expireUnixSeconds != 0 && entry.expireUnixSeconds <= nowUnix) {
            continue;
        }

        if (entry.scopeType == AntiCCScopeType::GlobalShared) {
            result.push_back(entry);
            continue;
        }

        if (entry.scopeType == AntiCCScopeType::InstanceOnly && !entry.targetInstanceIds.empty() &&
            entry.targetInstanceIds.front() == instanceId) {
            result.push_back(entry);
            continue;
        }

        if (entry.scopeType == AntiCCScopeType::SelectedInstances &&
            std::find(entry.targetInstanceIds.begin(), entry.targetInstanceIds.end(), instanceId) != entry.targetInstanceIds.end()) {
            result.push_back(entry);
        }
    }
    return result;
}

std::vector<std::pair<std::string, int>> GlobalAntiCCCoordinator::GetTopIpConnectionCounts(size_t limit) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<std::pair<std::string, int>> result;
    result.reserve(globalIpMetrics_.size());
    for (const auto& pair : globalIpMetrics_) {
        if (pair.second.totalActiveConnections > 0) {
            result.emplace_back(pair.first, pair.second.totalActiveConnections);
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.second > b.second;
    });
    if (result.size() > limit) {
        result.resize(limit);
    }
    return result;
}

void GlobalAntiCCCoordinator::ReplaceGlobalBlacklist(const std::vector<std::string>& ips) {
    EnsureLoaded();

    std::unordered_set<std::string> oldBlacklist;
    std::unordered_set<std::string> newBlacklist;
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        oldBlacklist = globalBlacklist_;
        globalBlacklist_.clear();
        for (const auto& ip : ips) {
            if (!ip.empty()) {
                globalBlacklist_.insert(ip);
            }
        }
        SaveGlobalBlacklistLocked();
        newBlacklist = globalBlacklist_;
    }

    SyncGlobalBlacklistDiff(oldBlacklist, newBlacklist);
}

void GlobalAntiCCCoordinator::ReplaceInstanceManualWhitelist(const std::string& instanceId, const std::vector<std::string>& ips) {
    EnsureLoaded();

    std::unordered_set<std::string> oldWhitelist;
    std::unordered_set<std::string> newWhitelist;
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        const int64_t nowUnix = CurrentUnixSeconds();
        oldWhitelist = GetApplicableWhitelistSetLocked(instanceId, nowUnix);

        scopedWhitelistEntries_.erase(
            std::remove_if(scopedWhitelistEntries_.begin(), scopedWhitelistEntries_.end(),
                [&instanceId](const ScopedWhitelistEntry& entry) {
                    return entry.source == "manual" &&
                        entry.sourceInstanceId == instanceId &&
                        entry.scopeType == AntiCCScopeType::SelectedInstances &&
                        entry.targetInstanceIds.size() == 1 &&
                        entry.targetInstanceIds.front() == instanceId;
                }),
            scopedWhitelistEntries_.end());

        for (const auto& ip : ips) {
            if (ip.empty()) {
                continue;
            }

            ScopedWhitelistEntry entry;
            entry.id = "manual_" + instanceId + "_" + ip;
            entry.ip = ip;
            entry.scopeType = AntiCCScopeType::SelectedInstances;
            entry.targetInstanceIds = { instanceId };
            entry.source = "manual";
            entry.sourceInstanceId = instanceId;
            entry.expireUnixSeconds = 0;
            scopedWhitelistEntries_.push_back(entry);
        }

        SaveScopedWhitelistEntriesLocked();
        newWhitelist = GetApplicableWhitelistSetLocked(instanceId, nowUnix);
    }

    SyncInstanceWhitelistDiff(instanceId, oldWhitelist, newWhitelist);
}

void GlobalAntiCCCoordinator::ReplaceScopedWhitelistEntries(const std::vector<ScopedWhitelistEntry>& entries) {
    EnsureLoaded();

    std::unordered_map<std::string, std::unordered_set<std::string>> oldByInstance;
    std::unordered_map<std::string, std::unordered_set<std::string>> newByInstance;

    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        const int64_t nowUnix = CurrentUnixSeconds();
        for (const auto& pair : instances_) {
            oldByInstance[pair.first] = GetApplicableWhitelistSetLocked(pair.first, nowUnix);
        }

        scopedWhitelistEntries_ = entries;
        SaveScopedWhitelistEntriesLocked();

        for (const auto& pair : instances_) {
            newByInstance[pair.first] = GetApplicableWhitelistSetLocked(pair.first, nowUnix);
        }
    }

    for (const auto& pair : newByInstance) {
        SyncInstanceWhitelistDiff(pair.first, oldByInstance[pair.first], pair.second);
    }
}

void GlobalAntiCCCoordinator::ReplaceManualWhitelistEntriesForSource(const std::string& sourceInstanceId, const std::vector<ScopedWhitelistEntry>& entries) {
    EnsureLoaded();

    std::unordered_map<std::string, std::unordered_set<std::string>> oldByInstance;
    std::unordered_map<std::string, std::unordered_set<std::string>> newByInstance;

    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        const int64_t nowUnix = CurrentUnixSeconds();
        for (const auto& pair : instances_) {
            oldByInstance[pair.first] = GetApplicableWhitelistSetLocked(pair.first, nowUnix);
        }

        scopedWhitelistEntries_.erase(
            std::remove_if(scopedWhitelistEntries_.begin(), scopedWhitelistEntries_.end(),
                [&sourceInstanceId](const ScopedWhitelistEntry& entry) {
                    return entry.source == "manual" && entry.sourceInstanceId == sourceInstanceId;
                }),
            scopedWhitelistEntries_.end());

        for (const auto& entry : entries) {
            if (!entry.ip.empty()) {
                scopedWhitelistEntries_.push_back(entry);
            }
        }

        SaveScopedWhitelistEntriesLocked();

        for (const auto& pair : instances_) {
            newByInstance[pair.first] = GetApplicableWhitelistSetLocked(pair.first, nowUnix);
        }
    }

    for (const auto& pair : newByInstance) {
        SyncInstanceWhitelistDiff(pair.first, oldByInstance[pair.first], pair.second);
    }
}

void GlobalAntiCCCoordinator::CleanupExpiredEntriesLocked(std::vector<std::string>* expiredTransientBans) {
    const int64_t nowUnix = CurrentUnixSeconds();

    for (auto it = transientBanUntilUnix_.begin(); it != transientBanUntilUnix_.end();) {
        if (it->second <= nowUnix) {
            if (expiredTransientBans) {
                expiredTransientBans->push_back(it->first);
            }
            globalBlacklist_.erase(it->first);
            it = transientBanUntilUnix_.erase(it);
        }
        else {
            ++it;
        }
    }

    scopedWhitelistEntries_.erase(
        std::remove_if(scopedWhitelistEntries_.begin(), scopedWhitelistEntries_.end(),
            [nowUnix](const ScopedWhitelistEntry& entry) {
                return entry.expireUnixSeconds != 0 && entry.expireUnixSeconds <= nowUnix;
            }),
        scopedWhitelistEntries_.end());
}

bool GlobalAntiCCCoordinator::IsBlacklistedLocked(const std::string& clientIP, int64_t nowUnix) const {
    auto transientIt = transientBanUntilUnix_.find(clientIP);
    if (transientIt != transientBanUntilUnix_.end() && transientIt->second > nowUnix) {
        return true;
    }
    return globalBlacklist_.find(clientIP) != globalBlacklist_.end();
}

bool GlobalAntiCCCoordinator::IsWhitelistedForInstanceLocked(const std::string& instanceId, const std::string& clientIP, int64_t nowUnix) const {
    for (const auto& entry : scopedWhitelistEntries_) {
        if (entry.ip != clientIP) {
            continue;
        }
        if (entry.expireUnixSeconds != 0 && entry.expireUnixSeconds <= nowUnix) {
            continue;
        }

        if (entry.scopeType == AntiCCScopeType::GlobalShared) {
            return true;
        }
        if (entry.scopeType == AntiCCScopeType::InstanceOnly &&
            !entry.targetInstanceIds.empty() &&
            entry.targetInstanceIds.front() == instanceId) {
            return true;
        }
        if (entry.scopeType == AntiCCScopeType::SelectedInstances &&
            std::find(entry.targetInstanceIds.begin(), entry.targetInstanceIds.end(), instanceId) != entry.targetInstanceIds.end()) {
            return true;
        }
    }
    return false;
}

AntiCCPressureState GlobalAntiCCCoordinator::ComputeGlobalPressureLocked() const {
    int totalCapacity = 0;
    for (const auto& pair : instances_) {
        totalCapacity += std::max(0, pair.second.maxConnections);
    }

    if (totalCapacity <= 0) {
        return AntiCCPressureState::Normal;
    }

    const double ratio = static_cast<double>(globalConnectionCount_) / static_cast<double>(totalCapacity);
    if (ratio >= 1.0) {
        return AntiCCPressureState::Critical;
    }
    if (ratio >= 0.85) {
        return AntiCCPressureState::Overloaded;
    }
    if (ratio >= 0.70) {
        return AntiCCPressureState::Busy;
    }
    return AntiCCPressureState::Normal;
}

AntiCCPressureState GlobalAntiCCCoordinator::ComputeInstancePressureLocked(const std::string& instanceId) const {
    auto it = instances_.find(instanceId);
    if (it == instances_.end() || it->second.maxConnections <= 0) {
        return AntiCCPressureState::Normal;
    }

    const double ratio = static_cast<double>(it->second.activeConnections) / static_cast<double>(it->second.maxConnections);
    if (ratio >= 1.0) {
        return AntiCCPressureState::Critical;
    }
    if (ratio >= 0.85) {
        return AntiCCPressureState::Overloaded;
    }
    if (ratio >= 0.70) {
        return AntiCCPressureState::Busy;
    }
    return AntiCCPressureState::Normal;
}

float GlobalAntiCCCoordinator::GetEffectiveScoreLocked(const std::string& instanceId, const std::string& clientIP, bool whitelisted) const {
    if (whitelisted) {
        return 100.0f;
    }

    auto it = globalIpMetrics_.find(clientIP);
    if (it == globalIpMetrics_.end()) {
        return 50.0f;
    }

    float score = it->second.globalScore;
    auto perIpIt = perIpPerInstanceConnections_.find(clientIP);
    if (perIpIt != perIpPerInstanceConnections_.end()) {
        auto instIt = perIpIt->second.find(instanceId);
        if (instIt != perIpIt->second.end() && instIt->second > 0) {
            score = std::min(100.0f, score + 5.0f);
        }
    }
    return std::max(0.0f, std::min(100.0f, score));
}

void GlobalAntiCCCoordinator::LoadFromDatabaseLocked() {
    if (!dbManager_) {
        return;
    }

    try {
        std::string blacklistJson = dbManager_->GetConfigValue(kGlobalBlacklistDbKey, "[]");
        if (!blacklistJson.empty()) {
            json blacklist = json::parse(blacklistJson, nullptr, false);
            if (blacklist.is_array()) {
                for (const auto& item : blacklist) {
                    if (item.is_string()) {
                        globalBlacklist_.insert(item.get<std::string>());
                    }
                }
            }
        }
    }
    catch (...) {
        AB_LOG_WARNING_CAT(LOG_CAT_ANTICC, "[GlobalAntiCC] Failed to load global blacklist from DB");
    }

    try {
        std::string whitelistJson = dbManager_->GetConfigValue(kScopedWhitelistDbKey, "[]");
        if (!whitelistJson.empty()) {
            json whitelist = json::parse(whitelistJson, nullptr, false);
            if (whitelist.is_array()) {
                for (const auto& item : whitelist) {
                    if (!item.is_object()) {
                        continue;
                    }

                    ScopedWhitelistEntry entry;
                    entry.id = item.value("id", "");
                    entry.ip = item.value("ip", "");
                    entry.scopeType = static_cast<AntiCCScopeType>(item.value("scopeType", static_cast<int>(AntiCCScopeType::SelectedInstances)));
                    entry.source = item.value("source", "manual");
                    entry.sourceInstanceId = item.value("sourceInstanceId", "");
                    entry.expireUnixSeconds = item.value("expireUnixSeconds", static_cast<int64_t>(0));
                    if (item.contains("targetInstanceIds") && item["targetInstanceIds"].is_array()) {
                        for (const auto& targetId : item["targetInstanceIds"]) {
                            if (targetId.is_string()) {
                                entry.targetInstanceIds.push_back(targetId.get<std::string>());
                            }
                        }
                    }
                    if (!entry.ip.empty()) {
                        scopedWhitelistEntries_.push_back(entry);
                    }
                }
            }
        }
    }
    catch (...) {
        AB_LOG_WARNING_CAT(LOG_CAT_ANTICC, "[GlobalAntiCC] Failed to load scoped whitelist entries from DB");
    }

    CleanupExpiredEntriesLocked();
}

void GlobalAntiCCCoordinator::SaveGlobalBlacklistLocked() const {
    if (!dbManager_) {
        return;
    }

    json blacklist = json::array();
    for (const auto& ip : globalBlacklist_) {
        blacklist.push_back(ip);
    }
    dbManager_->SetConfigValue(kGlobalBlacklistDbKey, blacklist.dump());
}

void GlobalAntiCCCoordinator::SaveScopedWhitelistEntriesLocked() const {
    if (!dbManager_) {
        return;
    }

    json entries = json::array();
    for (const auto& entry : scopedWhitelistEntries_) {
        json item;
        item["id"] = entry.id;
        item["ip"] = entry.ip;
        item["scopeType"] = static_cast<int>(entry.scopeType);
        item["source"] = entry.source;
        item["sourceInstanceId"] = entry.sourceInstanceId;
        item["expireUnixSeconds"] = entry.expireUnixSeconds;
        item["targetInstanceIds"] = entry.targetInstanceIds;
        entries.push_back(item);
    }
    dbManager_->SetConfigValue(kScopedWhitelistDbKey, entries.dump());
}

void GlobalAntiCCCoordinator::SyncGlobalBlacklistDiff(
    const std::unordered_set<std::string>& oldBlacklist,
    const std::unordered_set<std::string>& newBlacklist) {

    std::vector<InstanceRegistration> callbacks;
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        callbacks.reserve(instances_.size());
        for (const auto& pair : instances_) {
            InstanceRegistration item;
            item.instanceId = pair.first;
            item.disconnectIpCallback = pair.second.disconnectIpCallback;
            item.addBlacklistCallback = pair.second.addBlacklistCallback;
            item.removeBlacklistCallback = pair.second.removeBlacklistCallback;
            callbacks.push_back(item);
        }
    }

    for (const auto& ip : oldBlacklist) {
        if (newBlacklist.find(ip) != newBlacklist.end()) {
            continue;
        }

        if (IsBlacklisted(ip)) {
            continue;
        }

        RemoveGlobalFirewallRule(ip);
        for (const auto& callback : callbacks) {
            if (callback.removeBlacklistCallback) {
                callback.removeBlacklistCallback(ip);
            }
        }
        AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[GlobalAntiCC] Removed global blacklist IP: " + ip);
    }

    for (const auto& ip : newBlacklist) {
        if (oldBlacklist.find(ip) != oldBlacklist.end()) {
            continue;
        }

        AddGlobalFirewallRule(ip);
        for (const auto& callback : callbacks) {
            if (callback.addBlacklistCallback) {
                callback.addBlacklistCallback(ip);
            }
            if (callback.disconnectIpCallback) {
                callback.disconnectIpCallback(ip);
            }
        }
        AB_LOG_WARNING_CAT(LOG_CAT_ANTICC, "[GlobalAntiCC] Added global blacklist IP: " + ip);
    }
}

void GlobalAntiCCCoordinator::SyncInstanceWhitelistDiff(
    const std::string& instanceId,
    const std::unordered_set<std::string>& oldWhitelist,
    const std::unordered_set<std::string>& newWhitelist) {

    InstanceRegistration callback;
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = instances_.find(instanceId);
        if (it == instances_.end()) {
            return;
        }
        callback.instanceId = instanceId;
        callback.addWhitelistCallback = it->second.addWhitelistCallback;
        callback.removeWhitelistCallback = it->second.removeWhitelistCallback;
    }

    for (const auto& ip : oldWhitelist) {
        if (newWhitelist.find(ip) == newWhitelist.end() && callback.removeWhitelistCallback) {
            callback.removeWhitelistCallback(ip);
        }
    }

    for (const auto& ip : newWhitelist) {
        if (oldWhitelist.find(ip) == oldWhitelist.end() && callback.addWhitelistCallback) {
            callback.addWhitelistCallback(ip);
        }
    }
}

std::unordered_set<std::string> GlobalAntiCCCoordinator::GetApplicableWhitelistSetLocked(const std::string& instanceId, int64_t nowUnix) const {
    std::unordered_set<std::string> result;
    for (const auto& entry : scopedWhitelistEntries_) {
        if (entry.expireUnixSeconds != 0 && entry.expireUnixSeconds <= nowUnix) {
            continue;
        }
        if (entry.scopeType == AntiCCScopeType::GlobalShared) {
            result.insert(entry.ip);
            continue;
        }
        if (entry.scopeType == AntiCCScopeType::InstanceOnly &&
            !entry.targetInstanceIds.empty() &&
            entry.targetInstanceIds.front() == instanceId) {
            result.insert(entry.ip);
            continue;
        }
        if (entry.scopeType == AntiCCScopeType::SelectedInstances &&
            std::find(entry.targetInstanceIds.begin(), entry.targetInstanceIds.end(), instanceId) != entry.targetInstanceIds.end()) {
            result.insert(entry.ip);
        }
    }
    return result;
}

void GlobalAntiCCCoordinator::AddGlobalFirewallRule(const std::string& clientIP) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);

    std::vector<int> ports;
    bool useFirewall = false;
    for (const auto& pair : instances_) {
        if (!pair.second.useFirewall || pair.second.listenPort <= 0) {
            continue;
        }
        useFirewall = true;
        ports.push_back(pair.second.listenPort);
    }

    if (!useFirewall || ports.empty()) {
        return;
    }

    std::sort(ports.begin(), ports.end());
    ports.erase(std::unique(ports.begin(), ports.end()), ports.end());

    std::ostringstream portStream;
    for (size_t i = 0; i < ports.size(); ++i) {
        if (i > 0) {
            portStream << ",";
        }
        portStream << ports[i];
    }

    const std::string ruleName = MakeFirewallRuleName(clientIP);
    ExecutePowerShell("Remove-NetFirewallRule -DisplayName '" + ruleName + "' -ErrorAction SilentlyContinue");

    std::ostringstream cmd;
    cmd << "New-NetFirewallRule -DisplayName '" << ruleName << "' "
        << "-Direction Inbound -Action Block "
        << "-Protocol TCP -LocalPort " << portStream.str() << " "
        << "-RemoteAddress '" << clientIP << "' "
        << "-ErrorAction SilentlyContinue";

    ExecutePowerShell(cmd.str());
}

void GlobalAntiCCCoordinator::RemoveGlobalFirewallRule(const std::string& clientIP) const {
    ExecutePowerShell("Remove-NetFirewallRule -DisplayName '" + MakeFirewallRuleName(clientIP) + "' -ErrorAction SilentlyContinue");
}
