#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <chrono>
#include <mutex>
#include <shared_mutex>  // 🔥 修复2: 添加读写锁支持
#include <memory>
#include <atomic>
#include <deque>
#include <functional>
#include <set>
#include <winsock2.h>

// 前向声明
class DatabaseManager;

// AntiCC配置结构
struct AntiCCConfig {
    bool enabled = false;                    // 是否启用AntiCC
    int timeWindowSeconds = 10;              // 时间窗口(秒)
    int maxRequestsInWindow = 20;            // 时间窗口内最大请求数
    int banTimeSeconds = 300;                // 封禁时间(秒)
    int maxConnections = 100;                // 最大连接数
    bool useBlacklist = true;                // 是否使用黑名单
    bool useWhitelist = true;                // 是否使用白名单
    int whitelistDuration = 3600;            // 白名单持续时间(秒)
    int authFailBanTime = 60;                // 认证失败封禁时间(秒)
    int noAuthBanTime = 30;                  // 无认证封禁时间(秒)
    bool useFirewall = false;                // 是否使用防火墙
    bool rateLimitEnabled = false;           // 是否启用速率限制
    int rateLimit = 100;                     // 速率限制(请求/秒)
    int rateTimeWindow = 1;                  // 速率时间窗口(秒)
    int listenPort = 1081;                   // 监听端口(用于防火墙规则)
    bool blockNonSocks = false;              // 是否拦截非SOCKS5连接
};

// IP统计信息
struct IPStats {
    std::atomic<int> currentConnections{ 0 };                          // 当前连接数
    std::vector<std::chrono::steady_clock::time_point> connectionTimes; // 连接时间记录
    std::chrono::steady_clock::time_point banUntil;                    // 封禁到期时间
    std::chrono::steady_clock::time_point firstSeenTime;               // 首次见到时间
    std::chrono::steady_clock::time_point lastSuccessTime;             // 最后成功时间
    std::chrono::steady_clock::time_point whitelistUntil;              // 白名单到期时间
    std::chrono::steady_clock::time_point lastActiveTime;              // 白名单最后活跃时间（用于12小时自动清理）

    std::atomic<int> failedAttempts{ 0 };        // 失败尝试次数
    std::atomic<int> successfulAuths{ 0 };       // 成功认证次数
    std::atomic<int> authFailures{ 0 };          // 认证失败次数
    std::atomic<int> consecutiveFailures{ 0 };   // 连续失败次数
    std::atomic<float> trustScore{ 50.0f };      // 信任分数(0-100)
    std::atomic<int> recentBehaviorScore{ 0 };   // 近期行为分数

    bool permanentBan = false;                   // 是否永久封禁
    bool isWhitelisted = false;                  // 是否在白名单中
    bool hasAuth = false;                        // 是否有认证连接

    // 速率限制相关
    std::vector<std::chrono::steady_clock::time_point> rateLimitTimes; // 速率限制时间记录
    std::atomic<bool> rateLimited{ false };      // 是否被速率限制

    std::mutex statsMutex;                       // 统计数据互斥锁

    // ===== 🔥 修复5: 信任分数缓存（避免重复计算）=====
    mutable std::atomic<float> cachedTrustScore{ -1.0f };
    mutable std::chrono::steady_clock::time_point trustScoreCacheTime;
    static constexpr int TRUST_SCORE_CACHE_SECONDS = 60;  // 缓存60秒

    // 计算信任分数
    float calculateTrustScore();

    // 获取动态最大请求数
    int getDynamicMaxRequests(int baseMax);
};

// 黑白名单管理器
class IPListManager {
private:
    std::unordered_set<std::string> blacklist;
    std::unordered_set<std::string> whitelist;
    std::mutex listMutex;

public:
    bool isBlacklisted(const std::string& ip);
    bool isWhitelisted(const std::string& ip);
    void addToBlacklist(const std::string& ip);
    void addToWhitelist(const std::string& ip);
    void removeFromBlacklist(const std::string& ip);
    void removeFromWhitelist(const std::string& ip);
    std::vector<std::string> getBlacklist();
    std::vector<std::string> getWhitelist();
    void clearBlacklist();
    void clearWhitelist();
};

// 攻击检测器
class AttackDetector {
private:
    std::atomic<int> recentConnections{ 0 };
    std::atomic<int> recentFailures{ 0 };
    std::atomic<int> recentBlocks{ 0 };
    std::chrono::steady_clock::time_point lastCheck;
    std::atomic<bool> underAttack{ false };
    std::mutex detectorMutex;

public:
    void RecordConnection();
    void RecordFailure();
    void RecordBlock();
    int UpdateAttackStatus();  // 返回: 0=无变化, 1=进入攻击, -1=离开攻击
    bool IsUnderAttack() const;
    float GetAttackIntensity() const;
};

// 防火墙规则结构
struct FirewallRule {
    std::string ip;
    std::string ruleName;
    bool permanent;
    std::chrono::steady_clock::time_point expireTime;
};

// 性能监控器
class PerformanceMonitor {
private:
    std::atomic<int> checkCount{ 0 };
    std::atomic<int> totalCheckTime{ 0 };
    std::chrono::steady_clock::time_point lastResetTime;
    std::mutex monitorMutex;

public:
    PerformanceMonitor();
    void EnterCheck();
    void ExitCheck();
    bool IsOverloaded() const;
    void Reset();
};

// AntiCC主类
class AntiCC {
private:
    AntiCCConfig config;
    std::unordered_map<std::string, std::unique_ptr<IPStats>> ipStats;
    IPListManager ipListManager;
    AttackDetector attackDetector;
    PerformanceMonitor perfMonitor;

    // 🔥 修复2&4: 改为读写锁，允许多个读操作并发
    mutable std::shared_mutex statsMutex;
    std::mutex ipStatsCreateMutex;  // 🔥 修复2: IP创建专用锁

    std::atomic<int> blockedCount{ 0 };
    std::atomic<int> totalConnections{ 0 };
    std::atomic<int> currentConnectionCount{ 0 };

    // ===== 🔥 双连接池架构：白名单和普通用户独立计数 =====
    std::atomic<int> whitelistConnectionCount{ 0 };  // 白名单连接池
    std::atomic<int> normalConnectionCount{ 0 };     // 普通连接池

    // 防火墙相关
    std::vector<FirewallRule> firewallRules;
    std::mutex firewallMutex;
    std::string rulePrefix = "AntiCC_";

    // 数据库相关（用于白名单持久化）
    DatabaseManager* dbManager;
    std::string scopeId = "default";

    // ===== 🔥 修复4: 内存清理线程 =====
    std::thread cleanupThread;
    std::atomic<bool> cleanupThreadRunning{ false };
    std::condition_variable cleanupCV;  // 用于提前唤醒清理线程
    std::mutex cleanupMutex;  // 配合条件变量使用
    void CleanupThreadLoop();

    // 内部辅助函数
    void cleanupExpiredRecords(const std::string& clientIP);
    std::string ExecutePowerShell(const std::string& command);

public:
    AntiCC();
    ~AntiCC();

    // 配置管理
    void SetConfig(const AntiCCConfig& cfg);
    AntiCCConfig GetConfig() const;
    void SetEnabled(bool enabled);
    bool IsEnabled() const;

    // 核心检查函数
    bool CheckAntiCC(const std::string& clientIP, bool isWhitelistConnection = false);

    // 连接管理
    void OnConnectionEstablished(const std::string& clientIP, bool isWhitelistConnection = false);
    void OnConnectionClosed(const std::string& clientIP, bool isWhitelistConnection = false);
    void OnAuthSuccess(const std::string& clientIP);
    void OnAuthFailure(const std::string& clientIP);

    // 封禁管理
    void BanIP(const std::string& clientIP, int banSeconds = 0);
    void BanIPForNoAuth(const std::string& clientIP);
    void BanIPForAuthFailure(const std::string& clientIP);
    void UnbanIP(const std::string& clientIP);
    bool IsIPBanned(const std::string& clientIP);

    // 黑白名单管理
    void AddToBlacklist(const std::string& ip);
    void AddToWhitelist(const std::string& ip);
    void RemoveFromBlacklist(const std::string& ip);
    void RemoveFromWhitelist(const std::string& ip);
    bool IsInBlacklist(const std::string& ip);
    bool IsInWhitelist(const std::string& ip);
    std::vector<std::string> GetBlacklist();
    std::vector<std::string> GetWhitelist();

    // 统计信息
    int GetBlockedCount() const;
    int GetTotalConnections() const;
    int GetCurrentConnections() const;
    int GetWhitelistConnections() const;  // 🔥 新增：获取白名单连接数
    int GetNormalConnections() const;     // 🔥 新增：获取普通连接数
    bool IsUnderAttack() const;

    // 获取IP详细信息
    struct IPDetailInfo {
        std::string ip;
        int currentConnections = 0;
        int totalConnections = 0;
        int failedAttempts = 0;
        int successfulAuths = 0;
        float trustScore = 0.0f;
        bool isBanned = false;
        bool isWhitelisted = false;
        int remainingBanSeconds = 0;
    };
    std::vector<IPDetailInfo> GetAllIPStats();

    // 清理过期数据
    void CleanupExpiredBans();
    void CleanupExpiredWhitelists();

    // 白名单持久化（数据库）
    void SetDatabaseManager(DatabaseManager* db);
    void SetScopeId(const std::string& id);
    std::string GetScopeId() const { return scopeId; }
    void LoadWhitelistFromDB();
    void SaveWhitelistToDB(const std::string& ip);
    void RemoveWhitelistFromDB(const std::string& ip);
    void CleanupInactiveWhitelist();  // 清理12小时无活动的白名单IP

    // 速率限制
    bool CheckRateLimit(const std::string& clientIP);

    // 防火墙管理
    bool AddFirewallRule(const std::string& ip, int banSeconds, bool permanent = false);
    bool RemoveFirewallRule(const std::string& ruleName);
    void CleanupExpiredFirewallRules();
    void ClearAllFirewallRules();
    std::vector<FirewallRule> GetFirewallRules();

    // 性能监控
    bool IsOverloaded() const;

    // 连接数管理
    void SetCurrentConnectionCount(int count);
    int GetCurrentConnectionCount() const;

    // 配置持久化
    bool SaveConfig(const std::string& configPath);
    bool LoadConfig(const std::string& configPath);

    // 非SOCKS连接检测
    bool IsValidSocks5Connection(SOCKET clientSocket, const std::string& clientIP);
    void BanNonSocksConnection(const std::string& clientIP);

    // IP踢出功能
    void KickAllConnectionsFromIP(const std::string& targetIP,
        std::function<void(const std::string&)> closeConnectionCallback);

    // 连接详情统计
    struct ConnectionDetail {
        std::string ip;
        std::set<std::string> usernames;
        int activeConnections = 0;
        int totalConnections = 0;
        std::chrono::steady_clock::time_point lastConnectTime;
        bool hasAuth = false;
    };
    void RecordConnectionDetail(const std::string& ip, const std::string& username);
    std::vector<ConnectionDetail> GetAllConnectionDetails();

private:
    std::unordered_map<std::string, ConnectionDetail> connectionDetails;
    std::mutex connectionDetailsMutex;
};
