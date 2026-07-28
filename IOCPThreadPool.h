#pragma once

// IOCP High-Performance Thread Pool
// Uses Windows IOCP (I/O Completion Port) for non-blocking IO multiplexing
// A few threads can handle thousands of concurrent connections

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>

#include <vector>
#include <queue>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include "DisconnectRuleTypes.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "mswsock.lib")

// Forward declaration
class IOCPThreadPool;
class UserFilterWebServer;
class PacketCollector;

// ==================== IO Operation Types ====================
enum class IOOperationType {
    IO_ACCEPT,          // Accept new connection
    IO_RECV_CLIENT,     // Receive data from client
    IO_SEND_CLIENT,     // Send data to client
    IO_RECV_SERVER,     // Receive data from target server
    IO_SEND_SERVER,     // Send data to target server
    IO_CONNECT,         // Connect to target server
    IO_DISCONNECT       // Disconnect
};

// ==================== Connection States ====================
enum class ConnectionState {
    STATE_ACCEPTING,        // Waiting to accept connection
    STATE_SOCKS5_INIT,      // SOCKS5 initialization phase
    STATE_SOCKS5_AUTH,      // SOCKS5 authentication phase
    STATE_SOCKS5_REQUEST,   // SOCKS5 request phase
    STATE_CONNECTING,       // Connecting to target server
    STATE_FORWARDING,       // Data forwarding in progress
    STATE_CLOSING           // Closing connection
};

// ==================== IO Context (OVERLAPPED extension) ====================
struct IOContext : public OVERLAPPED {
    IOOperationType opType;         // Operation type
    WSABUF wsaBuf;                  // Data buffer
    char buffer[8192];              // Actual data buffer
    DWORD bytesTransferred;         // Bytes transferred
    DWORD flags;                    // WSA flags

    // Connection info
    SOCKET socket;                  // Associated socket
    uint64_t connectionId;          // Connection ID

    IOContext() {
        memset(static_cast<OVERLAPPED*>(this), 0, sizeof(OVERLAPPED));
        opType = IOOperationType::IO_RECV_CLIENT;
        wsaBuf.buf = buffer;
        wsaBuf.len = sizeof(buffer);
        bytesTransferred = 0;
        flags = 0;
        socket = INVALID_SOCKET;
        connectionId = 0;
    }

    void Reset() {
        memset(static_cast<OVERLAPPED*>(this), 0, sizeof(OVERLAPPED));
        bytesTransferred = 0;
        flags = 0;
    }
};

// ==================== Proxy Connection Info ====================
struct ProxyConnectionInfo {
    uint64_t id;                    // Unique connection ID
    SOCKET clientSocket;            // Client socket
    SOCKET serverSocket;            // Target server socket

    std::string clientAddr;         // Client address
    std::string clientIP;           // Client IP
    std::string targetHost;         // Target host
    int targetPort;                 // Target port

    std::string authenticatedUser;  // Authenticated username
    PacketCollector* accountStateOwner;  // 认证时实际占用账号状态的源Collector
    std::string gameID;             // Game session ID

    ConnectionState state;          // Connection state
    bool isWhitelisted;             // Is whitelisted
    bool bypassModifier;            // Bypass data processing
    bool isActive;                  // Is active
    bool sniChecked;                // SNI嗅探是否已执行（与传统模式一致的一次性检查）
    bool authPrioritySlotHeld;      // 是否占用了认证优先准入名额

    // IO contexts
    std::unique_ptr<IOContext> clientRecvCtx;   // Client receive context
    std::unique_ptr<IOContext> clientSendCtx;   // Client send context
    std::unique_ptr<IOContext> serverRecvCtx;   // Server receive context
    std::unique_ptr<IOContext> serverSendCtx;   // Server send context

    // SOCKS5 handshake buffer
    std::vector<uint8_t> handshakeBuffer;
    int handshakeStep;              // Handshake step

    // Data buffers (for packet assembly)
    std::vector<uint8_t> clientDataBuffer;
    std::vector<uint8_t> serverDataBuffer;

    // 🔥 与传统模式一致的缓冲区状态
    int fragmentCount;              // 分片计数
    bool gameIDExtracted;           // GameID是否已提取

    // Send queues
    std::queue<std::vector<uint8_t>> clientSendQueue;
    std::queue<std::vector<uint8_t>> serverSendQueue;
    std::mutex sendMutex;
    bool clientSending;             // Is sending to client
    bool serverSending;             // Is sending to server

    // 断网规则运行时状态
    DisconnectRuntimeState disconnectState;

    // Statistics
    std::chrono::steady_clock::time_point createTime;
    uint64_t bytesFromClient;
    uint64_t bytesToClient;
    uint64_t bytesFromServer;
    uint64_t bytesToServer;

    ProxyConnectionInfo()
        : id(0)
        , clientSocket(INVALID_SOCKET)
        , serverSocket(INVALID_SOCKET)
        , targetPort(0)
        , accountStateOwner(nullptr)
        , state(ConnectionState::STATE_ACCEPTING)
        , isWhitelisted(false)
        , bypassModifier(false)
        , isActive(true)
        , sniChecked(false)
        , authPrioritySlotHeld(false)
        , handshakeStep(0)
        , fragmentCount(0)
        , gameIDExtracted(false)
        , clientSending(false)
        , serverSending(false)
        , bytesFromClient(0)
        , bytesToClient(0)
        , bytesFromServer(0)
        , bytesToServer(0) {
        createTime = std::chrono::steady_clock::now();
    }
};

// ==================== Callback Types ====================
// Data modifier callback: returns modified data
struct DataModifierResult {
    std::vector<uint8_t> forwardedData;   // 实际用于转发的数据（为空表示丢弃）
    std::vector<uint8_t> callbackData;    // 用于回调侧（UI/采集）的数据（为空则默认等同 forwardedData）
    bool intercepted;                      // 是否拦截（不转发）

    DataModifierResult() : intercepted(false) {}
};

using DataModifierCallback = std::function<DataModifierResult(
    ProxyConnectionInfo* conn,
    const std::vector<uint8_t>& data,
    bool isFromClient
)>;

using RawDataObserverCallback = std::function<bool(
    ProxyConnectionInfo* conn,
    const std::vector<uint8_t>& data,
    bool isFromClient
)>;

// Connection event callback
using ConnectionEventCallback = std::function<void(ProxyConnectionInfo* conn, const std::string& event)>;

// Whitelist check callback
using WhitelistCheckCallback = std::function<bool(const std::string& ip)>;

// Authentication callback
using AuthCallback = std::function<bool(const std::string& username, const std::string& password, const std::string& clientIP, PacketCollector** outAccountStateOwner)>;

// Filter check callback: returns true if should bypass modifier (direct forward)
using FilterCheckCallback = std::function<bool(const std::string& host, int port)>;

// SNI sniff/recheck callback: returns true if connection is still allowed
using SNIRecheckCallback = std::function<bool(ProxyConnectionInfo* conn, const std::vector<uint8_t>& data)>;

// 🔥 与传统模式一致的数据包接收回调（用于UI显示）
struct PacketInfo;  // Forward declaration
using PacketReceivedCallback = std::function<void(const PacketInfo& info, const std::vector<uint8_t>& forwardedData, const std::vector<uint8_t>& callbackData)>;

// 🔥 认证失败回调（用于防CC记录）
using AuthFailureCallback = std::function<void(const std::string& clientIP)>;

// 🔥 非SOCKS5连接检测回调
using NonSocksCheckCallback = std::function<bool(SOCKET socket, const std::string& clientIP)>;

// 🔥 二级代理连接回调
using SecondaryProxyCallback = std::function<SOCKET(const std::string& targetHost, int targetPort)>;

// 🔥 防CC检查回调（返回true表示允许连接，false表示拒绝）
using AntiCCCheckCallback = std::function<bool(const std::string& clientIP)>;

// 🔥 服务器响应数据处理回调（用于WPE滤镜处理响应包）
struct ServerDataResult {
    bool intercepted;                     // 是否拦截（不转发给客户端）
    bool modified;                        // 是否修改
    std::vector<uint8_t> modifiedData;    // 修改后的数据

    ServerDataResult() : intercepted(false), modified(false) {}
};

using ServerDataCallback = std::function<ServerDataResult(
    ProxyConnectionInfo* conn,
    std::vector<uint8_t>& data
)>;

// ==================== IOCP Thread Pool Class ====================
class IOCPThreadPool {
public:
    IOCPThreadPool(int workerThreads = 0);  // 0 means auto-detect CPU cores
    ~IOCPThreadPool();

    // Start/Stop
    bool Start(SOCKET listenSocket);
    void Stop();
    bool IsRunning() const { return isRunning.load(); }

    // Set callbacks
    void SetDataModifier(DataModifierCallback callback) { dataModifier = callback; }
    void SetRawDataObserver(RawDataObserverCallback callback) { rawDataObserver = callback; }
    void SetConnectionEventCallback(ConnectionEventCallback callback) { connectionEvent = callback; }
    void SetWhitelistChecker(WhitelistCheckCallback callback) { whitelistChecker = callback; }
    void SetAuthCallback(AuthCallback callback) { authCallback = callback; }
    void SetFilterChecker(FilterCheckCallback callback) { filterChecker = callback; }
    void SetSNIRecheckCallback(SNIRecheckCallback callback) { sniRecheckCallback = callback; }
    void SetPacketReceivedCallback(PacketReceivedCallback callback) { packetReceived = callback; }
    void SetAuthFailureCallback(AuthFailureCallback callback) { authFailure = callback; }  // 🔥 新增
    void SetNonSocksChecker(NonSocksCheckCallback callback) { nonSocksChecker = callback; }  // 🔥 新增
    void SetSecondaryProxyConnector(SecondaryProxyCallback callback) { secondaryProxyConnector = callback; }  // 🔥 新增
    void SetAntiCCChecker(AntiCCCheckCallback callback) { antiCCChecker = callback; }  // 🔥 防CC检查
    void SetServerDataCallback(ServerDataCallback callback) { serverDataCallback = callback; }  // 🔥 服务器响应数据处理
    void SetAntiCCAuthPriorityAdmissionEnabled(bool enabled) { antiCCAuthPriorityAdmissionEnabled = enabled; }
    void SetAntiCCAuthPriorityQueueLimit(int limit) { antiCCAuthPriorityQueueLimit = limit > 0 ? limit : 128; }

    // Configuration
    void SetMaxConnections(int maxWhitelist, int maxNormal);
    void SetQueueLimits(int whitelistLimit, int normalLimit);
    void SetSocks5Auth(bool enable) { enableSocks5Auth = enable; }  // 🔥 设置SOCKS5认证开关
    bool IsSocks5AuthEnabled() const { return enableSocks5Auth; }  // 🔥 获取SOCKS5认证状态

    // 🔥 分包处理配置
    void SetPacketSplitEnabled(bool enabled);
    bool IsPacketSplitEnabled() const;
    void SetPacketSplitPorts(const std::vector<int>& ports);
    std::vector<int> GetPacketSplitPorts() const;
    bool IsPacketSplitEnabledForPort(int port) const;

    // 🔥 对不分包流量应用WPE滤镜
    void SetApplyWpeOnNonSplitTraffic(bool enabled);
    bool IsApplyWpeOnNonSplitTraffic() const;

    // 设置/获取实例ID（用于WPE滤镜判断）
    void SetInstanceId(const std::string& id) { instanceId = id; }
    std::string GetInstanceId() const { return instanceId; }

    // 🔥 用户自定义滤镜模式
    void SetUserFilterMode(bool enabled) { enableUserFilterMode = enabled; }
    bool IsUserFilterModeEnabled() const { return enableUserFilterMode.load(); }

    // User filter HTTP server
    void SetUserFilterHttpPort(int port) { userFilterHttpPort = port; }
    int GetUserFilterHttpPort() const { return userFilterHttpPort; }
    bool StartUserFilterHttpServer();
    void StopUserFilterHttpServer();
    bool IsUserFilterHttpServerRunning() const;

    // Statistics
    int GetWhitelistConnectionCount() const { return whitelistConnCount.load(); }
    int GetNormalConnectionCount() const { return normalConnCount.load(); }
    int GetTotalConnectionCount() const { return totalConnCount.load(); }
    int GetWhitelistProcessed() const { return whitelistProcessed.load(); }
    int GetNormalProcessed() const { return normalProcessed.load(); }
    int GetWhitelistQueueSize() const;
    int GetNormalQueueSize() const;

    // 🔥 与传统模式一致的数据包统计
    uint64_t GetTotalPackets() const { return totalPackets.load(); }
    uint64_t GetTotalBytes() const { return totalBytes.load(); }
    int GetFragmentedPackets() const { return fragmentedPackets.load(); }
    int GetMultiPackets() const { return multiPackets.load(); }

    // Connection management
    std::shared_ptr<ProxyConnectionInfo> GetConnection(uint64_t id);
    std::vector<std::shared_ptr<ProxyConnectionInfo>> GetAllConnections();
    void CloseConnection(uint64_t connId, const std::string& reason);

private:
    // IOCP core
    HANDLE iocpHandle;                      // IOCP handle
    SOCKET listenSocket;                    // Listen socket
    std::vector<std::thread> workerThreads; // Worker threads
    int numWorkerThreads;                   // Number of worker threads
    std::atomic<bool> isRunning;            // Running state

    // Connection management
    std::map<uint64_t, std::shared_ptr<ProxyConnectionInfo>> connections;
    std::mutex connectionsMutex;
    std::atomic<uint64_t> nextConnectionId;

    // Connection counts
    std::atomic<int> whitelistConnCount;
    std::atomic<int> normalConnCount;
    std::atomic<int> totalConnCount;
    std::atomic<int> whitelistProcessed;
    std::atomic<int> normalProcessed;
    std::atomic<uint64_t> lifecycleCreatedCount;
    std::atomic<uint64_t> lifecycleCloseRequestedCount;
    std::atomic<uint64_t> lifecycleClosedCount;

    // 🔥 与传统模式一致的数据包统计
    std::atomic<uint64_t> totalPackets;
    std::atomic<uint64_t> totalBytes;
    std::atomic<int> fragmentedPackets;
    std::atomic<int> multiPackets;

    // Configuration
    int maxWhitelistConnections;
    int maxNormalConnections;
    int whitelistQueueLimit;
    int normalQueueLimit;
    bool enableSocks5Auth;  // 🔥 是否启用SOCKS5认证（与传统模式一致）

    // 🔥 分包处理配置
    std::atomic<bool> enablePacketSplit{true};  // 默认启用分包处理
    std::vector<int> packetSplitPorts;          // 需要分包处理的端口列表
    mutable std::mutex packetSplitMutex;        // 保护端口列表
    std::atomic<bool> applyWpeOnNonSplitTraffic{false};  // 对不分包流量也应用WPE滤镜

    // 实例ID（用于WPE滤镜判断）
    std::string instanceId;
    std::atomic<bool> antiCCAuthPriorityAdmissionEnabled{ true };
    std::atomic<int> antiCCAuthPriorityQueueLimit{ 128 };
    std::atomic<int> antiCCAuthPriorityInFlight{ 0 };

    // 🔥 用户自定义滤镜模式
    std::atomic<bool> enableUserFilterMode{false};
    int userFilterHttpPort{8080};
    std::unique_ptr<UserFilterWebServer> userFilterHttpServer;

    // Wait queues (when max connections reached)
    std::queue<SOCKET> whitelistWaitQueue;
    std::queue<SOCKET> normalWaitQueue;
    std::mutex queueMutex;

    // Callbacks
    DataModifierCallback dataModifier;
    RawDataObserverCallback rawDataObserver;
    ConnectionEventCallback connectionEvent;
    WhitelistCheckCallback whitelistChecker;
    AuthCallback authCallback;
    FilterCheckCallback filterChecker;
    SNIRecheckCallback sniRecheckCallback;
    PacketReceivedCallback packetReceived;  // 🔥 与传统模式一致的数据包接收回调
    AuthFailureCallback authFailure;  // 🔥 认证失败回调
    NonSocksCheckCallback nonSocksChecker;  // 🔥 非SOCKS5连接检测
    SecondaryProxyCallback secondaryProxyConnector;  // 🔥 二级代理连接
    AntiCCCheckCallback antiCCChecker;  // 🔥 防CC检查
    ServerDataCallback serverDataCallback;  // 🔥 服务器响应数据处理

    // AcceptEx function pointers
    LPFN_ACCEPTEX lpfnAcceptEx;
    LPFN_GETACCEPTEXSOCKADDRS lpfnGetAcceptExSockaddrs;
    LPFN_CONNECTEX lpfnConnectEx;

    // Pre-created Accept socket pool
    std::vector<SOCKET> acceptSocketPool;
    std::vector<std::unique_ptr<IOContext>> acceptContextPool;
    std::mutex acceptPoolMutex;
    int acceptPoolSize;

    // Worker thread function
    void WorkerThread();

    // IO completion handlers
    void OnAcceptComplete(IOContext* ctx, DWORD bytesTransferred);
    void OnRecvComplete(IOContext* ctx, DWORD bytesTransferred);
    void OnSendComplete(IOContext* ctx, DWORD bytesTransferred);
    void OnConnectComplete(IOContext* ctx, DWORD bytesTransferred);

    // Connection handling
    ProxyConnectionInfo* CreateConnection(SOCKET clientSocket, const std::string& clientAddr, const std::string& clientIP);
    void ProcessSocks5Handshake(ProxyConnectionInfo* conn, const uint8_t* data, int len);
    bool ConnectToTarget(ProxyConnectionInfo* conn);
    void StartForwarding(ProxyConnectionInfo* conn);
    void ProcessClientData(ProxyConnectionInfo* conn, const uint8_t* data, int len);
    void ProcessServerData(ProxyConnectionInfo* conn, const uint8_t* data, int len);
    void RemoveConnection(uint64_t connId);

    // Async IO operations
    bool PostAccept();
    bool PostRecv(ProxyConnectionInfo* conn, bool isClient);
    bool PostSend(ProxyConnectionInfo* conn, bool isClient, const std::vector<uint8_t>& data);
    bool PostConnect(ProxyConnectionInfo* conn, const sockaddr_in& addr);

    // Helper functions
    bool InitializeExtensionFunctions();
    void SetSocketNonBlocking(SOCKET sock);
    bool AssociateWithIOCP(SOCKET sock, ULONG_PTR completionKey);
};
