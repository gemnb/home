#pragma once
#include "PacketParser.h"
#include "AntiCC.h"
#include "DatabaseManager.h"
#include "WhitelistThreadPool.h"
#include "IOCPThreadPool.h"
#include "DisconnectRuleTypes.h"
#include "UserFilterWebServer.h"
#include "SSLMitmContext.h"
#include "CollectedPacketPool.h"
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <functional>
#include <queue>
#include <condition_variable>
#include <unordered_map>
#include <unordered_set>
#include <list>

#pragma comment(lib, "ws2_32.lib")

class PacketCollector;

enum FilterType {
    FILTER_NONE = 0,
    FILTER_IP = 1,
    FILTER_DOMAIN = 2,
    FILTER_PORT = 3
};

struct FilterConfig {
    FilterType type;
    std::string value;
    FilterConfig() : type(FILTER_NONE) {}
};

// ===== 🔥 新增：流量嗅探过滤规则 =====
enum class TrafficRuleType {
    PORT_MATCH = 0,           // 端口匹配
    DOMAIN_MATCH = 1,         // 域名匹配
    IP_MATCH = 2,             // IP匹配
    PORT_DOMAIN_SNI = 3       // 指定端口嗅探指定域名
};

struct TrafficFilterRule {
    int id;                           // 规则ID
    TrafficRuleType type;             // 规则类型
    std::string value1;               // 值1（端口/域名/IP）
    std::string value2;               // 值2（用于PORT_DOMAIN_SNI的域名）
    bool enabled;                     // 是否启用
    std::string description;          // 规则描述

    TrafficFilterRule() : id(0), type(TrafficRuleType::PORT_MATCH), enabled(true) {}
};

struct TrafficFilterConfig {
    bool enabled;                                 // 是否启用流量过滤
    std::vector<TrafficFilterRule> rules;         // 过滤规则列表
    bool sniSniffingEnabled;                      // 是否启用SNI嗅探

    TrafficFilterConfig() : enabled(false), sniSniffingEnabled(false) {}
};

// ===== 🔥 新增：SOCKS5账号信息结构 =====
struct Socks5Account {
    int id;
    std::string username;
    std::string password;
    std::string expireTime;      // 到期时间 (YYYY-MM-DD HH:MM:SS)
    int maxConnections;          // 最大连接数
    int currentConnections;      // 当前连接数
    bool isEnabled;              // 是否启用
    std::string createdAt;       // 创建时间
    std::string lastLoginTime;   // 最后登录时间
    std::string lastLoginIP;     // 最后登录IP

    // ===== 在线统计字段 =====
    std::chrono::system_clock::time_point firstConnectTime;  // 首次连接时间
    std::chrono::system_clock::time_point lastDisconnectTime; // 最后断开时间
    uint64_t totalOnlineSeconds;  // 累计在线时长（秒）

    // ===== 游戏会话ID绑定 =====
    std::string currentGameID;   // 当前绑定的游戏会话ID（非00开头）

    // ===== 🔥 新增：多实例支持 =====
    std::string instanceId;      // 所属实例ID（空字符串表示全局/默认实例）
    int remotePort;              // 独立远程端口（0表示使用默认端口）
    std::string remoteHost;      // 独立远程地址（空表示使用默认地址）

    Socks5Account() : id(0), maxConnections(0), currentConnections(0),
        isEnabled(true), totalOnlineSeconds(0), remotePort(0) {}
};

struct SharedAccountLeaseInfo {
    std::string ownerInstanceId;
    std::string ownerDeviceKey;
    int refCount = 0;
};

struct PacketTransformResult {
    std::vector<uint8_t> forwardedData;   // 实际用于转发的数据（为空表示丢弃）
    std::vector<uint8_t> callbackData;    // 用于回调侧（UI/采集）的数据（为空则默认等同 forwardedData）
    bool intercepted;                      // 是否拦截（不转发）

    PacketTransformResult() : intercepted(false) {}
};

using PacketModifierCallback = std::function<PacketTransformResult(const PacketInfo&, const std::vector<uint8_t>&)>;
using PacketReceivedCallback = std::function<void(const PacketInfo&, const std::vector<uint8_t>& forwardedData, const std::vector<uint8_t>& callbackData)>;

// 响应包修改回调（服务器→客户端）
using ResponseModifierCallback = std::function<PacketTransformResult(const PacketInfo&, const std::vector<uint8_t>&)>;

// GameID清除原因
enum class GameIDClearReason {
    GAMEID_CHANGED,  // GameID变化时清理旧的
    DISCONNECT       // 用户断开连接时清理
};
using GameIDClearedCallback = std::function<void(const std::string& gameID, GameIDClearReason reason)>;
using UserDisconnectedCallback = std::function<void(const std::string& username, const std::string& gameID)>;

// 🔥 修复2: 每个连接独立缓冲区和锁
struct ConnectionBuffer {
    std::string gameID;
    std::vector<uint8_t> buffer;
    bool gameIDExtracted;
    uint32_t expectedLength;
    int fragmentCount;
    int packetsInBuffer;
    std::mutex bufferMutex;  // 🔥 修复2: 每个连接独立的锁，减少锁竞争

    ConnectionBuffer() : gameIDExtracted(false), expectedLength(0),
        fragmentCount(0), packetsInBuffer(0) {}
};

struct ProxyConnection {
    uint64_t connectionId;
    SOCKET clientSocket;
    SOCKET serverSocket;
    std::string clientAddr;
    std::string clientIP;
    std::string targetAddr;
    int targetPort;
    std::string gameID;
    std::atomic<bool> isActive;
    std::thread clientToServerThread;
    std::thread serverToClientThread;

    bool useSecondaryProxy;
    std::string secondaryProxyHost;
    int secondaryProxyPort;

    bool bypassModifier;

    // ===== 🔥 新增：SOCKS5认证用户名 =====
    std::string authenticatedUser;
    PacketCollector* accountStateOwner;  // 认证时实际占用账号状态的源Collector

    // ===== 🔥 双连接池：标记是否为白名单连接 =====
    bool isWhitelistConnection = false;

    // ===== 断网规则运行时状态 =====
    DisconnectRuntimeState disconnectState;

    // ===== SSL MITM 扩展 =====
    bool sslMitmEnabled = false;
    std::shared_ptr<SSLMitmContext> sslCtx;
    std::string sniHostname;

    // ===== HTTP 本地映射（类似 Charles Map Local）=====
    std::vector<uint8_t> httpLocalMapBuffer;
    bool httpLocalMapBypass = false;

    ProxyConnection() : connectionId(0),
        clientSocket(INVALID_SOCKET),
        serverSocket(INVALID_SOCKET),
        targetPort(0),
        isActive(false),
        useSecondaryProxy(false),
        secondaryProxyPort(0),
        bypassModifier(false),
        accountStateOwner(nullptr),
        sslMitmEnabled(false) {}
};

struct CallbackTask {
    PacketInfo packetInfo;
    std::vector<uint8_t> forwardedData;
    std::vector<uint8_t> callbackData;
};


struct DomainAccessRecord {
    std::string username;          // 访问用户
    std::string domain;            // 访问的域名
    std::chrono::system_clock::time_point accessTime;  // 访问时间

    DomainAccessRecord() {}
    DomainAccessRecord(const std::string& user, const std::string& dom)
        : username(user), domain(dom), accessTime(std::chrono::system_clock::now()) {}
};

// ===== 🔥 代理数据包记录（用于UI查看） =====
struct ProxyPacketRecord {
    uint64_t sequence;
    uint64_t connectionId;         // 连接ID（用于 Charles 风格会话聚合）
    std::string timestamp;         // 时间戳
    std::string username;          // SOCKS用户名
    std::string gameID;            // GameID
    std::string clientIP;          // 客户端IP
    std::string targetHost;        // 目标主机
    std::string sniHost;           // SNI/解密主机
    int targetPort;                // 目标端口
    bool sslMitmEnabled;           // 是否启用 SSL MITM
    bool isRequest;                // true=请求(客户端→服务器), false=响应(服务器→客户端)
    uint32_t dataLength;           // 数据长度
    std::string dataPreview;       // 数据预览（前64字节的hex）
    std::vector<uint8_t> fullData; // 完整数据包

    ProxyPacketRecord()
        : sequence(0), connectionId(0), targetPort(0), sslMitmEnabled(false),
          isRequest(true), dataLength(0) {}
};

struct ProxyPacketRecordSummary {
    uint64_t sequence;
    uint64_t connectionId;
    std::string timestamp;
    std::string username;
    std::string gameID;
    std::string clientIP;
    std::string targetHost;
    std::string sniHost;
    int targetPort;
    bool sslMitmEnabled;
    bool isRequest;
    uint32_t dataLength;
    std::string dataPreview;

    ProxyPacketRecordSummary()
        : sequence(0), connectionId(0), targetPort(0), sslMitmEnabled(false),
          isRequest(true), dataLength(0) {}
};

struct HttpLocalMapRule {
    int id = 0;
    bool enabled = true;
    std::string scheme = "*";      // http / https / *
    std::string hostPattern = "*"; // example.com / *.example.com / *
    std::string pathPattern = "/"; // 路径前缀或 *
    std::string method = "*";      // GET / HEAD / *
    std::string localFilePath;     // 本地文件绝对路径
    std::string contentType;       // 留空自动推断
};

class PacketCollector {
private:
    struct SharedAccountLease {
        std::string ownerInstanceId;
        std::string ownerDeviceKey;
        int refCount = 0;
        std::chrono::system_clock::time_point firstAcquireTime;
        std::chrono::system_clock::time_point lastActiveTime;
    };

    int listenPort;
    SOCKET listenSocket;
    std::thread serverThread;
    std::atomic<bool> isRunning;

    // 实例ID（用于WPE滤镜判断）
    std::string m_instanceId;

    // 启动失败/运行错误信息（用于实例管理UI显示更明确的失败原因）
    mutable std::mutex lastErrorMutex;
    std::string lastErrorMessage;
    void SetLastErrorMessage_(const std::string& msg) {
        std::lock_guard<std::mutex> lock(lastErrorMutex);
        lastErrorMessage = msg;
    }

    std::vector<std::shared_ptr<ProxyConnection>> proxyConnections;
    mutable std::mutex connectionsMutex;

    std::map<SOCKET, ConnectionBuffer> buffers;
    std::mutex bufferMapMutex;  // 🔥 修复2: 只保护map本身的增删操作，不保护buffer内容

    PacketReceivedCallback onPacketReceived;
    PacketModifierCallback onPacketModifier;
    ResponseModifierCallback onResponseModifier;  // 响应包修改回调（服务器→客户端）
    GameIDClearedCallback onGameIDCleared;  // 🔥 新增：GameID清除回调
    UserDisconnectedCallback onUserDisconnected; // 断开连接回调（提供username/gameID）

    std::atomic<bool> enableDisconnectClear;  // 🔥 断开时是否清理GameID绑定

    std::atomic<int> totalPackets;
    std::atomic<int> totalConnections;
    std::atomic<uint64_t> totalBytes;
    std::atomic<int> fragmentedPackets;
    std::atomic<int> multiPackets;
    std::atomic<int> filteredConnections;

    FilterConfig filterConfig;
    mutable std::mutex filterMutex;

    // ===== 🔥 修复3: 多消费者回调线程池 =====
    std::vector<std::thread> callbackThreads;  // 多个回调处理线程
    int callbackThreadCount = 4;  // 默认4个线程
    std::queue<CallbackTask> callbackQueue;
    std::mutex callbackQueueMutex;
    std::condition_variable callbackQueueCV;
    std::atomic<bool> callbackThreadRunning{false};
    std::atomic<size_t> callbackQueueSize{0};  // 🔥 修复3: 队列大小统计

    // ===== 🔥 修复1: 异步数据库写入队列（避免主线程阻塞）=====
    struct DatabaseWriteTask {
        HeartbeatRecord record;
        std::chrono::steady_clock::time_point enqueueTime;
    };
    std::queue<DatabaseWriteTask> dbWriteQueue;
    std::mutex dbWriteMutex;
    std::condition_variable dbWriteCV;
    std::thread dbWriteThread;
    std::atomic<bool> dbWriteThreadRunning{false};
    std::atomic<size_t> dbQueueSize{0};  // 🔥 修复1: 数据库队列大小统计

    bool enableSecondaryProxy;
    std::string secondaryProxyHost;
    int secondaryProxyPort;
    std::string secondaryProxyUsername;  // 二级代理用户名
    std::string secondaryProxyPassword;  // 二级代理密码
    std::mutex secondaryProxyMutex;

    // ===== 🔥 SOCKS5认证相关 =====
    bool enableSocks5Auth;
    std::map<std::string, Socks5Account> accounts;
    mutable std::mutex accountsMutex;
    std::atomic<int> nextAccountId;
    int instanceDeviceLimit;
    std::unordered_map<std::string, int> activeDeviceIpRefCounts;
    mutable std::mutex deviceLimitMutex;
    std::atomic<bool> sharedAccountSingleDeviceModeEnabled{true};
    std::unordered_map<std::string, SharedAccountLease> sharedAccountLeases;
    mutable std::mutex sharedAccountLeasesMutex;

    // ===== 外部账号源（用于采集端共享伪心跳端账号）=====
    PacketCollector* externalAccountSource;
    std::vector<PacketCollector*> externalAccountConsumers;
    mutable std::mutex externalAccountConsumersMutex;

    int CountLocalAuthenticatedConnectionsForUser(const std::string& username) const;
    int CountAggregatedAuthenticatedConnectionsForUser(const std::string& username) const;
    void RegisterExternalAccountConsumer(PacketCollector* consumer);
    void UnregisterExternalAccountConsumer(PacketCollector* consumer);
    std::vector<PacketCollector*> SnapshotSharedAccountSessionCollectors() const;
    void DisconnectAuthenticatedUserAcrossSharedCollectors(const std::string& username, const std::string& reason);

    void ServerLoop();
    void HandleClient(SOCKET clientSocket, const std::string& clientAddr);
    bool HandleSocks5Handshake(SOCKET clientSocket, std::string& targetHost,
        int& targetPort, std::string& username, PacketCollector*& accountStateOwner,
        const std::string& clientIP);
    SOCKET ConnectToTarget(const std::string& host, int port);
    void ForwardClientToServer(std::shared_ptr<ProxyConnection> conn);
    void ForwardServerToClient(std::shared_ptr<ProxyConnection> conn);

    std::vector<std::vector<uint8_t>> ProcessBuffer(
        SOCKET socket,
        const std::string& gameID,
        const std::string& socksUsername,  // 🔥 新增：SOCKS账号用户名
        const std::vector<uint8_t>& newData,
        bool bypassModifier = false
    );

    bool ShouldFilterConnection(const std::string& host, int port);

    void CallbackThreadLoop(int threadId);  // 🔥 修复3: 添加线程ID参数
    void EnqueueCallback(const CallbackTask& task);

    // ===== 🔥 修复1: 数据库异步写入函数 =====
    void DatabaseWriteThreadLoop();
    void EnqueueDatabaseWrite(const HeartbeatRecord& record);

    bool SendDataComplete(SOCKET targetSocket,
        const std::vector<uint8_t>& data,
        const std::string& direction,
        int targetPort = 0);

    SOCKET ConnectToSecondaryProxy(const std::string& targetHost, int targetPort);

    // ===== 🔥 认证处理函数 =====
    bool HandleSocks5Auth(SOCKET clientSocket, std::string& username,
        PacketCollector*& accountStateOwner, const std::string& clientIP);
    bool ValidateAccount(const std::string& username, const std::string& password);
    bool ValidateAccountWithReason(const std::string& username, const std::string& password, std::string& failReason);
    bool CheckAccountExpired(const Socks5Account& account);
    bool TryAcquireConnection(const std::string& username);
    void ReleaseConnection(const std::string& username);
    bool TryAcquireDeviceSlot(const std::string& clientIP, const std::string& username, std::string* outError = nullptr);
    void ReleaseDeviceSlot(const std::string& clientIP);
    void UpdateLoginInfo(const std::string& username, const std::string& clientIP);
    PacketCollector* ResolveAccountStateOwner() const;
    bool TryAcquireSharedAccountLease(const std::string& username,
        const std::string& instanceId, const std::string& deviceKey, std::string* outError = nullptr);
    void ReleaseSharedAccountLease(const std::string& username,
        const std::string& instanceId, const std::string& deviceKey);
    bool TryAcquireAuthenticatedSession(const std::string& username, const std::string& password,
        const std::string& clientIP, PacketCollector*& accountStateOwner, std::string* outError = nullptr);
    void ReleaseAuthenticatedSession(const std::string& username,
        const std::string& clientIP, PacketCollector* accountStateOwner);

    // ===== 域名访问记录（域名 -> 访问记录）=====
    std::map<std::string, DomainAccessRecord> domainAccessHistory;
    mutable std::mutex domainAccessMutex;

    // ===== 🔥 新增：流量过滤规则 =====
    TrafficFilterConfig trafficFilterConfig;
    mutable std::mutex trafficFilterMutex;
    std::atomic<int> nextTrafficRuleId;

    // ===== 🔥 优化：SNI嗅探缓存（LRU缓存） =====
    struct SNICacheEntry {
        std::string domain;
        std::chrono::steady_clock::time_point timestamp;
        SNICacheEntry() = default;
        SNICacheEntry(const std::string& d) : domain(d), timestamp(std::chrono::steady_clock::now()) {}
    };
    std::unordered_map<std::string, SNICacheEntry> sniCache;  // IP:端口 -> {域名, 时间戳}
    std::list<std::string> sniCacheLRU;  // LRU链表，存储缓存键
    mutable std::mutex sniCacheMutex;
    static constexpr size_t MAX_SNI_CACHE_SIZE = 10000;  // 最大缓存条目数
    static constexpr int SNI_CACHE_EXPIRE_SECONDS = 3600;  // 缓存过期时间（1小时）

    // SNI缓存清理线程
    std::thread sniCleanupThread;
    std::atomic<bool> sniCleanupThreadRunning{false};
    std::condition_variable sniCleanupCV;  // 用于提前唤醒清理线程
    std::mutex sniCleanupMutex;  // 配合条件变量使用
    void SNICacheCleanupThreadLoop();  // SNI缓存清理线程循环
    void CleanupExpiredSNICache();     // 清理过期的SNI缓存

    // ===== 🔥 优化：规则快速查找索引 =====
    std::unordered_set<int> portRuleSet;           // 端口规则集合
    std::unordered_set<std::string> ipRuleSet;     // IP规则集合
    std::unordered_set<std::string> domainRuleSet; // 域名规则集合
    std::unordered_map<int, std::unordered_set<std::string>> portDomainSniMap; // 端口->域名集合
    bool needsSNISniffing;  // 是否需要SNI嗅探（有DOMAIN_MATCH或PORT_DOMAIN_SNI规则）

    // 流量过滤辅助函数
    bool CheckTrafficRules(const std::string& targetHost, int targetPort, const std::string& sniDomain);
    std::string ExtractSNIFromTLS(const std::vector<uint8_t>& data);
    void RebuildRuleIndexes();  // 重建规则索引
    void UpdateSNICache(const std::string& key, const std::string& domain);  // 更新SNI缓存
    std::string GetFromSNICache(const std::string& key);  // 从SNI缓存获取

    // ===== 防CC功能 =====
    std::unique_ptr<AntiCC> antiCC;

    // ===== 白名单专用线程池（旧版阻塞式）=====
    std::unique_ptr<WhitelistThreadPool> threadPool;
    bool useThreadPool;
    int whitelistPoolSize;
    int normalPoolSize;

    // ===== IOCP高性能线程池（新版非阻塞式）=====
    std::unique_ptr<IOCPThreadPool> iocpPool;
    bool useIOCP;
    int maxWhitelistConnections;    // 最大白名单连接数
    int maxNormalConnections;       // 最大普通连接数

    // ===== 用户滤镜Web服务器（独立于线程模型）=====
    std::unique_ptr<UserFilterWebServer> userFilterHttpServer;
    int userFilterHttpPort;
    std::vector<std::pair<int, std::string>> userFilterAvailableFiltersCache;
    std::chrono::steady_clock::time_point userFilterAvailableFiltersCacheAt{};
    mutable std::mutex userFilterAvailableFiltersMutex;
    CollectedPacketPool* userWebCollectedPacketPool = nullptr;

    // ===== 🔥 代理数据包记录（循环缓冲区） =====
    std::vector<ProxyPacketRecord> proxyPacketBuffer;
    size_t proxyPacketWriteIndex;
    size_t proxyPacketBufferSize;  // 动态缓冲区大小
    std::atomic<bool> proxyPacketRecordEnabled;  // 是否启用记录
    std::atomic<bool> proxyPacketFirstRecordLogged{ false };
    mutable std::mutex proxyPacketMutex;
    std::atomic<uint64_t> proxyPacketSequenceCounter{ 0 };
    std::atomic<uint64_t> proxyConnectionIdCounter{ 0 };
    std::vector<ProxyPacketRecordSummary> proxyPacketRealtimePending;
    std::mutex proxyPacketRealtimeMutex;
    std::condition_variable proxyPacketRealtimeCV;
    std::thread proxyPacketRealtimeThread;
    std::atomic<bool> proxyPacketRealtimeThreadRunning{ false };

    // ===== SSL MITM 配置 =====
    std::atomic<bool> sslMitmGlobalEnabled{false};
    struct SSLMitmRule {
        bool   matchByPort;   // true=端口匹配, false=域名匹配
        int    port;          // 端口（matchByPort=true）
        std::string domain;   // 域名，支持前缀 * 通配（matchByPort=false）
        SSLMitmRule() : matchByPort(true), port(443) {}
    };
    std::vector<SSLMitmRule> sslMitmRules;
    mutable std::mutex sslMitmMutex;

    // 检查是否命中 SSL MITM 规则（port=目标端口, host=目标域名）
    bool ShouldSSLMitm(const std::string& host, int port) const;

    // ===== HTTP 本地映射配置 =====
    std::atomic<bool> httpLocalMapEnabled{ false };
    std::vector<HttpLocalMapRule> httpLocalMapRules;
    mutable std::mutex httpLocalMapMutex;

    // ===== 断网规则 =====
    std::shared_ptr<const std::vector<DisconnectRule>> disconnectRulesSnapshot;
    std::thread disconnectCheckThread;
    std::atomic<bool> disconnectCheckThreadRunning{ false };
    std::condition_variable disconnectCheckCV;
    mutable std::mutex disconnectCheckMutex;

    // 线程池连接处理入口
    void HandleClientFromPool(SOCKET clientSocket, const std::string& clientAddr, const std::string& clientIP);

    std::shared_ptr<const std::vector<DisconnectRule>> GetDisconnectRulesSnapshot_() const;
    void DisconnectCheckThreadLoop();
    bool HandleDisconnectFeed(std::shared_ptr<ProxyConnection> conn, const std::vector<uint8_t>& data, DisconnectDirection direction);
    bool HandleDisconnectFeed(ProxyConnectionInfo* conn, const std::vector<uint8_t>& data, DisconnectDirection direction);
    bool TryHandleHttpLocalMapRequest(const std::shared_ptr<ProxyConnection>& conn,
        const std::vector<uint8_t>& data,
        std::vector<std::vector<uint8_t>>& passthroughRequests,
        bool& consumedLocally,
        bool& waitForMore);
    void RequestTraditionalDisconnect(const std::shared_ptr<ProxyConnection>& conn, const std::string& reason);
    void RequestIOCPDisconnect(ProxyConnectionInfo* conn, const std::string& reason);
    std::vector<std::pair<int, std::string>> GetUserFilterAvailableFiltersCached();
    void EnqueueProxyPacketRealtimePush(const ProxyPacketRecordSummary& summary);
    void ProxyPacketRealtimePushThreadLoop();

    // IOCP数据处理回调
    DataModifierResult OnIOCPDataModifier(ProxyConnectionInfo* conn, const std::vector<uint8_t>& data, bool isFromClient);
    void OnIOCPConnectionEvent(ProxyConnectionInfo* conn, const std::string& event);
    ServerDataResult OnIOCPServerData(ProxyConnectionInfo* conn, std::vector<uint8_t>& data);

public:
    PacketCollector(int port);
    ~PacketCollector();

    bool Start();
    void Stop();
    bool IsRunning() const { return isRunning; }
    std::string GetLastErrorMessage() const {
        std::lock_guard<std::mutex> lock(lastErrorMutex);
        return lastErrorMessage;
    }

    void SetPacketCallback(PacketReceivedCallback callback) {
        onPacketReceived = callback;
    }

    void SetPacketModifier(PacketModifierCallback callback) {
        onPacketModifier = callback;
    }

    void SetResponseModifier(ResponseModifierCallback callback) {
        onResponseModifier = callback;
    }

    // 设置/获取实例ID（用于WPE滤镜判断）
    void SetInstanceId(const std::string& instanceId) {
        m_instanceId = instanceId;
        if (antiCC) {
            antiCC->SetScopeId(instanceId);
        }
    }
    std::string GetInstanceId() const {
        return m_instanceId;
    }

    // 🔥 新增：设置GameID清除回调（用户断线时通知清空内存池）
    void SetGameIDClearedCallback(GameIDClearedCallback callback) {
        onGameIDCleared = callback;
    }

    // 断开连接回调（提供username/gameID，供上层做计数/状态清理等）
    void SetUserDisconnectedCallback(UserDisconnectedCallback callback) {
        onUserDisconnected = callback;
    }

    // 🔥 新增：设置断开时是否清理GameID绑定
    void SetDisconnectClearEnabled(bool enabled) {
        enableDisconnectClear = enabled;
    }
    bool IsDisconnectClearEnabled() const { return enableDisconnectClear.load(); }

    void SetFilter(FilterType type, const std::string& value);
    void ClearFilter();
    FilterConfig GetFilter() const;

    // 🔥 修改：IOCP模式下返回IOCP线程池的统计
    int GetTotalPackets() const {
        if (iocpPool) return static_cast<int>(iocpPool->GetTotalPackets());
        return totalPackets;
    }
    int GetTotalConnections() const { return totalConnections; }
    int GetCurrentConnections() const {
        if (iocpPool) return GetIOCPTotalConnCount();
        std::lock_guard<std::mutex> lock(connectionsMutex);
        return static_cast<int>(proxyConnections.size());
    }
    int GetCurrentDeviceCount() const {
        std::lock_guard<std::mutex> lock(deviceLimitMutex);
        return static_cast<int>(activeDeviceIpRefCounts.size());
    }
    void SetInstanceDeviceLimit(int limit) {
        std::lock_guard<std::mutex> lock(deviceLimitMutex);
        instanceDeviceLimit = (std::max)(0, limit);
    }
    int GetInstanceDeviceLimit() const {
        std::lock_guard<std::mutex> lock(deviceLimitMutex);
        return instanceDeviceLimit;
    }
    int GetListenPort() const { return listenPort; }
    void SetListenPort(int port) { listenPort = port; }
    uint64_t GetTotalBytes() const {
        if (iocpPool) return iocpPool->GetTotalBytes();
        return totalBytes;
    }
    int GetFragmentedPackets() const {
        if (iocpPool) return iocpPool->GetFragmentedPackets();
        return fragmentedPackets;
    }
    int GetMultiPackets() const {
        if (iocpPool) return iocpPool->GetMultiPackets();
        return multiPackets;
    }
    int GetFilteredConnections() const { return filteredConnections; }

    std::vector<std::string> GetActiveConnections();

    void SetSecondaryProxy(bool enable, const std::string& host, int port,
        const std::string& username = "", const std::string& password = "");
    bool IsSecondaryProxyEnabled() const { return enableSecondaryProxy; }
    std::string GetSecondaryProxyHost() const { return secondaryProxyHost; }
    int GetSecondaryProxyPort() const { return secondaryProxyPort; }
    std::string GetSecondaryProxyUsername() const { return secondaryProxyUsername; }
    std::string GetSecondaryProxyPassword() const { return secondaryProxyPassword; }

    // ===== 🔥 账号管理接口 =====
    void SetSocks5Auth(bool enable);
    bool IsSocks5AuthEnabled() const { return enableSocks5Auth; }

    // 设置外部账号源（用于采集端共享伪心跳端账号）
    void SetExternalAccountSource(PacketCollector* source);
    PacketCollector* GetExternalAccountSource() const { return externalAccountSource; }
    PacketCollector* GetResolvedAccountStateOwner() const { return ResolveAccountStateOwner(); }
    bool TryGetSharedAccountLeaseInfo(const std::string& username, SharedAccountLeaseInfo& outInfo) const;
    void SetSharedAccountSingleDeviceModeEnabled(bool enabled) { sharedAccountSingleDeviceModeEnabled.store(enabled); }
    bool IsSharedAccountSingleDeviceModeEnabled() const { return sharedAccountSingleDeviceModeEnabled.load(); }

    bool AddAccount(const std::string& username, const std::string& password,
        const std::string& expireTime, int maxConnections);
    bool RemoveAccount(const std::string& username);
    bool UpdateAccount(const std::string& username, const std::string& password,
        const std::string& expireTime, int maxConnections, bool isEnabled);
    bool UpdateAccountEx(const Socks5Account& account);  // 🔥 新增：更新账号扩展字段
    std::vector<Socks5Account> GetAllAccounts() const;
    std::vector<Socks5Account> GetAccountsByInstance(const std::string& instanceId) const;  // 🔥 新增：按实例获取账号
    Socks5Account GetAccount(const std::string& username) const;
    bool AccountExists(const std::string& username) const;

    // 🔥 公共账号验证接口（供外部服务使用）
    bool PublicValidateAccount(const std::string& username, const std::string& password) {
        return ValidateAccount(username, password);
    }

    // ===== 游戏会话ID管理接口 =====
    bool BindGameIDToUser(const std::string& username, const std::string& gameID);
    void ClearGameIDForUser(const std::string& username);
    std::string GetGameIDForUser(const std::string& username) const;
    std::vector<std::pair<std::string, std::string>> GetAllGameIDBindings() const;

    // ===== 记录域名访问 =====
    void RecordDomainAccess(const std::string& username, const std::string& domain);

    // ===== 获取最后访问指定域名的用户 =====
    std::string GetLastUserForDomain(const std::string& domain) const;

    // ===== 获取所有域名访问记录 =====
    std::map<std::string, DomainAccessRecord> GetAllDomainAccess() const;

    // ===== SSL MITM 管理接口 =====
    // 全局开关
    void SetSSLMitmEnabled(bool enabled) { sslMitmGlobalEnabled = enabled; }
    bool IsSSLMitmEnabled() const { return sslMitmGlobalEnabled.load(); }

    // 添加按端口规则（如 443）
    void AddSSLMitmPortRule(int port) {
        std::lock_guard<std::mutex> lk(sslMitmMutex);
        SSLMitmRule r; r.matchByPort = true; r.port = port;
        sslMitmRules.push_back(r);
    }
    // 添加按域名规则（如 "example.com" 或 "*.example.com"）
    void AddSSLMitmDomainRule(const std::string& domain) {
        std::lock_guard<std::mutex> lk(sslMitmMutex);
        SSLMitmRule r; r.matchByPort = false; r.domain = domain;
        sslMitmRules.push_back(r);
    }
    void ClearSSLMitmRules() {
        std::lock_guard<std::mutex> lk(sslMitmMutex);
        sslMitmRules.clear();
    }
    std::vector<SSLMitmRule> GetSSLMitmRules() const {
        std::lock_guard<std::mutex> lk(sslMitmMutex);
        return sslMitmRules;
    }
    // 导出 CA 证书文件（方案2：用户手动安装）
    static bool ExportSSLMitmCACert(const std::string& filePath) {
        return SSLMitmContext::ExportCACert(filePath);
    }
    // 初始化 CA（Start() 时自动调用，也可手动调用）
    static bool InitSSLMitmCA() {
        return SSLMitmContext::InitGlobalCA();
    }
    static std::string GetSSLMitmLastErrorDetail() {
        return SSLMitmContext::GetLastErrorDetail();
    }

    // ===== HTTP 本地映射（类似 Charles Map Local）=====
    void SetHttpLocalMapEnabled(bool enabled) { httpLocalMapEnabled = enabled; }
    bool IsHttpLocalMapEnabled() const { return httpLocalMapEnabled.load(); }
    void SetHttpLocalMapRules(const std::vector<HttpLocalMapRule>& rules) {
        std::lock_guard<std::mutex> lk(httpLocalMapMutex);
        httpLocalMapRules = rules;
    }
    std::vector<HttpLocalMapRule> GetHttpLocalMapRules() const {
        std::lock_guard<std::mutex> lk(httpLocalMapMutex);
        return httpLocalMapRules;
    }
    void ClearHttpLocalMapRules() {
        std::lock_guard<std::mutex> lk(httpLocalMapMutex);
        httpLocalMapRules.clear();
    }

    // ===== 🔥 新增：流量过滤规则管理接口 =====
    void SetTrafficFilterEnabled(bool enabled);
    bool IsTrafficFilterEnabled() const;
    void SetSNISniffingEnabled(bool enabled);
    bool IsSNISniffingEnabled() const;

    bool AddTrafficRule(const TrafficFilterRule& rule);
    bool RemoveTrafficRule(int ruleId);
    bool UpdateTrafficRule(const TrafficFilterRule& rule);
    std::vector<TrafficFilterRule> GetAllTrafficRules() const;
    TrafficFilterRule GetTrafficRule(int ruleId) const;
    void ClearAllTrafficRules();

    // ===== 防CC管理接口 =====
    void SetAntiCCConfig(const AntiCCConfig& config);
    AntiCCConfig GetAntiCCConfig() const;
    void SetAntiCCEnabled(bool enabled);
    bool IsAntiCCEnabled() const;
    void SetAntiCCAuthPriorityAdmissionEnabled(bool enabled) { antiCCAuthPriorityAdmissionEnabled = enabled; }
    bool IsAntiCCAuthPriorityAdmissionEnabled() const { return antiCCAuthPriorityAdmissionEnabled.load(); }
    void SetAntiCCAuthPriorityQueueLimit(int limit) { antiCCAuthPriorityQueueLimit = (std::max)(1, limit); }
    int GetAntiCCAuthPriorityQueueLimit() const { return antiCCAuthPriorityQueueLimit.load(); }
    void SetAntiCCLowPriorityEvictionEnabled(bool enabled) { antiCCLowPriorityEvictionEnabled = enabled; }
    bool IsAntiCCLowPriorityEvictionEnabled() const { return antiCCLowPriorityEvictionEnabled.load(); }
    void SetAntiCCLowPriorityEvictionThreshold(int threshold) { antiCCLowPriorityEvictionThreshold = (std::max)(1, threshold); }
    int GetAntiCCLowPriorityEvictionThreshold() const { return antiCCLowPriorityEvictionThreshold.load(); }
    int GetAntiCCBlockedCount() const;
    int GetAntiCCTotalConnections() const;
    bool IsUnderAttack() const;
    std::vector<AntiCC::IPDetailInfo> GetAllIPStats();
    void AddToBlacklist(const std::string& ip);
    void AddToWhitelist(const std::string& ip);
    void RemoveFromBlacklist(const std::string& ip);
    void RemoveFromWhitelist(const std::string& ip);
    std::vector<std::string> GetBlacklist();
    std::vector<std::string> GetWhitelist();

    // ===== 白名单持久化接口 =====
    void SetAntiCCDatabaseManager(DatabaseManager* db);
    void LoadAntiCCWhitelistFromDB();
    void CleanupAntiCCInactiveWhitelist();

    // ===== 线程池模式选择 =====
    enum class ThreadPoolMode {
        TRADITIONAL,    // 传统模式：每连接一个线程
        BLOCKING,       // 阻塞式线程池：固定线程数，串行处理
        IOCP            // IOCP非阻塞模式：少量线程处理大量连接
    };

    void SetThreadPoolMode(ThreadPoolMode mode);
    ThreadPoolMode GetThreadPoolMode() const;

    // ===== 白名单线程池配置（旧版阻塞式）=====
    void SetThreadPoolEnabled(bool enabled);
    bool IsThreadPoolEnabled() const { return useThreadPool; }
    void SetThreadPoolSizes(int whitelistSize, int normalSize);
    int GetWhitelistPoolSize() const { return whitelistPoolSize; }
    int GetNormalPoolSize() const { return normalPoolSize; }

    // ===== IOCP高性能模式配置（新版）=====
    void SetIOCPEnabled(bool enabled);
    bool IsIOCPEnabled() const { return useIOCP; }
    void SetIOCPMaxConnections(int maxWhitelist, int maxNormal);
    int GetIOCPMaxWhitelistConnections() const { return maxWhitelistConnections; }
    int GetIOCPMaxNormalConnections() const { return maxNormalConnections; }

    // 线程池统计
    int GetWhitelistPoolProcessed() const;
    int GetNormalPoolProcessed() const;
    int GetWhitelistQueueSize() const;
    int GetNormalQueueSize() const;

    // IOCP统计
    int GetIOCPWhitelistConnCount() const;
    int GetIOCPNormalConnCount() const;
    int GetIOCPTotalConnCount() const;

    // ===== 🔥 修复1&3: 性能统计接口 =====
    size_t GetDbQueueSize() const { return dbQueueSize.load(); }
    size_t GetCallbackQueueSize() const { return callbackQueueSize.load(); }

    // ===== 🔥 分包处理配置接口 =====
    void SetPacketSplitEnabled(bool enabled);
    bool IsPacketSplitEnabled() const;
    void SetPacketSplitPorts(const std::vector<int>& ports);
    std::vector<int> GetPacketSplitPorts() const;
    bool IsPacketSplitEnabledForPort(int port) const;

    // ===== 🔥 对不分包流量应用WPE滤镜 =====
    void SetApplyWpeOnNonSplitTraffic(bool enabled);
    bool IsApplyWpeOnNonSplitTraffic() const;

    // ===== 断网规则配置 =====
    bool SetDisconnectRules(const std::vector<DisconnectRule>& rules, std::string* error = nullptr);
    std::vector<DisconnectRule> GetDisconnectRules() const;
    void ClearDisconnectRules();

    // ===== 🔥 用户滤镜模式 =====
    void SetUserFilterMode(bool enabled);
    bool IsUserFilterModeEnabled() const;
    void SetUserFilterHttpPort(int port);
    int GetUserFilterHttpPort() const;
    void SetCollectedPacketPoolForUserWeb(CollectedPacketPool* pool) { userWebCollectedPacketPool = pool; }
    bool StartUserFilterHttpServer();
    void StopUserFilterHttpServer();
    bool IsUserFilterHttpServerRunning() const;

    // ===== 🔥 代理数据包记录 =====
    void RecordProxyPacket(uint64_t connectionId,
                          const std::string& username,
                          const std::string& gameID,
                          const std::string& clientIP,
                          const std::string& targetHost,
                          const std::string& sniHost,
                          int targetPort,
                          bool sslMitmEnabled,
                          bool isRequest,
                          const std::vector<uint8_t>& data);
        std::vector<ProxyPacketRecord> GetProxyPacketRecords() const;
    std::vector<ProxyPacketRecordSummary> GetProxyPacketRecordSummaries() const;
    bool GetProxyPacketRecordSummariesDelta(
        size_t knownCount,
        const std::string& firstKey,
        const std::string& lastKey,
        bool& outReset,
        size_t& outTotalCount,
        std::string& outCurrentFirstKey,
        std::string& outCurrentLastKey,
        std::vector<ProxyPacketRecordSummary>& outRecords) const;
    bool GetProxyPacketRecordByOrderedIndex(size_t orderedIndex, ProxyPacketRecord& outRecord) const;
    bool GetProxyPacketRecordBySequence(uint64_t sequence, ProxyPacketRecord& outRecord) const;
    void ClearProxyPacketRecords();
    void SetProxyPacketRecordEnabled(bool enabled);
    bool IsProxyPacketRecordEnabled() const;
    void SetProxyPacketBufferSize(size_t size);
    size_t GetProxyPacketBufferSize() const;

    // ===== 🔥 断开用户连接 =====
    bool DisconnectUser(const std::string& username, const std::string& reason = "管理员踢出");
    bool DisconnectIP(const std::string& clientIP);

private:
    ThreadPoolMode threadPoolMode;
    std::atomic<bool> antiCCAuthPriorityAdmissionEnabled{ true };
    std::atomic<int> antiCCAuthPriorityQueueLimit{ 128 };
    std::atomic<int> antiCCAuthPriorityInFlight{ 0 };
    std::atomic<bool> antiCCLowPriorityEvictionEnabled{ true };
    std::atomic<int> antiCCLowPriorityEvictionThreshold{ 80 };
    bool DisconnectOneLowPriorityConnection_();

    // ===== 🔥 分包处理配置 =====
    std::atomic<bool> enablePacketSplit{true};  // 默认启用分包处理
    std::vector<int> packetSplitPorts;          // 需要分包处理的端口列表
    mutable std::mutex packetSplitMutex;        // 保护端口列表
    std::atomic<bool> applyWpeOnNonSplitTraffic{false};  // 对不分包流量也应用WPE滤镜

    // ===== 🔥 用户滤镜模式配置 =====
    std::atomic<bool> enableUserFilterMode{false};  // 用户滤镜模式开关
};


