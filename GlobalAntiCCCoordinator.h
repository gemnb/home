#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class DatabaseManager;

enum class AntiCCScopeType {
    GlobalShared = 0,
    InstanceOnly = 1,
    SelectedInstances = 2
};

enum class AntiCCPressureState {
    Normal = 0,
    Busy = 1,
    Overloaded = 2,
    Critical = 3
};

struct ScopedWhitelistEntry {
    std::string id;
    std::string ip;
    AntiCCScopeType scopeType = AntiCCScopeType::SelectedInstances;
    std::vector<std::string> targetInstanceIds;
    std::string source;  // manual / auth_auto / import
    std::string sourceInstanceId;
    int64_t expireUnixSeconds = 0;  // 0 = never expire
};

class GlobalAntiCCCoordinator {
public:
    struct InstanceRegistration {
        std::string instanceId;
        int listenPort = 0;
        int maxConnections = 0;
        bool useFirewall = false;
        std::function<void(const std::string&)> disconnectIpCallback;
        std::function<void(const std::string&)> addWhitelistCallback;
        std::function<void(const std::string&)> removeWhitelistCallback;
        std::function<void(const std::string&)> addBlacklistCallback;
        std::function<void(const std::string&)> removeBlacklistCallback;
    };

    struct PreCheckResult {
        bool allowed = true;
        bool globallyBlocked = false;
        bool instanceWhitelisted = false;
        float globalScore = 50.0f;
        float effectiveScore = 50.0f;
        AntiCCPressureState globalPressure = AntiCCPressureState::Normal;
        AntiCCPressureState instancePressure = AntiCCPressureState::Normal;
    };

    static GlobalAntiCCCoordinator& GetInstance();

    void SetDatabaseManager(DatabaseManager* db);
    void EnsureLoaded();

    void RegisterInstance(const InstanceRegistration& registration);
    void UnregisterInstance(const std::string& instanceId);
    void UpdateInstanceConfig(const std::string& instanceId, int listenPort, int maxConnections, bool useFirewall);

    PreCheckResult PreCheck(const std::string& instanceId, const std::string& clientIP);
    void RecordAuthHint(const std::string& instanceId, const std::string& clientIP);
    void RecordAuthSuccess(const std::string& instanceId, const std::string& clientIP);
    void RecordAuthFailure(const std::string& instanceId, const std::string& clientIP);
    void RecordProtocolViolation(const std::string& instanceId, const std::string& clientIP, int banSeconds);
    void RecordConnectionOpened(const std::string& instanceId, const std::string& clientIP);
    void RecordConnectionClosed(const std::string& instanceId, const std::string& clientIP);

    bool IsBlacklisted(const std::string& clientIP) const;
    bool IsWhitelistedForInstance(const std::string& instanceId, const std::string& clientIP) const;

    int GetGlobalConnectionCount() const;
    int GetInstanceConnectionCount(const std::string& instanceId) const;
    int GetIpGlobalConnectionCount(const std::string& clientIP) const;
    AntiCCPressureState GetGlobalPressure() const;
    AntiCCPressureState GetInstancePressure(const std::string& instanceId) const;
    float GetEffectiveScoreForInstance(const std::string& instanceId, const std::string& clientIP) const;
    float GetGlobalScore(const std::string& clientIP) const;

    std::vector<std::string> GetGlobalBlacklist() const;
    std::vector<ScopedWhitelistEntry> GetScopedWhitelistEntries() const;
    std::vector<ScopedWhitelistEntry> GetScopedWhitelistEntriesForInstance(const std::string& instanceId) const;
    std::vector<std::pair<std::string, int>> GetTopIpConnectionCounts(size_t limit = 20) const;

    void ReplaceGlobalBlacklist(const std::vector<std::string>& ips);
    void ReplaceInstanceManualWhitelist(const std::string& instanceId, const std::vector<std::string>& ips);
    void ReplaceScopedWhitelistEntries(const std::vector<ScopedWhitelistEntry>& entries);
    void ReplaceManualWhitelistEntriesForSource(const std::string& sourceInstanceId, const std::vector<ScopedWhitelistEntry>& entries);

private:
    GlobalAntiCCCoordinator() = default;

    struct GlobalIpMetrics {
        int totalActiveConnections = 0;
        int recentConnections = 0;
        int recentBlocks = 0;
        int recentAuthSuccess = 0;
        int recentAuthFailures = 0;
        int recentAuthHints = 0;
        float globalScore = 50.0f;
    };

    struct InstanceRuntimeState {
        int listenPort = 0;
        int maxConnections = 0;
        bool useFirewall = false;
        int activeConnections = 0;
        std::function<void(const std::string&)> disconnectIpCallback;
        std::function<void(const std::string&)> addWhitelistCallback;
        std::function<void(const std::string&)> removeWhitelistCallback;
        std::function<void(const std::string&)> addBlacklistCallback;
        std::function<void(const std::string&)> removeBlacklistCallback;
    };

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, InstanceRuntimeState> instances_;
    std::unordered_map<std::string, GlobalIpMetrics> globalIpMetrics_;
    std::unordered_map<std::string, std::unordered_map<std::string, int>> perIpPerInstanceConnections_;
    std::unordered_set<std::string> globalBlacklist_;
    std::unordered_map<std::string, int64_t> transientBanUntilUnix_;
    std::vector<ScopedWhitelistEntry> scopedWhitelistEntries_;
    int globalConnectionCount_ = 0;

    DatabaseManager* dbManager_ = nullptr;
    bool loaded_ = false;

    void CleanupExpiredEntriesLocked(std::vector<std::string>* expiredTransientBans = nullptr);
    bool IsBlacklistedLocked(const std::string& clientIP, int64_t nowUnix) const;
    bool IsWhitelistedForInstanceLocked(const std::string& instanceId, const std::string& clientIP, int64_t nowUnix) const;
    AntiCCPressureState ComputeGlobalPressureLocked() const;
    AntiCCPressureState ComputeInstancePressureLocked(const std::string& instanceId) const;
    float GetEffectiveScoreLocked(const std::string& instanceId, const std::string& clientIP, bool whitelisted) const;

    void LoadFromDatabaseLocked();
    void SaveGlobalBlacklistLocked() const;
    void SaveScopedWhitelistEntriesLocked() const;

    void SyncGlobalBlacklistDiff(
        const std::unordered_set<std::string>& oldBlacklist,
        const std::unordered_set<std::string>& newBlacklist);
    void SyncInstanceWhitelistDiff(
        const std::string& instanceId,
        const std::unordered_set<std::string>& oldWhitelist,
        const std::unordered_set<std::string>& newWhitelist);
    std::unordered_set<std::string> GetApplicableWhitelistSetLocked(const std::string& instanceId, int64_t nowUnix) const;

    void AddGlobalFirewallRule(const std::string& clientIP) const;
    void RemoveGlobalFirewallRule(const std::string& clientIP) const;
};
