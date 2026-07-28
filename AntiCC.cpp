#include "AntiCC.h"
#include "Logger.h"
#include "DatabaseManager.h"
#include "ABProtectSDK.h"
#include "ABProtectIntegration.h"
#include <algorithm>
#include <sstream>
#include <set>
#include <functional>
#include <shared_mutex>

#define NOMINMAX
#include <windows.h>
#undef min
#undef max

// IPStats Implementation
// ===== 🔥 修复5: 带缓存的信任分数计算 =====
float IPStats::calculateTrustScore() {
    ABPROTECT_CFF_BEGIN;
    ABPROTECT_CHECK_INTEGRITY;
    auto now = std::chrono::steady_clock::now();

    float cached = cachedTrustScore.load();
    if (cached >= 0.0f) {
        auto cacheDuration = std::chrono::duration_cast<std::chrono::seconds>(
            now - trustScoreCacheTime).count();

        if (cacheDuration < TRUST_SCORE_CACHE_SECONDS) {
            ABPROTECT_CFF_END;
            return cached;
        }
    }

    // Call cloud function for trust score calculation
    int64_t hours = 0;
    if (firstSeenTime != std::chrono::steady_clock::time_point{}) {
        hours = std::chrono::duration_cast<std::chrono::hours>(now - firstSeenTime).count();
    }
    int64_t packedFlags = (hours << 16) | ((int64_t)isWhitelisted << 1) | (int64_t)permanentBan;

    int64_t cloudScore = ABProtectLayer::Cloud_TrustScore(
        successfulAuths.load(), authFailures.load(), failedAttempts.load(), packedFlags);

    float finalScore = (float)cloudScore / 100.0f;

    cachedTrustScore.store(finalScore);
    trustScoreCacheTime = now;

    ABPROTECT_CFF_END;
    return finalScore;
}

int IPStats::getDynamicMaxRequests(int baseMax) {
    ABPROTECT_CFF_BEGIN;
    ABPROTECT_CHECK_INTEGRITY;
    int64_t result = ABProtectLayer::Cloud_DynamicRateLimit(
        baseMax, (int64_t)(trustScore * 100), successfulAuths.load(), consecutiveFailures);
    ABPROTECT_CFF_END;
    return (int)result;
}

// IPListManager Implementation
bool IPListManager::isBlacklisted(const std::string& ip) {
    std::lock_guard<std::mutex> lock(listMutex);
    return blacklist.count(ip) > 0;
}

bool IPListManager::isWhitelisted(const std::string& ip) {
    std::lock_guard<std::mutex> lock(listMutex);
    return whitelist.count(ip) > 0;
}

void IPListManager::addToBlacklist(const std::string& ip) {
    std::lock_guard<std::mutex> lock(listMutex);
    blacklist.insert(ip);
    whitelist.erase(ip);
}

void IPListManager::addToWhitelist(const std::string& ip) {
    std::lock_guard<std::mutex> lock(listMutex);
    whitelist.insert(ip);
    blacklist.erase(ip);
}

void IPListManager::removeFromBlacklist(const std::string& ip) {
    std::lock_guard<std::mutex> lock(listMutex);
    blacklist.erase(ip);
}

void IPListManager::removeFromWhitelist(const std::string& ip) {
    std::lock_guard<std::mutex> lock(listMutex);
    whitelist.erase(ip);
}

std::vector<std::string> IPListManager::getBlacklist() {
    std::lock_guard<std::mutex> lock(listMutex);
    return std::vector<std::string>(blacklist.begin(), blacklist.end());
}

std::vector<std::string> IPListManager::getWhitelist() {
    std::lock_guard<std::mutex> lock(listMutex);
    return std::vector<std::string>(whitelist.begin(), whitelist.end());
}

void IPListManager::clearBlacklist() {
    std::lock_guard<std::mutex> lock(listMutex);
    blacklist.clear();
}

void IPListManager::clearWhitelist() {
    std::lock_guard<std::mutex> lock(listMutex);
    whitelist.clear();
}

// AttackDetector Implementation
void AttackDetector::RecordConnection() {
    recentConnections++;
}

void AttackDetector::RecordFailure() {
    recentFailures++;
}

void AttackDetector::RecordBlock() {
    recentBlocks++;
}

int AttackDetector::UpdateAttackStatus() {
    std::lock_guard<std::mutex> lock(detectorMutex);
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastCheck);

    if (elapsed.count() >= 2) {
        float connRate = recentConnections.load() / std::max(1.0f, (float)elapsed.count());
        float failRate = (float)recentFailures / std::max(1, recentConnections.load());
        float blockRate = (float)recentBlocks / std::max(1, recentConnections.load());

        bool wasUnderAttack = underAttack.load();
        underAttack = (connRate > 30 && (failRate > 0.5 || blockRate > 0.3));

        recentConnections = 0;
        recentFailures = 0;
        recentBlocks = 0;
        lastCheck = now;

        if (!wasUnderAttack && underAttack) {
            return 1;
        }
        else if (wasUnderAttack && !underAttack) {
            return -1;
        }
    }
    return 0;
}

bool AttackDetector::IsUnderAttack() const {
    return underAttack.load();
}

float AttackDetector::GetAttackIntensity() const {
    if (!underAttack) return 0.0f;
    return std::min(1.0f, (float)recentConnections / 100.0f);
}

// AntiCC Implementation
AntiCC::AntiCC() : dbManager(nullptr) {
    // ===== 🔥 修复4: 启动内存清理线程 =====
    cleanupThreadRunning = true;
    cleanupThread = std::thread(&AntiCC::CleanupThreadLoop, this);

    AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] 系统已初始化，清理线程已启动");
}

AntiCC::~AntiCC() {
    // ===== 🔥 修复4: 停止清理线程 =====
    cleanupThreadRunning = false;
    cleanupCV.notify_all();  // 唤醒清理线程
    if (cleanupThread.joinable()) {
        cleanupThread.join();
    }

    AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] 系统已关闭");
}

void AntiCC::SetConfig(const AntiCCConfig& cfg) {
    config = cfg;
    AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] Config Updated");
}

AntiCCConfig AntiCC::GetConfig() const {
    return config;
}

void AntiCC::SetEnabled(bool enabled) {
    config.enabled = enabled;
    AB_LOG_INFO(enabled ? "[AntiCC] Enabled" : "[AntiCC] Disabled");
}

bool AntiCC::IsEnabled() const {
    return config.enabled;
}

bool AntiCC::CheckAntiCC(const std::string& clientIP, bool isWhitelistConnection) {
    if (!config.enabled || clientIP.empty()) {
        return true;
    }

    // 🔥 双连接池：快速路径 - 白名单用户检查独立配额
    if (isWhitelistConnection || (config.useWhitelist && ipListManager.isWhitelisted(clientIP))) {
        if (whitelistConnectionCount >= config.maxConnections) {
            AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] Whitelist pool full: " + clientIP);
            blockedCount++;
            return false;
        }
        whitelistConnectionCount++;
        totalConnections++;
        return true;
    }

    // 🔥 修复2: 快速路径 - 黑名单直接拒绝（无锁）
    if (config.useBlacklist && ipListManager.isBlacklisted(clientIP)) {
        AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] Blacklisted IP rejected: " + clientIP);
        blockedCount++;
        attackDetector.RecordBlock();
        return false;
    }

    // Performance monitoring
    perfMonitor.EnterCheck();
    struct CheckGuard {
        PerformanceMonitor& pm;
        CheckGuard(PerformanceMonitor& p) : pm(p) {}
        ~CheckGuard() { pm.ExitCheck(); }
    } guard(perfMonitor);

    totalConnections++;
    attackDetector.RecordConnection();
    attackDetector.UpdateAttackStatus();

    // Rate limit check
    if (!CheckRateLimit(clientIP)) {
        blockedCount++;
        attackDetector.RecordBlock();
        return false;
    }

    auto now = std::chrono::steady_clock::now();

    // 🔥 修复2: 第一步 - 尝试只读访问（允许多个线程并发读取）
    IPStats* stats = nullptr;
    {
        std::shared_lock<std::shared_mutex> readLock(statsMutex);
        auto it = ipStats.find(clientIP);
        if (it != ipStats.end()) {
            stats = it->second.get();
        }
    }

    // 🔥 修复2: 第二步 - 如果不存在，才使用写锁创建
    if (!stats) {
        std::lock_guard<std::mutex> createLock(ipStatsCreateMutex);

        // 双重检查，避免重复创建
        {
            std::shared_lock<std::shared_mutex> readLock(statsMutex);
            auto it = ipStats.find(clientIP);
            if (it != ipStats.end()) {
                stats = it->second.get();
            }
        }

        if (!stats) {
            std::unique_lock<std::shared_mutex> writeLock(statsMutex);
            ipStats[clientIP] = std::make_unique<IPStats>();
            ipStats[clientIP]->firstSeenTime = now;
            stats = ipStats[clientIP].get();
            AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] New IP first connection: " + clientIP);
        }
    }

    // 在整个检查期间持有map读锁，避免后台清理线程回收当前IP条目。
    std::shared_lock<std::shared_mutex> statsLifetimeLock(statsMutex);
    std::lock_guard<std::mutex> statsLock(stats->statsMutex);

    // 🔥 修复2: 使用原子操作或直接访问（大部分字段是atomic的）
    if (stats->permanentBan) {
        blockedCount++;
        attackDetector.RecordBlock();
        AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] Permanently banned IP: " + clientIP);
        return false;
    }

    if (stats->banUntil > now) {
        auto remainingSeconds = std::chrono::duration_cast<std::chrono::seconds>(
            stats->banUntil - now).count();
        blockedCount++;
        attackDetector.RecordBlock();
        AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] IP still banned: " + clientIP + " remaining: " +
            std::to_string(remainingSeconds) + "s");
        return false;
    }

    bool inWhitelist = stats->isWhitelisted;

    // ===== 🔥 双连接池架构：独立检查配额 =====
    if (inWhitelist || isWhitelistConnection) {
        // 白名单用户检查白名单池
        if (whitelistConnectionCount >= config.maxConnections) {
            AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] Whitelist pool full: " + clientIP);
            blockedCount++;
            attackDetector.RecordBlock();
            return false;
        }
    } else {
        // 普通用户检查普通池（80%配额）
        int normalMaxConns = static_cast<int>(config.maxConnections * 0.8);
        if (normalConnectionCount >= normalMaxConns) {
            float trustScore = stats->calculateTrustScore();
            if (trustScore < 50) {
                AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] Normal pool full, low trust rejected: " + clientIP);
                blockedCount++;
                attackDetector.RecordBlock();
                return false;
            }
        }
    }

    int maxConns = config.maxConnections;

    // ===== 双连接池架构：已在上面检查过配额，这里不再重复检查 =====

    auto windowStart = now - std::chrono::seconds(config.timeWindowSeconds);
    auto validStart = std::lower_bound(
        stats->connectionTimes.begin(),
        stats->connectionTimes.end(),
        windowStart
    );

    if (validStart != stats->connectionTimes.begin()) {
        stats->connectionTimes.erase(stats->connectionTimes.begin(), validStart);
    }

    stats->connectionTimes.push_back(now);
    int requestCount = static_cast<int>(stats->connectionTimes.size());

    int baseMaxRequests = config.maxRequestsInWindow;
    int dynamicMaxRequests = baseMaxRequests;

    if (inWhitelist) {
        dynamicMaxRequests = baseMaxRequests * 2;
    }
    else {
        float trustScore = stats->calculateTrustScore();
        if (trustScore >= 70) {
            dynamicMaxRequests = baseMaxRequests * 3;
        }
        else if (trustScore >= 50) {
            dynamicMaxRequests = baseMaxRequests * 2;
        }
        else if (trustScore >= 30) {
            dynamicMaxRequests = static_cast<int>(baseMaxRequests * 1.5);
        }

        if (attackDetector.IsUnderAttack() && stats->currentConnections > 1) {
            dynamicMaxRequests = static_cast<int>(dynamicMaxRequests * 0.8);
        }
    }

    if (requestCount > dynamicMaxRequests) {
        if (inWhitelist) {
            if (stats->consecutiveFailures <= 10) {
                AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] Warning - Whitelist IP: " + clientIP + " high frequency (" +
                    std::to_string(requestCount) + "/" +
                    std::to_string(dynamicMaxRequests) + ") but allowed");
                stats->currentConnections++;
                return true;
            }
            else {
                int banTime = config.banTimeSeconds / 2;
                stats->banUntil = now + std::chrono::seconds(banTime);
                AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] Whitelist IP over limit banned - " + clientIP + " ban: " +
                    std::to_string(banTime) + "s (half penalty)");
                blockedCount++;
                attackDetector.RecordBlock();
                return false;
            }
        }
        else {
            stats->consecutiveFailures++;

            if (stats->consecutiveFailures <= 5) {
                AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] Warning - IP: " + clientIP + " high frequency (" +
                    std::to_string(requestCount) + "/" +
                    std::to_string(dynamicMaxRequests) + ") but allowed");
                stats->currentConnections++;
                return true;
            }
            else if (stats->consecutiveFailures <= 7) {
                stats->banUntil = now + std::chrono::seconds(5);
                AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] Brief limit - IP: " + clientIP + " 5s cooldown");
                attackDetector.RecordFailure();
                return false;
            }
            else {
                int banTime = config.banTimeSeconds;
                stats->banUntil = now + std::chrono::seconds(banTime);
                AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] Ban IP - " + clientIP + " [violations: " +
                    std::to_string(stats->consecutiveFailures.load()) +
                    "] ban: " + std::to_string(banTime) + "s");
                blockedCount++;
                attackDetector.RecordBlock();
                return false;
            }
        }
    }
    else {
        if (requestCount <= dynamicMaxRequests / 2) {
            stats->consecutiveFailures = 0;
            stats->recentBehaviorScore = std::min(100,
                stats->recentBehaviorScore.load() + 10);
        }
        stats->lastSuccessTime = now;
    }

    // 新增：如果是白名单IP，更新最后活跃时间并保存到数据库
    if (inWhitelist || isWhitelistConnection) {
        stats->lastActiveTime = now;
        whitelistConnectionCount++;  // 增加白名单池计数

        // 定期保存到数据库（避免每次都写数据库，降低性能开销）
        static std::atomic<int> updateCounter(0);
        if (++updateCounter % 10 == 0 && dbManager) {
            SaveWhitelistToDB(clientIP);
        }
    } else {
        normalConnectionCount++;  // 增加普通池计数
    }

    stats->currentConnections++;
    currentConnectionCount++;

    AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] Check passed: " + clientIP +
        " [" + (inWhitelist || isWhitelistConnection ? "whitelist" : "normal") + " pool] " +
        "whitelist: " + std::to_string(whitelistConnectionCount.load()) + "/" + std::to_string(config.maxConnections) +
        ", normal: " + std::to_string(normalConnectionCount.load()) + "/" + std::to_string(static_cast<int>(config.maxConnections * 0.8)));

    return true;
}

void AntiCC::OnConnectionEstablished(const std::string& clientIP, bool isWhitelistConnection) {
    (void)clientIP;
    (void)isWhitelistConnection;
}

void AntiCC::OnConnectionClosed(const std::string& clientIP, bool isWhitelistConnection) {
    // ===== 🔥 双连接池：根据连接类型减少对应池的计数 =====
    if (isWhitelistConnection || ipListManager.isWhitelisted(clientIP)) {
        whitelistConnectionCount--;
        if (whitelistConnectionCount < 0) whitelistConnectionCount = 0;
    } else {
        normalConnectionCount--;
        if (normalConnectionCount < 0) normalConnectionCount = 0;
    }

    currentConnectionCount--;
    if (currentConnectionCount < 0) currentConnectionCount = 0;

    std::unique_lock<std::shared_mutex> lock(statsMutex);
    auto it = ipStats.find(clientIP);
    if (it != ipStats.end()) {
        std::lock_guard<std::mutex> statsLock(it->second->statsMutex);
        it->second->currentConnections--;
        if (it->second->currentConnections < 0) {
            it->second->currentConnections = 0;
        }
    }
}

void AntiCC::OnAuthSuccess(const std::string& clientIP) {
    std::unique_lock<std::shared_mutex> lock(statsMutex);
    auto it = ipStats.find(clientIP);
    if (it != ipStats.end()) {
        std::lock_guard<std::mutex> statsLock(it->second->statsMutex);
        it->second->successfulAuths++;
        it->second->hasAuth = true;
        it->second->consecutiveFailures = 0;
        it->second->isWhitelisted = true;  // 标记为白名单状态

        // 清除封禁状态（认证成功的用户应该立即解禁）
        it->second->banUntil = std::chrono::steady_clock::time_point{};
        it->second->permanentBan = false;

        it->second->trustScore = it->second->calculateTrustScore();

        // 设置白名单最后活跃时间为当前时间
        auto now = std::chrono::steady_clock::now();
        it->second->lastActiveTime = now;

        // 新增：认证成功自动加入白名单（同时自动移除黑名单）
        ipListManager.addToWhitelist(clientIP);

        // 新增：保存白名单到数据库
        if (dbManager) {
            SaveWhitelistToDB(clientIP);
        }

        AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] IP auth success and added to whitelist: " + clientIP);
    }
}

void AntiCC::OnAuthFailure(const std::string& clientIP) {
    std::unique_lock<std::shared_mutex> lock(statsMutex);
    auto it = ipStats.find(clientIP);
    if (it != ipStats.end()) {
        std::lock_guard<std::mutex> statsLock(it->second->statsMutex);
        it->second->authFailures++;
        it->second->failedAttempts++;
        it->second->consecutiveFailures++;
        it->second->trustScore = it->second->calculateTrustScore();
        AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] IP auth failed: " + clientIP);
    }
}

void AntiCC::BanIP(const std::string& clientIP, int banSeconds) {
    if (clientIP.empty()) return;

    if (ipListManager.isWhitelisted(clientIP)) {
        AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] Whitelist user " + clientIP + " will not be banned");
        return;
    }

    if (banSeconds == 0) {
        banSeconds = config.banTimeSeconds;
    }

    auto now = std::chrono::steady_clock::now();

    std::unique_lock<std::shared_mutex> lock(statsMutex);
    if (ipStats.find(clientIP) == ipStats.end()) {
        ipStats[clientIP] = std::make_unique<IPStats>();
    }

    auto& stats = ipStats[clientIP];
    stats->banUntil = now + std::chrono::seconds(banSeconds);

    float trustScore = stats->calculateTrustScore();
    if (trustScore < 20) {
        banSeconds *= 2;
    }

    ipListManager.addToBlacklist(clientIP);

    // Add firewall rule if enabled
    if (config.useFirewall) {
        AddFirewallRule(clientIP, banSeconds, false);
    }

    AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] IP " + clientIP + " banned " +
        std::to_string(banSeconds) + " seconds");
}

void AntiCC::BanIPForNoAuth(const std::string& clientIP) {
    if (clientIP.empty()) return;

    if (ipListManager.isWhitelisted(clientIP)) {
        AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] Whitelist user " + clientIP + " will not be banned for no auth");
        return;
    }

    int banSeconds = config.noAuthBanTime;
    auto now = std::chrono::steady_clock::now();

    std::unique_lock<std::shared_mutex> lock(statsMutex);
    if (ipStats.find(clientIP) == ipStats.end()) {
        ipStats[clientIP] = std::make_unique<IPStats>();
    }

    auto& stats = ipStats[clientIP];
    stats->banUntil = now + std::chrono::seconds(banSeconds);

    float trustScore = stats->calculateTrustScore();
    if (trustScore < 20) {
        banSeconds *= 2;
    }

    ipListManager.addToBlacklist(clientIP);

    // Add firewall rule if enabled
    if (config.useFirewall) {
        AddFirewallRule(clientIP, banSeconds, false);
    }

    AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] IP " + clientIP + " banned for no auth " +
        std::to_string(banSeconds) + " seconds");
}

void AntiCC::BanIPForAuthFailure(const std::string& clientIP) {
    if (clientIP.empty()) return;

    int banTime = config.authFailBanTime;
    if (ipListManager.isWhitelisted(clientIP)) {
        banTime = banTime / 2;
    }

    auto now = std::chrono::steady_clock::now();

    std::unique_lock<std::shared_mutex> lock(statsMutex);
    if (ipStats.find(clientIP) == ipStats.end()) {
        ipStats[clientIP] = std::make_unique<IPStats>();
    }

    auto& stats = ipStats[clientIP];
    stats->authFailures++;
    stats->failedAttempts++;

    if (stats->authFailures > 3) {
        banTime *= (stats->authFailures.load() / 3);
    }

    stats->banUntil = now + std::chrono::seconds(banTime);
    stats->trustScore = stats->calculateTrustScore();

    ipListManager.addToBlacklist(clientIP);

    // Add firewall rule if enabled
    if (config.useFirewall) {
        AddFirewallRule(clientIP, banTime, false);
    }

    AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] IP " + clientIP + " banned for auth failure " +
        std::to_string(banTime) + " seconds");
}

void AntiCC::UnbanIP(const std::string& clientIP) {
    std::unique_lock<std::shared_mutex> lock(statsMutex);
    auto it = ipStats.find(clientIP);
    if (it != ipStats.end()) {
        it->second->banUntil = std::chrono::steady_clock::time_point{};
        it->second->permanentBan = false;
    }
    ipListManager.removeFromBlacklist(clientIP);
    AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] IP " + clientIP + " unbanned");
}

bool AntiCC::IsIPBanned(const std::string& clientIP) {
    if (clientIP.empty()) return false;

    if (ipListManager.isWhitelisted(clientIP)) {
        return false;
    }

    std::unique_lock<std::shared_mutex> lock(statsMutex);
    auto it = ipStats.find(clientIP);
    if (it != ipStats.end()) {
        if (it->second->permanentBan) {
            return true;
        }
        auto now = std::chrono::steady_clock::now();
        return it->second->banUntil > now;
    }

    return ipListManager.isBlacklisted(clientIP);
}

void AntiCC::AddToBlacklist(const std::string& ip) {
    {
        std::unique_lock<std::shared_mutex> lock(statsMutex);
        auto it = ipStats.find(ip);
        if (it != ipStats.end()) {
            std::lock_guard<std::mutex> statsLock(it->second->statsMutex);
            it->second->isWhitelisted = false;
        }
    }
    ipListManager.addToBlacklist(ip);
    AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] IP " + ip + " added to blacklist");
}

void AntiCC::AddToWhitelist(const std::string& ip) {
    {
        std::unique_lock<std::shared_mutex> lock(statsMutex);
        auto& stats = ipStats[ip];
        if (!stats) {
            stats = std::make_unique<IPStats>();
            stats->firstSeenTime = std::chrono::steady_clock::now();
        }
        std::lock_guard<std::mutex> statsLock(stats->statsMutex);
        stats->isWhitelisted = true;
        stats->whitelistUntil = std::chrono::steady_clock::now() + std::chrono::seconds(config.whitelistDuration);
        stats->lastActiveTime = std::chrono::steady_clock::now();
    }
    ipListManager.addToWhitelist(ip);
    AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] IP " + ip + " added to whitelist");
}

void AntiCC::RemoveFromBlacklist(const std::string& ip) {
    ipListManager.removeFromBlacklist(ip);
    AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] IP " + ip + " removed from blacklist");
}

void AntiCC::RemoveFromWhitelist(const std::string& ip) {
    {
        std::unique_lock<std::shared_mutex> lock(statsMutex);
        auto it = ipStats.find(ip);
        if (it != ipStats.end()) {
            std::lock_guard<std::mutex> statsLock(it->second->statsMutex);
            it->second->isWhitelisted = false;
            it->second->whitelistUntil = std::chrono::steady_clock::time_point{};
        }
    }
    ipListManager.removeFromWhitelist(ip);
    AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] IP " + ip + " removed from whitelist");
}

bool AntiCC::IsInBlacklist(const std::string& ip) {
    return ipListManager.isBlacklisted(ip);
}

bool AntiCC::IsInWhitelist(const std::string& ip) {
    return ipListManager.isWhitelisted(ip);
}

std::vector<std::string> AntiCC::GetBlacklist() {
    return ipListManager.getBlacklist();
}

std::vector<std::string> AntiCC::GetWhitelist() {
    return ipListManager.getWhitelist();
}

int AntiCC::GetBlockedCount() const {
    return blockedCount.load();
}

int AntiCC::GetTotalConnections() const {
    return totalConnections.load();
}

int AntiCC::GetCurrentConnections() const {
    return currentConnectionCount.load();
}

int AntiCC::GetWhitelistConnections() const {
    return whitelistConnectionCount.load();
}

int AntiCC::GetNormalConnections() const {
    return normalConnectionCount.load();
}

bool AntiCC::IsUnderAttack() const {
    return attackDetector.IsUnderAttack();
}

std::vector<AntiCC::IPDetailInfo> AntiCC::GetAllIPStats() {
    std::vector<IPDetailInfo> result;
    std::unique_lock<std::shared_mutex> lock(statsMutex);

    auto now = std::chrono::steady_clock::now();

    for (const auto& pair : ipStats) {
        IPDetailInfo info;
        info.ip = pair.first;
        info.currentConnections = pair.second->currentConnections.load();
        info.totalConnections = static_cast<int>(pair.second->connectionTimes.size());
        info.failedAttempts = pair.second->failedAttempts.load();
        info.successfulAuths = pair.second->successfulAuths.load();
        info.trustScore = pair.second->calculateTrustScore();
        info.isBanned = pair.second->banUntil > now || pair.second->permanentBan;
        info.isWhitelisted = pair.second->isWhitelisted;

        if (pair.second->banUntil > now) {
            info.remainingBanSeconds = static_cast<int>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    pair.second->banUntil - now).count());
        }
        else {
            info.remainingBanSeconds = 0;
        }

        result.push_back(info);
    }

    return result;
}

void AntiCC::CleanupExpiredBans() {
    std::unique_lock<std::shared_mutex> lock(statsMutex);
    auto now = std::chrono::steady_clock::now();

    for (auto& pair : ipStats) {
        if (pair.second->banUntil > std::chrono::steady_clock::time_point{} &&
            pair.second->banUntil <= now) {
            pair.second->banUntil = std::chrono::steady_clock::time_point{};
            ipListManager.removeFromBlacklist(pair.first);
            AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] IP " + pair.first + " ban expired, auto unban");
        }
    }
}

void AntiCC::CleanupExpiredWhitelists() {
    std::unique_lock<std::shared_mutex> lock(statsMutex);
    auto now = std::chrono::steady_clock::now();

    for (auto& pair : ipStats) {
        if (pair.second->whitelistUntil > std::chrono::steady_clock::time_point{} &&
            pair.second->whitelistUntil <= now) {
            pair.second->whitelistUntil = std::chrono::steady_clock::time_point{};
            pair.second->isWhitelisted = false;
            ipListManager.removeFromWhitelist(pair.first);
            AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] IP " + pair.first + " whitelist expired");
        }
    }
}

// Performance Monitor Implementation
PerformanceMonitor::PerformanceMonitor() {
    lastResetTime = std::chrono::steady_clock::now();
}

void PerformanceMonitor::EnterCheck() {
    checkCount++;
}

void PerformanceMonitor::ExitCheck() {
    // Can track timing if needed
}

bool PerformanceMonitor::IsOverloaded() const {
    return checkCount.load() > 1000; // Simple threshold
}

void PerformanceMonitor::Reset() {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(monitorMutex));
    checkCount = 0;
    lastResetTime = std::chrono::steady_clock::now();
}

// Rate Limit Check
bool AntiCC::CheckRateLimit(const std::string& clientIP) {
    if (!config.rateLimitEnabled || clientIP.empty()) {
        return true;
    }

    auto now = std::chrono::steady_clock::now();
    std::unique_lock<std::shared_mutex> lock(statsMutex);

    auto it = ipStats.find(clientIP);
    if (it == ipStats.end()) {
        ipStats[clientIP] = std::make_unique<IPStats>();
        it = ipStats.find(clientIP);
    }

    auto& stats = it->second;
    std::lock_guard<std::mutex> statsLock(stats->statsMutex);

    // Clean up old rate limit records
    auto windowStart = now - std::chrono::seconds(config.rateTimeWindow);
    auto validStart = std::lower_bound(
        stats->rateLimitTimes.begin(),
        stats->rateLimitTimes.end(),
        windowStart
    );

    if (validStart != stats->rateLimitTimes.begin()) {
        stats->rateLimitTimes.erase(stats->rateLimitTimes.begin(), validStart);
    }

    // Check rate limit
    if (static_cast<int>(stats->rateLimitTimes.size()) >= config.rateLimit) {
        stats->rateLimited = true;
        AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] Rate limit exceeded: " + clientIP);
        return false;
    }

    stats->rateLimitTimes.push_back(now);
    stats->rateLimited = false;
    return true;
}

// PowerShell Execution
std::string AntiCC::ExecutePowerShell(const std::string& command) {
    try {
        std::string fullCommand = "powershell.exe -ExecutionPolicy Bypass -NoProfile -Command \"" + command + "\"";

        SECURITY_ATTRIBUTES sa;
        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = NULL;
        sa.bInheritHandle = TRUE;

        HANDLE hStdOutRead, hStdOutWrite;
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
        DWORD bytesRead;
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

// Firewall Management
bool AntiCC::AddFirewallRule(const std::string& ip, int banSeconds, bool permanent) {
    try {
        if (!config.useFirewall || ip.empty()) {
            return false;
        }

        std::string ruleName = rulePrefix + ip;
        if (permanent) {
            ruleName += "_PERM";
        }

        RemoveFirewallRule(ruleName);

        std::stringstream cmd;
        cmd << "New-NetFirewallRule -DisplayName '" << ruleName << "' "
            << "-Direction Inbound -Action Block "
            << "-Protocol TCP -LocalPort " << config.listenPort << " "
            << "-RemoteAddress '" << ip << "' "
            << "-ErrorAction SilentlyContinue";

        std::string result = ExecutePowerShell(cmd.str());

        if (result.find("Error") == std::string::npos) {
            std::lock_guard<std::mutex> lock(firewallMutex);
            FirewallRule rule;
            rule.ip = ip;
            rule.ruleName = ruleName;
            rule.permanent = permanent;
            if (!permanent) {
                rule.expireTime = std::chrono::steady_clock::now() + std::chrono::seconds(banSeconds);
            }
            firewallRules.push_back(rule);

            if (permanent) {
                AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] Firewall rule added: " + ip + " (permanent)");
            }
            else {
                AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] Firewall rule added: " + ip + " (ban " + std::to_string(banSeconds) + "s)");
            }
            return true;
        }
        else {
            AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] Failed to add firewall rule: " + ip);
            return false;
        }
    }
    catch (...) {
        return false;
    }
}

bool AntiCC::RemoveFirewallRule(const std::string& ruleName) {
    try {
        if (ruleName.empty()) return false;

        std::stringstream cmd;
        cmd << "Remove-NetFirewallRule -DisplayName '" << ruleName << "' -ErrorAction SilentlyContinue";

        ExecutePowerShell(cmd.str());

        std::lock_guard<std::mutex> lock(firewallMutex);
        firewallRules.erase(
            std::remove_if(firewallRules.begin(), firewallRules.end(),
                [&ruleName](const FirewallRule& rule) { return rule.ruleName == ruleName; }),
            firewallRules.end()
        );

        return true;
    }
    catch (...) {
        return false;
    }
}

void AntiCC::CleanupExpiredFirewallRules() {
    try {
        auto now = std::chrono::steady_clock::now();
        std::vector<std::string> rulesToRemove;

        {
            std::lock_guard<std::mutex> lock(firewallMutex);
            for (auto it = firewallRules.begin(); it != firewallRules.end(); ) {
                if (!it->permanent && it->expireTime <= now) {
                    rulesToRemove.push_back(it->ruleName);
                    it = firewallRules.erase(it);
                }
                else {
                    ++it;
                }
            }
        }

        for (const auto& ruleName : rulesToRemove) {
            RemoveFirewallRule(ruleName);
            AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] Firewall rule expired: " + ruleName);
        }
    }
    catch (...) {
    }
}

void AntiCC::ClearAllFirewallRules() {
    try {
        std::vector<std::string> rulesToRemove;

        {
            std::lock_guard<std::mutex> lock(firewallMutex);
            for (const auto& rule : firewallRules) {
                rulesToRemove.push_back(rule.ruleName);
            }
            firewallRules.clear();
        }

        for (const auto& ruleName : rulesToRemove) {
            RemoveFirewallRule(ruleName);
        }

        AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] All firewall rules cleared");
    }
    catch (...) {
    }
}

std::vector<FirewallRule> AntiCC::GetFirewallRules() {
    std::lock_guard<std::mutex> lock(firewallMutex);
    return firewallRules;
}

bool AntiCC::IsOverloaded() const {
    return perfMonitor.IsOverloaded();
}

void AntiCC::SetCurrentConnectionCount(int count) {
    currentConnectionCount = count;
}

int AntiCC::GetCurrentConnectionCount() const {
    return currentConnectionCount.load();
}

// Configuration Persistence
bool AntiCC::SaveConfig(const std::string& configPath) {
    try {
        std::wstring wpath(configPath.begin(), configPath.end());
        wchar_t buffer[256];

        // Save AntiCC config
        swprintf_s(buffer, L"%d", config.timeWindowSeconds);
        WritePrivateProfileStringW(L"AntiCC", L"TimeWindow", buffer, wpath.c_str());

        swprintf_s(buffer, L"%d", config.maxRequestsInWindow);
        WritePrivateProfileStringW(L"AntiCC", L"MaxRequests", buffer, wpath.c_str());

        swprintf_s(buffer, L"%d", config.banTimeSeconds);
        WritePrivateProfileStringW(L"AntiCC", L"BanTime", buffer, wpath.c_str());

        swprintf_s(buffer, L"%d", config.maxConnections);
        WritePrivateProfileStringW(L"AntiCC", L"MaxConnections", buffer, wpath.c_str());

        swprintf_s(buffer, L"%d", config.whitelistDuration);
        WritePrivateProfileStringW(L"AntiCC", L"WhitelistTime", buffer, wpath.c_str());

        swprintf_s(buffer, L"%d", config.authFailBanTime);
        WritePrivateProfileStringW(L"AntiCC", L"AuthFailBanTime", buffer, wpath.c_str());

        swprintf_s(buffer, L"%d", config.noAuthBanTime);
        WritePrivateProfileStringW(L"AntiCC", L"NoAuthBanTime", buffer, wpath.c_str());

        swprintf_s(buffer, L"%d", config.rateLimit);
        WritePrivateProfileStringW(L"RateLimit", L"Rate", buffer, wpath.c_str());

        swprintf_s(buffer, L"%d", config.rateTimeWindow);
        WritePrivateProfileStringW(L"RateLimit", L"TimeWindow", buffer, wpath.c_str());

        swprintf_s(buffer, L"%d", config.listenPort);
        WritePrivateProfileStringW(L"AntiCC", L"ListenPort", buffer, wpath.c_str());

        // Save boolean options
        WritePrivateProfileStringW(L"Options", L"Enabled", config.enabled ? L"1" : L"0", wpath.c_str());
        WritePrivateProfileStringW(L"Options", L"UseFirewall", config.useFirewall ? L"1" : L"0", wpath.c_str());
        WritePrivateProfileStringW(L"Options", L"RateLimitEnabled", config.rateLimitEnabled ? L"1" : L"0", wpath.c_str());
        WritePrivateProfileStringW(L"Options", L"UseBlacklist", config.useBlacklist ? L"1" : L"0", wpath.c_str());
        WritePrivateProfileStringW(L"Options", L"UseWhitelist", config.useWhitelist ? L"1" : L"0", wpath.c_str());
        WritePrivateProfileStringW(L"Options", L"BlockNonSocks", config.blockNonSocks ? L"1" : L"0", wpath.c_str());

        AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] Config saved to: " + configPath);
        return true;
    }
    catch (...) {
        AB_LOG_ERROR_CAT(LOG_CAT_ANTICC, "[AntiCC] Failed to save config");
        return false;
    }
}

bool AntiCC::LoadConfig(const std::string& configPath) {
    try {
        std::wstring wpath(configPath.begin(), configPath.end());
        wchar_t buffer[256];

        // Load AntiCC config
        GetPrivateProfileStringW(L"AntiCC", L"TimeWindow", L"10", buffer, 256, wpath.c_str());
        config.timeWindowSeconds = _wtoi(buffer);

        GetPrivateProfileStringW(L"AntiCC", L"MaxRequests", L"20", buffer, 256, wpath.c_str());
        config.maxRequestsInWindow = _wtoi(buffer);

        GetPrivateProfileStringW(L"AntiCC", L"BanTime", L"300", buffer, 256, wpath.c_str());
        config.banTimeSeconds = _wtoi(buffer);

        GetPrivateProfileStringW(L"AntiCC", L"MaxConnections", L"100", buffer, 256, wpath.c_str());
        config.maxConnections = _wtoi(buffer);

        GetPrivateProfileStringW(L"AntiCC", L"WhitelistTime", L"3600", buffer, 256, wpath.c_str());
        config.whitelistDuration = _wtoi(buffer);

        GetPrivateProfileStringW(L"AntiCC", L"AuthFailBanTime", L"60", buffer, 256, wpath.c_str());
        config.authFailBanTime = _wtoi(buffer);

        GetPrivateProfileStringW(L"AntiCC", L"NoAuthBanTime", L"30", buffer, 256, wpath.c_str());
        config.noAuthBanTime = _wtoi(buffer);

        GetPrivateProfileStringW(L"RateLimit", L"Rate", L"100", buffer, 256, wpath.c_str());
        config.rateLimit = _wtoi(buffer);

        GetPrivateProfileStringW(L"RateLimit", L"TimeWindow", L"1", buffer, 256, wpath.c_str());
        config.rateTimeWindow = _wtoi(buffer);

        GetPrivateProfileStringW(L"AntiCC", L"ListenPort", L"1081", buffer, 256, wpath.c_str());
        config.listenPort = _wtoi(buffer);

        // Load boolean options
        GetPrivateProfileStringW(L"Options", L"Enabled", L"0", buffer, 256, wpath.c_str());
        config.enabled = (_wtoi(buffer) == 1);

        GetPrivateProfileStringW(L"Options", L"UseFirewall", L"0", buffer, 256, wpath.c_str());
        config.useFirewall = (_wtoi(buffer) == 1);

        GetPrivateProfileStringW(L"Options", L"RateLimitEnabled", L"0", buffer, 256, wpath.c_str());
        config.rateLimitEnabled = (_wtoi(buffer) == 1);

        GetPrivateProfileStringW(L"Options", L"UseBlacklist", L"1", buffer, 256, wpath.c_str());
        config.useBlacklist = (_wtoi(buffer) == 1);

        GetPrivateProfileStringW(L"Options", L"UseWhitelist", L"1", buffer, 256, wpath.c_str());
        config.useWhitelist = (_wtoi(buffer) == 1);

        GetPrivateProfileStringW(L"Options", L"BlockNonSocks", L"0", buffer, 256, wpath.c_str());
        config.blockNonSocks = (_wtoi(buffer) == 1);

        AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] Config loaded from: " + configPath);
        return true;
    }
    catch (...) {
        AB_LOG_ERROR_CAT(LOG_CAT_ANTICC, "[AntiCC] Failed to load config");
        return false;
    }
}

// Non-SOCKS Connection Detection
bool AntiCC::IsValidSocks5Connection(SOCKET clientSocket, const std::string& clientIP) {
    try {
        unsigned char buffer[10];
        int len = recv(clientSocket, (char*)buffer, sizeof(buffer), MSG_PEEK);

        if (len >= 2) {
            // Check SOCKS5 version byte
            if (buffer[0] == 0x05) {
                return true;
            }
        }

        AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] Non-SOCKS5 connection detected from: " + clientIP);
        return false;
    }
    catch (...) {
        return false;
    }
}

void AntiCC::BanNonSocksConnection(const std::string& clientIP) {
    if (clientIP.empty()) return;

    std::unique_lock<std::shared_mutex> lock(statsMutex);

    if (ipStats.find(clientIP) == ipStats.end()) {
        ipStats[clientIP] = std::make_unique<IPStats>();
    }

    auto& stats = ipStats[clientIP];
    auto now = std::chrono::steady_clock::now();

    int banTime = config.noAuthBanTime;
    stats->banUntil = now + std::chrono::seconds(banTime);
    stats->trustScore = 0;
    stats->authFailures += 5;
    stats->failedAttempts += 5;

    ipListManager.addToBlacklist(clientIP);

    if (config.useFirewall) {
        AddFirewallRule(clientIP, banTime, false);
    }

    AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] Non-SOCKS connection banned: " + clientIP + " for " +
        std::to_string(banTime) + "s");
}

// IP Kick Function
void AntiCC::KickAllConnectionsFromIP(const std::string& targetIP,
    std::function<void(const std::string&)> closeConnectionCallback) {

    if (targetIP.empty() || !closeConnectionCallback) return;

    AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] Kicking all connections from IP: " + targetIP);

    // Call the callback to close connections
    closeConnectionCallback(targetIP);

    // Ban the IP
    BanIP(targetIP, config.banTimeSeconds);
}

// Connection Detail Statistics
void AntiCC::RecordConnectionDetail(const std::string& ip, const std::string& username) {
    if (ip.empty()) return;

    std::lock_guard<std::mutex> lock(connectionDetailsMutex);

    auto& detail = connectionDetails[ip];
    detail.ip = ip;

    if (!username.empty()) {
        detail.usernames.insert(username);
        detail.hasAuth = true;
    }

    detail.activeConnections++;
    detail.totalConnections++;
    detail.lastConnectTime = std::chrono::steady_clock::now();
}

std::vector<AntiCC::ConnectionDetail> AntiCC::GetAllConnectionDetails() {
    std::vector<ConnectionDetail> result;
    std::lock_guard<std::mutex> lock(connectionDetailsMutex);

    for (const auto& pair : connectionDetails) {
        result.push_back(pair.second);
    }

    return result;
}

// ==================== 白名单持久化实现 ====================

// 设置数据库管理器
void AntiCC::SetDatabaseManager(DatabaseManager* db) {
    dbManager = db;
    AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] Database manager set for whitelist persistence");
}

void AntiCC::SetScopeId(const std::string& id) {
    scopeId = id.empty() ? "default" : id;
    AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] Scope updated: " + scopeId);
}

// 从数据库加载白名单
void AntiCC::LoadWhitelistFromDB() {
    if (!dbManager) {
        AB_LOG_WARNING_CAT(LOG_CAT_ANTICC, "[AntiCC] Database manager not set, cannot load whitelist");
        return;
    }

    std::unique_lock<std::shared_mutex> lock(statsMutex);

    // 从数据库读取白名单配置（格式：whitelist_<ip> = <timestamp>）
    // 我们使用配置数据库的键值对存储，键为"whitelist_<ip>"，值为Unix时间戳（最后活跃时间）

    // 注意：DatabaseManager没有提供遍历所有键的方法
    // 我们需要使用一个特殊的键来存储所有白名单IP列表
    const std::string listKey = "anticc_whitelist_ips_" + scopeId;
    std::string whitelistIPs = dbManager->GetConfigValue(listKey, "");

    if (whitelistIPs.empty()) {
        AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] No whitelist IPs found in database");
        return;
    }

    // 解析IP列表（用逗号分隔）
    std::istringstream ss(whitelistIPs);
    std::string ip;
    int loadedCount = 0;
    auto now = std::chrono::steady_clock::now();

    while (std::getline(ss, ip, ',')) {
        if (ip.empty()) continue;

        // 读取该IP的最后活跃时间
        std::string lastActiveKey = "anticc_whitelist_" + scopeId + "_" + ip;
        std::string lastActiveStr = dbManager->GetConfigValue(lastActiveKey, "0");
        int64_t lastActiveTimestamp = std::stoll(lastActiveStr);

        // 计算距离现在的时间（秒）
        int64_t currentTimestamp = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        int64_t secondsSinceActive = currentTimestamp - lastActiveTimestamp;

        // 如果超过12小时（43200秒），跳过不加载（已过期）
        const int64_t TWELVE_HOURS = 12 * 60 * 60;
        if (secondsSinceActive > TWELVE_HOURS) {
            AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] Whitelist IP expired (inactive for " +
                std::to_string(secondsSinceActive / 3600) + " hours): " + ip);
            RemoveWhitelistFromDB(ip);  // 从数据库删除过期IP
            continue;
        }

        // 加载到内存

        // 创建或更新IPStats
        if (ipStats.find(ip) == ipStats.end()) {
            ipStats[ip] = std::make_unique<IPStats>();
        }

        auto& stats = ipStats[ip];
        stats->isWhitelisted = true;
        ipListManager.addToWhitelist(ip);

        // 恢复最后活跃时间
        auto restoredSecondsSinceActive = std::max<int64_t>(0, currentTimestamp - lastActiveTimestamp);
        auto lastActiveTime = std::chrono::steady_clock::now() - std::chrono::seconds(restoredSecondsSinceActive);
        stats->lastActiveTime = lastActiveTime;

        loadedCount++;
        AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] Loaded whitelist IP from DB: " + ip +
            " (last active: " + std::to_string(restoredSecondsSinceActive / 60) + " minutes ago)");
    }

    AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] Loaded " + std::to_string(loadedCount) +
        " whitelist IPs from database");
}

// 保存白名单IP到数据库
void AntiCC::SaveWhitelistToDB(const std::string& ip) {
    if (!dbManager || ip.empty()) {
        return;
    }

    auto now = std::chrono::system_clock::now();
    int64_t timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();

    // 保存最后活跃时间
    std::string lastActiveKey = "anticc_whitelist_" + scopeId + "_" + ip;
    dbManager->SetConfigValue(lastActiveKey, std::to_string(timestamp));

    // 更新白名单IP列表
    const std::string listKey = "anticc_whitelist_ips_" + scopeId;
    std::string whitelistIPs = dbManager->GetConfigValue(listKey, "");

    // 检查IP是否已经在列表中
    if (whitelistIPs.find(ip) == std::string::npos) {
        if (!whitelistIPs.empty()) {
            whitelistIPs += ",";
        }
        whitelistIPs += ip;
        dbManager->SetConfigValue(listKey, whitelistIPs);
    }

    AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] Saved whitelist IP to DB: " + ip);
}

// 从数据库删除白名单IP
void AntiCC::RemoveWhitelistFromDB(const std::string& ip) {
    if (!dbManager || ip.empty()) {
        return;
    }

    // 删除最后活跃时间记录
    std::string lastActiveKey = "anticc_whitelist_" + scopeId + "_" + ip;
    dbManager->DeleteConfigValue(lastActiveKey);

    // 从白名单IP列表中移除
    const std::string listKey = "anticc_whitelist_ips_" + scopeId;
    std::string whitelistIPs = dbManager->GetConfigValue(listKey, "");

    if (!whitelistIPs.empty()) {
        std::istringstream ss(whitelistIPs);
        std::vector<std::string> ipList;
        std::string currentIP;

        while (std::getline(ss, currentIP, ',')) {
            if (!currentIP.empty() && currentIP != ip) {
                ipList.push_back(currentIP);
            }
        }

        // 重新组合IP列表
        std::string newList;
        for (size_t i = 0; i < ipList.size(); i++) {
            if (i > 0) newList += ",";
            newList += ipList[i];
        }

        dbManager->SetConfigValue(listKey, newList);
    }

    AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] Removed whitelist IP from DB: " + ip);
}

// 清理12小时无活动的白名单IP
void AntiCC::CleanupInactiveWhitelist() {
    std::unique_lock<std::shared_mutex> lock(statsMutex);
    auto now = std::chrono::steady_clock::now();
    const int64_t TWELVE_HOURS = 12 * 60 * 60;  // 12小时（秒）
    std::vector<std::string> toRemove;

    // 遍历所有IP统计
    for (auto& pair : ipStats) {
        const std::string& ip = pair.first;
        auto& stats = pair.second;

        // 只处理白名单IP
        if (!stats->isWhitelisted) {
            continue;
        }

        // 检查最后活跃时间
        if (stats->lastActiveTime == std::chrono::steady_clock::time_point{}) {
            // 如果从未设置过活跃时间，设置为当前时间
            if (dbManager) {
                SaveWhitelistToDB(ip);
            }
            continue;
        }

        auto secondsSinceActive = std::chrono::duration_cast<std::chrono::seconds>(
            now - stats->lastActiveTime).count();

        if (secondsSinceActive > TWELVE_HOURS) {
            toRemove.push_back(ip);
        }
    }

    // 删除过期的白名单IP
    for (const auto& ip : toRemove) {
        ipListManager.removeFromWhitelist(ip);

        auto it = ipStats.find(ip);
        if (it != ipStats.end()) {
            it->second->isWhitelisted = false;
        }

        // 从数据库删除
        if (dbManager) {
            RemoveWhitelistFromDB(ip);
        }

        AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] Whitelist IP expired (inactive for 12+ hours): " + ip);
    }

    if (!toRemove.empty()) {
        AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] Cleaned up " + std::to_string(toRemove.size()) +
            " inactive whitelist IPs");
    }
}

// ===========================================================================================
// 🔥 修复4: 内存清理线程实现
// ===========================================================================================

void AntiCC::CleanupThreadLoop() {
    AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC清理] 清理线程已启动");

    while (cleanupThreadRunning) {
        // 使用条件变量等待5分钟，可以被提前唤醒
        std::unique_lock<std::mutex> lock(cleanupMutex);
        cleanupCV.wait_for(lock, std::chrono::minutes(5), [this]() {
            return !cleanupThreadRunning.load();
        });

        if (!cleanupThreadRunning) break;

        auto now = std::chrono::steady_clock::now();
        std::vector<std::string> toRemove;
        int whitelistCleaned = 0;
        int inactiveCleaned = 0;

        // 🔥 修复4: 第一阶段 - 在读锁下收集需要清理的IP
        {
            std::shared_lock<std::shared_mutex> readLock(statsMutex);

            for (const auto& pair : ipStats) {
                const std::string& ip = pair.first;
                const auto& stats = pair.second;

                // 清理条件1: 当前无连接 + 非白名单 + 未被封禁
                bool shouldClean = (stats->currentConnections.load() == 0 &&
                    !stats->isWhitelisted &&
                    stats->banUntil < now);

                if (shouldClean) {
                    auto lastActive = stats->lastSuccessTime;
                    if (lastActive == std::chrono::steady_clock::time_point{}) {
                        lastActive = stats->firstSeenTime;
                    }

                    // 🔥 修复4: 1小时无活动则清理
                    auto inactiveHours = std::chrono::duration_cast<std::chrono::hours>(
                        now - lastActive).count();

                    if (inactiveHours >= 1) {
                        toRemove.push_back(ip);
                        inactiveCleaned++;
                    }
                }

                // 清理条件2: 白名单过期
                if (stats->whitelistUntil > std::chrono::steady_clock::time_point{} &&
                    stats->whitelistUntil <= now) {
                    whitelistCleaned++;
                }
            }
        }

        // 🔥 修复4: 第二阶段 - 使用写锁执行清理
        if (!toRemove.empty()) {
            std::unique_lock<std::shared_mutex> writeLock(statsMutex);
            for (const auto& ip : toRemove) {
                ipStats.erase(ip);
            }
        }

        // 清理过期白名单
        if (whitelistCleaned > 0) {
            CleanupExpiredWhitelists();
        }

        // 清理过期封禁
        CleanupExpiredBans();

        // 清理过期防火墙规则
        if (config.useFirewall) {
            CleanupExpiredFirewallRules();
        }

        if (inactiveCleaned > 0 || whitelistCleaned > 0) {
            AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC清理] 本次清理: 不活跃IP=" +
                std::to_string(inactiveCleaned) + ", 过期白名单=" + std::to_string(whitelistCleaned));
        }
    }

    AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC清理] 清理线程已退出");
}
