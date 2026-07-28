#pragma once

#include <string>
#include <memory>
#include <vector>
#include <map>
#include <deque>
#include <mutex>
#include <atomic>
#include <functional>
#include <algorithm>
#include "PacketCollector.h"
#include "HeartbeatCore.h"
#include "DatabaseManager.h"
#include "CollectedPacketPool.h"

// 前向声明
class HttpApiServer;

// ==================== 实例类型 ====================
enum class InstanceType {
    Collector,      // 单伪采集
    Heartbeat,      // 单伪伪心跳
    AbCollector,    // ab采集（独立模式同款逻辑）
    AbHeartbeat,    // ab伪心跳（独立模式同款逻辑）
    Socks5Pool,     // SOCKS5账号库
    SocksForward    // Socks转发（不记录数据包，支持WPE滤镜和二级代理）
};

// ==================== 实例状态 ====================
enum class InstanceState {
    Stopped,        // 已停止
    Starting,       // 启动中
    Running,        // 运行中
    Stopping,       // 停止中
    Error           // 错误状态
};

// ==================== InstMgr命名空间（避免与新伪心跳.cpp冲突） ====================
namespace InstMgr {

// ==================== 采集存储模式 ====================
enum class StorageMode {
    Hourly,         // 按小时数据库
    Daily,          // 按天数据库
    Memory          // 内存存储模式
};

// ==================== 伪心跳数据池来源 ====================
enum class PoolSource {
    Database,       // 从数据库读取
    BoundCollector  // 从绑定的采集实例内存池读取
};

// ==================== 数据包顺序模式 ====================
enum class OrderMode {
    Ascending,      // 升序（FIFO，先进先出）
    Descending      // 降序（LIFO，后进先出）
};

// ==================== 采集白名单规则（命中后允许采集）====================
struct CollectorWhitelistRule {
    int id = 0;
    std::string name;
    std::string searchPattern;      // 搜索模式字符串，格式: "pos|hex,pos|hex,..." 或纯文本
    bool usePatternSearch = true;   // true=模式搜索(pos|hex), false=包含匹配
    bool isEnabled = true;
    uint64_t matchCount = 0;        // 命中次数统计

    bool operator==(const CollectorWhitelistRule& other) const {
        return id == other.id && name == other.name &&
               searchPattern == other.searchPattern &&
               usePatternSearch == other.usePatternSearch &&
               isEnabled == other.isEnabled;
    }
};

// ==================== 采集黑名单规则（命中后禁止采集）====================
struct CollectorBlacklistRule {
    int id = 0;
    std::string name;
    std::string searchPattern;      // 搜索模式字符串，格式: "pos|hex,pos|hex,..." 或纯文本
    bool usePatternSearch = true;   // true=模式搜索(pos|hex), false=包含匹配
    bool isEnabled = true;
    uint64_t matchCount = 0;        // 命中次数统计

    bool operator==(const CollectorBlacklistRule& other) const {
        return id == other.id && name == other.name &&
               searchPattern == other.searchPattern &&
               usePatternSearch == other.usePatternSearch &&
               isEnabled == other.isEnabled;
    }
};

// ==================== 采集内存池 ====================
class PacketPool {
public:
    PacketPool() : m_totalCount(0) {}

    // 添加数据到池中
    void AddPacket(const HeartbeatRecord& record) {
        std::lock_guard<std::mutex> lock(m_poolMutex);
        m_dataByUsername[record.socksUsername].push_back(record);
        m_dataByGameID[record.gameID].push_back(record);
        m_totalCount++;
    }

    // 按SOCKS用户名获取并移除数据
    bool PopPacketByUsername(const std::string& username, HeartbeatRecord& outRecord, bool ascending = true) {
        std::lock_guard<std::mutex> lock(m_poolMutex);
        auto it = m_dataByUsername.find(username);
        if (it == m_dataByUsername.end() || it->second.empty()) {
            return false;
        }

        if (ascending) {
            outRecord = it->second.front();
            it->second.pop_front();
        } else {
            outRecord = it->second.back();
            it->second.pop_back();
        }

        RemoveFromGameIDIndex(outRecord);
        m_totalCount--;
        if (it->second.empty()) {
            m_dataByUsername.erase(it);
        }
        return true;
    }

    // 按游戏ID获取并移除数据
    bool PopPacketByGameID(const std::string& gameID, HeartbeatRecord& outRecord, bool ascending = true) {
        std::lock_guard<std::mutex> lock(m_poolMutex);
        auto it = m_dataByGameID.find(gameID);
        if (it == m_dataByGameID.end() || it->second.empty()) {
            return false;
        }

        if (ascending) {
            outRecord = it->second.front();
            it->second.pop_front();
        } else {
            outRecord = it->second.back();
            it->second.pop_back();
        }

        RemoveFromUsernameIndex(outRecord);
        m_totalCount--;
        if (it->second.empty()) {
            m_dataByGameID.erase(it);
        }
        return true;
    }

    // 获取某个SOCKS用户名的数据数量
    int GetCountByUsername(const std::string& username) const {
        std::lock_guard<std::mutex> lock(m_poolMutex);
        auto it = m_dataByUsername.find(username);
        if (it == m_dataByUsername.end()) return 0;
        return static_cast<int>(it->second.size());
    }

    // 获取某个游戏ID的数据数量
    int GetCountByGameID(const std::string& gameID) const {
        std::lock_guard<std::mutex> lock(m_poolMutex);
        auto it = m_dataByGameID.find(gameID);
        if (it == m_dataByGameID.end()) return 0;
        return static_cast<int>(it->second.size());
    }

    // 清空指定SOCKS用户名的所有数据
    int ClearByUsername(const std::string& username) {
        std::lock_guard<std::mutex> lock(m_poolMutex);
        auto it = m_dataByUsername.find(username);
        if (it == m_dataByUsername.end()) return 0;

        int count = static_cast<int>(it->second.size());
        for (const auto& record : it->second) {
            auto gameIt = m_dataByGameID.find(record.gameID);
            if (gameIt != m_dataByGameID.end()) {
                auto& gameQueue = gameIt->second;
                gameQueue.erase(
                    std::remove_if(gameQueue.begin(), gameQueue.end(),
                        [&](const HeartbeatRecord& r) { return r.socksUsername == username; }),
                    gameQueue.end());
                if (gameQueue.empty()) {
                    m_dataByGameID.erase(gameIt);
                }
            }
        }

        m_dataByUsername.erase(it);
        m_totalCount -= count;
        return count;
    }

    // 清空指定游戏ID的所有数据
    int ClearByGameID(const std::string& gameID) {
        std::lock_guard<std::mutex> lock(m_poolMutex);
        auto it = m_dataByGameID.find(gameID);
        if (it == m_dataByGameID.end()) return 0;

        int count = static_cast<int>(it->second.size());
        for (const auto& record : it->second) {
            auto userIt = m_dataByUsername.find(record.socksUsername);
            if (userIt != m_dataByUsername.end()) {
                auto& userQueue = userIt->second;
                userQueue.erase(
                    std::remove_if(userQueue.begin(), userQueue.end(),
                        [&](const HeartbeatRecord& r) { return r.gameID == gameID; }),
                    userQueue.end());
                if (userQueue.empty()) {
                    m_dataByUsername.erase(userIt);
                }
            }
        }

        m_dataByGameID.erase(it);
        m_totalCount -= count;
        return count;
    }

    // 清空所有数据
    void Clear() {
        std::lock_guard<std::mutex> lock(m_poolMutex);
        m_dataByUsername.clear();
        m_dataByGameID.clear();
        m_totalCount = 0;
    }

    // 获取总数据条数
    int GetTotalCount() const { return m_totalCount.load(); }

    // 获取SOCKS用户名数量
    int GetUsernameCount() const {
        std::lock_guard<std::mutex> lock(m_poolMutex);
        return static_cast<int>(m_dataByUsername.size());
    }

    // 获取游戏ID数量
    int GetGameIDCount() const {
        std::lock_guard<std::mutex> lock(m_poolMutex);
        return static_cast<int>(m_dataByGameID.size());
    }

    // 获取所有用户名列表
    std::vector<std::string> GetAllUsernames() const {
        std::lock_guard<std::mutex> lock(m_poolMutex);
        std::vector<std::string> result;
        for (const auto& pair : m_dataByUsername) {
            result.push_back(pair.first);
        }
        return result;
    }

    // 获取所有游戏ID列表
    std::vector<std::string> GetAllGameIDs() const {
        std::lock_guard<std::mutex> lock(m_poolMutex);
        std::vector<std::string> result;
        for (const auto& pair : m_dataByGameID) {
            result.push_back(pair.first);
        }
        return result;
    }

private:
    void RemoveFromGameIDIndex(const HeartbeatRecord& record) {
        auto gameIt = m_dataByGameID.find(record.gameID);
        if (gameIt != m_dataByGameID.end()) {
            auto& gameQueue = gameIt->second;
            for (auto it = gameQueue.begin(); it != gameQueue.end(); ++it) {
                if (it->socksUsername == record.socksUsername &&
                    it->gameID == record.gameID &&
                    it->timestamp == record.timestamp) {
                    gameQueue.erase(it);
                    break;
                }
            }
            if (gameQueue.empty()) {
                m_dataByGameID.erase(gameIt);
            }
        }
    }

    void RemoveFromUsernameIndex(const HeartbeatRecord& record) {
        auto userIt = m_dataByUsername.find(record.socksUsername);
        if (userIt != m_dataByUsername.end()) {
            auto& userQueue = userIt->second;
            for (auto it = userQueue.begin(); it != userQueue.end(); ++it) {
                if (it->socksUsername == record.socksUsername &&
                    it->gameID == record.gameID &&
                    it->timestamp == record.timestamp) {
                    userQueue.erase(it);
                    break;
                }
            }
            if (userQueue.empty()) {
                m_dataByUsername.erase(userIt);
            }
        }
    }

    std::map<std::string, std::deque<HeartbeatRecord>> m_dataByUsername;
    std::map<std::string, std::deque<HeartbeatRecord>> m_dataByGameID;
    mutable std::mutex m_poolMutex;
    std::atomic<int> m_totalCount;
};

} // namespace InstMgr

// ==================== 实例配置 ====================
struct InstanceConfig {
    std::string name;               // 实例名称
    InstanceType type;              // 实例类型
    int port;                       // 监听端口
    bool enableAuth;                // 是否启用认证
    bool enableWhitelist;           // 是否启用白名单
    int maxConnections;             // 最大连接数
    std::string bindToInstanceId;   // 绑定到的实例ID

    // 采集实例专用配置
    InstMgr::StorageMode storageMode;  // 存储模式
    bool allowCollect00ID;             // 是否采集00开头的GameID
    bool allowCollectOBID;             // 是否采集_OB结尾的GameID

    // 伪心跳实例专用配置
    InstMgr::PoolSource poolSource;    // 数据池来源
    InstMgr::OrderMode packetOrderMode;   // 数据包顺序模式

    // Socks转发实例专用配置
    std::string accountSourceInstanceId;  // 绑定的账号库实例ID（用于SOCKS5认证）

    InstanceConfig()
        : type(InstanceType::Collector)
        , port(0)
        , enableAuth(false)
        , enableWhitelist(false)
        , maxConnections(1000)
        , storageMode(InstMgr::StorageMode::Hourly)
        , allowCollect00ID(false)
        , allowCollectOBID(false)
        , poolSource(InstMgr::PoolSource::Database)
        , packetOrderMode(InstMgr::OrderMode::Ascending)
    {}
};

// ==================== 实例信息 ====================
struct InstanceInfo {
    std::string id;                 // 实例ID（唯一标识）
    std::string name;               // 实例名称
    InstanceType type;              // 实例类型
    InstanceState state;            // 实例状态
    int port;                       // 监听端口
    int currentConnections;         // 当前连接数
    uint64_t totalPackets;          // 总数据包数
    uint64_t totalBytes;            // 总字节数
    std::string bindToInstanceId;   // 绑定到的实例ID
    std::string createTime;         // 创建时间
    std::string lastError;          // 最后错误信息

    // 采集实例专用信息
    InstMgr::StorageMode storageMode;  // 存储模式
    int memoryPoolCount;               // 内存池数据条数

    // 伪心跳实例专用信息
    InstMgr::PoolSource poolSource;    // 数据池来源

    InstanceInfo()
        : type(InstanceType::Collector)
        , state(InstanceState::Stopped)
        , port(0)
        , currentConnections(0)
        , totalPackets(0)
        , totalBytes(0)
        , storageMode(InstMgr::StorageMode::Hourly)
        , memoryPoolCount(0)
        , poolSource(InstMgr::PoolSource::Database)
    {}
};

// ==================== 单伪采集实例类 ====================
class CollectorInstance {
public:
    CollectorInstance(const std::string& id, const InstanceConfig& config);
    ~CollectorInstance();

    bool Start();
    void Stop();
    bool IsRunning() const;

    InstanceInfo GetInfo() const;
    std::string GetId() const { return m_id; }
    std::string GetName() const { return m_config.name; }
    int GetPort() const { return m_config.port; }
    void SetPort(int port);
    void BindToHeartbeat(const std::string& heartbeatId);
    void UnbindHeartbeat();

    // 设置数据包回调（用于转发给绑定的伪心跳实例）
    void SetPacketCallback(PacketReceivedCallback callback);

    // 获取底层PacketCollector（用于高级操作）
    PacketCollector* GetCollector() { return m_collector.get(); }

    // 存储模式相关
    void SetStorageMode(InstMgr::StorageMode mode) { m_config.storageMode = mode; }
    InstMgr::StorageMode GetStorageMode() const { return m_config.storageMode; }

    // 特殊ID过滤配置
    void SetAllowCollect00ID(bool allow) { m_config.allowCollect00ID = allow; }
    bool GetAllowCollect00ID() const { return m_config.allowCollect00ID; }
    void SetAllowCollectOBID(bool allow) { m_config.allowCollectOBID = allow; }
    bool GetAllowCollectOBID() const { return m_config.allowCollectOBID; }

    // 包头类型/62特征采集（对齐独立项目关键选项）
    void SetDisablePacketHeaderFilter(bool disable);
    bool GetDisablePacketHeaderFilter() const;

    void SetEnableCollect62Pattern(bool enable);
    bool GetEnableCollect62Pattern() const;

    void SetCollectorPacketTypes(const std::vector<int>& types);
    std::vector<int> GetCollectorPacketTypes() const;

    void SetCollectorPacketTypeEnabled(int type, bool enabled);
    bool GetCollectorPacketTypeEnabled(int type) const;
    std::map<int, bool> GetCollectorPacketTypeEnabledMap() const;

    void SetDisconnectAutoClear(bool enable);
    bool GetDisconnectAutoClear() const;

    // 内存池相关
    DatabaseManager* GetDatabase() const { return m_database.get(); }

    // 采集白名单相关（命中后允许采集，启用后只采集匹配的数据）
    void SetEnableCollectorWhitelist(bool enable) { m_enableCollectorWhitelist = enable; }
    bool GetEnableCollectorWhitelist() const { return m_enableCollectorWhitelist; }
    void AddCollectorWhitelistRule(const InstMgr::CollectorWhitelistRule& rule);
    void UpdateCollectorWhitelistRule(int id, const InstMgr::CollectorWhitelistRule& rule);
    void RemoveCollectorWhitelistRule(int id);
    void ClearCollectorWhitelistRules();
    std::vector<InstMgr::CollectorWhitelistRule> GetCollectorWhitelistRules() const;
    int GetNextCollectorWhitelistRuleId();
    bool CheckCollectorWhitelist(const std::vector<uint8_t>& data, int* matchedIndex = nullptr, std::string* matchedRuleName = nullptr);

    // 采集黑名单相关（命中后禁止采集）
    void SetEnableCollectorBlacklist(bool enable) { m_enableCollectorBlacklist = enable; }
    bool GetEnableCollectorBlacklist() const { return m_enableCollectorBlacklist; }
    void AddCollectorBlacklistRule(const InstMgr::CollectorBlacklistRule& rule);
    void UpdateCollectorBlacklistRule(int id, const InstMgr::CollectorBlacklistRule& rule);
    void RemoveCollectorBlacklistRule(int id);
    void ClearCollectorBlacklistRules();
    std::vector<InstMgr::CollectorBlacklistRule> GetCollectorBlacklistRules() const;
    int GetNextCollectorBlacklistRuleId();
    bool CheckCollectorBlacklist(const std::vector<uint8_t>& data, int* matchedIndex = nullptr, std::string* matchedRuleName = nullptr);

private:
    std::string m_id;
    InstanceConfig m_config;
    std::unique_ptr<PacketCollector> m_collector;
    InstanceState m_state;
    std::string m_createTime;
    std::string m_lastError;
    std::string m_boundHeartbeatId;
    mutable std::mutex m_mutex;

    // 内存池（内存存储模式使用）
    std::unique_ptr<DatabaseManager> m_database;

    // ===== 单伪采集：与独立项目一致的关键配置（每实例持久化到 config/single_instances/collector_<id>.db）=====
    bool m_disablePacketHeaderFilter = false;
    bool m_enableCollect62Pattern = false;
    bool m_enableDisconnectAutoClear = true;
    std::vector<int> m_collectorPacketTypes{ 1, 2, 3, 4 };
    std::map<int, bool> m_collectorPacketTypeEnabled;

    // 采集白名单（命中后允许采集）
    bool m_enableCollectorWhitelist = false;
    std::vector<InstMgr::CollectorWhitelistRule> m_collectorWhitelistRules;
    mutable std::mutex m_collectorWhitelistMutex;
    int m_nextCollectorWhitelistRuleId = 1;

    // 采集黑名单（命中后禁止采集）
    bool m_enableCollectorBlacklist = false;
    std::vector<InstMgr::CollectorBlacklistRule> m_collectorBlacklistRules;
    mutable std::mutex m_collectorBlacklistMutex;
    int m_nextCollectorBlacklistRuleId = 1;
};

// ==================== 单伪伪心跳实例类 ====================
class HeartbeatInstance {
public:
    HeartbeatInstance(const std::string& id, const InstanceConfig& config);
    ~HeartbeatInstance();

    bool Start();
    void Stop();
    bool IsRunning() const;

    InstanceInfo GetInfo() const;
    std::string GetId() const { return m_id; }
    std::string GetName() const { return m_config.name; }
    int GetPort() const { return m_config.port; }
    void SetPort(int port);

    // 处理来自采集实例的数据包（绑定模式）
    void ProcessPacket(const PacketInfo& info, const std::vector<uint8_t>& data);

    // 绑定到采集实例
    void BindToCollector(const std::string& collectorId);
    std::string GetBoundCollectorId() const { return m_boundCollectorId; }

    void SetBoundCollectorDb(DatabaseManager* db) { m_boundCollectorDb.store(db); }
    std::atomic<DatabaseManager*>* GetBoundCollectorDbPtr() { return &m_boundCollectorDb; }

    // 获取HeartbeatCore（用于配置）
    HeartbeatCore* GetCore() { return m_core.get(); }

    // 获取底层PacketCollector（用于独立监听模式）
    PacketCollector* GetCollector() { return m_collector.get(); }

    // 数据池来源相关
    void SetPoolSource(InstMgr::PoolSource source) { m_config.poolSource = source; }
    InstMgr::PoolSource GetPoolSource() const { return m_config.poolSource; }

    // 数据包顺序模式
    void SetPacketOrderMode(InstMgr::OrderMode mode) { m_config.packetOrderMode = mode; }
    InstMgr::OrderMode GetPacketOrderMode() const { return m_config.packetOrderMode; }

    // 设置绑定的采集实例内存池指针
    // (已弃用) 旧版绑定内存池接口已移除：单伪多实例改为绑定采集端 DatabaseManager

    // 获取统计信息
    uint64_t GetTotalProcessed() const;
    uint64_t GetTotalReplaced() const;

    // 配置持久化
    bool SaveConfig();
    bool LoadConfig();

private:
    std::string m_id;
    InstanceConfig m_config;
    InstanceState m_state;
    std::string m_createTime;
    std::string m_lastError;
    std::string m_boundCollectorId;
    mutable std::mutex m_mutex;

    // 伪心跳核心逻辑
    std::unique_ptr<HeartbeatCore> m_core;
    std::atomic<DatabaseManager*> m_boundCollectorDb{nullptr};

    // 独立监听模式的PacketCollector
    std::unique_ptr<PacketCollector> m_collector;

    // 绑定的采集实例内存池指针（BoundCollector模式使用）
    // (已弃用) 旧版绑定内存池成员已移除
};

// ==================== ab采集实例类（多实例，复用独立运行算法） ====================
class AbCollectorInstance {
public:
    AbCollectorInstance(const std::string& id, const InstanceConfig& config);
    ~AbCollectorInstance();

    bool Start();
    void Stop();
    bool IsRunning() const;

    InstanceInfo GetInfo() const;
    std::string GetId() const { return m_id; }
    std::string GetName() const { return m_config.name; }
    int GetPort() const { return m_config.port; }
    void SetPort(int port);

    void BindToHeartbeat(const std::string& heartbeatId);
    void UnbindHeartbeat();

    PacketCollector* GetCollector() { return m_collector.get(); }
    CollectedPacketPool* GetPool() { return &m_pool; }

private:
    std::string m_id;
    InstanceConfig m_config;
    InstanceState m_state;
    std::string m_createTime;
    std::string m_lastError;
    std::string m_boundHeartbeatId;
    mutable std::mutex m_mutex;

    std::unique_ptr<PacketCollector> m_collector;
    CollectedPacketPool m_pool; // 每个ab采集实例独立内存池
};

// ==================== ab伪心跳实例类（多实例，复用独立运行算法） ====================
class AbHeartbeatInstance {
public:
    AbHeartbeatInstance(const std::string& id, const InstanceConfig& config);
    ~AbHeartbeatInstance();

    bool Start();
    void Stop();
    bool IsRunning() const;

    InstanceInfo GetInfo() const;
    std::string GetId() const { return m_id; }
    std::string GetName() const { return m_config.name; }
    int GetPort() const { return m_config.port; }
    void SetPort(int port);

    void BindToCollector(const std::string& collectorId);
    std::string GetBoundCollectorId() const { return m_boundCollectorId; }

    void SetBoundCollectorPool(CollectedPacketPool* pool) { m_boundCollectorPool.store(pool); }
    CollectedPacketPool* GetBoundCollectorPool() const { return m_boundCollectorPool.load(); }

    PacketCollector* GetForwarder() { return m_forwarder.get(); }

private:
    std::string m_id;
    InstanceConfig m_config;
    InstanceState m_state;
    std::string m_createTime;
    std::string m_lastError;
    std::string m_boundCollectorId;
    mutable std::mutex m_mutex;

    std::unique_ptr<PacketCollector> m_forwarder;
    std::atomic<CollectedPacketPool*> m_boundCollectorPool{nullptr}; // 绑定到某个ab采集实例的池（可为空）
};

// ==================== SOCKS5账号库实例类 ====================
class Socks5PoolInstance {
public:
    enum class OnlineDevicePolicy {
        Unlimited = 0,
        SingleDeviceSingleInstance = 1
    };

    Socks5PoolInstance(const std::string& id, const InstanceConfig& config);
    ~Socks5PoolInstance();

    InstanceInfo GetInfo() const;
    std::string GetId() const { return m_id; }
    std::string GetName() const { return m_config.name; }

    // 账号管理
    bool AddAccount(const std::string& username, const std::string& password,
        const std::string& expireTime, int maxConnections);
    bool RemoveAccount(const std::string& username);
    bool UpdateAccount(const std::string& username, const std::string& password,
        const std::string& expireTime, int maxConnections, bool isEnabled);
    bool AccountExists(const std::string& username) const;
    Socks5Account GetAccount(const std::string& username) const;
    std::vector<Socks5Account> GetAllAccounts() const;
    int GetAccountCount() const;
    OnlineDevicePolicy GetOnlineDevicePolicy() const;
    void SetOnlineDevicePolicy(OnlineDevicePolicy policy);

    // 账号验证（供绑定的实例调用）
    bool ValidateAccount(const std::string& username, const std::string& password) const;

    // 账号验证（返回失败原因）
    bool ValidateAccountWithReason(const std::string& username, const std::string& password, std::string& failReason) const;

    // 持久化
    void SaveToDatabase();
    void LoadFromDatabase();

    // ===== CCProxy API兼容服务 =====
    bool StartApiServer(int port, const std::string& username, const std::string& password);
    void StopApiServer(bool updateEnabledState = true);  // updateEnabledState: 是否更新启用状态（析构时传false）
    bool IsApiServerRunning() const;
    int GetApiServerPort() const { return m_apiPort; }
    std::string GetApiUsername() const { return m_apiUsername; }
    std::string GetApiPassword() const { return m_apiPassword; }
    bool IsApiEnabled() const { return m_apiEnabled; }  // 获取API启用状态
    void AutoStartApiIfEnabled();  // 如果启用状态为true则自动启动API
    void SetApiConfig(int port, const std::string& username, const std::string& password);

    // 获取内部的PacketCollector（用于API服务）
    PacketCollector* GetInternalCollector() { return m_internalCollector.get(); }

    // ===== 账号变更自动同步：订阅机制 =====
    // 当实例绑定此账号库时，注册其 PacketCollector 指针；账号变更时自动同步
    void RegisterBoundCollector(PacketCollector* collector);
    void UnregisterBoundCollector(PacketCollector* collector);

private:
    // 将当前账号全量同步到指定的 PacketCollector
    void SyncAccountsToCollector(PacketCollector* collector);
    // 将当前账号同步到所有已注册的 PacketCollector
    void NotifyBoundCollectors();

    // 从内部PacketCollector同步账号到m_accounts（API变更时调用）
    void SyncAccountsFromInternalCollector();
    std::string m_id;
    InstanceConfig m_config;
    std::string m_createTime;
    mutable std::mutex m_mutex;

    std::vector<Socks5Account> m_accounts;
    int m_nextAccountId = 1;
    // 注：配置现在统一存储到config.db，使用实例ID前缀

    // ===== 已绑定的 PacketCollector 列表（账号变更时自动同步）=====
    std::vector<PacketCollector*> m_boundCollectors;

    // CCProxy API服务相关
    std::unique_ptr<PacketCollector> m_internalCollector;  // 内部PacketCollector用于API服务
    HttpApiServer* m_apiServer = nullptr;
    int m_apiPort = 90;
    std::string m_apiUsername = "admin";
    std::string m_apiPassword = "admin";
    bool m_apiEnabled = false;  // API服务是否启用（用于持久化）
    OnlineDevicePolicy m_onlineDevicePolicy = OnlineDevicePolicy::SingleDeviceSingleInstance;
};

// ==================== Socks转发实例类（纯转发，不记录数据包，支持WPE滤镜和二级代理） ====================
class SocksForwardInstance {
public:
    SocksForwardInstance(const std::string& id, const InstanceConfig& config);
    ~SocksForwardInstance();

    bool Start();
    void Stop();
    bool IsRunning() const;

    InstanceInfo GetInfo() const;
    std::string GetId() const { return m_id; }
    std::string GetName() const { return m_config.name; }
    int GetPort() const { return m_config.port; }
    void SetPort(int port);

    // 获取底层PacketCollector（用于高级操作）
    PacketCollector* GetCollector() { return m_collector.get(); }

    // 🔥 配置缓存结构（从数据库加载，供UI读取和修改）
    struct CachedConfig {
        struct CachedAntiCCConfig {
            bool enabled = false;
            int timeWindowSeconds = 10;
            int maxRequestsInWindow = 20;
            int banTimeSeconds = 300;
            int maxConnections = 100;
            int authFailBanTime = 60;
            int noAuthBanTime = 30;
            int whitelistDuration = 3600;
            bool useBlacklist = true;
            bool useWhitelist = true;
            bool useFirewall = false;
            bool rateLimitEnabled = false;
            int rateLimit = 100;
            int rateTimeWindow = 1;
            bool blockNonSocks = false;
            bool enableAuthPriorityAdmission = true;
            int authPriorityQueueLimit = 128;
            bool enableLowPriorityEviction = true;
            int lowPriorityEvictionThreshold = 80;
            bool enableCoordinator = true;
        };

        // 线程模型配置
        int threadPoolMode = 0;
        int whitelistPoolSize = 10;
        int normalPoolSize = 50;
        int iocpMaxWhitelist = 2000;
        int iocpMaxNormal = 500;

        // SOCKS5认证配置
        bool enableSocks5Auth = false;
        std::string accountSourceInstanceId;
        int instanceDeviceLimit = 0;

        // 二级代理配置
        bool enableSecondaryProxy = false;
        std::string secondaryProxyHost = "127.0.0.1";
        int secondaryProxyPort = 1080;
        std::string secondaryProxyUsername;
        std::string secondaryProxyPassword;

        // 分包处理配置
        bool enablePacketSplit = true;
        std::string packetSplitPorts;
        bool applyWpeOnNonSplitTraffic = false;

        // 流量过滤配置
        bool enableTrafficFilter = false;
        bool enableSniSniffing = false;
        std::string trafficFilterRules;
        bool enableSSLMitm = false;
        std::string sslMitmRules = "[]";
        bool enableHttpLocalMap = false;
        std::string httpLocalMapRules = "[]";

        // 断网规则配置
        std::string disconnectRulesJson = "[]";

        // 用户滤镜配置
        bool enableUserFilterMode = false;
        int userFilterHttpPort = 8080;

        // 防CC配置
        CachedAntiCCConfig antiCC;
    };

    // 🔥 获取缓存的配置（供UI读取）
    CachedConfig GetCachedConfig() const;

    // 🔥 更新配置（供UI修改，会同步到数据库，如果实例运行中会立即应用）
    void UpdateConfig(const CachedConfig& newConfig);
    void ClearAccountSourceBinding();

    // 根据实例配置同步用户滤镜 HTTP 服务状态（独立于实例运行态）
    void SyncUserFilterHttpServerState(bool forceRestart = false);

private:
    void LoadConfigFromDatabase();  // 🔥 从数据库加载配置到 m_cachedConfig
    void ApplyConfigToCollector();  // 🔥 将 m_cachedConfig 应用到 PacketCollector
    void SaveConfigToDatabase();    // 🔥 将 m_cachedConfig 保存到数据库

    std::string m_id;
    InstanceConfig m_config;
    std::unique_ptr<PacketCollector> m_collector;
    InstanceState m_state;
    std::string m_createTime;
    std::string m_lastError;
    mutable std::mutex m_mutex;

    // 🔥 缓存的配置（从数据库加载，避免重复读取）
    CachedConfig m_cachedConfig;
};

// ==================== 绑定信息结构 ====================
struct BindingInfo {
    std::string collectorId;
    std::string heartbeatId;
    InstanceType collectorType;   // Collector 或 AbCollector
    InstanceType heartbeatType;   // Heartbeat 或 AbHeartbeat
    bool collectorRunning = false;
    bool heartbeatRunning = false;
};

// ==================== 实例管理器类 ====================
class InstanceManager {
public:
    static InstanceManager& GetInstance();

    // 从数据库加载实例（必须在g_database初始化后调用）
    void LoadFromDatabase();

    // 内置"ab采集 / ab伪心跳"实例ID（对应 新伪心跳.cpp 的独立运行逻辑）
    static constexpr const char* kAbCollectorInstanceId = "ab_collector";
    static constexpr const char* kAbHeartbeatInstanceId = "ab_heartbeat";

    // 创建实例
    std::string CreateCollectorInstance(const InstanceConfig& config);
    std::string CreateHeartbeatInstance(const InstanceConfig& config);
    std::string CreateAbCollectorInstance(const InstanceConfig& config);
    std::string CreateAbHeartbeatInstance(const InstanceConfig& config);
    std::string CreateSocks5PoolInstance(const InstanceConfig& config);
    std::string CreateSocksForwardInstance(const InstanceConfig& config);

    // 删除实例
    bool DeleteInstance(const std::string& instanceId);

    // 启动/停止实例
    bool StartInstance(const std::string& instanceId);
    bool StopInstance(const std::string& instanceId);

    // 绑定实例
    bool BindInstances(const std::string& collectorId, const std::string& heartbeatId);
    bool UnbindInstance(const std::string& instanceId);
    std::vector<BindingInfo> GetAllBindings();

    // 获取实例信息
    InstanceInfo GetInstanceInfo(const std::string& instanceId);
    std::vector<InstanceInfo> GetAllInstances();
    std::vector<InstanceInfo> GetInstancesByType(InstanceType type);

    // 查找实例
    CollectorInstance* GetCollectorInstance(const std::string& instanceId);
    HeartbeatInstance* GetHeartbeatInstance(const std::string& instanceId);
    AbCollectorInstance* GetAbCollectorInstance(const std::string& instanceId);
    AbHeartbeatInstance* GetAbHeartbeatInstance(const std::string& instanceId);
    Socks5PoolInstance* GetSocks5PoolInstance(const std::string& instanceId);
    SocksForwardInstance* GetSocksForwardInstance(const std::string& instanceId);

    // 获取所有实例（返回指针列表）
    std::vector<CollectorInstance*> GetCollectorInstances();
    std::vector<HeartbeatInstance*> GetHeartbeatInstances();
    std::vector<Socks5PoolInstance*> GetSocks5PoolInstances();
    std::vector<SocksForwardInstance*> GetSocksForwardInstances();

    // 统计信息
    int GetTotalInstances() const;
    int GetRunningInstances() const;

    // 辅助函数：生成带实例ID前缀的配置键
    static std::string MakeInstanceConfigKey(const std::string& instanceId, const std::string& key);

private:
    InstanceManager();
    ~InstanceManager();
    InstanceManager(const InstanceManager&) = delete;
    InstanceManager& operator=(const InstanceManager&) = delete;

    std::string GenerateInstanceId();
    std::string GetCurrentTimeString();
    void SaveInstanceList();  // 保存实例列表到config.db
    void CleanupInstanceConfig(const std::string& instanceId);  // 清理实例配置数据
    void RestoreSocksForwardAccountBindings();  // 恢复 Socks 转发实例的账号库绑定
    void RestoreSocksForwardUserFilterHttpServers();  // 恢复 Socks 转发实例的用户滤镜 HTTP 服务
    void AutoStartMarkedInstances();  // 🔥 自动启动标记为自动启动的实例

    std::map<std::string, std::unique_ptr<CollectorInstance>> m_collectors;
    std::map<std::string, std::unique_ptr<HeartbeatInstance>> m_heartbeats;
    std::map<std::string, std::unique_ptr<AbCollectorInstance>> m_abCollectors;
    std::map<std::string, std::unique_ptr<AbHeartbeatInstance>> m_abHeartbeats;
    std::map<std::string, std::unique_ptr<Socks5PoolInstance>> m_socks5Pools;
    std::map<std::string, std::unique_ptr<SocksForwardInstance>> m_socksForwards;
    mutable std::mutex m_mutex;
    int m_nextInstanceId;
    bool m_loaded;
    std::vector<std::string> m_autoStartInstances;  // 需要自动启动的实例ID列表
};
