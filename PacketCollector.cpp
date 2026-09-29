#include "PacketCollector.h"
#include "DisconnectRuleEngine.h"
#include "GlobalAntiCCCoordinator.h"
#include "ui_bridge.h"
#include "WPEFilterIntegration.h"
#include "UserFilterManager.h"
#include "res/json.hpp"
#include <ws2tcpip.h>
#include <mstcpip.h>  // 🔥 添加：用于 tcp_keepalive 和 SIO_KEEPALIVE_VALS
#include <iostream>
#include "Logger.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <unordered_map>

#define NOMINMAX  // Prevent Windows.h min/max macros
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

// 外部全局变量声明
extern UserFilterManager* g_userFilterManager;
using json = nlohmann::json;

namespace {
constexpr size_t kProxyRealtimeBatchSize = 48;
constexpr auto kProxyRealtimeBatchWindow = std::chrono::milliseconds(250);

bool DetectPasswordAuthHint(SOCKET clientSocket) {
    if (clientSocket == INVALID_SOCKET) {
        return false;
    }

    u_long nonblocking = 1;
    ioctlsocket(clientSocket, FIONBIO, &nonblocking);

    unsigned char buffer[8] = { 0 };
    fd_set readSet;
    timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 50000;

    FD_ZERO(&readSet);
    FD_SET(clientSocket, &readSet);

    const int selectResult = select(0, &readSet, nullptr, nullptr, &timeout);
    bool hasAuthHint = false;

    if (selectResult > 0 && FD_ISSET(clientSocket, &readSet)) {
        const int len = recv(clientSocket, reinterpret_cast<char*>(buffer), sizeof(buffer), MSG_PEEK);
        if (len >= 2 && buffer[0] == 0x05) {
            const int methodCount = buffer[1];
            if (methodCount > 0 && len >= 2 + methodCount) {
                for (int i = 0; i < methodCount; ++i) {
                    if (buffer[2 + i] == 0x02) {
                        hasAuthHint = true;
                        break;
                    }
                }
            }
        }
    }

    nonblocking = 0;
    ioctlsocket(clientSocket, FIONBIO, &nonblocking);
    return hasAuthHint;
}
}

namespace {
std::shared_ptr<const std::vector<DisconnectRule>> GetEmptyDisconnectRuleSnapshot() {
    static const auto emptyRules = std::make_shared<const std::vector<DisconnectRule>>();
    return emptyRules;
}

constexpr size_t kHttpLocalMapMaxHeaderBytes = 64 * 1024;
constexpr size_t kHttpLocalMapMaxBufferedBytes = 512 * 1024;
constexpr auto kSharedLeaseStaleWindow = std::chrono::seconds(2);

struct ParsedHttpRequest {
    std::string method;
    std::string host;
    std::string path;
    std::string scheme;
    size_t totalSize = 0;
    bool chunked = false;
};

std::string TrimAscii(const std::string& value) {
    size_t start = 0;
    size_t end = value.size();
    while (start < end && std::isspace(static_cast<unsigned char>(value[start]))) {
        ++start;
    }
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(start, end - start);
}

std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string ToUpperAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return value;
}

bool SendRawBytes(SOCKET socket, const uint8_t* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        const int result = send(socket,
            reinterpret_cast<const char*>(data + sent),
            static_cast<int>(len - sent), 0);
        if (result <= 0) {
            return false;
        }
        sent += static_cast<size_t>(result);
    }
    return true;
}

bool IsLikelyHttpRequestPrefix(const std::vector<uint8_t>& buffer) {
    static const char* kMethods[] = {
        "GET ", "HEAD ", "POST ", "PUT ", "PATCH ",
        "DELETE ", "OPTIONS ", "TRACE ", "CONNECT "
    };

    if (buffer.empty()) {
        return true;
    }

    const std::string prefix(reinterpret_cast<const char*>(buffer.data()), buffer.size());
    for (const char* method : kMethods) {
        const std::string methodStr(method);
        const size_t compareLen = (std::min)(prefix.size(), methodStr.size());
        if (_strnicmp(prefix.c_str(), methodStr.c_str(), static_cast<int>(compareLen)) == 0) {
            return true;
        }
    }
    return false;
}

bool WildcardHostMatch(const std::string& pattern, const std::string& host) {
    if (pattern.empty() || pattern == "*") {
        return true;
    }

    const std::string normalizedPattern = ToLowerAscii(pattern);
    const std::string normalizedHost = ToLowerAscii(host);
    if (!normalizedPattern.empty() && normalizedPattern[0] == '*') {
        const std::string suffix = normalizedPattern.substr(1);
        if (normalizedHost.size() < suffix.size()) {
            return false;
        }
        return normalizedHost.compare(normalizedHost.size() - suffix.size(), suffix.size(), suffix) == 0;
    }
    return normalizedPattern == normalizedHost;
}

bool PathPatternMatch(const std::string& pattern, const std::string& path) {
    if (pattern.empty() || pattern == "*") {
        return true;
    }

    if (!pattern.empty() && pattern.back() == '*') {
        const std::string prefix = pattern.substr(0, pattern.size() - 1);
        return path.compare(0, prefix.size(), prefix) == 0;
    }

    return pattern == path;
}

std::string GuessMimeType(const std::string& filePath) {
    static const std::unordered_map<std::string, std::string> kMimeTypes = {
        { ".html", "text/html; charset=utf-8" },
        { ".htm", "text/html; charset=utf-8" },
        { ".js", "application/javascript; charset=utf-8" },
        { ".mjs", "application/javascript; charset=utf-8" },
        { ".css", "text/css; charset=utf-8" },
        { ".json", "application/json; charset=utf-8" },
        { ".txt", "text/plain; charset=utf-8" },
        { ".xml", "application/xml; charset=utf-8" },
        { ".svg", "image/svg+xml" },
        { ".png", "image/png" },
        { ".jpg", "image/jpeg" },
        { ".jpeg", "image/jpeg" },
        { ".gif", "image/gif" },
        { ".webp", "image/webp" },
        { ".ico", "image/x-icon" },
        { ".woff", "font/woff" },
        { ".woff2", "font/woff2" },
        { ".ttf", "font/ttf" },
        { ".map", "application/json; charset=utf-8" }
    };

    const std::string extension = ToLowerAscii(std::filesystem::path(filePath).extension().string());
    auto it = kMimeTypes.find(extension);
    if (it != kMimeTypes.end()) {
        return it->second;
    }
    return "application/octet-stream";
}

bool BuildLocalMapResponseBytes(const HttpLocalMapRule& rule,
                                const ParsedHttpRequest& request,
                                std::vector<uint8_t>& responseBytes) {
    std::ifstream file(rule.localFilePath, std::ios::binary);
    int statusCode = 200;
    std::string reasonPhrase = "OK";
    std::vector<uint8_t> body;

    if (file) {
        file.seekg(0, std::ios::end);
        const std::streamoff size = file.tellg();
        file.seekg(0, std::ios::beg);
        if (size > 0) {
            body.resize(static_cast<size_t>(size));
            file.read(reinterpret_cast<char*>(body.data()), size);
        }
    } else {
        statusCode = 500;
        reasonPhrase = "Local Map Error";
        const std::string errorText = "Local map file not found: " + rule.localFilePath;
        body.assign(errorText.begin(), errorText.end());
    }

    const std::string contentType = rule.contentType.empty()
        ? GuessMimeType(rule.localFilePath)
        : rule.contentType;

    std::ostringstream headers;
    headers << "HTTP/1.1 " << statusCode << ' ' << reasonPhrase << "\r\n";
    headers << "Content-Type: " << contentType << "\r\n";
    headers << "Content-Length: " << body.size() << "\r\n";
    headers << "Connection: close\r\n";
    headers << "X-AB-Local-Map: hit\r\n";
    headers << "Cache-Control: no-cache\r\n";
    headers << "\r\n";

    const std::string headerText = headers.str();
    responseBytes.assign(headerText.begin(), headerText.end());
    if (request.method != "HEAD") {
        responseBytes.insert(responseBytes.end(), body.begin(), body.end());
    }
    return true;
}

bool TryParseHttpRequest(const std::vector<uint8_t>& buffer,
                         const ProxyConnection& conn,
                         ParsedHttpRequest& request,
                         bool& needMore) {
    needMore = false;
    request = ParsedHttpRequest{};

    if (buffer.empty()) {
        needMore = true;
        return true;
    }

    if (!IsLikelyHttpRequestPrefix(buffer)) {
        return false;
    }

    const std::string data(reinterpret_cast<const char*>(buffer.data()), buffer.size());
    const size_t headerEnd = data.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        if (buffer.size() > kHttpLocalMapMaxHeaderBytes) {
            return false;
        }
        needMore = true;
        return true;
    }

    const std::string headerText = data.substr(0, headerEnd);
    std::istringstream headerStream(headerText);

    std::string requestLine;
    if (!std::getline(headerStream, requestLine)) {
        return false;
    }
    if (!requestLine.empty() && requestLine.back() == '\r') {
        requestLine.pop_back();
    }

    std::istringstream requestLineStream(requestLine);
    std::string method;
    std::string requestTarget;
    std::string httpVersion;
    if (!(requestLineStream >> method >> requestTarget >> httpVersion)) {
        return false;
    }

    method = ToUpperAscii(method);
    if (httpVersion.rfind("HTTP/", 0) != 0) {
        return false;
    }

    size_t contentLength = 0;
    std::string hostHeader;
    std::string transferEncoding;

    std::string line;
    while (std::getline(headerStream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }

        const size_t colonPos = line.find(':');
        if (colonPos == std::string::npos) {
            continue;
        }

        const std::string name = ToLowerAscii(TrimAscii(line.substr(0, colonPos)));
        const std::string value = TrimAscii(line.substr(colonPos + 1));

        if (name == "host") {
            hostHeader = value;
        } else if (name == "content-length") {
            try {
                contentLength = static_cast<size_t>(std::stoull(value));
            } catch (...) {
                return false;
            }
        } else if (name == "transfer-encoding") {
            transferEncoding = ToLowerAscii(value);
        }
    }

    if (transferEncoding.find("chunked") != std::string::npos) {
        request.chunked = true;
        return false;
    }

    const size_t totalSize = headerEnd + 4 + contentLength;
    if (buffer.size() < totalSize) {
        if (buffer.size() > kHttpLocalMapMaxBufferedBytes) {
            return false;
        }
        needMore = true;
        return true;
    }

    std::string scheme = conn.sslMitmEnabled ? "https" : "http";
    std::string host = hostHeader;
    std::string path = requestTarget.empty() ? "/" : requestTarget;

    if (_strnicmp(requestTarget.c_str(), "http://", 7) == 0 ||
        _strnicmp(requestTarget.c_str(), "https://", 8) == 0) {
        const bool https = _strnicmp(requestTarget.c_str(), "https://", 8) == 0;
        scheme = https ? "https" : "http";
        const size_t schemeSep = requestTarget.find("://");
        const size_t hostStart = schemeSep == std::string::npos ? 0 : schemeSep + 3;
        const size_t pathStart = requestTarget.find('/', hostStart);
        host = requestTarget.substr(hostStart, pathStart == std::string::npos
            ? std::string::npos
            : pathStart - hostStart);
        path = pathStart == std::string::npos ? "/" : requestTarget.substr(pathStart);
    }

    if (host.empty()) {
        host = !conn.sniHostname.empty() ? conn.sniHostname : conn.targetAddr;
    }

    const size_t colonPos = host.find(':');
    if (colonPos != std::string::npos) {
        host = host.substr(0, colonPos);
    }

    request.method = method;
    request.host = host;
    request.path = path.empty() ? "/" : path;
    request.scheme = ToLowerAscii(scheme);
    request.totalSize = totalSize;
    return true;
}
}




void Log(const std::string& msg) {
    // 极速模式会关闭 Logger，这里同步静音控制台输出，避免残留热路径日志开销。
    if (!Logger::IsEnabled()) {
        return;
    }

    // UTF-8 转 GBK 后输出到控制台
    int wlen = MultiByteToWideChar(CP_UTF8, 0, msg.c_str(), -1, NULL, 0);
    if (wlen > 0) {
        std::vector<wchar_t> wstr(wlen);
        MultiByteToWideChar(CP_UTF8, 0, msg.c_str(), -1, wstr.data(), wlen);

        int gbkLen = WideCharToMultiByte(CP_ACP, 0, wstr.data(), -1, NULL, 0, NULL, NULL);
        if (gbkLen > 0) {
            std::vector<char> gbkStr(gbkLen);
            WideCharToMultiByte(CP_ACP, 0, wstr.data(), -1, gbkStr.data(), gbkLen, NULL, NULL);
            std::cout << gbkStr.data() << std::endl;
            return;
        }
    }

    // 转换失败时直接输出
    std::cout << msg << std::endl;
}


PacketCollector::PacketCollector(int port)
    : listenPort(port), listenSocket(INVALID_SOCKET),
    isRunning(false), totalPackets(0), totalConnections(0), totalBytes(0),
    fragmentedPackets(0), multiPackets(0), filteredConnections(0),
    callbackThreadRunning(false),
    enableSecondaryProxy(false), secondaryProxyPort(0),
    enableSocks5Auth(false), nextAccountId(1),
    externalAccountSource(nullptr), instanceDeviceLimit(0),
    useThreadPool(false), whitelistPoolSize(10), normalPoolSize(50),
    useIOCP(false), maxWhitelistConnections(2000), maxNormalConnections(500),
    threadPoolMode(ThreadPoolMode::TRADITIONAL),
    enableDisconnectClear(true),  // 默认启用断开时清理GameID绑定
    nextTrafficRuleId(1),  // 🔥 新增：流量规则ID初始化
    needsSNISniffing(false),  // 🔥 优化：默认不需要SNI嗅探
    userFilterHttpPort(0),  // 🔥 用户滤镜HTTP端口初始化
    proxyPacketWriteIndex(0),  // 🔥 代理数据包记录索引初始化
    proxyPacketBufferSize(200),  // 🔥 默认200条
    proxyPacketRecordEnabled(false) {  // 🔥 默认关闭记录
    // 初始化防CC功能
    antiCC = std::make_unique<AntiCC>();
    // 初始化代理数据包缓冲区
    proxyPacketBuffer.resize(proxyPacketBufferSize);
    std::atomic_store(&disconnectRulesSnapshot, GetEmptyDisconnectRuleSnapshot());
}







PacketCollector::~PacketCollector() {
    Stop();
}

bool PacketCollector::Start() {
    if (isRunning) {
        Log("[警告] 采集器已在运行");
        SetLastErrorMessage_("采集器已在运行");
        return false;
    }

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        Log("[错误] WSAStartup失败");
        SetLastErrorMessage_("WSAStartup失败 (WSA=" + std::to_string(WSAGetLastError()) + ")");
        return false;
    }

    // IOCP模式需要使用WSASocket创建支持重叠IO的socket
    if (useIOCP) {
        listenSocket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
    }
    else {
        listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    }

    if (listenSocket == INVALID_SOCKET) {
        Log("[错误] 创建Socket失败");
        SetLastErrorMessage_("创建Socket失败 (WSA=" + std::to_string(WSAGetLastError()) + ")");
        WSACleanup();
        return false;
    }

    int reuse = 1;
    setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR,
        (char*)&reuse, sizeof(reuse));

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(listenPort);

    if (bind(listenSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        const int wsa = WSAGetLastError();
        Log("[错误] 绑定端口失败: " + std::to_string(listenPort));
        SetLastErrorMessage_("绑定端口失败: " + std::to_string(listenPort) + " (WSA=" + std::to_string(wsa) + ")");
        closesocket(listenSocket);
        WSACleanup();
        return false;
    }

    if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) {
        Log("[错误] 监听失败");
        SetLastErrorMessage_("监听失败 (WSA=" + std::to_string(WSAGetLastError()) + ")");
        closesocket(listenSocket);
        WSACleanup();
        return false;
    }

    isRunning = true;
    SetLastErrorMessage_("");

    // ===== SSL MITM: 初始化全局 CA =====
    if (sslMitmGlobalEnabled.load()) {
        SSLMitmContext::InitGlobalCA();
    }

    // ===== 🔥 修复1: 启动数据库异步写入线程 =====
    dbWriteThreadRunning = true;
    dbWriteThread = std::thread(&PacketCollector::DatabaseWriteThreadLoop, this);
    Log("[数据库] 异步写入线程已启动");

    // ===== 🔥 优化：启动SNI缓存清理线程 =====
    sniCleanupThreadRunning = true;
    sniCleanupThread = std::thread(&PacketCollector::SNICacheCleanupThreadLoop, this);
    Log("[SNI缓存] 清理线程已启动");

    // ===== 🔥 修复3: 启动多个回调处理线程 =====
    callbackThreadRunning = true;

    // 根据CPU核心数自动调整线程数（最多8个）
    if (callbackThreadCount <= 0) {
        SYSTEM_INFO sysInfo;
        GetSystemInfo(&sysInfo);
        callbackThreadCount = std::min(static_cast<int>(sysInfo.dwNumberOfProcessors), 8);
    }

    for (int i = 0; i < callbackThreadCount; i++) {
        callbackThreads.emplace_back(&PacketCollector::CallbackThreadLoop, this, i);
    }
    Log("[回调] 已启动 " + std::to_string(callbackThreadCount) + " 个回调处理线程");

    proxyPacketRealtimeThreadRunning = true;
    proxyPacketRealtimeThread = std::thread(&PacketCollector::ProxyPacketRealtimePushThreadLoop, this);

    disconnectCheckThreadRunning = true;
    disconnectCheckThread = std::thread(&PacketCollector::DisconnectCheckThreadLoop, this);
    Log("[断网] 断网检查线程已启动");

    // ===== 根据模式选择启动方式 =====
    if (useIOCP) {
        // ===== IOCP高性能模式 =====
        iocpPool = std::make_unique<IOCPThreadPool>();
        iocpPool->SetMaxConnections(maxWhitelistConnections, maxNormalConnections);

        // 设置实例ID
        iocpPool->SetInstanceId(m_instanceId);

        // 设置白名单检查回调
        if (antiCC) {
            iocpPool->SetWhitelistChecker([this](const std::string& ip) -> bool {
                return antiCC->IsInWhitelist(ip);
            });
        }

        // 🔥 同步SOCKS5认证标志到IOCP
        iocpPool->SetSocks5Auth(enableSocks5Auth);
        iocpPool->SetAntiCCAuthPriorityAdmissionEnabled(antiCCAuthPriorityAdmissionEnabled.load());
        iocpPool->SetAntiCCAuthPriorityQueueLimit(antiCCAuthPriorityQueueLimit.load());

        // 🔥 同步分包处理配置到IOCP
        iocpPool->SetPacketSplitEnabled(enablePacketSplit.load());
        iocpPool->SetPacketSplitPorts(GetPacketSplitPorts());

        // 🔥 同步"对不分包流量应用WPE滤镜"配置到IOCP
        iocpPool->SetApplyWpeOnNonSplitTraffic(applyWpeOnNonSplitTraffic.load());

        // 设置认证回调
        if (enableSocks5Auth) {
            iocpPool->SetAuthCallback([this](const std::string& username,
                const std::string& password,
                const std::string& clientIP,
                PacketCollector** outAccountStateOwner) -> bool {
                if (clientIP.empty() || !outAccountStateOwner) {
                    return this->ValidateAccount(username, password);
                }
                PacketCollector* accountStateOwner = nullptr;
                std::string failReason;
                const bool authSuccess = this->TryAcquireAuthenticatedSession(
                    username, password, clientIP, accountStateOwner, &failReason);
                if (!authSuccess && !failReason.empty()) {
                    Log("[SOCKS5-Auth][IOCP] 认证失败: " + username +
                        " [IP: " + clientIP + "] 原因: " + failReason);
                }
                if (outAccountStateOwner) {
                    *outAccountStateOwner = accountStateOwner;
                }
                return authSuccess;
            });
        }

        // 🔥🔥🔥 设置过滤检查回调（使用新的流量过滤系统，与传统模式一致）
        iocpPool->SetFilterChecker([this](const std::string& host, int port) -> bool {
            // 优先使用新的流量过滤系统
            if (IsTrafficFilterEnabled()) {
                // 在SOCKS5握手阶段还没有SNI数据，先用空字符串
                // SNI嗅探会在数据转发阶段进行
                std::string sniDomain = "";
                bool allowed = this->CheckTrafficRules(host, port, sniDomain);
                // CheckTrafficRules返回true表示允许，false表示拒绝
                // 但bypassModifier=true表示跳过处理（不采集），false表示需要处理（采集）
                // 所以如果流量被拒绝，应该返回true（跳过/过滤掉）
                return !allowed;  // 允许连接则返回false（需要处理），拒绝连接则返回true（跳过）
            }
            // 如果新系统未启用，回退到旧的过滤系统
            return this->ShouldFilterConnection(host, port);
        });

        // 设置SNI嗅探复检回调（补齐IOCP与传统模式一致行为）
        iocpPool->SetSNIRecheckCallback([this](ProxyConnectionInfo* conn, const std::vector<uint8_t>& probeData) -> bool {
            if (!conn || probeData.empty()) {
                return true;
            }

            if (!IsTrafficFilterEnabled() || !IsSNISniffingEnabled() || !needsSNISniffing) {
                return true;
            }

            std::string cacheKey = conn->targetHost + ":" + std::to_string(conn->targetPort);
            std::string sniDomain = GetFromSNICache(cacheKey);
            if (!sniDomain.empty()) {
                AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[SNI缓存][IOCP] 命中: " + cacheKey + " -> " + sniDomain);
            } else {
                sniDomain = ExtractSNIFromTLS(probeData);
                if (!sniDomain.empty()) {
                    UpdateSNICache(cacheKey, sniDomain);
                }
            }

            if (!sniDomain.empty()) {
                if (!CheckTrafficRules(conn->targetHost, conn->targetPort, sniDomain)) {
                    AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[流量过滤][IOCP] SNI嗅探后拒绝连接: " +
                        conn->targetHost + ":" + std::to_string(conn->targetPort) +
                        " (SNI: " + sniDomain + ")");
                    return false;
                }
            }

            return true;
        });

        iocpPool->SetRawDataObserver([this](ProxyConnectionInfo* conn, const std::vector<uint8_t>& data, bool isFromClient) -> bool {
            if (!conn || data.empty()) {
                return true;
            }

            if (isFromClient) {
                return !this->HandleDisconnectFeed(conn, data, DisconnectDirection::ClientToServer);
            }
            return !this->HandleDisconnectFeed(conn, data, DisconnectDirection::ServerToClient);
        });

        // 设置数据修改回调
        iocpPool->SetDataModifier([this](ProxyConnectionInfo* conn,
            const std::vector<uint8_t>& data,
            bool isFromClient) -> DataModifierResult {
            return this->OnIOCPDataModifier(conn, data, isFromClient);
        });

        // 设置连接事件回调
        iocpPool->SetConnectionEventCallback([this](ProxyConnectionInfo* conn, const std::string& event) {
            this->OnIOCPConnectionEvent(conn, event);
        });

        // 🔥🔥🔥 设置数据包接收回调（用于UI显示，与传统模式完全一致）
        if (onPacketReceived) {
            iocpPool->SetPacketReceivedCallback([this](const PacketInfo& info, const std::vector<uint8_t>& forwardedData, const std::vector<uint8_t>& callbackData) {
                // 统一走回调线程队列，避免重回调直接阻塞 IOCP 工作线程。
                CallbackTask task;
                task.packetInfo = info;
                task.forwardedData = forwardedData;
                task.callbackData = callbackData;
                this->EnqueueCallback(task);
            });
        }

        // 🔥🔥🔥 设置认证失败回调（与传统模式一致）
        if (antiCC && antiCC->IsEnabled()) {
            iocpPool->SetAuthFailureCallback([this](const std::string& clientIP) {
                this->antiCC->OnAuthFailure(clientIP);
                GlobalAntiCCCoordinator::GetInstance().RecordAuthFailure(this->m_instanceId, clientIP);
            });

            // 🔥🔥🔥 设置非SOCKS5连接检测回调（与传统模式一致）
            iocpPool->SetNonSocksChecker([this](SOCKET socket, const std::string& clientIP) -> bool {
                // 封禁非SOCKS5连接
                AntiCCConfig antiCCConfig = this->antiCC->GetConfig();
                GlobalAntiCCCoordinator::GetInstance().RecordProtocolViolation(
                    this->m_instanceId,
                    clientIP,
                    antiCCConfig.noAuthBanTime);
                this->antiCC->BanNonSocksConnection(clientIP);
                Log("[安全] 检测到非SOCKS5连接，已拒绝: " + clientIP);
                return false;  // 返回false表示不是有效连接
            });

            // 🔥🔥🔥 设置防CC检查回调（与传统模式一致）
            iocpPool->SetAntiCCChecker([this](const std::string& clientIP) -> bool {
                auto globalResult = GlobalAntiCCCoordinator::GetInstance().PreCheck(this->m_instanceId, clientIP);
                if (!globalResult.allowed) {
                    AB_LOG_WARNING_CAT(LOG_CAT_ANTICC, "[GlobalAntiCC] IOCP拒绝连接: " + clientIP +
                        " instance=" + this->m_instanceId);
                    return false;
                }
                if (this->iocpPool) {
                    this->antiCC->SetCurrentConnectionCount(this->iocpPool->GetTotalConnectionCount());
                }
                return this->antiCC->CheckAntiCC(clientIP);
            });
        }

        // 🔥🔥🔥 设置二级代理连接回调（与传统模式一致）
        {
            std::lock_guard<std::mutex> lock(secondaryProxyMutex);
            AB_LOG_INFO("[调试] IOCP启动时检查二级代理: enableSecondaryProxy=" + std::to_string(enableSecondaryProxy) +
                ", host=" + secondaryProxyHost + ", port=" + std::to_string(secondaryProxyPort));
            if (enableSecondaryProxy) {
                iocpPool->SetSecondaryProxyConnector([this](const std::string& targetHost, int targetPort) -> SOCKET {
                    return this->ConnectToSecondaryProxy(targetHost, targetPort);
                });
                AB_LOG_INFO("[IOCP] 二级代理模式已启用");
            } else {
                // 🔥 显式清除二级代理回调，确保不会使用二级代理
                iocpPool->SetSecondaryProxyConnector(nullptr);
                AB_LOG_INFO("[IOCP] 二级代理已禁用，已清除回调");
            }
        }

        // 🔥🔥🔥 设置服务器响应数据处理回调（用于WPE滤镜处理响应包）
        iocpPool->SetServerDataCallback([this](ProxyConnectionInfo* conn, std::vector<uint8_t>& data) -> ServerDataResult {
            return this->OnIOCPServerData(conn, data);
        });

        if (iocpPool->Start(listenSocket)) {
            Log("[成功] IOCP高性能模式已启动 (最大白名单连接:" + std::to_string(maxWhitelistConnections) +
                ", 最大普通连接:" + std::to_string(maxNormalConnections) + ")");
        }
        else {
            Log("[错误] IOCP模式启动失败，回退到传统模式");
            iocpPool.reset();
            useIOCP = false;
            // 启动传统服务器循环
            serverThread = std::thread(&PacketCollector::ServerLoop, this);
        }
    }
    else if (useThreadPool) {
        // ===== 阻塞式线程池模式 =====
        threadPool = std::make_unique<WhitelistThreadPool>(whitelistPoolSize, normalPoolSize);
        if (threadPool->Start([this](SOCKET sock, const std::string& addr, const std::string& ip) {
            this->HandleClientFromPool(sock, addr, ip);
        })) {
            Log("[成功] 白名单专用线程池已启动 (白名单:" + std::to_string(whitelistPoolSize) +
                ", 普通:" + std::to_string(normalPoolSize) + ")");
        }
        else {
            Log("[警告] 白名单线程池启动失败，将使用传统模式");
            threadPool.reset();
            useThreadPool = false;
        }
        serverThread = std::thread(&PacketCollector::ServerLoop, this);
    }
    else {
        // ===== 传统模式 =====
        serverThread = std::thread(&PacketCollector::ServerLoop, this);
    }

    Log("[成功] SOCKS5代理启动，监听端口: " + std::to_string(listenPort));
    return true;
}

void PacketCollector::Stop() {
    if (!isRunning) return;

    isRunning = false;

    disconnectCheckThreadRunning = false;
    disconnectCheckCV.notify_all();
    if (disconnectCheckThread.joinable()) {
        disconnectCheckThread.join();
        Log("[断网] 断网检查线程已停止");
    }

    // ===== 停止IOCP线程池 =====
    if (iocpPool) {
        iocpPool->Stop();
        iocpPool.reset();
        Log("[信息] IOCP线程池已停止");
    }

    // ===== 停止白名单线程池 =====
    if (threadPool) {
        threadPool->Stop();
        threadPool.reset();
        Log("[信息] 白名单线程池已停止");
    }

    // ===== 🔥 修复1: 停止数据库写入线程（确保所有数据写入完成）=====
    dbWriteThreadRunning = false;
    dbWriteCV.notify_all();
    if (dbWriteThread.joinable()) {
        dbWriteThread.join();
        Log("[数据库] 异步写入线程已停止，剩余队列大小: " + std::to_string(dbQueueSize.load()));
    }

    // ===== 🔥 优化：停止SNI缓存清理线程 =====
    sniCleanupThreadRunning = false;
    sniCleanupCV.notify_all();  // 唤醒清理线程
    if (sniCleanupThread.joinable()) {
        sniCleanupThread.join();
        Log("[SNI缓存] 清理线程已停止");
    }

    // ===== 🔥 修复3: 停止所有回调线程 =====
    callbackThreadRunning = false;
    callbackQueueCV.notify_all();  // 唤醒所有线程

    for (auto& thread : callbackThreads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    callbackThreads.clear();
    Log("[回调] 所有回调线程已停止，剩余队列: " + std::to_string(callbackQueueSize.load()));

    proxyPacketRealtimeThreadRunning = false;
    proxyPacketRealtimeCV.notify_all();
    if (proxyPacketRealtimeThread.joinable()) {
        proxyPacketRealtimeThread.join();
    }

    {
        std::lock_guard<std::mutex> lock(connectionsMutex);
        for (auto& conn : proxyConnections) {
            conn->isActive = false;
            if (conn->clientSocket != INVALID_SOCKET) {
                closesocket(conn->clientSocket);
            }
            if (conn->serverSocket != INVALID_SOCKET) {
                closesocket(conn->serverSocket);
            }
        }
    }

    if (listenSocket != INVALID_SOCKET) {
        closesocket(listenSocket);
        listenSocket = INVALID_SOCKET;
    }

    if (serverThread.joinable()) {
        serverThread.join();
    }

    {
        std::lock_guard<std::mutex> lock(connectionsMutex);
        for (auto& conn : proxyConnections) {
            if (conn->clientToServerThread.joinable()) {
                conn->clientToServerThread.join();
            }
            if (conn->serverToClientThread.joinable()) {
                conn->serverToClientThread.join();
            }
        }
        proxyConnections.clear();
    }

    {
        std::lock_guard<std::mutex> lock(deviceLimitMutex);
        activeDeviceIpRefCounts.clear();
    }

    // 🔥 修复：不调用 WSACleanup()，因为这会影响整个进程的所有网络连接
    // 多个实例共享同一个进程，不应该在单个实例停止时清理全局 Winsock
    // WSACleanup();
    Log("[信息] 代理已停止");
}

void PacketCollector::ServerLoop() {
    while (isRunning) {
        // 全局连接数限制检查（防止资源耗尽）
        {
            std::lock_guard<std::mutex> lock(connectionsMutex);
            if (proxyConnections.size() >= 1000) {  // 最大1000个并发连接
                Sleep(100);  // 等待现有连接释放
                continue;
            }
        }

        sockaddr_in clientAddr;
        int clientAddrSize = sizeof(clientAddr);

        SOCKET clientSocket = accept(listenSocket, (sockaddr*)&clientAddr, &clientAddrSize);

        if (clientSocket == INVALID_SOCKET) {
            if (isRunning) {
                Log("[错误] 接受连接失败");
            }
            continue;
        }

        char clientIP[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, clientIP, INET_ADDRSTRLEN);
        std::string clientAddrStr = std::string(clientIP) + ":" +
            std::to_string(ntohs(clientAddr.sin_port));
        std::string clientIPStr = clientIP;

        totalConnections++;

        // ===== 线程池模式：根据IP白名单状态分发到不同线程池 =====
        if (useThreadPool && threadPool && threadPool->IsRunning()) {
            // 检查IP是否在白名单中
            bool isWhitelisted = false;
            if (antiCC && antiCC->IsEnabled()) {
                isWhitelisted = antiCC->IsInWhitelist(clientIPStr);
            }

            if (isWhitelisted) {
                // 白名单IP：提交到白名单专用线程池
                if (threadPool->SubmitWhitelistConnection(clientSocket, clientAddrStr, clientIPStr)) {
                    Log("[线程池] 白名单连接已分配: " + clientIPStr);
                }
                else {
                    // 白名单队列满了，使用传统方式处理（确保白名单用户不被拒绝）
                    Log("[线程池] 白名单队列满，使用传统线程: " + clientIPStr);
                    std::thread(&PacketCollector::HandleClient, this, clientSocket, clientAddrStr).detach();
                }
            }
            else {
                // 普通IP：提交到普通线程池
                if (threadPool->SubmitNormalConnection(clientSocket, clientAddrStr, clientIPStr)) {
                    // 普通连接不记录日志，避免刷屏
                }
                else {
                    // 普通队列满了，直接拒绝连接
                    Log("[线程池] 普通队列满，拒绝连接: " + clientIPStr);
                    closesocket(clientSocket);
                }
            }
        }
        else {
            // ===== 传统模式：为每个连接创建新线程 =====
            Log("[连接] 新客户端: " + clientAddrStr);
            std::thread(&PacketCollector::HandleClient, this, clientSocket, clientAddrStr).detach();
        }
    }
}

// ===== 线程池连接处理入口（从线程池工作线程调用）=====
void PacketCollector::HandleClientFromPool(SOCKET clientSocket, const std::string& clientAddr, const std::string& clientIP) {
    // 调用原有的HandleClient逻辑
    HandleClient(clientSocket, clientAddr);
}

PacketCollector* PacketCollector::ResolveAccountStateOwner() const {
    PacketCollector* owner = externalAccountSource;
    PacketCollector* previous = const_cast<PacketCollector*>(this);
    int depth = 0;

    while (owner && owner != previous && depth < 8) {
        if (!owner->externalAccountSource || owner->externalAccountSource == owner) {
            return owner;
        }
        previous = owner;
        owner = owner->externalAccountSource;
        ++depth;
    }

    return owner ? owner : const_cast<PacketCollector*>(this);
}

bool PacketCollector::TryGetSharedAccountLeaseInfo(const std::string& username, SharedAccountLeaseInfo& outInfo) const {
    outInfo = SharedAccountLeaseInfo{};
    if (username.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(sharedAccountLeasesMutex);
    auto it = sharedAccountLeases.find(username);
    if (it == sharedAccountLeases.end()) {
        return false;
    }

    outInfo.ownerInstanceId = it->second.ownerInstanceId;
    outInfo.ownerDeviceKey = it->second.ownerDeviceKey;
    outInfo.refCount = it->second.refCount;
    return true;
}

bool PacketCollector::TryAcquireSharedAccountLease(const std::string& username,
    const std::string& instanceId, const std::string& deviceKey, std::string* outError) {
    if (outError) outError->clear();
    if (username.empty()) {
        if (outError) {
            *outError = "用户名为空，无法建立共享在线占用";
        }
        return false;
    }
    if (deviceKey.empty()) {
        if (outError) {
            *outError = "客户端IP为空，无法建立共享在线占用";
        }
        return false;
    }

    const auto now = std::chrono::system_clock::now();
    std::unique_lock<std::mutex> lock(sharedAccountLeasesMutex);

    auto it = sharedAccountLeases.find(username);
    if (it == sharedAccountLeases.end()) {
        SharedAccountLease lease;
        lease.ownerInstanceId = instanceId;
        lease.ownerDeviceKey = deviceKey;
        lease.refCount = 1;
        lease.firstAcquireTime = now;
        lease.lastActiveTime = now;
        sharedAccountLeases[username] = lease;

        Log("[SOCKS5-Auth] 共享账号占用建立: " + username +
            " [实例: " + instanceId + "] [设备: " + deviceKey + "]");
        return true;
    }

    SharedAccountLease& lease = it->second;
    const auto previousLastActiveTime = lease.lastActiveTime;
    const std::string previousOwnerInstanceId = lease.ownerInstanceId;
    const std::string previousOwnerDeviceKey = lease.ownerDeviceKey;
    lease.lastActiveTime = now;

    if (lease.ownerInstanceId == instanceId && lease.ownerDeviceKey == deviceKey) {
        lease.refCount++;
        Log("[SOCKS5-Auth] 共享账号占用复用: " + username +
            " [实例: " + instanceId + "] [设备: " + deviceKey +
            "] [引用: " + std::to_string(lease.refCount) + "]");
        return true;
    }

    const int liveConnections = CountAggregatedAuthenticatedConnectionsForUser(username);
    if (liveConnections <= 0 && (now - previousLastActiveTime) >= kSharedLeaseStaleWindow) {
        Log("[SOCKS5-Auth] 检测到共享账号僵尸占用，自动回收: " + username +
            " [旧实例: " + lease.ownerInstanceId + "] [旧设备: " + lease.ownerDeviceKey +
            "] [新实例: " + instanceId + "] [新设备: " + deviceKey + "]");
        lease.ownerInstanceId = instanceId;
        lease.ownerDeviceKey = deviceKey;
        lease.refCount = 1;
        lease.firstAcquireTime = now;
        lease.lastActiveTime = now;
        return true;
    }

    lease.ownerInstanceId = instanceId;
    lease.ownerDeviceKey = deviceKey;
    lease.refCount = 1;
    lease.firstAcquireTime = now;
    lease.lastActiveTime = now;

    Log("[SOCKS5-Auth] 抢占共享账号占用: " + username +
        " [旧实例: " + previousOwnerInstanceId + "] [旧设备: " + previousOwnerDeviceKey +
        "] [新实例: " + instanceId + "] [新设备: " + deviceKey + "]");

    lock.unlock();
    DisconnectAuthenticatedUserAcrossSharedCollectors(username, "新登录抢占旧会话");
    return true;
}

void PacketCollector::ReleaseSharedAccountLease(const std::string& username,
    const std::string& instanceId, const std::string& deviceKey) {
    if (username.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(sharedAccountLeasesMutex);
    auto it = sharedAccountLeases.find(username);
    if (it == sharedAccountLeases.end()) {
        return;
    }

    SharedAccountLease& lease = it->second;
    if (lease.ownerInstanceId != instanceId) {
        Log("[SOCKS5-Auth] 忽略共享账号释放(实例不匹配): " + username +
            " [释放实例: " + instanceId + "] [占用实例: " + lease.ownerInstanceId + "]");
        return;
    }
    if (!deviceKey.empty() && lease.ownerDeviceKey != deviceKey) {
        Log("[SOCKS5-Auth] 忽略共享账号释放(设备不匹配): " + username +
            " [释放设备: " + deviceKey + "] [占用设备: " + lease.ownerDeviceKey + "]");
        return;
    }

    if (lease.refCount > 1) {
        lease.refCount--;
        lease.lastActiveTime = std::chrono::system_clock::now();
        Log("[SOCKS5-Auth] 共享账号占用释放引用: " + username +
            " [实例: " + instanceId + "] [设备: " + lease.ownerDeviceKey +
            "] [剩余引用: " + std::to_string(lease.refCount) + "]");
        return;
    }

    Log("[SOCKS5-Auth] 共享账号占用释放完成: " + username +
        " [实例: " + instanceId + "] [设备: " + lease.ownerDeviceKey + "]");
    sharedAccountLeases.erase(it);
}

bool PacketCollector::TryAcquireAuthenticatedSession(const std::string& username, const std::string& password,
    const std::string& clientIP, PacketCollector*& accountStateOwner, std::string* outError) {
    if (outError) outError->clear();
    accountStateOwner = nullptr;

    PacketCollector* owner = ResolveAccountStateOwner();
    std::string failReason;
    if (!owner->ValidateAccountWithReason(username, password, failReason)) {
        if (outError) {
            *outError = failReason.empty() ? "账号或密码错误" : failReason;
        }
        return false;
    }

    const bool usingSharedAccountSource = (owner != this);
    const bool enforceSharedSingleDevicePolicy =
        usingSharedAccountSource && owner->IsSharedAccountSingleDeviceModeEnabled();
    bool sharedLeaseAcquired = false;
    if (enforceSharedSingleDevicePolicy) {
        std::string leaseError;
        if (!owner->TryAcquireSharedAccountLease(username, m_instanceId, clientIP, &leaseError)) {
            if (outError) {
                *outError = leaseError.empty() ? "该账号已被其他实例/设备占用" : leaseError;
            }
            return false;
        }
        sharedLeaseAcquired = true;
    }

    if (!owner->TryAcquireConnection(username)) {
        if (sharedLeaseAcquired) {
            owner->ReleaseSharedAccountLease(username, m_instanceId, clientIP);
        }
        if (outError) {
            *outError = "连接数已达上限";
        }
        return false;
    }

    std::string deviceLimitError;
    if (!TryAcquireDeviceSlot(clientIP, username, &deviceLimitError)) {
        if (sharedLeaseAcquired) {
            owner->ReleaseSharedAccountLease(username, m_instanceId, clientIP);
        }
        owner->ReleaseConnection(username);
        if (outError) {
            *outError = deviceLimitError.empty() ? "实例设备数已达上限" : deviceLimitError;
        }
        return false;
    }

    if (sharedLeaseAcquired) {
        SharedAccountLeaseInfo leaseInfo;
        if (!owner->TryGetSharedAccountLeaseInfo(username, leaseInfo) ||
            leaseInfo.ownerInstanceId != m_instanceId ||
            leaseInfo.ownerDeviceKey != clientIP) {
            ReleaseDeviceSlot(clientIP);
            owner->ReleaseConnection(username);
            owner->ReleaseSharedAccountLease(username, m_instanceId, clientIP);
            if (outError) {
                *outError = "该账号正在被其他登录请求抢占，请重试";
            }
            return false;
        }
    }

    owner->UpdateLoginInfo(username, clientIP);
    accountStateOwner = owner;
    return true;
}

void PacketCollector::ReleaseAuthenticatedSession(const std::string& username,
    const std::string& clientIP, PacketCollector* accountStateOwner) {
    if (username.empty() || !accountStateOwner) {
        return;
    }

    PacketCollector* owner = accountStateOwner;
    if (owner) {
        owner->ReleaseConnection(username);
        if (owner != this) {
            owner->ReleaseSharedAccountLease(username, m_instanceId, clientIP);
        }
    }

    ReleaseDeviceSlot(clientIP);
}


bool PacketCollector::HandleSocks5Handshake(SOCKET clientSocket, std::string& targetHost,
    int& targetPort, std::string& username, PacketCollector*& accountStateOwner,
    const std::string& clientIP) {
    char buffer[512];

    int n = recv(clientSocket, buffer, 2, 0);
    if (n < 2) {
        Log("[SOCKS5] 握手失败: 无法读取版本");
        return false;
    }

    uint8_t version = buffer[0];
    uint8_t nmethods = buffer[1];

    if (version != 5) {
        Log("[SOCKS5] 不支持的版本: " + std::to_string(version));
        return false;
    }

    // 验证nmethods长度，防止缓冲区溢出
    if (nmethods > 255 || nmethods > sizeof(buffer)) {
        Log("[SOCKS5] 非法的nmethods值: " + std::to_string(nmethods));
        return false;
    }

    n = recv(clientSocket, buffer, nmethods, 0);
    if (n < nmethods) {
        Log("[SOCKS5] 握手失败: 无法读取方法列表");
        return false;
    }

    uint8_t selectedMethod = 0x00;

    if (enableSocks5Auth) {
        bool supportsAuth = false;
        for (int i = 0; i < nmethods; i++) {
            if (buffer[i] == 0x02) {
                supportsAuth = true;
                break;
            }
        }

        if (supportsAuth) {
            selectedMethod = 0x02;
        }
        else {
            char response[2] = { 5, (char)0xFF };
            send(clientSocket, response, 2, 0);
            Log("[SOCKS5] 客户端不支持认证,拒绝连接");
            return false;
        }
    }

    char response[2] = { 5, (char)selectedMethod };
    send(clientSocket, response, 2, 0);

    if (selectedMethod == 0x02) {
        if (!HandleSocks5Auth(clientSocket, username, accountStateOwner, clientIP)) {
            Log("[SOCKS5] 认证失败 [IP: " + clientIP + "]");
            return false;
        }
        Log("[SOCKS5] 认证成功: " + username + " [IP: " + clientIP + "]");
    }

    n = recv(clientSocket, buffer, 4, 0);
    if (n < 4) {
        Log("[SOCKS5] 无法读取连接请求");
        return false;
    }

    uint8_t ver = buffer[0];
    uint8_t cmd = buffer[1];
    uint8_t atyp = buffer[3];

    if (ver != 5 || cmd != 1) {
        Log("[SOCKS5] 不支持的命令");
        return false;
    }

    if (atyp == 1) {
        n = recv(clientSocket, buffer, 4, 0);
        if (n < 4) return false;

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, buffer, ip, INET_ADDRSTRLEN);
        targetHost = ip;
    }
    else if (atyp == 3) {
        n = recv(clientSocket, buffer, 1, 0);
        if (n < 1) return false;

        uint8_t domainLen = buffer[0];

        // 验证域名长度，防止缓冲区溢出
        if (domainLen > 255 || domainLen > sizeof(buffer)) {
            Log("[SOCKS5] 非法的域名长度: " + std::to_string(domainLen));
            return false;
        }

        n = recv(clientSocket, buffer, domainLen, 0);
        if (n < domainLen) return false;

        targetHost = std::string(buffer, domainLen);
    }
    else if (atyp == 4) {
        Log("[SOCKS5] 暂不支持IPv6");
        return false;
    }
    else {
        Log("[SOCKS5] 未知地址类型");
        return false;
    }

    n = recv(clientSocket, buffer, 2, 0);
    if (n < 2) return false;

    targetPort = (static_cast<uint8_t>(buffer[0]) << 8) | static_cast<uint8_t>(buffer[1]);

    Log("[SOCKS5] 目标: " + targetHost + ":" + std::to_string(targetPort));

    // ===== 🔥 新增：流量过滤检查 =====
    if (IsTrafficFilterEnabled()) {
        // 暂时不进行SNI嗅探，因为此时还没有TLS数据
        // SNI嗅探将在ForwardClientToServer中进行
        std::string sniDomain = "";

        if (!CheckTrafficRules(targetHost, targetPort, sniDomain)) {
            // 流量被拒绝
            char reply[10] = { 5, 5, 0, 1, 0, 0, 0, 0, 0, 0 }; // 5 = Connection refused
            send(clientSocket, reply, 10, 0);
            Log("[流量过滤] 连接被拒绝: " + targetHost + ":" + std::to_string(targetPort));
            return false;
        }
    }

    char reply[10] = { 5, 0, 0, 1, 0, 0, 0, 0, 0, 0 };
    send(clientSocket, reply, 10, 0);

    return true;
}




SOCKET PacketCollector::ConnectToTarget(const std::string& host, int port) {
    SOCKET targetSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (targetSocket == INVALID_SOCKET) {
        Log("[错误] 创建目标Socket失败");
        return INVALID_SOCKET;
    }

    // 🔥 设置socket超时，防止阻塞（用于检测连接状态，不会真正断开）
    DWORD timeout = 30000;  // 30秒（增加超时时间，避免频繁超时）
    setsockopt(targetSocket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    setsockopt(targetSocket, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));

    // 🔥 启用 TCP Keepalive，保持长连接活跃
    BOOL keepalive = TRUE;
    setsockopt(targetSocket, SOL_SOCKET, SO_KEEPALIVE, (const char*)&keepalive, sizeof(keepalive));

    // 🔥 配置 Keepalive 参数（Windows）
    tcp_keepalive keepaliveParams;
    keepaliveParams.onoff = 1;                    // 启用
    keepaliveParams.keepalivetime = 60000;        // 60秒后开始发送keepalive探测
    keepaliveParams.keepaliveinterval = 10000;    // 每10秒发送一次探测
    DWORD bytesReturned;
    WSAIoctl(targetSocket, SIO_KEEPALIVE_VALS, &keepaliveParams, sizeof(keepaliveParams),
             nullptr, 0, &bytesReturned, nullptr, nullptr);

    sockaddr_in targetAddr;
    targetAddr.sin_family = AF_INET;
    targetAddr.sin_port = htons(port);

    // 尝试解析IP地址
    if (inet_pton(AF_INET, host.c_str(), &targetAddr.sin_addr) != 1) {
        // 如果不是IP，尝试DNS解析
        struct addrinfo hints = { 0 }, * result = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        if (getaddrinfo(host.c_str(), nullptr, &hints, &result) == 0) {
            targetAddr.sin_addr = ((sockaddr_in*)result->ai_addr)->sin_addr;
            freeaddrinfo(result);
        }
        else {
            Log("[错误] 无法解析主机名: " + host);
            closesocket(targetSocket);
            return INVALID_SOCKET;
        }
    }

    // 🔥 使用非阻塞模式连接，避免长时间阻塞
    u_long mode = 1;
    ioctlsocket(targetSocket, FIONBIO, &mode);

    // 尝试连接（非阻塞，会立即返回）
    int connectResult = connect(targetSocket, (sockaddr*)&targetAddr, sizeof(targetAddr));
    if (connectResult == SOCKET_ERROR) {
        int error = WSAGetLastError();
        if (error != WSAEWOULDBLOCK) {
            Log("[错误] 连接目标服务器失败: " + host + ":" + std::to_string(port) +
                " (错误码: " + std::to_string(error) + ")");
            closesocket(targetSocket);
            return INVALID_SOCKET;
        }

        // 使用select等待连接完成（最多10秒）
        fd_set writeSet, errorSet;
        FD_ZERO(&writeSet);
        FD_ZERO(&errorSet);
        FD_SET(targetSocket, &writeSet);
        FD_SET(targetSocket, &errorSet);

        timeval tv;
        tv.tv_sec = 10;   // 10秒超时
        tv.tv_usec = 0;

        int selectResult = select(0, nullptr, &writeSet, &errorSet, &tv);
        if (selectResult <= 0 || FD_ISSET(targetSocket, &errorSet)) {
            if (selectResult == 0) {
                Log("[错误] 连接目标服务器超时: " + host + ":" + std::to_string(port));
            } else {
                Log("[错误] 连接目标服务器失败: " + host + ":" + std::to_string(port));
            }
            closesocket(targetSocket);
            return INVALID_SOCKET;
        }
    }

    // 恢复为阻塞模式
    mode = 0;
    ioctlsocket(targetSocket, FIONBIO, &mode);

    Log("[成功] 已连接到目标服务器: " + host + ":" + std::to_string(port));
    return targetSocket;
}



void PacketCollector::HandleClient(SOCKET clientSocket, const std::string& clientAddr) {
    std::string targetHost;
    int targetPort;
    std::string authenticatedUser;
    PacketCollector* accountStateOwner = nullptr;

    // 提取客户端IP（去掉端口）
    std::string clientIP = clientAddr;
    size_t colonPos = clientIP.find(':');
    if (colonPos != std::string::npos) {
        clientIP = clientIP.substr(0, colonPos);
    }

    bool antiCCTrackedConnection = false;
    bool coordinatorTrackedConnection = false;
    bool authPrioritySlotAcquired = false;
    struct AntiCCConnectionGuard {
        PacketCollector* owner;
        std::string clientIP;
        bool* tracked;

        ~AntiCCConnectionGuard() {
            if (tracked && *tracked && owner && owner->antiCC && owner->antiCC->IsEnabled()) {
                owner->antiCC->OnConnectionClosed(clientIP);
            }
        }
    } antiCCGuard{ this, clientIP, &antiCCTrackedConnection };
    struct CoordinatorConnectionGuard {
        PacketCollector* owner;
        std::string clientIP;
        bool* tracked;

        ~CoordinatorConnectionGuard() {
            if (tracked && *tracked && owner) {
                GlobalAntiCCCoordinator::GetInstance().RecordConnectionClosed(owner->GetInstanceId(), clientIP);
            }
        }
    } coordinatorGuard{ this, clientIP, &coordinatorTrackedConnection };
    auto releaseAuthPrioritySlot = [this, &authPrioritySlotAcquired]() {
        if (authPrioritySlotAcquired) {
            antiCCAuthPriorityInFlight.fetch_sub(1);
            authPrioritySlotAcquired = false;
        }
    };

    auto& globalAntiCC = GlobalAntiCCCoordinator::GetInstance();
    auto globalPreCheck = globalAntiCC.PreCheck(m_instanceId, clientIP);
    if (!globalPreCheck.allowed) {
        AB_LOG_WARNING_CAT(LOG_CAT_ANTICC, "[GlobalAntiCC] 拒绝连接: " + clientIP +
            " instance=" + m_instanceId);
        closesocket(clientSocket);
        return;
    }

    bool authHint = false;
    if (antiCCAuthPriorityAdmissionEnabled.load() &&
        !globalPreCheck.instanceWhitelisted &&
        (globalPreCheck.globalPressure >= AntiCCPressureState::Busy ||
         globalPreCheck.instancePressure >= AntiCCPressureState::Busy)) {
        authHint = DetectPasswordAuthHint(clientSocket);
        if (authHint) {
            int currentInFlight = antiCCAuthPriorityInFlight.fetch_add(1) + 1;
            if (currentInFlight > antiCCAuthPriorityQueueLimit.load()) {
                antiCCAuthPriorityInFlight.fetch_sub(1);
                AB_LOG_WARNING_CAT(LOG_CAT_ANTICC, "[AntiCC] 认证优先名额已满，拒绝连接: " + clientIP +
                    " instance=" + m_instanceId);
                closesocket(clientSocket);
                return;
            }
            authPrioritySlotAcquired = true;
            globalAntiCC.RecordAuthHint(m_instanceId, clientIP);
        }
        else if (globalPreCheck.globalPressure >= AntiCCPressureState::Critical ||
                 globalPreCheck.instancePressure >= AntiCCPressureState::Critical) {
            AB_LOG_WARNING_CAT(LOG_CAT_ANTICC, "[GlobalAntiCC] 高压下拒绝非认证提示连接: " + clientIP +
                " instance=" + m_instanceId);
            closesocket(clientSocket);
            return;
        }
    }

    // 非SOCKS5连接检测（如果启用）
    if (antiCC && antiCC->IsEnabled()) {
        AntiCCConfig antiCCConfig = antiCC->GetConfig();
        if (antiCCConfig.blockNonSocks) {
            if (!antiCC->IsValidSocks5Connection(clientSocket, clientIP)) {
                Log("[安全] 检测到非SOCKS5连接，已拒绝: " + clientIP);
                globalAntiCC.RecordProtocolViolation(m_instanceId, clientIP, antiCCConfig.noAuthBanTime);
                antiCC->BanNonSocksConnection(clientIP);
                closesocket(clientSocket);
                return;
            }
        }
    }

    // 防CC检查
    if (antiCC && antiCC->IsEnabled()) {
        int currentConnections = 0;
        {
            std::lock_guard<std::mutex> lock(connectionsMutex);
            currentConnections = static_cast<int>(proxyConnections.size());
        }
        antiCC->SetCurrentConnectionCount(currentConnections);
        if (!antiCC->CheckAntiCC(clientIP)) {
            Log("[防CC] IP被拒绝: " + clientIP);
            closesocket(clientSocket);
            return;
        }
        antiCCTrackedConnection = true;
    }

    bool handshakeSuccess = HandleSocks5Handshake(
        clientSocket, targetHost, targetPort, authenticatedUser, accountStateOwner, clientIP);
    releaseAuthPrioritySlot();
    if (!handshakeSuccess) {
        Log("[错误] SOCKS5握手失败，来自: " + clientAddr);
        // 记录认证失败
        if (antiCC && antiCC->IsEnabled()) {
            antiCC->OnAuthFailure(clientIP);
        }
        globalAntiCC.RecordAuthFailure(m_instanceId, clientIP);
        // 🔥🔥🔥 关键修复：如果认证成功但后续握手失败，需要释放连接
        if (accountStateOwner) {
            ReleaseAuthenticatedSession(authenticatedUser, clientIP, accountStateOwner);
            Log("[SOCKS5] 握手失败，释放已认证用户连接: " + authenticatedUser);
        }
        closesocket(clientSocket);
        return;
    }

    // 记录认证成功
    if (antiCC && antiCC->IsEnabled() && !authenticatedUser.empty()) {
        antiCC->OnAuthSuccess(clientIP);
    }
    if (!authenticatedUser.empty()) {
        globalAntiCC.RecordAuthSuccess(m_instanceId, clientIP);
    }

    if (!authenticatedUser.empty() &&
        antiCCLowPriorityEvictionEnabled.load() &&
        (globalPreCheck.globalPressure >= AntiCCPressureState::Overloaded ||
         globalPreCheck.instancePressure >= AntiCCPressureState::Overloaded)) {
        DisconnectOneLowPriorityConnection_();
    }

    if (!authenticatedUser.empty()) {
        RecordDomainAccess(authenticatedUser, targetHost);
    }

    SOCKET serverSocket = INVALID_SOCKET;
    bool usingSecondaryProxy = false;

    {
        std::lock_guard<std::mutex> lock(secondaryProxyMutex);
        usingSecondaryProxy = enableSecondaryProxy;
        AB_LOG_INFO("[调试-新连接] 准备连接到 " + targetHost + ":" + std::to_string(targetPort) +
            ", 二级代理状态: " + std::string(enableSecondaryProxy ? "启用" : "禁用") +
            ", 二级代理地址: " + secondaryProxyHost + ":" + std::to_string(secondaryProxyPort));
    }

    if (usingSecondaryProxy) {
        Log("[调试] 准备通过二级代理连接: " + targetHost + ":" + std::to_string(targetPort));
        serverSocket = ConnectToSecondaryProxy(targetHost, targetPort);
        if (serverSocket == INVALID_SOCKET) {
            Log("[错误] 通过二级代理连接失败: " + targetHost + ":" + std::to_string(targetPort));
            ReleaseAuthenticatedSession(authenticatedUser, clientIP, accountStateOwner);
            closesocket(clientSocket);
            return;
        }
        Log("[二级代理] 已建立连接: " + clientAddr + " -> 二级代理 -> " +
            targetHost + ":" + std::to_string(targetPort));
    }
    else {
        serverSocket = ConnectToTarget(targetHost, targetPort);
        if (serverSocket == INVALID_SOCKET) {
            Log("[错误] 无法为客户端 " + clientAddr + " 建立代理连接到 " +
                targetHost + ":" + std::to_string(targetPort));
            ReleaseAuthenticatedSession(authenticatedUser, clientIP, accountStateOwner);
            closesocket(clientSocket);
            return;
        }
        Log("[直连] 已建立连接: " + clientAddr + " -> " +
            targetHost + ":" + std::to_string(targetPort));
    }

    auto conn = std::make_shared<ProxyConnection>();
    conn->connectionId = proxyConnectionIdCounter.fetch_add(1, std::memory_order_relaxed) + 1;
    conn->clientSocket = clientSocket;
    conn->serverSocket = serverSocket;
    conn->clientAddr = clientAddr;
    conn->clientIP = clientIP;
    conn->targetAddr = targetHost;
    conn->targetPort = targetPort;
    conn->isActive = true;
    conn->useSecondaryProxy = usingSecondaryProxy;
    conn->authenticatedUser = authenticatedUser;
    conn->accountStateOwner = accountStateOwner;
    conn->isWhitelistConnection = !authenticatedUser.empty();  // 🔥 认证用户标记为白名单连接
    conn->bypassModifier = ShouldFilterConnection(targetHost, targetPort);

    if (usingSecondaryProxy) {
        std::lock_guard<std::mutex> lock(secondaryProxyMutex);
        conn->secondaryProxyHost = secondaryProxyHost;
        conn->secondaryProxyPort = secondaryProxyPort;
    }

    static std::atomic<uint64_t> tempIdCounter(0);
    conn->gameID = "temp_" + std::to_string(++tempIdCounter);
    DisconnectRuleEngine::ResetRuntimeState(conn->disconnectState);

    {
        std::lock_guard<std::mutex> lock(connectionsMutex);
        proxyConnections.push_back(conn);
    }
    globalAntiCC.RecordConnectionOpened(m_instanceId, clientIP);
    coordinatorTrackedConnection = true;

    std::string userInfo = authenticatedUser.empty() ? "" : " [用户: " + authenticatedUser + "]";

    if (usingSecondaryProxy) {
        Log("[连接] " + clientAddr + " -> 二级代理(" +
            conn->secondaryProxyHost + ":" + std::to_string(conn->secondaryProxyPort) + ") -> " +
            targetHost + ":" + std::to_string(targetPort) + userInfo);
    }
    else {
        Log("[连接] " + clientAddr + " -> " +
            targetHost + ":" + std::to_string(targetPort) + userInfo);
    }

    if (conn->bypassModifier) {
        Log("[过滤] " + targetHost + ":" + std::to_string(targetPort) +
            " 不符合过滤条件，直接转发（不处理）");
    }
    else {
        Log("[处理] " + targetHost + ":" + std::to_string(targetPort) +
            " 符合过滤条件，将进行数据处理");
    }

    // ===== SSL MITM 握手 =====
    if (ShouldSSLMitm(targetHost, targetPort)) {
        auto sslCtx = std::make_shared<SSLMitmContext>();
        if (sslCtx->Handshake(clientSocket, serverSocket, targetHost)) {
            conn->sslMitmEnabled = true;
            conn->sslCtx = sslCtx;
            conn->sniHostname = targetHost;
            Log("[SSL-MITM] 握手成功: " + targetHost + ":" + std::to_string(targetPort));
        } else {
            Log("[SSL-MITM] 握手失败，降级为透传: " + targetHost + ":" + std::to_string(targetPort));
        }
    }

    conn->clientToServerThread = std::thread(&PacketCollector::ForwardClientToServer, this, conn);
    conn->serverToClientThread = std::thread(&PacketCollector::ForwardServerToClient, this, conn);

    if (conn->clientToServerThread.joinable()) {
        conn->clientToServerThread.join();
    }
    if (conn->serverToClientThread.joinable()) {
        conn->serverToClientThread.join();
    }

    // 断开连接回调：提供username/gameID（用于上层重置计数等）
    if (onUserDisconnected && !conn->authenticatedUser.empty()) {
        const std::string boundGameID = GetGameIDForUser(conn->authenticatedUser);
        onUserDisconnected(conn->authenticatedUser, boundGameID.empty() ? conn->gameID : boundGameID);
    }

    // 重置该用户的滤镜状态（如果有用户名）
    if (!conn->authenticatedUser.empty() && g_wpeFilterManager) {
        g_wpeFilterManager->ResetUserFilterStates(this->m_instanceId, conn->authenticatedUser);
    }

    // 🔥 断开连接时根据设置决定是否清理GameID绑定
    if (!conn->authenticatedUser.empty() && enableDisconnectClear) {
        ClearGameIDForUser(conn->authenticatedUser);
    }

    ReleaseAuthenticatedSession(conn->authenticatedUser, conn->clientIP, conn->accountStateOwner);

    if (conn->clientSocket != INVALID_SOCKET) {
        closesocket(conn->clientSocket);
        conn->clientSocket = INVALID_SOCKET;
    }
    if (conn->serverSocket != INVALID_SOCKET) {
        closesocket(conn->serverSocket);
        conn->serverSocket = INVALID_SOCKET;
    }

    // 🔥 修复4: 连接关闭时确保清理缓冲区（使用正确的锁）
    {
        std::lock_guard<std::mutex> lock(bufferMapMutex);
        buffers.erase(clientSocket);
    }

    {
        std::lock_guard<std::mutex> lock(connectionsMutex);
        proxyConnections.erase(
            std::remove_if(proxyConnections.begin(), proxyConnections.end(),
                [conn](const std::shared_ptr<ProxyConnection>& c) {
                    return c.get() == conn.get();
                }),
            proxyConnections.end()
                    );
    }

    if (usingSecondaryProxy) {
        Log("[断开] " + clientAddr + " -> 二级代理 -> " +
            targetHost + ":" + std::to_string(targetPort) +
            " [GameID: " + conn->gameID + "]" + userInfo);
    }
    else {
        Log("[断开] " + clientAddr + " -> " +
            targetHost + ":" + std::to_string(targetPort) +
            " [GameID: " + conn->gameID + "]" + userInfo);
    }

}














bool PacketCollector::TryHandleHttpLocalMapRequest(const std::shared_ptr<ProxyConnection>& conn,
    const std::vector<uint8_t>& data,
    std::vector<std::vector<uint8_t>>& passthroughRequests,
    bool& consumedLocally,
    bool& waitForMore) {

    passthroughRequests.clear();
    consumedLocally = false;
    waitForMore = false;

    if (!conn) {
        return true;
    }

    if (!httpLocalMapEnabled.load() || conn->httpLocalMapBypass) {
        if (!data.empty()) {
            passthroughRequests.push_back(data);
        }
        return true;
    }

    if (!data.empty()) {
        conn->httpLocalMapBuffer.insert(conn->httpLocalMapBuffer.end(), data.begin(), data.end());
    }

    if (conn->httpLocalMapBuffer.empty()) {
        waitForMore = true;
        return true;
    }

    if (conn->httpLocalMapBuffer.size() > kHttpLocalMapMaxBufferedBytes) {
        conn->httpLocalMapBypass = true;
        passthroughRequests.push_back(std::move(conn->httpLocalMapBuffer));
        conn->httpLocalMapBuffer.clear();
        return true;
    }

    ParsedHttpRequest request;
    bool needMore = false;
    if (!TryParseHttpRequest(conn->httpLocalMapBuffer, *conn, request, needMore)) {
        conn->httpLocalMapBypass = true;
        passthroughRequests.push_back(std::move(conn->httpLocalMapBuffer));
        conn->httpLocalMapBuffer.clear();
        return true;
    }

    if (needMore) {
        waitForMore = true;
        return true;
    }

    std::vector<uint8_t> requestBytes(
        conn->httpLocalMapBuffer.begin(),
        conn->httpLocalMapBuffer.begin() + static_cast<std::ptrdiff_t>(request.totalSize));
    conn->httpLocalMapBuffer.erase(
        conn->httpLocalMapBuffer.begin(),
        conn->httpLocalMapBuffer.begin() + static_cast<std::ptrdiff_t>(request.totalSize));

    const auto rules = GetHttpLocalMapRules();
    const std::string requestMethod = ToUpperAscii(request.method);
    const std::string requestScheme = ToLowerAscii(request.scheme);

    for (const auto& rule : rules) {
        if (!rule.enabled || rule.localFilePath.empty()) {
            continue;
        }

        const std::string ruleScheme = ToLowerAscii(rule.scheme);
        const std::string ruleMethod = ToUpperAscii(rule.method);

        if (ruleScheme != "*" && ruleScheme != requestScheme) {
            continue;
        }
        if (ruleMethod != "*" && ruleMethod != requestMethod) {
            continue;
        }
        if (!WildcardHostMatch(rule.hostPattern, request.host)) {
            continue;
        }
        if (!PathPatternMatch(rule.pathPattern, request.path)) {
            continue;
        }

        std::vector<uint8_t> responseBytes;
        if (!BuildLocalMapResponseBytes(rule, request, responseBytes)) {
            return false;
        }

        bool sendOk = false;
        if (conn->sslMitmEnabled && conn->sslCtx) {
            sendOk = conn->sslCtx->WriteToClient(responseBytes.data(), static_cast<int>(responseBytes.size()));
        } else {
            sendOk = SendRawBytes(conn->clientSocket, responseBytes.data(), responseBytes.size());
        }

        if (!sendOk) {
            return false;
        }

        AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[本地映射] 命中规则: " +
            requestScheme + "://" + request.host + request.path +
            " -> " + rule.localFilePath);

        consumedLocally = true;
        return true;
    }

    passthroughRequests.push_back(std::move(requestBytes));
    return true;
}


void PacketCollector::ForwardClientToServer(std::shared_ptr<ProxyConnection> conn) {
    char recvBuffer[8192];

    // ===== 🔥 优化：SNI嗅探标志（只在第一个数据包时尝试）=====
    bool sniChecked = false;

    while (conn->isActive && isRunning) {
        int bytesReceived;
        std::vector<uint8_t> data;

        if (conn->sslMitmEnabled && conn->sslCtx) {
            // SSL MITM 路径：从客户端读明文
            bytesReceived = conn->sslCtx->ReadFromClient(data);
        } else {
            bytesReceived = recv(conn->clientSocket, recvBuffer, sizeof(recvBuffer), 0);
            if (bytesReceived > 0)
                data.assign(recvBuffer, recvBuffer + bytesReceived);
        }

        if (bytesReceived <= 0) {
            // 🔥 区分真正的断开和超时
            if (bytesReceived == 0) {
                // 对端正常关闭连接
                AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[转发] 客户端正常关闭连接");
                conn->isActive = false;
                shutdown(conn->serverSocket, SD_SEND);
                break;
            } else {
                // bytesReceived < 0，检查错误类型
                if (conn->sslMitmEnabled) {
                    // SSL 错误直接断开
                    conn->isActive = false;
                    shutdown(conn->serverSocket, SD_SEND);
                    break;
                }
                int error = WSAGetLastError();
                if (error == WSAETIMEDOUT) {
                    continue;
                } else if (error == WSAECONNRESET || error == WSAECONNABORTED) {
                    AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[转发] 客户端连接被重置 (错误码: " + std::to_string(error) + ")");
                    conn->isActive = false;
                    shutdown(conn->serverSocket, SD_SEND);
                    break;
                } else {
                    AB_LOG_ERROR_CAT(LOG_CAT_COLLECTOR, "[转发] 接收数据失败 (错误码: " + std::to_string(error) + ")");
                    conn->isActive = false;
                    shutdown(conn->serverSocket, SD_SEND);
                    break;
                }
            }
        }

        if (HandleDisconnectFeed(conn, data, DisconnectDirection::ClientToServer)) {
            break;
        }

        RecordProxyPacket(
            conn->connectionId,
            conn->authenticatedUser,
            conn->gameID,
            conn->clientIP,
            conn->targetAddr,
            conn->sniHostname,
            conn->targetPort,
            conn->sslMitmEnabled,
            true,
            data);

        // ===== 🔥 优化：智能SNI嗅探（仅在需要时触发）=====
        if (!sniChecked && IsTrafficFilterEnabled() && IsSNISniffingEnabled() && needsSNISniffing) {
            sniChecked = true;

            // 先检查缓存
            std::string cacheKey = conn->targetAddr + ":" + std::to_string(conn->targetPort);
            std::string cachedSniDomain = GetFromSNICache(cacheKey);

            std::string sniDomain;
            if (!cachedSniDomain.empty()) {
                // 缓存命中
                sniDomain = cachedSniDomain;
                AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[SNI缓存] 命中: " + cacheKey + " -> " + sniDomain);
            } else {
                // 缓存未命中，尝试从TLS ClientHello中提取SNI
                sniDomain = ExtractSNIFromTLS(data);

                if (!sniDomain.empty()) {
                    // 更新缓存（使用LRU机制）
                    UpdateSNICache(cacheKey, sniDomain);
                }
            }

            // 如果提取到SNI域名，重新检查流量规则
            if (!sniDomain.empty()) {
                if (!CheckTrafficRules(conn->targetAddr, conn->targetPort, sniDomain)) {
                    AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[流量过滤] SNI嗅探后拒绝连接: " +
                        conn->targetAddr + ":" + std::to_string(conn->targetPort) +
                        " (SNI: " + sniDomain + ")");
                    conn->isActive = false;
                    shutdown(conn->serverSocket, SD_SEND);
                    break;
                }
            }
        }

        // ===== 🔥 关键修改：根据 bypassModifier 和分包处理配置决定处理方式 =====
        bool needPacketSplit = IsPacketSplitEnabledForPort(conn->targetPort);

        if (conn->bypassModifier || !needPacketSplit) {
            std::vector<std::vector<uint8_t>> pendingRequests;
            bool waitForMore = false;
            std::vector<uint8_t> localMapInput = data;

            while (true) {
                std::vector<std::vector<uint8_t>> parsedRequests;
                bool consumedLocally = false;
                bool localWaitForMore = false;
                if (!TryHandleHttpLocalMapRequest(conn, localMapInput, parsedRequests, consumedLocally, localWaitForMore)) {
                    AB_LOG_ERROR_CAT(LOG_CAT_COLLECTOR, "[本地映射] 处理请求失败，关闭连接");
                    conn->isActive = false;
                    shutdown(conn->serverSocket, SD_BOTH);
                    shutdown(conn->clientSocket, SD_SEND);
                    break;
                }

                for (auto& requestChunk : parsedRequests) {
                    pendingRequests.push_back(std::move(requestChunk));
                }

                if (consumedLocally) {
                    conn->isActive = false;
                    shutdown(conn->serverSocket, SD_BOTH);
                    shutdown(conn->clientSocket, SD_SEND);
                    break;
                }

                if (localWaitForMore || conn->httpLocalMapBuffer.empty() ||
                    conn->httpLocalMapBypass || !IsHttpLocalMapEnabled()) {
                    waitForMore = localWaitForMore;
                    break;
                }

                localMapInput.clear();
            }

            if (!conn->isActive) {
                break;
            }

            if (waitForMore && pendingRequests.empty()) {
                continue;
            }

            for (const auto& requestData : pendingRequests) {
                if (requestData.empty()) {
                    continue;
                }

                if (conn->bypassModifier) {
                    AB_LOG_WARNING_CAT(LOG_CAT_COLLECTOR, "[传统-bypass] 目标=" + conn->targetAddr + ":" +
                        std::to_string(conn->targetPort) + " 不匹配过滤条件，跳过数据处理(不触发回调) " +
                        std::to_string(requestData.size()) + "B");
                } else {
                    AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[传统-直通] 端口 " + std::to_string(conn->targetPort) +
                        " 不需要分包处理，直接转发 " + std::to_string(requestData.size()) + "B");
                }

                std::vector<uint8_t> dataToSend = requestData;
                bool shouldApplyWpe = !conn->bypassModifier && onPacketModifier &&
                                      (applyWpeOnNonSplitTraffic.load() || !enablePacketSplit.load());
                if (shouldApplyWpe) {
                    PacketInfo dummyPacket;
                    dummyPacket.gameID = conn->gameID;
                    dummyPacket.socksUsername = conn->authenticatedUser;

                    PacketTransformResult modResult = onPacketModifier(dummyPacket, requestData);
                    if (!modResult.forwardedData.empty()) {
                        dataToSend = modResult.forwardedData;
                    }
                    if (modResult.intercepted) {
                        AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[传统-直通] 数据被WPE滤镜拦截，不转发");
                        continue;
                    }
                }

                bool sendOk;
                if (conn->sslMitmEnabled && conn->sslCtx) {
                    sendOk = conn->sslCtx->WriteToServer(dataToSend.data(), (int)dataToSend.size());
                } else {
                    sendOk = SendDataComplete(conn->serverSocket, dataToSend, "客户端→服务器", conn->targetPort);
                }
                if (!sendOk) {
                    AB_LOG_ERROR_CAT(LOG_CAT_COLLECTOR, "[转发-直通] 发送失败，关闭连接");
                    conn->isActive = false;
                    shutdown(conn->clientSocket, SD_SEND);
                    break;
                }
            }

            if (!conn->isActive) {
                break;
            }

            if (pendingRequests.empty()) {
                continue;
            }
        }
        else {
            // ===== 符合过滤条件（10012端口）：进行粘包分包处理 =====

            // 🔥🔥🔥 关键修改：每次都尝试提取和绑定GameID
            std::string gameID = PacketParser::ExtractGameID(data);

            if (!gameID.empty()) {
                // 更新连接的GameID（如果是首次识别或GameID变化）
                bool isFirstTime = (conn->gameID.find("temp_") == 0);
                bool isChanged = (conn->gameID != gameID);

                if (isFirstTime || isChanged) {
                    conn->gameID = gameID;

                    if (isFirstTime) {
                        AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR,"[识别-处理] GameID: " + gameID +
                            " (" + conn->clientAddr + ") [将进行数据处理]");
                    }
                }

                // 🔥🔥🔥 关键修改：每次都尝试绑定（即使GameID相同）
                if (!conn->authenticatedUser.empty()) {
                    // 检查是否为00开头的ID
                    bool is00ID = (gameID.length() >= 2 &&
                        gameID[0] == '0' && gameID[1] == '0');

                    // 检查是否为_OB结尾的ID
                    bool isOBID = false;
                    if (gameID.length() >= 3) {
                        size_t len = gameID.length();
                        isOBID = (gameID.substr(len - 3) == "_OB");
                    }

                    // 只绑定非00开头且非_OB结尾的GameID
                    if (!is00ID && !isOBID) {
                        // ✅ 每次都调用绑定（函数内部会判断是否需要记录日志）
                        BindGameIDToUser(conn->authenticatedUser, gameID);
                    }
                }
            }

            // 处理缓冲区并获取完整数据包
            std::vector<std::vector<uint8_t>> completePackets =
                ProcessBuffer(conn->clientSocket, conn->gameID, conn->authenticatedUser, data, conn->bypassModifier);

            // 🔥🔥🔥 关键调试：如果没有提取到任何包，记录警告
            if (completePackets.empty()) {
                AB_LOG_WARNING_CAT(LOG_CAT_COLLECTOR, "[转发-处理] 收到" + std::to_string(data.size()) +
                    "B数据但未提取到完整包，可能在等待分包数据");
            }

            // 发送数据
            for (const auto& packet : completePackets) {
                // 允许 modifier 通过返回空包实现拦截/丢弃
                if (packet.empty()) {
                    continue;
                }
                bool pktSendOk;
                if (conn->sslMitmEnabled && conn->sslCtx) {
                    pktSendOk = conn->sslCtx->WriteToServer(packet.data(), (int)packet.size());
                } else {
                    pktSendOk = SendDataComplete(conn->serverSocket, packet, "客户端→服务器", conn->targetPort);
                }
                if (!pktSendOk) {
                    AB_LOG_ERROR_CAT(LOG_CAT_COLLECTOR,"[转发-处理] 发送失败，关闭连接");
                    conn->isActive = false;
                    shutdown(conn->clientSocket, SD_SEND);
                    break;
                }
            }
        }

        if (!conn->isActive) break;
    }
}






void PacketCollector::ForwardServerToClient(std::shared_ptr<ProxyConnection> conn) {
    char recvBuffer[8192];

    while (conn->isActive && isRunning) {
        int bytesReceived;
        std::vector<uint8_t> data;

        if (conn->sslMitmEnabled && conn->sslCtx) {
            // SSL MITM 路径：从服务器读明文
            bytesReceived = conn->sslCtx->ReadFromServer(data);
        } else {
            bytesReceived = recv(conn->serverSocket, recvBuffer, sizeof(recvBuffer), 0);
            if (bytesReceived > 0)
                data.assign(recvBuffer, recvBuffer + bytesReceived);
        }

        if (bytesReceived <= 0) {
            // 🔥 区分真正的断开和超时
            if (bytesReceived == 0) {
                AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[转发] 服务器正常关闭连接");
                conn->isActive = false;
                shutdown(conn->clientSocket, SD_SEND);
                break;
            } else {
                if (conn->sslMitmEnabled) {
                    conn->isActive = false;
                    shutdown(conn->clientSocket, SD_SEND);
                    break;
                }
                int error = WSAGetLastError();
                if (error == WSAETIMEDOUT) {
                    continue;
                } else if (error == WSAECONNRESET || error == WSAECONNABORTED) {
                    AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[转发] 服务器连接被重置 (错误码: " + std::to_string(error) + ")");
                    conn->isActive = false;
                    shutdown(conn->clientSocket, SD_SEND);
                    break;
                } else {
                    AB_LOG_ERROR_CAT(LOG_CAT_COLLECTOR, "[转发] 接收数据失败 (错误码: " + std::to_string(error) + ")");
                    conn->isActive = false;
                    shutdown(conn->clientSocket, SD_SEND);
                    break;
                }
            }
        }

        if (HandleDisconnectFeed(conn, data, DisconnectDirection::ServerToClient)) {
            break;
        }

        // 与请求方向保持一致：在传统模式下也先记录原始响应包，
        // 避免代理数据页只稳定记录请求而遗漏/误感知响应方向。
        RecordProxyPacket(
            conn->connectionId,
            conn->authenticatedUser,
            conn->gameID,
            conn->clientIP,
            conn->targetAddr,
            conn->sniHostname,
            conn->targetPort,
            conn->sslMitmEnabled,
            false,
            data);

        // ===== 🔥 关键修改：根据 bypassModifier 和分包处理配置决定处理方式 =====
        bool needPacketSplit = IsPacketSplitEnabledForPort(conn->targetPort);

        if (conn->bypassModifier || !needPacketSplit) {
            // 🔥 如果启用了"对不分包流量应用WPE滤镜"或分包处理完全禁用，则调用 onResponseModifier
            std::vector<uint8_t> dataToSend = data;
            bool shouldApplyWpe = !conn->bypassModifier && onResponseModifier &&
                                  (applyWpeOnNonSplitTraffic.load() || !enablePacketSplit.load());
            if (shouldApplyWpe) {
                PacketInfo dummyPacket;
                dummyPacket.gameID = conn->gameID;
                dummyPacket.socksUsername = conn->authenticatedUser;

                PacketTransformResult modResult = onResponseModifier(dummyPacket, data);
                if (!modResult.forwardedData.empty()) {
                    dataToSend = modResult.forwardedData;
                }
                // 如果被拦截，不转发
                if (modResult.intercepted) {
                    AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[传统-直通-响应] 数据被WPE滤镜拦截，不转发");
                    continue;
                }
            }

            // ===== 不符合过滤条件或不需要分包处理：直接转发 =====
            bool rspSendOk;
            if (conn->sslMitmEnabled && conn->sslCtx) {
                rspSendOk = conn->sslCtx->WriteToClient(dataToSend.data(), (int)dataToSend.size());
            } else {
                rspSendOk = SendDataComplete(conn->clientSocket, dataToSend, "服务器→客户端", conn->targetPort);
            }
            if (!rspSendOk) {
                AB_LOG_ERROR_CAT(LOG_CAT_COLLECTOR,"[转发-直通-响应] 发送失败，关闭连接");
                conn->isActive = false;
                shutdown(conn->serverSocket, SD_SEND);
                break;
            }
            continue;
        }

        // ===== 需要分包处理的流程 =====
        // 🔥 应用 WPE 滤镜（响应方向：服务器→客户端）
        if (!conn->bypassModifier && onResponseModifier) {
            PacketInfo packet;
            packet.gameID = conn->gameID;
            packet.socksUsername = conn->authenticatedUser;

            PacketTransformResult modResult = onResponseModifier(packet, data);

            // 如果被拦截，不转发
            if (modResult.intercepted) {
                AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[传统-响应] 数据被WPE滤镜拦截，不转发");
                continue;
            }

            // 如果数据被修改，使用修改后的数据
            if (!modResult.forwardedData.empty()) {
                data = modResult.forwardedData;
            }
        }

        // ========== 发送���客户端 ==========
        bool s2cSendOk;
        if (conn->sslMitmEnabled && conn->sslCtx) {
            s2cSendOk = conn->sslCtx->WriteToClient(data.data(), (int)data.size());
        } else {
            s2cSendOk = SendDataComplete(conn->clientSocket, data, "服务器→客户端", conn->targetPort);
        }
        if (!s2cSendOk) {
            AB_LOG_ERROR_CAT(LOG_CAT_COLLECTOR,"[转发] 发送失败，关闭连接");
            conn->isActive = false;
            shutdown(conn->serverSocket, SD_SEND);
            break;
        }
    }
}




// ===== 修复2: 细粒度锁优化的 ProcessBuffer =====
std::vector<std::vector<uint8_t>> PacketCollector::ProcessBuffer(
    SOCKET socket,
    const std::string& gameID,
    const std::string& socksUsername,
    const std::vector<uint8_t>& newData,
    bool bypassModifier)
{
    std::vector<std::vector<uint8_t>> completePackets;
    std::vector<CallbackTask> tasksToEnqueue;

    // 🔥 修复2: 第一阶段 - 获取连接缓冲区引用（最小锁范围）
    ConnectionBuffer* connBuf = nullptr;
    {
        std::lock_guard<std::mutex> lock(bufferMapMutex);
        connBuf = &buffers[socket];  // 获取引用，不复制
    }

    // 🔥 修复2: 第二阶段 - 只锁该连接的缓冲区（其他连接不受影响）
    {
        std::lock_guard<std::mutex> lock(connBuf->bufferMutex);

        totalBytes += newData.size();

        if (!connBuf->gameIDExtracted && !gameID.empty() && gameID.find("temp_") != 0) {
            connBuf->gameID = gameID;
            connBuf->gameIDExtracted = true;
        }

        bool hadBufferedData = (connBuf->buffer.size() > 0);

        // 限制缓冲区大小，防止内存耗尽攻击
        const size_t MAX_BUFFER_SIZE = 1024 * 1024;  // 最大1MB
        if (connBuf->buffer.size() + newData.size() > MAX_BUFFER_SIZE) {
            Log("[错误] 缓冲区超过限制，可能遭受攻击，断开连接 [Socket: " +
                std::to_string(socket) + "]");
            connBuf->buffer.clear();  // 清空缓冲区
            return {};  // 返回空，触发连接关闭
        }

        connBuf->buffer.insert(connBuf->buffer.end(), newData.begin(), newData.end());
        connBuf->fragmentCount++;

        int currentFragmentCount = connBuf->fragmentCount;
        int packetsExtractedThisTime = 0;

        // 处理所有完整的包
        while (true) {
            if (connBuf->buffer.size() < 5) {
                break;
            }

            uint32_t totalLength = PacketParser::ParseHeader(connBuf->buffer);

            // 🔥🔥🔥 关键修复：处理包头解析失败的情况
            // 如果 totalLength == 0，说明包头格式不正确，直接转发原始数据
            if (totalLength == 0) {
                AB_LOG_WARNING_CAT(LOG_CAT_COLLECTOR, "[ProcessBuffer] 包头解析失败(长度=0)，直接转发原始数据 " +
                    std::to_string(connBuf->buffer.size()) + "B");
                // 将整个缓冲区作为一个包转发
                completePackets.push_back(connBuf->buffer);
                connBuf->buffer.clear();
                connBuf->fragmentCount = 0;
                break;
            }

            // 🔥🔥🔥 关键修复：如果解析出的长度明显不合理（超过64KB），直接转发
            if (totalLength > 65535) {
                AB_LOG_WARNING_CAT(LOG_CAT_COLLECTOR, "[ProcessBuffer] 包头长度异常(" +
                    std::to_string(totalLength) + ")，直接转发原始数据 " +
                    std::to_string(connBuf->buffer.size()) + "B");
                completePackets.push_back(connBuf->buffer);
                connBuf->buffer.clear();
                connBuf->fragmentCount = 0;
                break;
            }

            // 等待更多数据（正常的分包情况）
            if (connBuf->buffer.size() < totalLength) {
                break;
            }

            // 提取完整包
            std::vector<uint8_t> completePacket(connBuf->buffer.begin(),
                connBuf->buffer.begin() + totalLength);
            connBuf->buffer.erase(connBuf->buffer.begin(),
                connBuf->buffer.begin() + totalLength);

            // 解析数据包信息
            PacketInfo packetInfo = PacketParser::ParsePacket(completePacket);

            if (!connBuf->gameID.empty()) {
                packetInfo.gameID = connBuf->gameID;
            }

            // 🔥 设置SOCKS用户名（用于按账号索引替换数据）
            packetInfo.socksUsername = socksUsername;

            // 判断数据包处理类型
            bool wasFragmented = false;
            bool wasMultiPacket = false;

            if (hadBufferedData || currentFragmentCount > 1) {
                wasFragmented = true;
            }

            if (packetsExtractedThisTime > 0 || connBuf->buffer.size() > 0) {
                wasMultiPacket = true;
            }

            if (wasFragmented && wasMultiPacket) {
                packetInfo.processType = PACKET_BOTH;
                fragmentedPackets++;
                multiPackets++;
            }
            else if (wasFragmented) {
                packetInfo.processType = PACKET_FRAGMENTED;
                fragmentedPackets++;
            }
            else if (wasMultiPacket) {
                packetInfo.processType = PACKET_MULTI;
                multiPackets++;
            }
            else {
                packetInfo.processType = PACKET_NORMAL;
            }

            packetInfo.fragmentCount = currentFragmentCount;
            packetsExtractedThisTime++;
            totalPackets++;

            // ===== 在锁内调用modifier，确保数据修改的原子性 =====
            std::vector<uint8_t> packetToForward = completePacket;
            std::vector<uint8_t> callbackData = packetToForward;

            // ===== 修改：只有不跳过时才调用 modifier =====
            if (onPacketModifier && !bypassModifier) {
                // 调用外部设置的 modifier
                PacketTransformResult transform = onPacketModifier(packetInfo, completePacket);
                packetToForward = std::move(transform.forwardedData);
                callbackData = transform.callbackData.empty() ? packetToForward : std::move(transform.callbackData);
            }

            // 添加到转发列表
            completePackets.push_back(packetToForward);

            // ===== 收集回调任务（深拷贝数据，避免引用失效）=====
            if (onPacketReceived) {
                CallbackTask task;
                task.packetInfo = packetInfo;           // 拷贝PacketInfo
                task.forwardedData = packetToForward;   // 拷贝实际转发的数据
                task.callbackData = callbackData;       // 拷贝回调侧数据
                tasksToEnqueue.push_back(task);
            }
        }

        if (connBuf->buffer.size() < 5) {
            connBuf->fragmentCount = 0;
        }
    }  // 🔥 修复2: 释放连接缓冲区锁

    // ===== 🔥 修复2&3: 第三阶段 - 在锁外将回调任务加入队列 =====
    for (const auto& task : tasksToEnqueue) {
        EnqueueCallback(task);
    }

    return completePackets;
}







// ===== 🔥 修复3: 多线程回调处理函数 =====
void PacketCollector::CallbackThreadLoop(int threadId) {
    Log("[回调线程-" + std::to_string(threadId) + "] 已启动");

    while (callbackThreadRunning) {
        CallbackTask task;
        bool hasTask = false;

        {
            std::unique_lock<std::mutex> lock(callbackQueueMutex);

            // 等待任务或停止信号
            callbackQueueCV.wait(lock, [this] {
                return !callbackQueue.empty() || !callbackThreadRunning;
                });

            // 检查是否应该退出
            if (!callbackThreadRunning && callbackQueue.empty()) {
                break;
            }

            // 取出任务
            if (!callbackQueue.empty()) {
                task = std::move(callbackQueue.front());
                callbackQueue.pop();
                callbackQueueSize.fetch_sub(1);  // 🔥 修复3: 更新队列大小
                hasTask = true;
            }
        }

        // ===== 在锁外执行回调 =====
        if (hasTask && onPacketReceived) {
            try {
                onPacketReceived(task.packetInfo, task.forwardedData, task.callbackData);
            }
            catch (const std::exception& e) {
                Log("[回调线程-" + std::to_string(threadId) + "] 执行异常: " + std::string(e.what()));
            }
            catch (...) {
                Log("[回调线程-" + std::to_string(threadId) + "] 执行未知异常");
            }
        }
    }

    Log("[回调线程-" + std::to_string(threadId) + "] 已退出");
}

// ===== 🔥 修复3: 改进的回调任务入队 =====
void PacketCollector::EnqueueCallback(const CallbackTask& task) {
    {
        std::lock_guard<std::mutex> lock(callbackQueueMutex);
        callbackQueue.push(task);
        callbackQueueSize.fetch_add(1);  // 🔥 修复3: 更新队列大小

        // 🔥 修复3: 只在队列极大时警告，不丢弃数据
        size_t currentSize = callbackQueueSize.load();
        if (currentSize > 10000) {
            static std::atomic<int> warnCount{ 0 };
            if (++warnCount % 100 == 0) {  // 每100次警告记录一次
                AB_LOG_WARNING("[回调队列] 积压严重: " + std::to_string(currentSize) + " 条待处理");
            }
        }
    }
    callbackQueueCV.notify_one();  // 唤醒一个等待的回调线程
}

std::vector<std::string> PacketCollector::GetActiveConnections() {
    std::lock_guard<std::mutex> lock(connectionsMutex);
    std::vector<std::string> result;

    for (const auto& conn : proxyConnections) {
        if (conn->isActive) {
            std::string connInfo = conn->clientAddr + " -> " +
                conn->targetAddr + ":" + std::to_string(conn->targetPort) +
                " [" + conn->gameID + "]";
            result.push_back(connInfo);
        }
    }

    return result;
}

int PacketCollector::CountLocalAuthenticatedConnectionsForUser(const std::string& username) const {
    if (username.empty()) {
        return 0;
    }

    int count = 0;

    if (iocpPool) {
        auto connections = iocpPool->GetAllConnections();
        for (const auto& conn : connections) {
            if (conn && conn->isActive && conn->authenticatedUser == username) {
                ++count;
            }
        }
    }

    std::lock_guard<std::mutex> lock(connectionsMutex);
    for (const auto& conn : proxyConnections) {
        if (conn && conn->isActive && conn->authenticatedUser == username) {
            ++count;
        }
    }

    return count;
}

int PacketCollector::CountAggregatedAuthenticatedConnectionsForUser(const std::string& username) const {
    int total = CountLocalAuthenticatedConnectionsForUser(username);

    std::vector<PacketCollector*> consumers;
    {
        std::lock_guard<std::mutex> lock(externalAccountConsumersMutex);
        consumers = externalAccountConsumers;
    }

    for (auto* consumer : consumers) {
        if (!consumer || consumer == this) {
            continue;
        }
        total += consumer->CountLocalAuthenticatedConnectionsForUser(username);
    }

    return total;
}

void PacketCollector::RegisterExternalAccountConsumer(PacketCollector* consumer) {
    if (!consumer || consumer == this) {
        return;
    }

    std::lock_guard<std::mutex> lock(externalAccountConsumersMutex);
    if (std::find(externalAccountConsumers.begin(), externalAccountConsumers.end(), consumer) == externalAccountConsumers.end()) {
        externalAccountConsumers.push_back(consumer);
    }
}

void PacketCollector::UnregisterExternalAccountConsumer(PacketCollector* consumer) {
    if (!consumer) {
        return;
    }

    std::lock_guard<std::mutex> lock(externalAccountConsumersMutex);
    externalAccountConsumers.erase(
        std::remove(externalAccountConsumers.begin(), externalAccountConsumers.end(), consumer),
        externalAccountConsumers.end()
    );
}

std::vector<PacketCollector*> PacketCollector::SnapshotSharedAccountSessionCollectors() const {
    std::vector<PacketCollector*> collectors;
    collectors.push_back(const_cast<PacketCollector*>(this));

    std::lock_guard<std::mutex> lock(externalAccountConsumersMutex);
    for (auto* consumer : externalAccountConsumers) {
        if (!consumer) {
            continue;
        }
        if (std::find(collectors.begin(), collectors.end(), consumer) == collectors.end()) {
            collectors.push_back(consumer);
        }
    }

    return collectors;
}

void PacketCollector::DisconnectAuthenticatedUserAcrossSharedCollectors(const std::string& username, const std::string& reason) {
    if (username.empty()) {
        return;
    }

    const auto collectors = SnapshotSharedAccountSessionCollectors();
    int disconnectedCollectors = 0;
    for (auto* collector : collectors) {
        if (!collector) {
            continue;
        }
        if (collector->DisconnectUser(username, reason)) {
            disconnectedCollectors++;
        }
    }

    Log("[SOCKS5-Auth] 共享账号抢占断连完成: " + username +
        " [原因: " + reason + "] [命中实例数: " + std::to_string(disconnectedCollectors) + "]");
}

void PacketCollector::SetFilter(FilterType type, const std::string& value) {
    std::lock_guard<std::mutex> lock(filterMutex);
    filterConfig.type = type;
    filterConfig.value = value;

    std::string typeStr;
    switch (type) {
    case FILTER_IP: typeStr = "IP"; break;
    case FILTER_DOMAIN: typeStr = "域名"; break;
    case FILTER_PORT: typeStr = "端口"; break;
    default: typeStr = "无"; break;
    }

    if (type != FILTER_NONE) {
        Log("[过滤] 已设置过滤: " + typeStr + " = " + value);
    }
}

void PacketCollector::ClearFilter() {
    std::lock_guard<std::mutex> lock(filterMutex);
    filterConfig.type = FILTER_NONE;
    filterConfig.value = "";
    Log("[过滤] 已清除过滤");
}

FilterConfig PacketCollector::GetFilter() const {
    std::lock_guard<std::mutex> lock(filterMutex);
    return filterConfig;
}

bool PacketCollector::ShouldFilterConnection(const std::string& host, int port) {
    std::lock_guard<std::mutex> lock(filterMutex);

    if (filterConfig.type == FILTER_NONE) {
        return false;  // 不过滤，允许连接
    }

    switch (filterConfig.type) {
    case FILTER_IP:
        if (host == filterConfig.value) {
            return false;  // 匹配成功，不过滤（采集）
        }
        return true;  // 不匹配，过滤掉

    case FILTER_DOMAIN:
        if (host.find(filterConfig.value) != std::string::npos) {
            return false;  // 匹配成功，不过滤（采集）
        }
        return true;  // 不匹配，过滤掉

    case FILTER_PORT:
        if (std::to_string(port) == filterConfig.value) {
            return false;  // 匹配成功，不过滤（采集）
        }
        return true;  // 不匹配，过滤掉

    default:
        return false;
    }
}



// ===== SSL MITM: 域名通配匹配 =====
static bool DomainMatch(const std::string& pattern, const std::string& host) {
    if (pattern.empty()) return false;
    if (pattern[0] == '*') {
        // *.example.com 匹配 foo.example.com
        std::string suffix = pattern.substr(1); // ".example.com"
        if (host.size() < suffix.size()) return false;
        return host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0;
    }
    return pattern == host;
}

bool PacketCollector::ShouldSSLMitm(const std::string& host, int port) const {
    if (!sslMitmGlobalEnabled.load()) return false;
    std::lock_guard<std::mutex> lk(sslMitmMutex);
    for (const auto& rule : sslMitmRules) {
        if (rule.matchByPort) {
            if (rule.port == port) return true;
        } else {
            if (DomainMatch(rule.domain, host)) return true;
        }
    }
    return false;
}

bool PacketCollector::SendDataComplete(SOCKET targetSocket,
    const std::vector<uint8_t>& data,
    const std::string& direction,
    int targetPort) {
    if (data.empty()) {
        AB_LOG_WARNING_CAT(LOG_CAT_COLLECTOR,"[发送] 尝试发送空包，已跳过 [" + direction + "]");
        return true;
    }

    size_t totalSize = data.size();

    // ===== 解析GameID =====
    std::string gameID = PacketParser::ExtractGameID(data);
    if (gameID.empty()) {
        gameID = "未知";
    }

    // ===== 判断是否需要记录日志（只记录客户端→服务器）=====
    bool shouldLog = (direction.find("客户端→服务器") != std::string::npos);

    // ===== 🔥 只在启用分包处理的端口才解析包头 =====
    bool needPacketSplit = IsPacketSplitEnabledForPort(targetPort);

    // ===== 发送前计算包大小并记录 =====
    if (shouldLog && needPacketSplit && data.size() >= 5) {
        uint16_t headerSize = (static_cast<uint16_t>(data[3]) << 8) |
            static_cast<uint16_t>(data[4]);

        std::string logPrefix = "[发送|" + gameID + "] ";

        if (headerSize == totalSize) {
            AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR,logPrefix + "准备发送 | " +
                "包头:" + std::to_string(headerSize) + "B | " +
                "实际:" + std::to_string(totalSize) + "B | " +
                "[" + direction + "] [正常]");
        }
        else {
            AB_LOG_WARNING_CAT(LOG_CAT_COLLECTOR,logPrefix + "大小不一致 | " +
                "包头:" + std::to_string(headerSize) + "B != " +
                "实际:" + std::to_string(totalSize) + "B | " +
                "[" + direction + "] [异常]");
        }
    }
    else if (shouldLog && needPacketSplit && data.size() < 5) {
        AB_LOG_WARNING_CAT(LOG_CAT_COLLECTOR,"[发送|" + gameID + "] " +
            "数据过短(" + std::to_string(totalSize) + "B) | " +
            "[" + direction + "]");
    }

    size_t totalSent = 0;
    int retryCount = 0;
    const int MAX_RETRIES = 3;

    while (totalSent < totalSize) {
        size_t remaining = totalSize - totalSent;

        int bytesSent = send(targetSocket,
            reinterpret_cast<const char*>(data.data() + totalSent),
            static_cast<int>(remaining),
            0);

        if (bytesSent == SOCKET_ERROR) {
            int error = WSAGetLastError();

            if (error == WSAEWOULDBLOCK) {
                retryCount++;
                if (retryCount > MAX_RETRIES) {
                    if (shouldLog) {
                        AB_LOG_ERROR_CAT(LOG_CAT_COLLECTOR,"[发送] 缓冲区持续阻塞，重试次数超限 [" + direction + "]");
                    }
                    return false;
                }

                if (shouldLog) {
                    AB_LOG_WARNING_CAT(LOG_CAT_COLLECTOR,"[发送] 缓冲区满，等待重试(" +
                        std::to_string(retryCount) + "/" +
                        std::to_string(MAX_RETRIES) + ") [" + direction + "]");
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(10 * retryCount));
                continue;
            }
            else if (error == WSAECONNRESET || error == WSAECONNABORTED) {
                if (shouldLog) {
                    AB_LOG_ERROR_CAT(LOG_CAT_COLLECTOR,"[发送] 连接已断开 [" + direction + "]，错误码: " + std::to_string(error));
                }
                return false;
            }
            else {
                if (shouldLog) {
                    AB_LOG_ERROR_CAT(LOG_CAT_COLLECTOR,"[发送] 发送失败 [" + direction + "]，错误码: " + std::to_string(error) +
                        "，已发送: " + std::to_string(totalSent) + "/" + std::to_string(totalSize));
                }
                return false;
            }
        }
        else if (bytesSent == 0) {
            if (shouldLog) {
                AB_LOG_WARNING_CAT(LOG_CAT_COLLECTOR,"[发送] send返回0，连接可能异常 [" + direction + "]");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            retryCount++;
            if (retryCount > MAX_RETRIES) {
                if (shouldLog) {
                    AB_LOG_ERROR_CAT(LOG_CAT_COLLECTOR,"[发送] send持续返回0，放弃发送 [" + direction + "]");
                }
                return false;
            }
            continue;
        }
        else {
            totalSent += bytesSent;
            retryCount = 0;
        }
    }

    // ===== 发送成功（只记录客户端→服务器）=====
    if (totalSent == totalSize) {
        if (shouldLog) {
            std::string logPrefix = "[发送|" + gameID + "] ";
            AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR,logPrefix + "成功发送 " +
                std::to_string(totalSize) + "B [" + direction + "]");
        }
        return true;
    }
    else {
        if (shouldLog) {
            AB_LOG_ERROR_CAT(LOG_CAT_COLLECTOR,"[发送] 不完整: " +
                std::to_string(totalSent) + "/" +
                std::to_string(totalSize) + "B [" + direction + "]");
        }
        return false;
    }
}





// ===== 通过二级SOCKS5代理连接目标服务器 =====
SOCKET PacketCollector::ConnectToSecondaryProxy(const std::string& targetHost, int targetPort) {
    std::lock_guard<std::mutex> lock(secondaryProxyMutex);

    AB_LOG_INFO("[调试] ConnectToSecondaryProxy 被调用: 目标=" + targetHost + ":" + std::to_string(targetPort) +
        ", 二级代理=" + secondaryProxyHost + ":" + std::to_string(secondaryProxyPort));

    // ===== 步骤1: 验证二级代理配置 =====
    if (!enableSecondaryProxy || secondaryProxyHost.empty() || secondaryProxyPort == 0) {
        AB_LOG_ERROR("[错误] 二级代理配置无效: enableSecondaryProxy=" + std::to_string(enableSecondaryProxy) +
            ", host=" + secondaryProxyHost + ", port=" + std::to_string(secondaryProxyPort));
        return INVALID_SOCKET;
    }

    // ===== 步骤2: 创建套接字并连接到二级代理 =====
    SOCKET proxySocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (proxySocket == INVALID_SOCKET) {
        AB_LOG_ERROR("[错误] 创建二级代理Socket失败");
        return INVALID_SOCKET;
    }

    // 🔥 关键修复：设置socket超时，防止阻塞（用于检测连接状态，不会真正断开）
    // 设置连接超时为30秒
    DWORD timeout = 30000;  // 30秒（增加超时时间，避免频繁超时）
    setsockopt(proxySocket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    setsockopt(proxySocket, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));

    // 🔥 启用 TCP Keepalive，保持长连接活跃
    BOOL keepalive = TRUE;
    setsockopt(proxySocket, SOL_SOCKET, SO_KEEPALIVE, (const char*)&keepalive, sizeof(keepalive));

    // 🔥 配置 Keepalive 参数（Windows）
    tcp_keepalive keepaliveParams;
    keepaliveParams.onoff = 1;                    // 启用
    keepaliveParams.keepalivetime = 60000;        // 60秒后开始发送keepalive探测
    keepaliveParams.keepaliveinterval = 10000;    // 每10秒发送一次探测
    DWORD bytesReturned;
    WSAIoctl(proxySocket, SIO_KEEPALIVE_VALS, &keepaliveParams, sizeof(keepaliveParams),
             nullptr, 0, &bytesReturned, nullptr, nullptr);

    sockaddr_in proxyAddr;
    proxyAddr.sin_family = AF_INET;
    proxyAddr.sin_port = htons(secondaryProxyPort);

    // 解析二级代理地址
    if (inet_pton(AF_INET, secondaryProxyHost.c_str(), &proxyAddr.sin_addr) != 1) {
        // 不是IP地址，尝试DNS解析
        struct addrinfo hints = { 0 }, * result = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        if (getaddrinfo(secondaryProxyHost.c_str(), nullptr, &hints, &result) == 0) {
            proxyAddr.sin_addr = ((sockaddr_in*)result->ai_addr)->sin_addr;
            freeaddrinfo(result);
        }
        else {
            Log("[错误] 无法解析二级代理主机: " + secondaryProxyHost);
            closesocket(proxySocket);
            return INVALID_SOCKET;
        }
    }

    // 🔥 关键修复：使用非阻塞模式连接，避免长时间阻塞
    // 设置为非阻塞模式
    u_long mode = 1;
    ioctlsocket(proxySocket, FIONBIO, &mode);

    // 尝试连接（非阻塞，会立即返回）
    int connectResult = connect(proxySocket, (sockaddr*)&proxyAddr, sizeof(proxyAddr));
    if (connectResult == SOCKET_ERROR) {
        int error = WSAGetLastError();
        if (error != WSAEWOULDBLOCK) {
            AB_LOG_ERROR("[错误] 连接二级代理失败: " + secondaryProxyHost + ":" + std::to_string(secondaryProxyPort) +
                " (错误码: " + std::to_string(error) + ")");
            closesocket(proxySocket);
            return INVALID_SOCKET;
        }

        // 使用select等待连接完成（最多10秒）
        fd_set writeSet, errorSet;
        FD_ZERO(&writeSet);
        FD_ZERO(&errorSet);
        FD_SET(proxySocket, &writeSet);
        FD_SET(proxySocket, &errorSet);

        timeval tv;
        tv.tv_sec = 10;   // 10秒超时
        tv.tv_usec = 0;

        int selectResult = select(0, nullptr, &writeSet, &errorSet, &tv);
        if (selectResult <= 0 || FD_ISSET(proxySocket, &errorSet)) {
            if (selectResult == 0) {
                Log("[错误] 连接二级代理超时: " + secondaryProxyHost + ":" + std::to_string(secondaryProxyPort));
            } else {
                AB_LOG_ERROR("[错误] 连接二级代理失败: " + secondaryProxyHost + ":" + std::to_string(secondaryProxyPort));
            }
            closesocket(proxySocket);
            return INVALID_SOCKET;
        }
    }

    // 恢复为阻塞模式（后续的recv/send需要阻塞模式配合超时）
    mode = 0;
    ioctlsocket(proxySocket, FIONBIO, &mode);

    Log("[二级代理] 已连接到代理服务器: " + secondaryProxyHost + ":" + std::to_string(secondaryProxyPort));

    // ===== 步骤3: 执行SOCKS5握手 =====
    char buffer[512];
    bool needAuth = !secondaryProxyUsername.empty();

    // 3.1: 发送认证方法
    if (needAuth) {
        // 支持无认证(0x00)和用户名密码认证(0x02)
        char handshake[4] = { 5, 2, 0, 2 };  // 版本5，2个方法，无认证(0x00)和用户名密码(0x02)
        if (send(proxySocket, handshake, 4, 0) != 4) {
            Log("[错误] 二级代理握手失败：发送认证方法失败");
            closesocket(proxySocket);
            return INVALID_SOCKET;
        }
    }
    else {
        // 仅支持无认证
        char handshake[3] = { 5, 1, 0 };  // 版本5，1个方法，无认证(0x00)
        if (send(proxySocket, handshake, 3, 0) != 3) {
            Log("[错误] 二级代理握手失败：发送认证方法失败");
            closesocket(proxySocket);
            return INVALID_SOCKET;
        }
    }

    // 3.2: 接收认证响应
    int recvResult = recv(proxySocket, buffer, 2, 0);
    if (recvResult != 2) {
        int error = WSAGetLastError();
        if (error == WSAETIMEDOUT) {
            Log("[错误] 二级代理握手超时：接收认证响应超时（10秒）");
        } else {
            Log("[错误] 二级代理握手失败：接收认证响应失败 (错误码: " + std::to_string(error) + ")");
        }
        closesocket(proxySocket);
        return INVALID_SOCKET;
    }

    if (buffer[0] != 5) {
        Log("[错误] 二级代理版本不匹配：" + std::to_string((int)buffer[0]));
        closesocket(proxySocket);
        return INVALID_SOCKET;
    }

    // 3.3: 处理认证方法
    if (buffer[1] == 0x02) {
        // 需要用户名密码认证
        if (!needAuth) {
            AB_LOG_ERROR("[错误] 二级代理要求认证，但未配置用户名密码");
            closesocket(proxySocket);
            return INVALID_SOCKET;
        }

        Log("[二级代理] 执行用户名密码认证...");

        // 构建认证请求: VER(1) + ULEN(1) + UNAME(1-255) + PLEN(1) + PASSWD(1-255)
        std::vector<char> authRequest;
        authRequest.push_back(0x01);  // 认证子协议版本
        authRequest.push_back(static_cast<char>(secondaryProxyUsername.length()));
        authRequest.insert(authRequest.end(), secondaryProxyUsername.begin(), secondaryProxyUsername.end());
        authRequest.push_back(static_cast<char>(secondaryProxyPassword.length()));
        authRequest.insert(authRequest.end(), secondaryProxyPassword.begin(), secondaryProxyPassword.end());

        if (send(proxySocket, authRequest.data(), static_cast<int>(authRequest.size()), 0) != authRequest.size()) {
            Log("[错误] 发送二级代理认证信息失败");
            closesocket(proxySocket);
            return INVALID_SOCKET;
        }

        // 接收认证结果
        recvResult = recv(proxySocket, buffer, 2, 0);
        if (recvResult != 2) {
            int error = WSAGetLastError();
            if (error == WSAETIMEDOUT) {
                Log("[错误] 二级代理认证超时：接收认证结果超时（10秒）");
            } else {
                Log("[错误] 接收二级代理认证结果失败 (错误码: " + std::to_string(error) + ")");
            }
            closesocket(proxySocket);
            return INVALID_SOCKET;
        }

        if (buffer[1] != 0x00) {
            Log("[错误] 二级代理认证失败：用户名或密码错误");
            closesocket(proxySocket);
            return INVALID_SOCKET;
        }

        AB_LOG_INFO("[二级代理] 认证成功");
    }
    else if (buffer[1] == 0x00) {
        // 无需认证
        Log("[二级代理] 无需认证");
    }
    else if (buffer[1] == 0xFF) {
        Log("[错误] 二级代理不接受任何认证方法");
        closesocket(proxySocket);
        return INVALID_SOCKET;
    }
    else {
        Log("[错误] 二级代理要求不支持的认证方法: " + std::to_string((int)buffer[1]));
        closesocket(proxySocket);
        return INVALID_SOCKET;
    }

    // ===== 步骤4: 发送连接请求 =====
    std::vector<uint8_t> request;
    request.push_back(5);  // SOCKS版本
    request.push_back(1);  // CONNECT命令
    request.push_back(0);  // 保留字节

    // 判断目标是IP还是域名
    sockaddr_in testAddr;
    if (inet_pton(AF_INET, targetHost.c_str(), &testAddr.sin_addr) == 1) {
        // IPv4地址
        request.push_back(1);  // 地址类型：IPv4

        // 添加IP地址（网络字节序）
        uint32_t ip = testAddr.sin_addr.s_addr;
        request.push_back((ip >> 0) & 0xFF);
        request.push_back((ip >> 8) & 0xFF);
        request.push_back((ip >> 16) & 0xFF);
        request.push_back((ip >> 24) & 0xFF);
    }
    else {
        // 域名
        request.push_back(3);  // 地址类型：域名
        request.push_back(static_cast<uint8_t>(targetHost.length()));  // 域名长度
        request.insert(request.end(), targetHost.begin(), targetHost.end());  // 域名内容
    }

    // 添加端口（网络字节序）
    request.push_back((targetPort >> 8) & 0xFF);  // 高字节
    request.push_back(targetPort & 0xFF);         // 低字节

    // 发送连接请求
    if (send(proxySocket, reinterpret_cast<char*>(request.data()),
        static_cast<int>(request.size()), 0) != request.size()) {
        AB_LOG_ERROR("[错误] 发送二级代理连接请求失败");
        closesocket(proxySocket);
        return INVALID_SOCKET;
    }

    // ===== 步骤5: 接收连接响应 =====
    // 最小响应长度：版本(1) + 状态(1) + 保留(1) + 地址类型(1) + 地址(最少4) + 端口(2) = 10字节
    int recvLen = recv(proxySocket, buffer, sizeof(buffer), 0);
    if (recvLen < 10) {
        int error = WSAGetLastError();
        if (error == WSAETIMEDOUT) {
            Log("[错误] 二级代理连接超时：接收连接响应超时（10秒）");
        } else {
            Log("[错误] 接收二级代理连接响应失败 (错误码: " + std::to_string(error) + ", 接收字节: " + std::to_string(recvLen) + ")");
        }
        closesocket(proxySocket);
        return INVALID_SOCKET;
    }

    // 检查响应
    if (buffer[0] != 5) {
        Log("[错误] 二级代理响应版本错误: " + std::to_string((int)buffer[0]));
        closesocket(proxySocket);
        return INVALID_SOCKET;
    }

    if (buffer[1] != 0) {
        std::string errorMsg;
        switch (buffer[1]) {
        case 1: errorMsg = "通用SOCKS服务器失败"; break;
        case 2: errorMsg = "规则集不允许连接"; break;
        case 3: errorMsg = "网络无法访问"; break;
        case 4: errorMsg = "主机无法访问"; break;
        case 5: errorMsg = "连接被拒绝"; break;
        case 6: errorMsg = "TTL过期"; break;
        case 7: errorMsg = "不支持的命令"; break;
        case 8: errorMsg = "不支持的地址类型"; break;
        default: errorMsg = "未知错误(" + std::to_string((int)buffer[1]) + ")"; break;
        }
        Log("[错误] 二级代理连接目标失败: " + errorMsg);
        closesocket(proxySocket);
        return INVALID_SOCKET;
    }

    // ===== 步骤6: 连接成功 =====
    Log("[成功] 通过二级代理连接到: " + targetHost + ":" + std::to_string(targetPort));
    return proxySocket;
}

// ===== 设置二级代理配置 =====
void PacketCollector::SetSecondaryProxy(bool enable, const std::string& host, int port,
    const std::string& username, const std::string& password) {
    AB_LOG_INFO("[调试] SetSecondaryProxy 被调用: enable=" + std::to_string(enable) +
        ", host=" + host + ", port=" + std::to_string(port));

    std::lock_guard<std::mutex> lock(secondaryProxyMutex);
    enableSecondaryProxy = enable;
    secondaryProxyHost = host;
    secondaryProxyPort = port;
    secondaryProxyUsername = username;
    secondaryProxyPassword = password;

    // 🔥🔥🔥 动态更新IOCP模式下的二级代理回调
    if (useIOCP && iocpPool) {
        if (enable) {
            iocpPool->SetSecondaryProxyConnector([this](const std::string& targetHost, int targetPort) -> SOCKET {
                return this->ConnectToSecondaryProxy(targetHost, targetPort);
            });
            AB_LOG_INFO("[IOCP] 二级代理已动态启用");
        }
        else {
            iocpPool->SetSecondaryProxyConnector(nullptr);
            AB_LOG_INFO("[IOCP] 二级代理已动态禁用");
        }
    }

    if (enable) {
        if (!username.empty()) {
            AB_LOG_INFO("[二级代理] 已启用: " + host + ":" + std::to_string(port) + " (带认证)");
        }
        else {
            AB_LOG_INFO("[二级代理] 已启用: " + host + ":" + std::to_string(port) + " (无认证)");
        }
    }
    else {
        AB_LOG_INFO("[二级代理] 已禁用");
    }
}


bool PacketCollector::HandleSocks5Auth(SOCKET clientSocket, std::string& username,
    PacketCollector*& accountStateOwner, const std::string& clientIP) {
    char buffer[512];

    int n = recv(clientSocket, buffer, 2, 0);
    if (n < 2) {
        Log("[SOCKS5-Auth] 接收认证请求失败");
        char response[2] = { 1, 1 };
        send(clientSocket, response, 2, 0);
        return false;
    }

    uint8_t authVersion = buffer[0];
    uint8_t usernameLen = buffer[1];

    if (authVersion != 1) {
        Log("[SOCKS5-Auth] 不支持的认证版本: " + std::to_string(authVersion));
        char response[2] = { 1, 1 };
        send(clientSocket, response, 2, 0);
        return false;
    }

    // 验证用户名长度，防止缓冲区溢出
    if (usernameLen > 255 || usernameLen > sizeof(buffer)) {
        Log("[SOCKS5-Auth] 非法的用户名长度: " + std::to_string(usernameLen));
        char response[2] = { 1, 1 };
        send(clientSocket, response, 2, 0);
        return false;
    }

    n = recv(clientSocket, buffer, usernameLen, 0);
    if (n < usernameLen) {
        Log("[SOCKS5-Auth] 读取用户名失败");
        char response[2] = { 1, 1 };
        send(clientSocket, response, 2, 0);
        return false;
    }

    username = std::string(buffer, usernameLen);

    n = recv(clientSocket, buffer, 1, 0);
    if (n < 1) {
        Log("[SOCKS5-Auth] 读取密码长度失败");
        char response[2] = { 1, 1 };
        send(clientSocket, response, 2, 0);
        return false;
    }

    uint8_t passwordLen = buffer[0];

    // 验证密码长度，防止缓冲区溢出
    if (passwordLen > 255 || passwordLen > sizeof(buffer)) {
        Log("[SOCKS5-Auth] 非法的密码长度: " + std::to_string(passwordLen));
        char response[2] = { 1, 1 };
        send(clientSocket, response, 2, 0);
        return false;
    }

    n = recv(clientSocket, buffer, passwordLen, 0);
    if (n < passwordLen) {
        Log("[SOCKS5-Auth] 读取密码失败");
        char response[2] = { 1, 1 };
        send(clientSocket, response, 2, 0);
        return false;
    }

    std::string password = std::string(buffer, passwordLen);
    std::string failReason;
    if (!TryAcquireAuthenticatedSession(username, password, clientIP, accountStateOwner, &failReason)) {
        Log("[SOCKS5-Auth] 认证失败: " + username + " [IP: " + clientIP + "]" +
            (failReason.empty() ? std::string() : (" 原因: " + failReason)));
        char response[2] = { 1, 1 };
        send(clientSocket, response, 2, 0);
        return false;
    }

    char response[2] = { 1, 0 };
    send(clientSocket, response, 2, 0);

    return true;
}

bool PacketCollector::ValidateAccount(const std::string& username, const std::string& password) {
    AB_LOG_INFO("[调试] ValidateAccount 被调用: username=" + username +
                 ", externalAccountSource=" +
                 (externalAccountSource ? std::to_string(reinterpret_cast<uintptr_t>(externalAccountSource)) : "nullptr"));

    // 如果设置了外部账号源，优先使用外部账号源验证
    if (externalAccountSource) {
        AB_LOG_INFO("[调试] 使用外部账号源验证: username=" + username + ", password=" + password);

        // 检查外部账号源中的账号数量
        int accountCount = externalAccountSource->GetAllAccounts().size();
        AB_LOG_INFO("[调试] 外部账号源中的账号数量: " + std::to_string(accountCount));

        bool result = externalAccountSource->ValidateAccount(username, password);
        AB_LOG_INFO("[调试] 外部账号源验证结果: " + std::string(result ? "成功" : "失败"));
        return result;
    }

    AB_LOG_INFO("[调试] 使用本地账号验证: username=" + username + ", password=" + password);

    std::lock_guard<std::mutex> lock(accountsMutex);

    auto it = accounts.find(username);
    if (it == accounts.end()) {
        AB_LOG_INFO("[调试] 本地账号不存在: " + username);
        return false;
    }

    const Socks5Account& account = it->second;

    if (!account.isEnabled) {
        Log("[SOCKS5-Auth] 账号已禁用: " + username);
        return false;
    }

    if (account.password != password) {
        return false;
    }

    if (CheckAccountExpired(account)) {
        Log("[SOCKS5-Auth] 账号已过期: " + username);
        return false;
    }

    return true;
}

bool PacketCollector::ValidateAccountWithReason(const std::string& username, const std::string& password, std::string& failReason) {
    // 如果设置了外部账号源，优先使用外部账号源验证
    if (externalAccountSource) {
        return externalAccountSource->ValidateAccountWithReason(username, password, failReason);
    }

    // 使用本地账号验证
    std::lock_guard<std::mutex> lock(accountsMutex);

    auto it = accounts.find(username);
    if (it == accounts.end()) {
        failReason = "用户名不存在";
        return false;
    }

    const Socks5Account& account = it->second;

    // 检查密码
    if (account.password != password) {
        failReason = "密码错误";
        return false;
    }

    // 检查是否启用
    if (!account.isEnabled) {
        failReason = "账号已被禁用";
        return false;
    }

    // 检查是否过期
    if (CheckAccountExpired(account)) {
        failReason = "账号已过期 (到期时间: " + account.expireTime + ")";
        return false;
    }

    failReason = "";
    return true;
}

bool PacketCollector::CheckAccountExpired(const Socks5Account& account) {
    if (account.expireTime.empty()) {
        return false;
    }

    struct tm expireTm = {};
    std::istringstream ss(account.expireTime);
    ss >> std::get_time(&expireTm, "%Y-%m-%d %H:%M:%S");

    if (ss.fail()) {
        Log("[SOCKS5-Auth] 过期时间格式错误: " + account.expireTime);
        return true;
    }

    time_t expireTime = mktime(&expireTm);
    time_t currentTime = time(NULL);

    return currentTime > expireTime;
}

bool PacketCollector::TryAcquireConnection(const std::string& username) {
    // 如果设置了外部账号源，使用外部账号源
    if (externalAccountSource) {
        return externalAccountSource->TryAcquireConnection(username);
    }

    std::lock_guard<std::mutex> lock(accountsMutex);

    auto it = accounts.find(username);
    if (it == accounts.end()) {
        return false;
    }

    Socks5Account& account = it->second;

    const int liveConnections = CountAggregatedAuthenticatedConnectionsForUser(username);
    if (account.currentConnections != liveConnections) {
        Log("[SOCKS5-Auth] 修正账号连接计数: " + username +
            " [缓存: " + std::to_string(account.currentConnections) +
            "] [实际: " + std::to_string(liveConnections) + "]");
        account.currentConnections = liveConnections;
    }

    // ===== 🔥 修改：maxConnections=0 表示不限制连接数 =====
    if (account.maxConnections > 0 && account.currentConnections >= account.maxConnections) {
        return false;
    }

    // ===== 🔥 新增：记录首次连接时间 =====
    if (account.currentConnections == 0) {
        account.firstConnectTime = std::chrono::system_clock::now();
    }

    account.currentConnections++;

    // ===== 🔥 修改日志显示 =====
    if (account.maxConnections == 0) {
        Log("[SOCKS5-Auth] 连接数: " + username + " (" +
            std::to_string(account.currentConnections) + "/无限制)");
    }
    else {
        Log("[SOCKS5-Auth] 连接数: " + username + " (" +
            std::to_string(account.currentConnections) + "/" +
            std::to_string(account.maxConnections) + ")");
    }

    return true;
}

bool PacketCollector::TryAcquireDeviceSlot(const std::string& clientIP, const std::string& username, std::string* outError) {
    if (outError) outError->clear();
    if (clientIP.empty()) {
        if (outError) {
            *outError = "客户端IP为空，无法统计设备数";
        }
        return false;
    }

    std::lock_guard<std::mutex> lock(deviceLimitMutex);

    auto it = activeDeviceIpRefCounts.find(clientIP);
    if (it != activeDeviceIpRefCounts.end()) {
        it->second++;
        Log("[SOCKS5-Auth] 复用设备IP: " + clientIP +
            " [用户: " + username + "] [设备数: " +
            std::to_string(activeDeviceIpRefCounts.size()) + "/" +
            (instanceDeviceLimit > 0 ? std::to_string(instanceDeviceLimit) : std::string("不限")) + "]");
        return true;
    }

    if (instanceDeviceLimit > 0 && static_cast<int>(activeDeviceIpRefCounts.size()) >= instanceDeviceLimit) {
        if (outError) {
            *outError = "实例设备数已达上限 (" + std::to_string(activeDeviceIpRefCounts.size()) +
                "/" + std::to_string(instanceDeviceLimit) + ")";
        }
        return false;
    }

    activeDeviceIpRefCounts[clientIP] = 1;
    Log("[SOCKS5-Auth] 新设备上线: " + clientIP +
        " [用户: " + username + "] [设备数: " +
        std::to_string(activeDeviceIpRefCounts.size()) + "/" +
        (instanceDeviceLimit > 0 ? std::to_string(instanceDeviceLimit) : std::string("不限")) + "]");
    return true;
}


void PacketCollector::ReleaseConnection(const std::string& username) {
    if (username.empty()) return;

    // 如果设置了外部账号源，使用外部账号源
    if (externalAccountSource) {
        externalAccountSource->ReleaseConnection(username);
        return;
    }

    std::lock_guard<std::mutex> lock(accountsMutex);

    auto it = accounts.find(username);
    if (it == accounts.end()) {
        return;
    }

    Socks5Account& account = it->second;
    const int previousConnections = account.currentConnections;
    const int liveConnections = CountAggregatedAuthenticatedConnectionsForUser(username);
    if (account.currentConnections != liveConnections) {
        Log("[SOCKS5-Auth] 修正账号连接计数(释放阶段): " + username +
            " [缓存: " + std::to_string(account.currentConnections) +
            "] [实际: " + std::to_string(liveConnections) + "]");
        account.currentConnections = liveConnections;
    }

    if (previousConnections > 0 && account.currentConnections == 0) {
        auto now = std::chrono::system_clock::now();
        account.lastDisconnectTime = now;

        auto duration = std::chrono::duration_cast<std::chrono::seconds>(
            now - account.firstConnectTime);
        account.totalOnlineSeconds += duration.count();

        Log("[SOCKS5-Auth] 账号离线: " + username +
            " | 本次在线: " + std::to_string(duration.count()) + "秒 | " +
            "累计在线: " + std::to_string(account.totalOnlineSeconds) + "秒");
    }

    if (account.maxConnections == 0) {
        Log("[SOCKS5-Auth] 释放连接: " + username + " (" +
            std::to_string(account.currentConnections) + "/无限制)");
    }
    else {
        Log("[SOCKS5-Auth] 释放连接: " + username + " (" +
            std::to_string(account.currentConnections) + "/" +
            std::to_string(account.maxConnections) + ")");
    }
}

void PacketCollector::ReleaseDeviceSlot(const std::string& clientIP) {
    if (clientIP.empty()) return;

    std::lock_guard<std::mutex> lock(deviceLimitMutex);
    auto it = activeDeviceIpRefCounts.find(clientIP);
    if (it == activeDeviceIpRefCounts.end()) {
        return;
    }

    if (it->second > 1) {
        it->second--;
        Log("[SOCKS5-Auth] 释放设备引用: " + clientIP +
            " [剩余连接: " + std::to_string(it->second) + "]");
        return;
    }

    activeDeviceIpRefCounts.erase(it);
    Log("[SOCKS5-Auth] 设备离线: " + clientIP +
        " [当前设备数: " + std::to_string(activeDeviceIpRefCounts.size()) + "/" +
        (instanceDeviceLimit > 0 ? std::to_string(instanceDeviceLimit) : std::string("不限")) + "]");
}





void PacketCollector::UpdateLoginInfo(const std::string& username, const std::string& clientIP) {
    // 如果设置了外部账号源，使用外部账号源
    if (externalAccountSource) {
        externalAccountSource->UpdateLoginInfo(username, clientIP);
        return;
    }

    std::lock_guard<std::mutex> lock(accountsMutex);

    auto it = accounts.find(username);
    if (it == accounts.end()) {
        return;
    }

    Socks5Account& account = it->second;

    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    struct tm timeinfo;
    localtime_s(&timeinfo, &time);
    char buffer[64];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);

    account.lastLoginTime = buffer;
    account.lastLoginIP = clientIP;
}

void PacketCollector::SetExternalAccountSource(PacketCollector* source) {
    PacketCollector* previous = externalAccountSource;
    if (previous == source) {
        return;
    }

    if (previous) {
        previous->UnregisterExternalAccountConsumer(this);
    }

    externalAccountSource = source;

    if (source) {
        source->RegisterExternalAccountConsumer(this);
    }

    if (source) {
        int accountCount = source->GetAllAccounts().size();
        AB_LOG_INFO("[调试] SetExternalAccountSource 被调用: source=" +
                     std::to_string(reinterpret_cast<uintptr_t>(source)) +
                     ", 账号数=" + std::to_string(accountCount));
    } else {
        AB_LOG_INFO("[调试] SetExternalAccountSource 被调用: source=nullptr");
    }
}

void PacketCollector::SetSocks5Auth(bool enable) {
    enableSocks5Auth = enable;
    Log(std::string("[SOCKS5-Auth] 认证") + (enable ? "已启用" : "已禁用"));

    // 如果IOCP模式正在运行，需要更新认证回调和认证标志
    if (iocpPool) {
        iocpPool->SetSocks5Auth(enable);  // 🔥 同步设置IOCP的认证标志
        if (enable) {
            iocpPool->SetAuthCallback([this](const std::string& username,
                const std::string& password,
                const std::string& clientIP,
                PacketCollector** outAccountStateOwner) -> bool {
                if (clientIP.empty() || !outAccountStateOwner) {
                    return this->ValidateAccount(username, password);
                }
                PacketCollector* accountStateOwner = nullptr;
                std::string failReason;
                const bool authSuccess = this->TryAcquireAuthenticatedSession(
                    username, password, clientIP, accountStateOwner, &failReason);
                if (!authSuccess && !failReason.empty()) {
                    Log("[SOCKS5-Auth][IOCP] 认证失败: " + username +
                        " [IP: " + clientIP + "] 原因: " + failReason);
                }
                if (outAccountStateOwner) {
                    *outAccountStateOwner = accountStateOwner;
                }
                return authSuccess;
            });
            Log("[SOCKS5-Auth] IOCP认证回调已设置");
        }
        else {
            iocpPool->SetAuthCallback(nullptr);
            Log("[SOCKS5-Auth] IOCP认证回调已清除");
        }
    }
}

bool PacketCollector::AddAccount(const std::string& username, const std::string& password,
    const std::string& expireTime, int maxConnections) {
    std::lock_guard<std::mutex> lock(accountsMutex);

    if (accounts.find(username) != accounts.end()) {
        Log("[SOCKS5-Auth] 账号已存在: " + username);
        return false;
    }

    Socks5Account account;
    account.id = nextAccountId++;
    account.username = username;
    account.password = password;
    account.expireTime = expireTime;
    account.maxConnections = maxConnections;
    account.currentConnections = 0;
    account.isEnabled = true;

    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    struct tm timeinfo;
    localtime_s(&timeinfo, &time);
    char buffer[64];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
    account.createdAt = buffer;

    accounts[username] = account;

    Log("[SOCKS5-Auth] 添加账号: " + username + " | 最大连接: " +
        std::to_string(maxConnections) + " | 到期: " + (expireTime.empty() ? "永不过期" : expireTime));

    return true;
}

bool PacketCollector::RemoveAccount(const std::string& username) {
    std::lock_guard<std::mutex> lock(accountsMutex);

    auto it = accounts.find(username);
    if (it == accounts.end()) {
        return false;
    }

    accounts.erase(it);
    Log("[SOCKS5-Auth] 删除账号: " + username);

    return true;
}

bool PacketCollector::UpdateAccount(const std::string& username, const std::string& password,
    const std::string& expireTime, int maxConnections, bool isEnabled) {
    std::lock_guard<std::mutex> lock(accountsMutex);

    auto it = accounts.find(username);
    if (it == accounts.end()) {
        return false;
    }

    Socks5Account& account = it->second;

    // 只有当密码不为空时才更新密码，避免API请求未提交密码导致清空
    if (!password.empty()) {
        account.password = password;
    }

    account.expireTime = expireTime;
    account.maxConnections = maxConnections;
    account.isEnabled = isEnabled;

    Log("[SOCKS5-Auth] 更新账号: " + username);

    return true;
}

std::vector<Socks5Account> PacketCollector::GetAllAccounts() const {
    // 如果设置了外部账号源，返回外部账号源的账号列表
    if (externalAccountSource) {
        return externalAccountSource->GetAllAccounts();
    }

    std::lock_guard<std::mutex> lock(accountsMutex);

    std::vector<Socks5Account> result;
    for (const auto& pair : accounts) {
        result.push_back(pair.second);
    }

    return result;
}

// 🔥 新增：更新账号扩展字段（包括实例ID和远程端口）
bool PacketCollector::UpdateAccountEx(const Socks5Account& account) {
    std::lock_guard<std::mutex> lock(accountsMutex);

    auto it = accounts.find(account.username);
    if (it == accounts.end()) {
        return false;
    }

    Socks5Account& existingAccount = it->second;

    // 更新所有字段
    if (!account.password.empty()) {
        existingAccount.password = account.password;
    }
    existingAccount.expireTime = account.expireTime;
    existingAccount.maxConnections = account.maxConnections;
    existingAccount.isEnabled = account.isEnabled;

    // 更新扩展字段
    existingAccount.instanceId = account.instanceId;
    existingAccount.remotePort = account.remotePort;
    existingAccount.remoteHost = account.remoteHost;

    Log("[SOCKS5-Auth] 更新账号扩展信息: " + account.username +
        " 实例: " + (account.instanceId.empty() ? "全局" : account.instanceId) +
        " 远程端口: " + std::to_string(account.remotePort));

    return true;
}

// 🔥 新增：按实例ID获取账号列表
std::vector<Socks5Account> PacketCollector::GetAccountsByInstance(const std::string& instanceId) const {
    std::lock_guard<std::mutex> lock(accountsMutex);

    std::vector<Socks5Account> result;
    for (const auto& pair : accounts) {
        if (pair.second.instanceId == instanceId) {
            result.push_back(pair.second);
        }
    }

    return result;
}

Socks5Account PacketCollector::GetAccount(const std::string& username) const {
    std::lock_guard<std::mutex> lock(accountsMutex);

    auto it = accounts.find(username);
    if (it != accounts.end()) {
        return it->second;
    }

    return Socks5Account();
}

bool PacketCollector::AccountExists(const std::string& username) const {
    std::lock_guard<std::mutex> lock(accountsMutex);
    return accounts.find(username) != accounts.end();
}


// ===== 绑定游戏会话ID到用户（只在变化时记录日志）=====
bool PacketCollector::BindGameIDToUser(const std::string& username, const std::string& gameID) {
    if (username.empty() || gameID.empty()) {
        return false;
    }

    // 过滤00开头的GameID
    if (gameID.length() >= 2 && gameID[0] == '0' && gameID[1] == '0') {
        return false;
    }

    // 如果设置了外部账号源，使用外部账号源
    if (externalAccountSource) {
        return externalAccountSource->BindGameIDToUser(username, gameID);
    }

    std::string oldGameID;  // 保存旧的GameID用于清理

    {
        std::lock_guard<std::mutex> lock(accountsMutex);

        auto it = accounts.find(username);
        if (it == accounts.end()) {
            return false;
        }

        Socks5Account& account = it->second;

        // 🔥 关键：只在GameID变化时才处理
        if (account.currentGameID != gameID) {
            oldGameID = account.currentGameID;  // 保存旧ID
            account.currentGameID = gameID;

            std::string oldIDStr = oldGameID.empty() ? "无" : oldGameID;
            AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR,"[游戏会话] 用户 [" + username + "] 绑定GameID: " +
                oldIDStr + " → " + gameID);
        }
        // 如果GameID相同，静默返回（不处理）
        else {
            return true;
        }
    }

    // 🔥 在锁外调用回调：清理旧GameID的数据包（GameID变化时总是清理）
    if (!oldGameID.empty() && onGameIDCleared) {
        AB_LOG_INFO("[游戏会话] GameID变更，清理旧GameID [" + oldGameID + "] 的数据包");
        onGameIDCleared(oldGameID, GameIDClearReason::GAMEID_CHANGED);
    }

    return true;
}



// ===== 清理用户的游戏会话ID（确保彻底清理）=====
void PacketCollector::ClearGameIDForUser(const std::string& username) {
    if (username.empty()) {
        return;
    }

    std::string clearedID;  // 🔥 在锁外保存需要清理的GameID

    // 如果设置了外部账号源，从外部账号源获取并清除GameID
    if (externalAccountSource) {
        clearedID = externalAccountSource->GetGameIDForUser(username);
        if (!clearedID.empty()) {
            externalAccountSource->ClearGameIDForUser(username);
            // 注意：外部账号源的ClearGameIDForUser会触发外部的回调
            // 但我们也需要触发本地的回调来清除本地内存池
        }
    }
    else {
        std::lock_guard<std::mutex> lock(accountsMutex);

        auto it = accounts.find(username);
        if (it == accounts.end()) {
            return;
        }

        Socks5Account& account = it->second;

        if (!account.currentGameID.empty()) {
            clearedID = account.currentGameID;
            account.currentGameID.clear();  // ✅ 清空GameID

            AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR,"[游戏会话] 用户 [" + username + "] 断开连接，清理GameID: " +
                clearedID);
        }
    }

    // 🔥 在锁外调用回调，通知清空该GameID的内存池数据（断开连接时）
    if (!clearedID.empty() && onGameIDCleared) {
        onGameIDCleared(clearedID, GameIDClearReason::DISCONNECT);
    }
}





// ===== 获取用户的游戏会话ID =====
std::string PacketCollector::GetGameIDForUser(const std::string& username) const {
    if (username.empty()) {
        return "";
    }

    // 如果设置了外部账号源，从外部账号源获取
    if (externalAccountSource) {
        return externalAccountSource->GetGameIDForUser(username);
    }

    std::lock_guard<std::mutex> lock(accountsMutex);

    auto it = accounts.find(username);
    if (it != accounts.end()) {
        return it->second.currentGameID;
    }

    return "";
}




// ===== 获取所有游戏会话ID绑定 =====
std::vector<std::pair<std::string, std::string>> PacketCollector::GetAllGameIDBindings() const {
    // 如果设置了外部账号源，从外部账号源获取
    if (externalAccountSource) {
        return externalAccountSource->GetAllGameIDBindings();
    }

    std::vector<std::pair<std::string, std::string>> bindings;

    std::lock_guard<std::mutex> lock(accountsMutex);

    for (const auto& pair : accounts) {
        if (!pair.second.currentGameID.empty()) {
            bindings.push_back(std::make_pair(pair.first, pair.second.currentGameID));
        }
    }

    return bindings;
}



// ===== 🔥 记录用户访问域名 =====
void PacketCollector::RecordDomainAccess(const std::string& username, const std::string& domain) {
    if (username.empty() || domain.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(domainAccessMutex);

    // 记录该域名的最新访问者
    domainAccessHistory[domain] = DomainAccessRecord(username, domain);

    AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR,"[域名访问] 用户 [" + username + "] 访问: " + domain);
}

// ===== 🔥 获取最后访问指定域名的用户 =====
std::string PacketCollector::GetLastUserForDomain(const std::string& domain) const {
    std::lock_guard<std::mutex> lock(domainAccessMutex);

    auto it = domainAccessHistory.find(domain);
    if (it != domainAccessHistory.end()) {
        return it->second.username;
    }

    return "";
}

// ===== 🔥 获取所有域名访问记录 =====
std::map<std::string, DomainAccessRecord> PacketCollector::GetAllDomainAccess() const {
    std::lock_guard<std::mutex> lock(domainAccessMutex);
    return domainAccessHistory;
}

// ==================== 防CC管理接口实现 ====================
void PacketCollector::SetAntiCCConfig(const AntiCCConfig& config) {
    if (antiCC) {
        antiCC->SetConfig(config);
    }
}

AntiCCConfig PacketCollector::GetAntiCCConfig() const {
    if (antiCC) {
        return antiCC->GetConfig();
    }
    return AntiCCConfig();
}

void PacketCollector::SetAntiCCEnabled(bool enabled) {
    if (antiCC) {
        antiCC->SetEnabled(enabled);
    }
}

bool PacketCollector::IsAntiCCEnabled() const {
    if (antiCC) {
        return antiCC->IsEnabled();
    }
    return false;
}

int PacketCollector::GetAntiCCBlockedCount() const {
    if (antiCC) {
        return antiCC->GetBlockedCount();
    }
    return 0;
}

int PacketCollector::GetAntiCCTotalConnections() const {
    if (antiCC) {
        return antiCC->GetTotalConnections();
    }
    return 0;
}

bool PacketCollector::IsUnderAttack() const {
    if (antiCC) {
        return antiCC->IsUnderAttack();
    }
    return false;
}

std::vector<AntiCC::IPDetailInfo> PacketCollector::GetAllIPStats() {
    if (antiCC) {
        return antiCC->GetAllIPStats();
    }
    return std::vector<AntiCC::IPDetailInfo>();
}

void PacketCollector::AddToBlacklist(const std::string& ip) {
    if (antiCC) {
        antiCC->AddToBlacklist(ip);
    }
}

void PacketCollector::AddToWhitelist(const std::string& ip) {
    if (antiCC) {
        antiCC->AddToWhitelist(ip);
    }
}

void PacketCollector::RemoveFromBlacklist(const std::string& ip) {
    if (antiCC) {
        antiCC->RemoveFromBlacklist(ip);
    }
}

void PacketCollector::RemoveFromWhitelist(const std::string& ip) {
    if (antiCC) {
        antiCC->RemoveFromWhitelist(ip);
    }
}

std::vector<std::string> PacketCollector::GetBlacklist() {
    if (antiCC) {
        return antiCC->GetBlacklist();
    }
    return std::vector<std::string>();
}

std::vector<std::string> PacketCollector::GetWhitelist() {
    if (antiCC) {
        return antiCC->GetWhitelist();
    }
    return std::vector<std::string>();
}

// ==================== 白名单持久化接口实现 ====================
void PacketCollector::SetAntiCCDatabaseManager(DatabaseManager* db) {
    if (antiCC) {
        antiCC->SetDatabaseManager(db);
        Log("[防CC] 数据库管理器已设置，白名单将持久化");
    }
}

void PacketCollector::LoadAntiCCWhitelistFromDB() {
    if (antiCC) {
        antiCC->LoadWhitelistFromDB();
    }
}

void PacketCollector::CleanupAntiCCInactiveWhitelist() {
    if (antiCC) {
        antiCC->CleanupInactiveWhitelist();
    }
}

// ==================== 白名单线程池配置实现 ====================
void PacketCollector::SetThreadPoolEnabled(bool enabled) {
    if (isRunning) {
        Log("[线程池] 警告：代理运行中无法更改线程池设置，请先停止代理");
        return;
    }
    useThreadPool = enabled;
    Log(std::string("[线程池] 白名单专用线程池") + (enabled ? "已启用" : "已禁用"));
}

void PacketCollector::SetThreadPoolSizes(int whitelistSize, int normalSize) {
    if (isRunning) {
        Log("[线程池] 警告：代理运行中无法更改线程池大小，请先停止代理");
        return;
    }
    whitelistPoolSize = (std::max)(1, whitelistSize);
    normalPoolSize = (std::max)(1, normalSize);
    Log("[线程池] 线程池大小已设置：白名单=" + std::to_string(whitelistPoolSize) +
        ", 普通=" + std::to_string(normalPoolSize));
}

int PacketCollector::GetWhitelistPoolProcessed() const {
    if (iocpPool) {
        return iocpPool->GetWhitelistProcessed();
    }
    if (threadPool) {
        return threadPool->GetWhitelistProcessed();
    }
    return 0;
}

int PacketCollector::GetNormalPoolProcessed() const {
    if (iocpPool) {
        return iocpPool->GetNormalProcessed();
    }
    if (threadPool) {
        return threadPool->GetNormalProcessed();
    }
    return 0;
}

int PacketCollector::GetWhitelistQueueSize() const {
    if (iocpPool) {
        return iocpPool->GetWhitelistQueueSize();
    }
    if (threadPool) {
        return threadPool->GetWhitelistQueueSize();
    }
    return 0;
}

int PacketCollector::GetNormalQueueSize() const {
    if (iocpPool) {
        return iocpPool->GetNormalQueueSize();
    }
    if (threadPool) {
        return threadPool->GetNormalQueueSize();
    }
    return 0;
}

// ==================== IOCP相关实现 ====================

void PacketCollector::SetIOCPEnabled(bool enabled) {
    if (isRunning) {
        Log("[IOCP] 警告：代理运行中无法更改IOCP设置，请先停止代理");
        return;
    }
    useIOCP = enabled;
    if (enabled) {
        useThreadPool = false;  // IOCP和阻塞式线程池互斥
    }
    Log(std::string("[IOCP] IOCP高性能模式") + (enabled ? "已启用" : "已禁用"));
}

void PacketCollector::SetIOCPMaxConnections(int maxWhitelist, int maxNormal) {
    if (isRunning) {
        Log("[IOCP] 警告：代理运行中无法更改最大连接数，请先停止代理");
        return;
    }
    maxWhitelistConnections = (std::max)(100, maxWhitelist);
    maxNormalConnections = (std::max)(50, maxNormal);
    Log("[IOCP] 最大连接数已设置：白名单=" + std::to_string(maxWhitelistConnections) +
        ", 普通=" + std::to_string(maxNormalConnections));
}

int PacketCollector::GetIOCPWhitelistConnCount() const {
    if (iocpPool) {
        return iocpPool->GetWhitelistConnectionCount();
    }
    return 0;
}

int PacketCollector::GetIOCPNormalConnCount() const {
    if (iocpPool) {
        return iocpPool->GetNormalConnectionCount();
    }
    return 0;
}

int PacketCollector::GetIOCPTotalConnCount() const {
    if (iocpPool) {
        return iocpPool->GetTotalConnectionCount();
    }
    return 0;
}

void PacketCollector::SetThreadPoolMode(ThreadPoolMode mode) {
    if (isRunning) {
        Log("[线程池] 警告：代理运行中无法更改模式，请先停止代理");
        return;
    }
    threadPoolMode = mode;
    switch (mode) {
    case ThreadPoolMode::TRADITIONAL:
        useIOCP = false;
        useThreadPool = false;
        Log("[线程池] 模式已设置：传统模式（每连接一个线程）");
        break;
    case ThreadPoolMode::BLOCKING:
        useIOCP = false;
        useThreadPool = true;
        Log("[线程池] 模式已设置：阻塞式线程池");
        break;
    case ThreadPoolMode::IOCP:
        useIOCP = true;
        useThreadPool = false;
        Log("[线程池] 模式已设置：IOCP高性能模式");
        break;
    }
}

PacketCollector::ThreadPoolMode PacketCollector::GetThreadPoolMode() const {
    return threadPoolMode;
}

// ==================== IOCP数据处理回调 ====================
DataModifierResult PacketCollector::OnIOCPDataModifier(ProxyConnectionInfo* conn,
    const std::vector<uint8_t>& data, bool isFromClient) {

    DataModifierResult result;
    result.forwardedData = data;
    result.callbackData = data;

    if (!conn || data.empty()) {
        return result;
    }

    if (isFromClient) {
        RecordProxyPacket(
            conn->id,
            conn->authenticatedUser,
            conn->gameID,
            conn->clientIP,
            conn->targetHost,
            std::string(),
            conn->targetPort,
            false,
            true,
            data);
    }

    // 🔥🔥🔥 调试日志
    AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[OnIOCPDataModifier] isFromClient=" + std::to_string(isFromClient) +
        ", onPacketModifier=" + (onPacketModifier ? "已设置" : "未设置") +
        ", dataSize=" + std::to_string(data.size()) +
        ", user=" + conn->authenticatedUser);

    bool needPacketSplit = IsPacketSplitEnabledForPort(conn->targetPort);
    bool shouldApplyWpe = !conn->bypassModifier && onPacketModifier &&
        (needPacketSplit || applyWpeOnNonSplitTraffic.load() || !enablePacketSplit.load());

    // 只处理客户端到服务器的数据（从客户端发来的）
    // 注意：GameID 已经在 IOCPThreadPool::ProcessClientData 中提取并设置到 conn->gameID
    if (isFromClient && shouldApplyWpe) {
        // 解析数据包信息
        PacketInfo packetInfo = PacketParser::ParsePacket(data);
        packetInfo.gameID = conn->gameID;
        packetInfo.socksUsername = conn->authenticatedUser;  // 🔥 设置SOCKS用户名

        AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[OnIOCPDataModifier] 调用WPE滤镜: gameID=" + conn->gameID +
            ", user=" + conn->authenticatedUser + ", dataSize=" + std::to_string(data.size()));

        // 绑定GameID到用户（如果有认证用户）
        if (!conn->gameID.empty() && !conn->authenticatedUser.empty()) {
            bool is00ID = (conn->gameID.length() >= 2 && conn->gameID[0] == '0' && conn->gameID[1] == '0');
            bool isOBID = (conn->gameID.length() >= 3 && conn->gameID.substr(conn->gameID.length() - 3) == "_OB");
            if (!is00ID && !isOBID) {
                BindGameIDToUser(conn->authenticatedUser, conn->gameID);
            }
        }

        // 调用外部设置的 modifier
        PacketTransformResult transform = onPacketModifier(packetInfo, data);
        result.forwardedData = std::move(transform.forwardedData);
        result.callbackData = transform.callbackData.empty() ? result.forwardedData : std::move(transform.callbackData);
        result.intercepted = transform.intercepted;

        // 判断是否修改：比较数据内容而不是大小
        bool isModified = (result.forwardedData != data);

        AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[OnIOCPDataModifier] WPE滤镜处理完成: intercepted=" +
            std::to_string(transform.intercepted) + ", modified=" +
            std::to_string(isModified) + ", size=" + std::to_string(result.forwardedData.size()));

        return result;
    }

    return result;
}

// ==================== IOCP连接事件回调 ====================
void PacketCollector::OnIOCPConnectionEvent(ProxyConnectionInfo* conn, const std::string& event) {
    if (!conn) return;

    if (event == "created") {
        totalConnections++;
    }
    else if (event == "forwarding") {
        DisconnectRuleEngine::ResetRuntimeState(conn->disconnectState);

        GlobalAntiCCCoordinator::GetInstance().RecordConnectionOpened(m_instanceId, conn->clientIP);

        // 记录域名访问
        if (!conn->authenticatedUser.empty()) {
            RecordDomainAccess(conn->authenticatedUser, conn->targetHost);
        }

        // 记录认证成功
        if (antiCC && antiCC->IsEnabled() && !conn->authenticatedUser.empty()) {
            antiCC->OnAuthSuccess(conn->clientIP);
        }
        if (!conn->authenticatedUser.empty()) {
            GlobalAntiCCCoordinator::GetInstance().RecordAuthSuccess(m_instanceId, conn->clientIP);
        }
        auto globalPressure = GlobalAntiCCCoordinator::GetInstance().GetGlobalPressure();
        auto instancePressure = GlobalAntiCCCoordinator::GetInstance().GetInstancePressure(m_instanceId);
        if (!conn->authenticatedUser.empty() &&
            antiCCLowPriorityEvictionEnabled.load() &&
            (globalPressure >= AntiCCPressureState::Overloaded ||
             instancePressure >= AntiCCPressureState::Overloaded)) {
            DisconnectOneLowPriorityConnection_();
        }

        Log("[IOCP连接] " + conn->clientAddr + " -> " +
            conn->targetHost + ":" + std::to_string(conn->targetPort) +
            (conn->isWhitelisted ? " [白名单]" : "") +
            (conn->authenticatedUser.empty() ? "" : " [用户: " + conn->authenticatedUser + "]"));
    }
    else if (event.find("closed:") == 0) {
        std::string reason = event.substr(7);

        // 🔥 断开连接时根据设置决定是否清理GameID绑定
        if (!conn->authenticatedUser.empty()) {
            if (onUserDisconnected) {
                const std::string boundGameID = GetGameIDForUser(conn->authenticatedUser);
                onUserDisconnected(conn->authenticatedUser, boundGameID);
            }
            if (enableDisconnectClear) {
                ClearGameIDForUser(conn->authenticatedUser);
            }
            ReleaseAuthenticatedSession(conn->authenticatedUser, conn->clientIP, conn->accountStateOwner);
        }

        // 通知防CC系统
        if (antiCC && antiCC->IsEnabled()) {
            antiCC->OnConnectionClosed(conn->clientIP);
        }
        GlobalAntiCCCoordinator::GetInstance().RecordConnectionClosed(m_instanceId, conn->clientIP);
    }
}

// ==================== IOCP服务器响应数据处理回调 ====================
ServerDataResult PacketCollector::OnIOCPServerData(ProxyConnectionInfo* conn, std::vector<uint8_t>& data) {
    ServerDataResult result;

    if (!conn || data.empty()) {
        return result;
    }

    if (HandleDisconnectFeed(conn, data, DisconnectDirection::ServerToClient)) {
        result.intercepted = true;
        return result;
    }

    // 获取当前连接的账号名
    std::string username = conn->authenticatedUser;

    // 与传统模式保持一致：先记录原始响应数据，再执行WPE/其他处理。
    // 这样即使响应被后续修改或拦截，代理数据里仍能看到上游返回的原始响应。
    RecordProxyPacket(
        conn->id,
        username,
        conn->gameID,
        conn->clientIP,
        conn->targetHost,
        std::string(),
        conn->targetPort,
        false,
        false,
        data);

    // 🔥 获取用户启用的滤镜列表（如果启用了用户滤镜模式）
    std::vector<int> userFilters;
    const std::vector<int>* userEnabledFiltersPtr = nullptr;
    if (IsUserFilterModeEnabled() && !username.empty() && g_userFilterManager) {
        // 🔥 使用 GetEffectiveUserFilters：如果用户未配置，返回默认配置
        std::set<int> filterSet = g_userFilterManager->GetEffectiveUserFilters(m_instanceId, username);
        userFilters.assign(filterSet.begin(), filterSet.end());
        userEnabledFiltersPtr = &userFilters;
    }

    // 应用WPE滤镜（响应方向）
    if (g_wpeFilterManager && g_wpeFilterManager->GetFilterCount() > 0) {
        // isRequest=false 表示这是响应包（服务器→客户端）
        // isCollector=false 表示这是伪心跳端（不是采集端）
        // 先执行"替换前"阶段的滤镜
        auto filterResult = ApplyWPEFilters(data, m_instanceId, false, false, WPEFilter::FilterPriority::BeforeHeartbeat, username, userEnabledFiltersPtr);

        if (filterResult.intercepted) {
            result.intercepted = true;
            return result;
        }

        if (filterResult.modified && !filterResult.modifiedData.empty()) {
            data = filterResult.modifiedData;
            result.modified = true;
            result.modifiedData = data;
        }

        // 再执行"替换后"阶段的滤镜
        filterResult = ApplyWPEFilters(data, m_instanceId, false, false, WPEFilter::FilterPriority::AfterHeartbeat, username, userEnabledFiltersPtr);

        if (filterResult.intercepted) {
            result.intercepted = true;
            return result;
        }

        if (filterResult.modified && !filterResult.modifiedData.empty()) {
            data = filterResult.modifiedData;
            result.modified = true;
            result.modifiedData = data;
        }
    }

    return result;
}

std::shared_ptr<const std::vector<DisconnectRule>> PacketCollector::GetDisconnectRulesSnapshot_() const {
    auto snapshot = std::atomic_load(&disconnectRulesSnapshot);
    if (!snapshot) {
        return GetEmptyDisconnectRuleSnapshot();
    }
    return snapshot;
}

bool PacketCollector::SetDisconnectRules(const std::vector<DisconnectRule>& rules, std::string* error) {
    std::vector<DisconnectRule> preparedRules;
    if (!DisconnectRuleCodec::PrepareRules(rules, preparedRules, error)) {
        return false;
    }

    auto snapshot = std::make_shared<const std::vector<DisconnectRule>>(std::move(preparedRules));
    std::atomic_store(&disconnectRulesSnapshot, snapshot);

    if (iocpPool) {
        auto allConnections = iocpPool->GetAllConnections();
        for (const auto& conn : allConnections) {
            if (conn) {
                DisconnectRuleEngine::ResetRuntimeState(conn->disconnectState);
            }
        }
    } else {
        std::vector<std::shared_ptr<ProxyConnection>> connectionSnapshot;
        {
            std::lock_guard<std::mutex> lock(connectionsMutex);
            connectionSnapshot = proxyConnections;
        }
        for (const auto& conn : connectionSnapshot) {
            if (conn) {
                DisconnectRuleEngine::ResetRuntimeState(conn->disconnectState);
            }
        }
    }

    disconnectCheckCV.notify_all();
    return true;
}

std::vector<DisconnectRule> PacketCollector::GetDisconnectRules() const {
    auto snapshot = GetDisconnectRulesSnapshot_();
    return snapshot ? *snapshot : std::vector<DisconnectRule>();
}

void PacketCollector::ClearDisconnectRules() {
    std::atomic_store(&disconnectRulesSnapshot, GetEmptyDisconnectRuleSnapshot());
    disconnectCheckCV.notify_all();
}

bool PacketCollector::HandleDisconnectFeed(std::shared_ptr<ProxyConnection> conn, const std::vector<uint8_t>& data, DisconnectDirection direction) {
    if (!conn || !conn->isActive.load()) {
        return false;
    }

    auto snapshot = GetDisconnectRulesSnapshot_();
    if (!DisconnectRuleEngine::HasEnabledRules(*snapshot)) {
        return false;
    }

    const DisconnectFeedResult feedResult = DisconnectRuleEngine::FeedData(
        *snapshot,
        static_cast<uint16_t>(conn->targetPort),
        conn->disconnectState,
        data,
        direction);

    if (feedResult.delayStarted) {
        AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[断网] 命中十六进制规则，开始延迟断开: " +
            conn->targetAddr + ":" + std::to_string(conn->targetPort));
    }

    if (feedResult.disconnectNow) {
        RequestTraditionalDisconnect(conn, "命中十六进制规则立即断开");
        return true;
    }

    return false;
}

bool PacketCollector::HandleDisconnectFeed(ProxyConnectionInfo* conn, const std::vector<uint8_t>& data, DisconnectDirection direction) {
    if (!conn || !conn->isActive) {
        return false;
    }

    auto snapshot = GetDisconnectRulesSnapshot_();
    if (!DisconnectRuleEngine::HasEnabledRules(*snapshot)) {
        return false;
    }

    const DisconnectFeedResult feedResult = DisconnectRuleEngine::FeedData(
        *snapshot,
        static_cast<uint16_t>(conn->targetPort),
        conn->disconnectState,
        data,
        direction);

    if (feedResult.delayStarted) {
        AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[断网][IOCP] 命中十六进制规则，开始延迟断开: " +
            conn->targetHost + ":" + std::to_string(conn->targetPort));
    }

    if (feedResult.disconnectNow) {
        RequestIOCPDisconnect(conn, "命中十六进制规则立即断开");
        return true;
    }

    return false;
}

void PacketCollector::RequestTraditionalDisconnect(const std::shared_ptr<ProxyConnection>& conn, const std::string& reason) {
    if (!conn) {
        return;
    }

    bool expected = false;
    if (!conn->disconnectState.disconnectRequested.compare_exchange_strong(expected, true)) {
        return;
    }

    conn->isActive = false;

    if (conn->clientSocket != INVALID_SOCKET) {
        shutdown(conn->clientSocket, SD_BOTH);
        closesocket(conn->clientSocket);
        conn->clientSocket = INVALID_SOCKET;
    }
    if (conn->serverSocket != INVALID_SOCKET) {
        shutdown(conn->serverSocket, SD_BOTH);
        closesocket(conn->serverSocket);
        conn->serverSocket = INVALID_SOCKET;
    }

    AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[断网] 已断开连接: " + conn->clientAddr +
        " -> " + conn->targetAddr + ":" + std::to_string(conn->targetPort) +
        " | 原因: " + reason);
}

void PacketCollector::RequestIOCPDisconnect(ProxyConnectionInfo* conn, const std::string& reason) {
    if (!conn || !iocpPool) {
        return;
    }

    bool expected = false;
    if (!conn->disconnectState.disconnectRequested.compare_exchange_strong(expected, true)) {
        return;
    }

    AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[断网][IOCP] 已请求断开连接: " + conn->clientAddr +
        " -> " + conn->targetHost + ":" + std::to_string(conn->targetPort) +
        " | 原因: " + reason);
    iocpPool->CloseConnection(conn->id, reason);
}

void PacketCollector::DisconnectCheckThreadLoop() {
    while (disconnectCheckThreadRunning) {
        auto snapshot = GetDisconnectRulesSnapshot_();
        const bool hasRules = DisconnectRuleEngine::HasEnabledRules(*snapshot);

        {
            std::unique_lock<std::mutex> lock(disconnectCheckMutex);
            disconnectCheckCV.wait_for(
                lock,
                hasRules ? std::chrono::seconds(1) : std::chrono::seconds(5),
                [this]() { return !disconnectCheckThreadRunning.load(); });
        }

        if (!disconnectCheckThreadRunning) {
            break;
        }

        snapshot = GetDisconnectRulesSnapshot_();
        if (!DisconnectRuleEngine::HasEnabledRules(*snapshot)) {
            continue;
        }

        if (iocpPool) {
            auto allConnections = iocpPool->GetAllConnections();
            for (const auto& conn : allConnections) {
                if (!conn || !conn->isActive || conn->state != ConnectionState::STATE_FORWARDING) {
                    continue;
                }

                std::string reason;
                if (DisconnectRuleEngine::ShouldDisconnectNow(*snapshot, static_cast<uint16_t>(conn->targetPort), conn->disconnectState, &reason)) {
                    RequestIOCPDisconnect(conn.get(), reason);
                }
            }
        }
        else {
            std::vector<std::shared_ptr<ProxyConnection>> connectionSnapshot;
            {
                std::lock_guard<std::mutex> lock(connectionsMutex);
                connectionSnapshot = proxyConnections;
            }

            for (const auto& conn : connectionSnapshot) {
                if (!conn || !conn->isActive.load()) {
                    continue;
                }

                std::string reason;
                if (DisconnectRuleEngine::ShouldDisconnectNow(*snapshot, static_cast<uint16_t>(conn->targetPort), conn->disconnectState, &reason)) {
                    RequestTraditionalDisconnect(conn, reason);
                }
            }
        }
    }
}

// ===========================================================================================
// 🔥 修复1: 异步数据库写入实现
// ===========================================================================================

// 外部全局变量声明（假设在主程序中定义）
extern DatabaseManager* g_database;
extern bool g_enableRecording;

// ===== 🔥 修复1: 数据库异步写入线程 =====
void PacketCollector::DatabaseWriteThreadLoop() {
    Log("[数据库] 异步写入线程已启动");

    std::vector<HeartbeatRecord> batchBuffer;
    batchBuffer.reserve(100);  // 预分配100条记录的空间

    auto lastBatchTime = std::chrono::steady_clock::now();
    const auto BATCH_INTERVAL = std::chrono::milliseconds(100);  // 每100ms批量写入一次

    while (dbWriteThreadRunning || !dbWriteQueue.empty()) {
        batchBuffer.clear();

        {
            std::unique_lock<std::mutex> lock(dbWriteMutex);

            // 🔥 等待数据或超时
            dbWriteCV.wait_for(lock, BATCH_INTERVAL, [this] {
                return !dbWriteQueue.empty() || !dbWriteThreadRunning;
                });

            // 如果线程停止且队列为空，退出
            if (!dbWriteThreadRunning && dbWriteQueue.empty()) {
                break;
            }

            // 🔥 批量取出最多100条记录（减少锁持有时间）
            while (!dbWriteQueue.empty() && batchBuffer.size() < 100) {
                batchBuffer.push_back(dbWriteQueue.front().record);
                dbWriteQueue.pop();
                dbQueueSize.fetch_sub(1);
            }
        }

        // 🔥 关键：在锁外执行数据库写入，避免阻塞其他线程
        if (!batchBuffer.empty() && g_database) {
            try {
                // 批量写入记录
                for (const auto& record : batchBuffer) {
                    g_database->InsertHeartbeatData(record);
                }

                // 每1000条记录记录一次日志
                static std::atomic<int> totalWritten{ 0 };
                totalWritten += static_cast<int>(batchBuffer.size());
                if (totalWritten % 1000 == 0) {
                    AB_LOG_INFO("[数据库] 已批量写入 " + std::to_string(totalWritten.load()) + " 条记录");
                }
            }
            catch (const std::exception& e) {
                AB_LOG_ERROR("[数据库] 批量写入失败: " + std::string(e.what()));
            }
        }

        lastBatchTime = std::chrono::steady_clock::now();
    }

    Log("[数据库] 异步写入线程已退出");
}

// ===== 🔥 修复1: 异步入队数据库写入任务 =====
void PacketCollector::EnqueueDatabaseWrite(const HeartbeatRecord& record) {
    DatabaseWriteTask task;
    task.record = record;
    task.enqueueTime = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(dbWriteMutex);
        dbWriteQueue.push(task);
        dbQueueSize.fetch_add(1);

        // 🔥 队列大小限制，防止内存溢出
        const size_t MAX_QUEUE_SIZE = 50000;
        if (dbWriteQueue.size() > MAX_QUEUE_SIZE) {
            dbWriteQueue.pop();  // 丢弃最旧的数据
            dbQueueSize.fetch_sub(1);

            static std::atomic<int> dropCount{ 0 };
            if (++dropCount % 1000 == 0) {
                AB_LOG_WARNING("[数据库] 写入队列溢出，已丢弃 " + std::to_string(dropCount.load()) + " 条旧数据");
            }
        }
    }

    dbWriteCV.notify_one();
}

// ===========================================================================================
// 🔥 优化：SNI缓存清理线程实现
// ===========================================================================================

// ===== 🔥 优化：SNI缓存清理线程循环 =====
void PacketCollector::SNICacheCleanupThreadLoop() {
    Log("[SNI缓存] 清理线程已启动");

    while (sniCleanupThreadRunning) {
        // 使用条件变量等待5分钟，可以被提前唤醒
        std::unique_lock<std::mutex> lock(sniCleanupMutex);
        sniCleanupCV.wait_for(lock, std::chrono::minutes(5), [this]() {
            return !sniCleanupThreadRunning.load();
        });

        if (!sniCleanupThreadRunning) break;

        CleanupExpiredSNICache();
    }

    Log("[SNI缓存] 清理线程已退出");
}

// ===== 🔥 优化：清理过期的SNI缓存 =====
void PacketCollector::CleanupExpiredSNICache() {
    std::lock_guard<std::mutex> lock(sniCacheMutex);

    auto now = std::chrono::steady_clock::now();
    size_t removedCount = 0;
    size_t originalSize = sniCache.size();

    // LRU策略：如果超过最大大小，删除最旧的条目
    if (sniCache.size() > MAX_SNI_CACHE_SIZE) {
        // 从LRU链表尾部开始删除（最旧的条目）
        while (sniCache.size() > MAX_SNI_CACHE_SIZE && !sniCacheLRU.empty()) {
            const std::string& oldestKey = sniCacheLRU.back();
            sniCache.erase(oldestKey);
            sniCacheLRU.pop_back();
            removedCount++;
        }

        AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[SNI缓存] LRU清理: 删除了 " +
            std::to_string(removedCount) + " 个最旧的条目");
    }

    // 删除过期条目
    size_t expiredCount = 0;
    auto it = sniCache.begin();
    while (it != sniCache.end()) {
        auto age = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.timestamp).count();
        if (age >= SNI_CACHE_EXPIRE_SECONDS) {
            // 从LRU链表中移除
            sniCacheLRU.remove(it->first);
            it = sniCache.erase(it);
            expiredCount++;
        } else {
            ++it;
        }
    }

    if (expiredCount > 0) {
        AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[SNI缓存] 过期清理: 删除了 " +
            std::to_string(expiredCount) + " 个过期条目");
    }

    removedCount += expiredCount;

    if (removedCount > 0) {
        AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[SNI缓存] 清理完成: " +
            std::to_string(originalSize) + " -> " + std::to_string(sniCache.size()) +
            " (删除 " + std::to_string(removedCount) + " 个)");
    }
}

// ==================== 分包处理配置接口 ====================
void PacketCollector::SetPacketSplitEnabled(bool enabled) {
    enablePacketSplit = enabled;
    Log("[分包处理] " + std::string(enabled ? "已启用" : "已禁用"));

    // 🔥 同步到IOCP线程池
    if (iocpPool) {
        iocpPool->SetPacketSplitEnabled(enabled);
    }
}

bool PacketCollector::IsPacketSplitEnabled() const {
    return enablePacketSplit.load();
}

void PacketCollector::SetPacketSplitPorts(const std::vector<int>& ports) {
    {
        std::lock_guard<std::mutex> lock(packetSplitMutex);
        packetSplitPorts = ports;
    }

    std::string portList;
    for (size_t i = 0; i < ports.size(); i++) {
        if (i > 0) portList += ", ";
        portList += std::to_string(ports[i]);
    }
    Log("[分包处理] 端口列表已更新: " + (portList.empty() ? "无" : portList));

    // 🔥 同步到IOCP线程池
    if (iocpPool) {
        iocpPool->SetPacketSplitPorts(ports);
    }
}

std::vector<int> PacketCollector::GetPacketSplitPorts() const {
    std::lock_guard<std::mutex> lock(packetSplitMutex);
    return packetSplitPorts;
}

bool PacketCollector::IsPacketSplitEnabledForPort(int port) const {
    if (!enablePacketSplit.load()) {
        return false;  // 全局禁用分包处理
    }

    std::lock_guard<std::mutex> lock(packetSplitMutex);

    // 如果端口列表为空，表示对所有端口都启用分包处理
    if (packetSplitPorts.empty()) {
        return true;
    }

    // 检查端口是否在列表中
    for (int p : packetSplitPorts) {
        if (p == port) {
            return true;
        }
    }

    return false;
}

// ===== 🔥 对不分包流量应用WPE滤镜 =====
void PacketCollector::SetApplyWpeOnNonSplitTraffic(bool enabled) {
    applyWpeOnNonSplitTraffic = enabled;

    // 同步到 IOCP 模式
    if (iocpPool) {
        iocpPool->SetApplyWpeOnNonSplitTraffic(enabled);
    }

    AB_LOG_INFO("[PacketCollector] 对不分包流量应用WPE滤镜: " + std::string(enabled ? "启用" : "禁用"));
}

bool PacketCollector::IsApplyWpeOnNonSplitTraffic() const {
    return applyWpeOnNonSplitTraffic.load();
}

// ===== 🔥 用户滤镜模式实现 =====
void PacketCollector::SetUserFilterMode(bool enabled) {
    enableUserFilterMode = enabled;
    if (iocpPool) {
        iocpPool->SetUserFilterMode(enabled);
    }
}

bool PacketCollector::IsUserFilterModeEnabled() const {
    return enableUserFilterMode.load();
}

void PacketCollector::SetUserFilterHttpPort(int port) {
    userFilterHttpPort = port;
    // 如果使用IOCP模式，同步设置到iocpPool
    if (iocpPool) {
        iocpPool->SetUserFilterHttpPort(port);
    }
}

int PacketCollector::GetUserFilterHttpPort() const {
    return userFilterHttpPort;
}

bool PacketCollector::StartUserFilterHttpServer() {
    // 🔥 如果对象已存在且正在运行，直接返回
    if (userFilterHttpServer && userFilterHttpServer->IsRunning()) {
        Log("[用户滤镜] HTTP服务器已在运行");
        return true;
    }

    // 🔥 销毁旧对象（如果存在），确保使用最新的端口配置
    if (userFilterHttpServer) {
        userFilterHttpServer.reset();
    }

    // 创建新的HTTP服务器对象
    userFilterHttpServer = std::make_unique<UserFilterWebServer>(m_instanceId, userFilterHttpPort);

    // 设置SOCKS验证回调
    // 设置SOCKS验证回调（带失败原因）
    userFilterHttpServer->SetSocksValidatorWithReason([this](const std::string& username, const std::string& password, std::string& failReason) {
        return this->ValidateAccountWithReason(username, password, failReason);
    });

    // 设置滤镜列表获取回调
    userFilterHttpServer->SetFilterListGetter([this]() {
        return this->GetUserFilterAvailableFiltersCached();
    });

    userFilterHttpServer->SetAuthorizedFilterListGetter([this](const std::string& username) {
        std::vector<AuthorizedWebFilter> result;
        if (!g_wpeFilterManager || !g_userFilterManager) return result;

        const auto state = g_userFilterManager->GetAuthorizedFilters(m_instanceId, username);
        const auto allFilters = g_wpeFilterManager->GetAllFilters();
        for (const auto& filter : allFilters) {
            if (state.authorizedFilterIds.count(filter.id) == 0) continue;
            if (!filter.target.applyToAllInstances) {
                const auto& ids = filter.target.targetInstanceIds;
                if (!ids.empty() && std::find(ids.begin(), ids.end(), m_instanceId) == ids.end()) {
                    continue;
                }
            }

            AuthorizedWebFilter item;
            item.id = filter.id;
            item.name = filter.webDisplayName.empty() ? filter.name : filter.webDisplayName;
            item.applyToCollector = filter.target.applyToCollector || filter.target.applyToAllInstances;
            item.applyToHeartbeat = filter.target.applyToHeartbeat || filter.target.applyToAllInstances;
            result.push_back(item);
        }
        return result;
    });

    // 设置账号信息获取回调（返回用户到期时间）
    userFilterHttpServer->SetAccountInfoGetter([this](const std::string& username) -> std::string {
        // 优先从外部账号源获取
        if (externalAccountSource) {
            auto allAccounts = externalAccountSource->GetAllAccounts();
            for (const auto& account : allAccounts) {
                if (account.username == username) {
                    return account.expireTime.empty() ? "永久" : account.expireTime;
                }
            }
        }

        // 从本地账号列表获取
        std::lock_guard<std::mutex> lock(accountsMutex);
        auto it = accounts.find(username);
        if (it != accounts.end()) {
            return it->second.expireTime.empty() ? "永久" : it->second.expireTime;
        }

        return "未知";
    });

    userFilterHttpServer->SetUserGameInfoGetter([this](const std::string& username) {
        UserGameInfo info;
        info.gameId = GetGameIDForUser(username);
        if (userWebCollectedPacketPool) {
            info.poolCount = userWebCollectedPacketPool->GetCountByUsername(username);
        }
        return info;
    });

    userFilterHttpServer->SetUserPoolClearer([this](const std::string& username) {
        if (username.empty() || !userWebCollectedPacketPool) return static_cast<size_t>(0);
        return userWebCollectedPacketPool->ClearByUsername(username);
    });

    // 启动HTTP服务器
    bool success = userFilterHttpServer->Start();
    if (success) {
        Log("[用户滤镜] HTTP服务器启动成功，端口: " + std::to_string(userFilterHttpPort));
    } else {
        Log("[用户滤镜] HTTP服务器启动失败");
    }
    return success;
}

void PacketCollector::StopUserFilterHttpServer() {
    if (userFilterHttpServer) {
        userFilterHttpServer->Stop();
        userFilterHttpServer.reset();
        Log("[用户滤镜] HTTP服务器已停止");
    }
}

bool PacketCollector::IsUserFilterHttpServerRunning() const {
    if (userFilterHttpServer) {
        return userFilterHttpServer->IsRunning();
    }
    return false;
}

std::vector<std::pair<int, std::string>> PacketCollector::GetUserFilterAvailableFiltersCached() {
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(userFilterAvailableFiltersMutex);
        if (!userFilterAvailableFiltersCache.empty() &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - userFilterAvailableFiltersCacheAt).count() < 1000) {
            return userFilterAvailableFiltersCache;
        }
    }

    std::vector<std::pair<int, std::string>> filters;
    if (g_wpeFilterManager) {
        auto allFilters = g_wpeFilterManager->GetAllFilters();
        for (const auto& filter : allFilters) {
            const std::string displayName = filter.webDisplayName.empty() ? filter.name : filter.webDisplayName;
            if (filter.target.applyToAllInstances) {
                filters.push_back({ filter.id, displayName });
            } else {
                const auto& targetIds = filter.target.targetInstanceIds;
                if (std::find(targetIds.begin(), targetIds.end(), m_instanceId) != targetIds.end()) {
                    filters.push_back({ filter.id, displayName });
                }
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(userFilterAvailableFiltersMutex);
        userFilterAvailableFiltersCache = filters;
        userFilterAvailableFiltersCacheAt = now;
    }

    return filters;
}

// ===== 🔥 断开用户连接实现 =====
bool PacketCollector::DisconnectUser(const std::string& username, const std::string& reason) {
    if (username.empty()) {
        return false;
    }

    bool disconnected = false;

    // IOCP模式：遍历所有连接并断开匹配的用户
    if (iocpPool) {
        auto allConns = iocpPool->GetAllConnections();
        for (const auto& conn : allConns) {
            if (conn && conn->isActive && conn->authenticatedUser == username) {
                iocpPool->CloseConnection(conn->id, reason);
                disconnected = true;
            }
        }
    }
    // 传统模式/线程池模式：遍历proxyConnections
    else {
        std::lock_guard<std::mutex> lock(connectionsMutex);
        for (auto& conn : proxyConnections) {
            if (conn && conn->isActive.load() && conn->authenticatedUser == username) {
                conn->isActive = false;
                // 关闭客户端和服务器socket
                if (conn->clientSocket != INVALID_SOCKET) {
                    shutdown(conn->clientSocket, SD_BOTH);
                    closesocket(conn->clientSocket);
                    conn->clientSocket = INVALID_SOCKET;
                }
                if (conn->serverSocket != INVALID_SOCKET) {
                    shutdown(conn->serverSocket, SD_BOTH);
                    closesocket(conn->serverSocket);
                    conn->serverSocket = INVALID_SOCKET;
                }
                disconnected = true;
            }
        }
    }

    if (disconnected) {
        AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[在线统计] 用户 " + username + " 已被断开，原因: " + reason);
    }

    return disconnected;
}

bool PacketCollector::DisconnectOneLowPriorityConnection_() {
    const auto globalPressure = GlobalAntiCCCoordinator::GetInstance().GetGlobalPressure();
    const auto instancePressure = GlobalAntiCCCoordinator::GetInstance().GetInstancePressure(m_instanceId);
    const int triggerThreshold = antiCCLowPriorityEvictionThreshold.load();
    const int currentConnections = iocpPool ? iocpPool->GetTotalConnectionCount() : GetCurrentConnections();
    const int maxConnections = antiCC && antiCC->GetConfig().maxConnections > 0 ? antiCC->GetConfig().maxConnections : 1;
    const int currentPercent = (currentConnections * 100) / maxConnections;

    if (globalPressure < AntiCCPressureState::Overloaded &&
        instancePressure < AntiCCPressureState::Overloaded &&
        currentPercent < triggerThreshold) {
        return false;
    }

    std::string targetIP;
    uint64_t targetConnId = 0;
    std::chrono::steady_clock::time_point oldest = std::chrono::steady_clock::now();
    float lowestScore = 101.0f;
    bool found = false;

    if (iocpPool) {
        auto allConns = iocpPool->GetAllConnections();
        for (const auto& conn : allConns) {
            if (!conn || !conn->isActive) {
                continue;
            }
            if (!conn->authenticatedUser.empty()) {
                continue;
            }
            if (antiCC && antiCC->IsInWhitelist(conn->clientIP)) {
                continue;
            }

            const float score = GlobalAntiCCCoordinator::GetInstance().GetEffectiveScoreForInstance(m_instanceId, conn->clientIP);
            if (!found || score < lowestScore ||
                (score == lowestScore && conn->createTime < oldest)) {
                found = true;
                lowestScore = score;
                oldest = conn->createTime;
                targetConnId = conn->id;
                targetIP = conn->clientIP;
            }
        }

        if (found && lowestScore < 60.0f) {
            iocpPool->CloseConnection(targetConnId, "AntiCC高压驱逐低优先级连接");
            AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] 高压驱逐低优先级IOCP连接: " + targetIP +
                " instance=" + m_instanceId +
                " score=" + std::to_string(lowestScore));
            return true;
        }
        return false;
    }

    std::shared_ptr<ProxyConnection> targetConn;
    {
        std::lock_guard<std::mutex> lock(connectionsMutex);
        for (const auto& conn : proxyConnections) {
            if (!conn || !conn->isActive.load()) {
                continue;
            }
            if (!conn->authenticatedUser.empty()) {
                continue;
            }

            std::string connIP = conn->clientAddr;
            size_t colonPos = connIP.find(':');
            if (colonPos != std::string::npos) {
                connIP = connIP.substr(0, colonPos);
            }
            if (antiCC && antiCC->IsInWhitelist(connIP)) {
                continue;
            }

            const float score = GlobalAntiCCCoordinator::GetInstance().GetEffectiveScoreForInstance(m_instanceId, connIP);
            if (!found || score < lowestScore) {
                found = true;
                lowestScore = score;
                targetConn = conn;
                targetIP = connIP;
            }
        }
    }

    if (targetConn && lowestScore < 60.0f) {
        targetConn->isActive = false;
        if (targetConn->clientSocket != INVALID_SOCKET) {
            closesocket(targetConn->clientSocket);
            targetConn->clientSocket = INVALID_SOCKET;
        }
        if (targetConn->serverSocket != INVALID_SOCKET) {
            closesocket(targetConn->serverSocket);
            targetConn->serverSocket = INVALID_SOCKET;
        }
        AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[AntiCC] 高压驱逐低优先级传统连接: " + targetIP +
            " instance=" + m_instanceId +
            " score=" + std::to_string(lowestScore));
        return true;
    }

    return false;
}

bool PacketCollector::DisconnectIP(const std::string& clientIP) {
    if (clientIP.empty()) {
        return false;
    }

    bool disconnected = false;

    if (iocpPool) {
        auto allConns = iocpPool->GetAllConnections();
        for (const auto& conn : allConns) {
            if (conn && conn->clientIP == clientIP) {
                iocpPool->CloseConnection(conn->id, "GlobalAntiCC按IP断开");
                disconnected = true;
            }
        }
    }
    else {
        std::lock_guard<std::mutex> lock(connectionsMutex);
        for (auto& conn : proxyConnections) {
            if (!conn) {
                continue;
            }

            std::string connIP = conn->clientAddr;
            size_t colonPos = connIP.find(':');
            if (colonPos != std::string::npos) {
                connIP = connIP.substr(0, colonPos);
            }

            if (connIP == clientIP) {
                conn->isActive = false;
                if (conn->clientSocket != INVALID_SOCKET) {
                    closesocket(conn->clientSocket);
                    conn->clientSocket = INVALID_SOCKET;
                }
                if (conn->serverSocket != INVALID_SOCKET) {
                    closesocket(conn->serverSocket);
                    conn->serverSocket = INVALID_SOCKET;
                }
                disconnected = true;
            }
        }
    }

    if (disconnected) {
        AB_LOG_INFO_CAT(LOG_CAT_ANTICC, "[GlobalAntiCC] 按IP断开连接: " + clientIP +
            " instance=" + m_instanceId);
    }

    return disconnected;
}

// ===== 🔥 新增：流量过滤规则管理实现 =====

void PacketCollector::SetTrafficFilterEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(trafficFilterMutex);
    trafficFilterConfig.enabled = enabled;
    AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[流量过滤] 过滤功能: " + std::string(enabled ? "启用" : "禁用"));
}

bool PacketCollector::IsTrafficFilterEnabled() const {
    std::lock_guard<std::mutex> lock(trafficFilterMutex);
    return trafficFilterConfig.enabled;
}

void PacketCollector::SetSNISniffingEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(trafficFilterMutex);
    trafficFilterConfig.sniSniffingEnabled = enabled;
    AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[流量过滤] SNI嗅探: " + std::string(enabled ? "启用" : "禁用"));
}

bool PacketCollector::IsSNISniffingEnabled() const {
    std::lock_guard<std::mutex> lock(trafficFilterMutex);
    return trafficFilterConfig.sniSniffingEnabled;
}

bool PacketCollector::AddTrafficRule(const TrafficFilterRule& rule) {
    std::lock_guard<std::mutex> lock(trafficFilterMutex);

    TrafficFilterRule newRule = rule;
    newRule.id = nextTrafficRuleId++;

    trafficFilterConfig.rules.push_back(newRule);

    AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[流量过滤] 添加规则 #" + std::to_string(newRule.id) +
        ": " + newRule.description);

    // 🔥 优化：重建规则索引
    RebuildRuleIndexes();

    return true;
}

bool PacketCollector::RemoveTrafficRule(int ruleId) {
    std::lock_guard<std::mutex> lock(trafficFilterMutex);

    auto it = std::find_if(trafficFilterConfig.rules.begin(), trafficFilterConfig.rules.end(),
        [ruleId](const TrafficFilterRule& r) { return r.id == ruleId; });

    if (it != trafficFilterConfig.rules.end()) {
        AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[流量过滤] 删除规则 #" + std::to_string(ruleId));
        trafficFilterConfig.rules.erase(it);

        // 🔥 优化：重建规则索引
        RebuildRuleIndexes();

        return true;
    }

    return false;
}

bool PacketCollector::UpdateTrafficRule(const TrafficFilterRule& rule) {
    std::lock_guard<std::mutex> lock(trafficFilterMutex);

    auto it = std::find_if(trafficFilterConfig.rules.begin(), trafficFilterConfig.rules.end(),
        [&rule](const TrafficFilterRule& r) { return r.id == rule.id; });

    if (it != trafficFilterConfig.rules.end()) {
        *it = rule;
        AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[流量过滤] 更新规则 #" + std::to_string(rule.id));

        // 🔥 优化：重建规则索引
        RebuildRuleIndexes();

        return true;
    }

    return false;
}

std::vector<TrafficFilterRule> PacketCollector::GetAllTrafficRules() const {
    std::lock_guard<std::mutex> lock(trafficFilterMutex);
    return trafficFilterConfig.rules;
}

TrafficFilterRule PacketCollector::GetTrafficRule(int ruleId) const {
    std::lock_guard<std::mutex> lock(trafficFilterMutex);

    auto it = std::find_if(trafficFilterConfig.rules.begin(), trafficFilterConfig.rules.end(),
        [ruleId](const TrafficFilterRule& r) { return r.id == ruleId; });

    if (it != trafficFilterConfig.rules.end()) {
        return *it;
    }

    return TrafficFilterRule();
}

// ===== 🔥 优化：重建规则索引（性能优化）=====
void PacketCollector::RebuildRuleIndexes() {
    // 注意：调用此函数前必须已持有 trafficFilterMutex 锁

    portRuleSet.clear();
    ipRuleSet.clear();
    domainRuleSet.clear();
    portDomainSniMap.clear();
    needsSNISniffing = false;

    for (const auto& rule : trafficFilterConfig.rules) {
        if (!rule.enabled) continue;

        switch (rule.type) {
            case TrafficRuleType::PORT_MATCH:
                try {
                    portRuleSet.insert(std::stoi(rule.value1));
                } catch (...) {
                    AB_LOG_WARNING("[流量过滤] 规则 #" + std::to_string(rule.id) + " 端口格式错误: " + rule.value1);
                }
                break;

            case TrafficRuleType::IP_MATCH:
                ipRuleSet.insert(rule.value1);
                break;

            case TrafficRuleType::DOMAIN_MATCH:
                domainRuleSet.insert(rule.value1);
                needsSNISniffing = true;  // 域名匹配可能需要SNI
                break;

            case TrafficRuleType::PORT_DOMAIN_SNI:
                try {
                    int port = std::stoi(rule.value1);
                    portDomainSniMap[port].insert(rule.value2);
                    needsSNISniffing = true;  // 强制需要SNI
                } catch (...) {
                    AB_LOG_WARNING("[流量过滤] 规则 #" + std::to_string(rule.id) + " 端口格式错误: " + rule.value1);
                }
                break;
        }
    }

    AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[流量过滤] 规则索引已重建: 端口=" +
        std::to_string(portRuleSet.size()) + ", IP=" + std::to_string(ipRuleSet.size()) +
        ", 域名=" + std::to_string(domainRuleSet.size()) +
        ", 端口+SNI=" + std::to_string(portDomainSniMap.size()) +
        ", 需要SNI=" + std::string(needsSNISniffing ? "是" : "否"));
}


void PacketCollector::ClearAllTrafficRules() {
    std::lock_guard<std::mutex> lock(trafficFilterMutex);
    trafficFilterConfig.rules.clear();

    // 🔥 优化：清空规则索引
    portRuleSet.clear();
    ipRuleSet.clear();
    domainRuleSet.clear();
    portDomainSniMap.clear();
    needsSNISniffing = false;

    AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[流量过滤] 清空所有规则");
}

// ===== 🔥 优化：更新SNI缓存（LRU） =====
void PacketCollector::UpdateSNICache(const std::string& key, const std::string& domain) {
    std::lock_guard<std::mutex> lock(sniCacheMutex);

    // 如果已存在，先从LRU链表中移除
    auto it = sniCache.find(key);
    if (it != sniCache.end()) {
        sniCacheLRU.remove(key);
    }

    // 如果缓存已满，移除最旧的条目
    if (sniCache.size() >= MAX_SNI_CACHE_SIZE) {
        std::string oldestKey = sniCacheLRU.back();
        sniCacheLRU.pop_back();
        sniCache.erase(oldestKey);
        AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[SNI缓存] LRU淘汰: " + oldestKey);
    }

    // 添加新条目到缓存和LRU链表头部
    sniCache[key] = SNICacheEntry(domain);
    sniCacheLRU.push_front(key);

    AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[SNI缓存] 添加: " + key + " -> " + domain +
        " (缓存大小: " + std::to_string(sniCache.size()) + ")");
}

// ===== 🔥 优化：从SNI缓存获取 =====
std::string PacketCollector::GetFromSNICache(const std::string& key) {
    std::lock_guard<std::mutex> lock(sniCacheMutex);

    auto it = sniCache.find(key);
    if (it != sniCache.end()) {
        // 检查是否过期
        auto now = std::chrono::steady_clock::now();
        auto age = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.timestamp).count();

        if (age < SNI_CACHE_EXPIRE_SECONDS) {
            // 未过期，移动到LRU链表头部
            sniCacheLRU.remove(key);
            sniCacheLRU.push_front(key);
            return it->second.domain;
        } else {
            // 已过期，删除
            AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[SNI缓存] 过期删除: " + key);
            sniCacheLRU.remove(key);
            sniCache.erase(it);
        }
    }

    return "";
}

// ===== 🔥 优化：清理过期SNI缓存 =====
// ===== 🔥 检查流量是否匹配规则（优化版：使用索引快速查找） =====
bool PacketCollector::CheckTrafficRules(const std::string& targetHost, int targetPort, const std::string& sniDomain) {
    std::lock_guard<std::mutex> lock(trafficFilterMutex);

    // 如果未启用过滤，直接放行
    if (!trafficFilterConfig.enabled) {
        return true;
    }

    // 如果没有规则，拒绝所有流量
    if (trafficFilterConfig.rules.empty()) {
        AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[流量过滤] 拒绝: " + targetHost + ":" +
            std::to_string(targetPort) + " (无匹配规则)");
        return false;
    }

    // 🔥🔥🔥 优先级1：端口+SNI规则（最高优先级）
    // 如果该端口设置了SNI规则，则必须嗅探SNI并匹配指定域名
    auto portSniIt = portDomainSniMap.find(targetPort);
    if (portSniIt != portDomainSniMap.end()) {
        // 该端口需要SNI嗅探
        if (sniDomain.empty()) {
            // 还没有嗅探到SNI，暂时放行（在数据转发阶段会再次检查）
            AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[流量过滤] 暂时放行（等待SNI嗅探）: 端口" +
                std::to_string(targetPort));
            return true;
        }

        // 已经嗅探到SNI，检查是否匹配
        if (portSniIt->second.find(sniDomain) != portSniIt->second.end()) {
            AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[流量过滤] 通过: 端口" + std::to_string(targetPort) +
                " SNI匹配 " + sniDomain);
            return true;
        } else {
            // SNI不匹配，拒绝
            AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[流量过滤] 拒绝: 端口" + std::to_string(targetPort) +
                " SNI不匹配 (期望: " + (portSniIt->second.empty() ? "无" : *portSniIt->second.begin()) +
                ", 实际: " + sniDomain + ")");
            return false;
        }
    }

    // 🔥 优先级2：IP匹配
    if (ipRuleSet.find(targetHost) != ipRuleSet.end()) {
        AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[流量过滤] 通过: IP匹配 " + targetHost);
        return true;
    }

    // 🔥 优先级3：域名匹配
    // 检查 SOCKS5 目标域名
    if (domainRuleSet.find(targetHost) != domainRuleSet.end()) {
        AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[流量过滤] 通过: 域名匹配 " + targetHost);
        return true;
    }

    // 检查 SNI 域名
    if (!sniDomain.empty() && domainRuleSet.find(sniDomain) != domainRuleSet.end()) {
        AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[流量过滤] 通过: SNI域名匹配 " + sniDomain);
        return true;
    }

    // 🔥 优先级4：端口匹配（最低优先级）
    if (portRuleSet.find(targetPort) != portRuleSet.end()) {
        AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[流量过滤] 通过: 端口匹配 " + std::to_string(targetPort));
        return true;
    }

    AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[流量过滤] 拒绝: " + targetHost + ":" +
        std::to_string(targetPort) + " (无匹配规则)");
    return false;
}

// ===== 🔥 从TLS ClientHello中提取SNI域名 =====
std::string PacketCollector::ExtractSNIFromTLS(const std::vector<uint8_t>& data) {
    // TLS ClientHello 最小长度检查
    if (data.size() < 43) {
        return "";
    }

    // 检查是否是TLS握手包 (0x16 = Handshake)
    if (data[0] != 0x16) {
        return "";
    }

    // 检查TLS版本 (0x03 0x01 = TLS 1.0, 0x03 0x03 = TLS 1.2)
    if (data[1] != 0x03) {
        return "";
    }

    // 检查握手类型 (0x01 = Client Hello)
    if (data[5] != 0x01) {
        return "";
    }

    // 跳过固定字段到扩展部分
    size_t pos = 43; // TLS Record(5) + Handshake(4) + Version(2) + Random(32)

    // 跳过 Session ID
    if (pos >= data.size()) return "";
    uint8_t sessionIdLen = data[pos];
    pos += 1 + sessionIdLen;

    // 跳过 Cipher Suites
    if (pos + 2 > data.size()) return "";
    uint16_t cipherSuitesLen = (data[pos] << 8) | data[pos + 1];
    pos += 2 + cipherSuitesLen;

    // 跳过 Compression Methods
    if (pos >= data.size()) return "";
    uint8_t compressionMethodsLen = data[pos];
    pos += 1 + compressionMethodsLen;

    // 读取扩展长度
    if (pos + 2 > data.size()) return "";
    uint16_t extensionsLen = (data[pos] << 8) | data[pos + 1];
    pos += 2;

    // 解析扩展
    size_t extensionsEnd = pos + extensionsLen;
    while (pos + 4 <= extensionsEnd && pos + 4 <= data.size()) {
        uint16_t extType = (data[pos] << 8) | data[pos + 1];
        uint16_t extLen = (data[pos + 2] << 8) | data[pos + 3];
        pos += 4;

        // 0x0000 = server_name extension
        if (extType == 0x0000 && pos + extLen <= data.size()) {
            // 跳过 Server Name List Length (2 bytes)
            if (extLen < 2) break;
            pos += 2;

            // 读取 Server Name Type (0x00 = host_name)
            if (pos >= data.size() || data[pos] != 0x00) break;
            pos += 1;

            // 读取 Server Name Length
            if (pos + 2 > data.size()) break;
            uint16_t nameLen = (data[pos] << 8) | data[pos + 1];
            pos += 2;

            // 提取域名
            if (pos + nameLen <= data.size()) {
                std::string sni(data.begin() + pos, data.begin() + pos + nameLen);
                AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[SNI嗅探] 提取到域名: " + sni);
                return sni;
            }
            break;
        }

        pos += extLen;
    }

    return "";
}

// ===== 🔥 代理数据包记录实现 =====
void PacketCollector::RecordProxyPacket(uint64_t connectionId,
                                        const std::string& username,
                                        const std::string& gameID,
                                        const std::string& clientIP,
                                        const std::string& targetHost,
                                        const std::string& sniHost,
                                        int targetPort,
                                        bool sslMitmEnabled,
                                        bool isRequest,
                                        const std::vector<uint8_t>& data) {
    // 检查是否启用记录
    if (!proxyPacketRecordEnabled.load()) {
        return;
    }

    ProxyPacketRecordSummary summary;
    uint64_t recordedSequence = 0;
    size_t currentBufferSize = 0;
    {
        std::lock_guard<std::mutex> lock(proxyPacketMutex);

        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);
        std::tm tm_now;
        localtime_s(&tm_now, &time_t_now);
        std::ostringstream oss;
        oss << std::put_time(&tm_now, "%H:%M:%S");

        std::ostringstream hexOss;
        size_t previewLen = std::min<size_t>(64, data.size());
        for (size_t i = 0; i < previewLen; ++i) {
            hexOss << std::hex << std::setw(2) << std::setfill('0') << (int)data[i];
            if (i < previewLen - 1) hexOss << " ";
        }
        if (data.size() > 64) {
            hexOss << " ...";
        }

        ProxyPacketRecord& record = proxyPacketBuffer[proxyPacketWriteIndex];
        record.sequence = proxyPacketSequenceCounter.fetch_add(1, std::memory_order_relaxed) + 1;
        record.connectionId = connectionId;
        record.timestamp = oss.str();
        record.username = username;
        record.gameID = gameID;
        record.clientIP = clientIP;
        record.targetHost = targetHost;
        record.sniHost = sniHost;
        record.targetPort = targetPort;
        record.sslMitmEnabled = sslMitmEnabled;
        record.isRequest = isRequest;
        record.dataLength = static_cast<uint32_t>(data.size());
        record.dataPreview = hexOss.str();
        record.fullData = data;
        recordedSequence = record.sequence;

        summary.sequence = record.sequence;
        summary.connectionId = record.connectionId;
        summary.timestamp = record.timestamp;
        summary.username = record.username;
        summary.gameID = record.gameID;
        summary.clientIP = record.clientIP;
        summary.targetHost = record.targetHost;
        summary.sniHost = record.sniHost;
        summary.targetPort = record.targetPort;
        summary.sslMitmEnabled = record.sslMitmEnabled;
        summary.isRequest = record.isRequest;
        summary.dataLength = record.dataLength;
        summary.dataPreview = record.dataPreview;
        currentBufferSize = proxyPacketBufferSize;

        proxyPacketWriteIndex = (proxyPacketWriteIndex + 1) % proxyPacketBufferSize;
    }

    const bool shouldLogFirstRecordedPacket = !proxyPacketFirstRecordLogged.exchange(true);
    if (shouldLogFirstRecordedPacket) {
        AB_LOG_INFO_CAT(
            LOG_CAT_COLLECTOR,
            "[代理数据] 实例=" + (m_instanceId.empty() ? std::string("<default>") : m_instanceId) +
            " 已记录首个数据包: seq=" + std::to_string(recordedSequence) +
            ", dir=" + std::string(isRequest ? "request" : "response") +
            ", len=" + std::to_string(data.size()) +
            ", host=" + (targetHost.empty() ? std::string("-") : targetHost) +
            ", port=" + std::to_string(targetPort) +
            ", bufferSize=" + std::to_string(currentBufferSize));
    }

    if (!m_instanceId.empty() && UIBridge_ShouldPushProxyPacketData(m_instanceId, data)) {
        EnqueueProxyPacketRealtimePush(summary);
    }
}

void PacketCollector::EnqueueProxyPacketRealtimePush(const ProxyPacketRecordSummary& summary) {
    bool shouldNotify = false;
    {
        std::lock_guard<std::mutex> lock(proxyPacketRealtimeMutex);
        proxyPacketRealtimePending.push_back(summary);
        shouldNotify = proxyPacketRealtimePending.size() >= kProxyRealtimeBatchSize || proxyPacketRealtimePending.size() == 1;
    }
    if (shouldNotify) {
        proxyPacketRealtimeCV.notify_one();
    }
}

void PacketCollector::ProxyPacketRealtimePushThreadLoop() {
    while (proxyPacketRealtimeThreadRunning) {
        std::vector<ProxyPacketRecordSummary> pending;
        {
            std::unique_lock<std::mutex> lock(proxyPacketRealtimeMutex);
            proxyPacketRealtimeCV.wait(lock, [this]() {
                return !proxyPacketRealtimeThreadRunning.load() || !proxyPacketRealtimePending.empty();
            });

            if (!proxyPacketRealtimeThreadRunning && proxyPacketRealtimePending.empty()) {
                break;
            }

            if (proxyPacketRealtimePending.empty()) {
                continue;
            }

            if (proxyPacketRealtimePending.size() < kProxyRealtimeBatchSize) {
                proxyPacketRealtimeCV.wait_for(lock, kProxyRealtimeBatchWindow, [this]() {
                    return !proxyPacketRealtimeThreadRunning.load() || proxyPacketRealtimePending.size() >= kProxyRealtimeBatchSize;
                });
            }

            pending.swap(proxyPacketRealtimePending);
        }

        if (pending.empty()) {
            continue;
        }

        if (!UIBridge_ShouldPushProxyPacket(m_instanceId)) {
            continue;
        }

        json message;
        message["type"] = "proxydata_list";
        message["instanceId"] = m_instanceId;
        message["reset"] = false;
        message["enabled"] = true;
        message["bufferSize"] = proxyPacketBufferSize;
        message["packets"] = json::array();

        for (const auto& summary : pending) {
            json pkt;
            pkt["sequence"] = summary.sequence;
            pkt["connectionId"] = summary.connectionId;
            pkt["timestamp"] = summary.timestamp;
            pkt["username"] = summary.username;
            pkt["gameID"] = summary.gameID;
            pkt["clientIP"] = summary.clientIP;
            pkt["targetHost"] = summary.targetHost;
            pkt["sniHost"] = summary.sniHost;
            pkt["targetPort"] = summary.targetPort;
            pkt["sslMitmEnabled"] = summary.sslMitmEnabled;
            pkt["isRequest"] = summary.isRequest;
            pkt["dataLength"] = summary.dataLength;
            pkt["dataPreview"] = summary.dataPreview;
            message["packets"].push_back(std::move(pkt));
        }

        UIBridge_PushMessage(message.dump());
    }
}

std::vector<ProxyPacketRecord> PacketCollector::GetProxyPacketRecords() const {
    std::lock_guard<std::mutex> lock(proxyPacketMutex);

    std::vector<ProxyPacketRecord> result;
    result.reserve(proxyPacketBufferSize);

    // 从写入索引开始读取（最旧的记录），按时间顺序返回
    for (size_t i = 0; i < proxyPacketBufferSize; ++i) {
        size_t idx = (proxyPacketWriteIndex + i) % proxyPacketBufferSize;
        const ProxyPacketRecord& record = proxyPacketBuffer[idx];
        // 跳过未初始化的记录（timestamp为空）
        if (!record.timestamp.empty()) {
            result.push_back(record);
        }
    }

    return result;
}

std::vector<ProxyPacketRecordSummary> PacketCollector::GetProxyPacketRecordSummaries() const {
    std::lock_guard<std::mutex> lock(proxyPacketMutex);

    std::vector<ProxyPacketRecordSummary> result;
    result.reserve(proxyPacketBufferSize);

    for (size_t i = 0; i < proxyPacketBufferSize; ++i) {
        size_t idx = (proxyPacketWriteIndex + i) % proxyPacketBufferSize;
        const ProxyPacketRecord& record = proxyPacketBuffer[idx];
        if (!record.timestamp.empty()) {
            ProxyPacketRecordSummary summary;
            summary.sequence = record.sequence;
            summary.connectionId = record.connectionId;
            summary.timestamp = record.timestamp;
            summary.username = record.username;
            summary.gameID = record.gameID;
            summary.clientIP = record.clientIP;
            summary.targetHost = record.targetHost;
            summary.sniHost = record.sniHost;
            summary.targetPort = record.targetPort;
            summary.sslMitmEnabled = record.sslMitmEnabled;
            summary.isRequest = record.isRequest;
            summary.dataLength = record.dataLength;
            summary.dataPreview = record.dataPreview;
            result.push_back(std::move(summary));
        }
    }

    return result;
}

bool PacketCollector::GetProxyPacketRecordSummariesDelta(
    size_t knownCount,
    const std::string& firstKey,
    const std::string& lastKey,
    bool& outReset,
    size_t& outTotalCount,
    std::string& outCurrentFirstKey,
    std::string& outCurrentLastKey,
    std::vector<ProxyPacketRecordSummary>& outRecords) const {
    std::lock_guard<std::mutex> lock(proxyPacketMutex);

    outReset = true;
    outTotalCount = 0;
    outCurrentFirstKey.clear();
    outCurrentLastKey.clear();
    outRecords.clear();

    auto buildSummary = [](const ProxyPacketRecord& record) {
        ProxyPacketRecordSummary summary;
        summary.sequence = record.sequence;
        summary.connectionId = record.connectionId;
        summary.timestamp = record.timestamp;
        summary.username = record.username;
        summary.gameID = record.gameID;
        summary.clientIP = record.clientIP;
        summary.targetHost = record.targetHost;
        summary.sniHost = record.sniHost;
        summary.targetPort = record.targetPort;
        summary.sslMitmEnabled = record.sslMitmEnabled;
        summary.isRequest = record.isRequest;
        summary.dataLength = record.dataLength;
        summary.dataPreview = record.dataPreview;
        return summary;
    };

    auto buildKey = [](const ProxyPacketRecord& record) {
        return std::to_string(record.sequence);
    };

    const ProxyPacketRecord* firstRecord = nullptr;
    const ProxyPacketRecord* lastRecord = nullptr;
    const ProxyPacketRecord* knownLastRecord = nullptr;
    size_t orderedIndex = 0;

    for (size_t i = 0; i < proxyPacketBufferSize; ++i) {
        size_t idx = (proxyPacketWriteIndex + i) % proxyPacketBufferSize;
        const ProxyPacketRecord& record = proxyPacketBuffer[idx];
        if (record.timestamp.empty()) {
            continue;
        }

        if (!firstRecord) {
            firstRecord = &record;
        }
        lastRecord = &record;
        if (knownCount > 0 && orderedIndex == knownCount - 1) {
            knownLastRecord = &record;
        }
        ++orderedIndex;
    }

    outTotalCount = orderedIndex;
    if (!firstRecord || !lastRecord) {
        return true;
    }

    outCurrentFirstKey = buildKey(*firstRecord);
    outCurrentLastKey = buildKey(*lastRecord);

    const bool canAppend = knownCount > 0
        && knownCount <= outTotalCount
        && !firstKey.empty()
        && !lastKey.empty()
        && knownLastRecord != nullptr
        && outCurrentFirstKey == firstKey
        && buildKey(*knownLastRecord) == lastKey;

    outReset = !canAppend;
    const size_t startIndex = canAppend ? knownCount : 0;
    if (startIndex >= outTotalCount) {
        return true;
    }

    outRecords.reserve(outTotalCount - startIndex);
    orderedIndex = 0;
    for (size_t i = 0; i < proxyPacketBufferSize; ++i) {
        size_t idx = (proxyPacketWriteIndex + i) % proxyPacketBufferSize;
        const ProxyPacketRecord& record = proxyPacketBuffer[idx];
        if (record.timestamp.empty()) {
            continue;
        }

        if (orderedIndex >= startIndex) {
            outRecords.push_back(buildSummary(record));
        }
        ++orderedIndex;
    }

    return true;
}

bool PacketCollector::GetProxyPacketRecordByOrderedIndex(size_t orderedIndex, ProxyPacketRecord& outRecord) const {
    std::lock_guard<std::mutex> lock(proxyPacketMutex);

    size_t currentIndex = 0;
    for (size_t i = 0; i < proxyPacketBufferSize; ++i) {
        size_t idx = (proxyPacketWriteIndex + i) % proxyPacketBufferSize;
        const ProxyPacketRecord& record = proxyPacketBuffer[idx];
        if (record.timestamp.empty()) {
            continue;
        }

        if (currentIndex == orderedIndex) {
            outRecord = record;
            return true;
        }
        ++currentIndex;
    }

    return false;
}

bool PacketCollector::GetProxyPacketRecordBySequence(uint64_t sequence, ProxyPacketRecord& outRecord) const {
    std::lock_guard<std::mutex> lock(proxyPacketMutex);

    for (size_t i = 0; i < proxyPacketBufferSize; ++i) {
        size_t idx = (proxyPacketWriteIndex + i) % proxyPacketBufferSize;
        const ProxyPacketRecord& record = proxyPacketBuffer[idx];
        if (!record.timestamp.empty() && record.sequence == sequence) {
            outRecord = record;
            return true;
        }
    }

    return false;
}
void PacketCollector::ClearProxyPacketRecords() {
    std::lock_guard<std::mutex> lock(proxyPacketMutex);
    proxyPacketBuffer.clear();
    proxyPacketBuffer.resize(proxyPacketBufferSize);
    proxyPacketWriteIndex = 0;
}

void PacketCollector::SetProxyPacketRecordEnabled(bool enabled) {
    proxyPacketRecordEnabled.store(enabled);
    if (enabled) {
        proxyPacketFirstRecordLogged.store(false);
    }
    AB_LOG_INFO_CAT(
        LOG_CAT_COLLECTOR,
        "[代理数据] 实例=" + (m_instanceId.empty() ? std::string("<default>") : m_instanceId) +
        " 记录开关已" + std::string(enabled ? "开启" : "关闭"));
}

bool PacketCollector::IsProxyPacketRecordEnabled() const {
    return proxyPacketRecordEnabled.load();
}

void PacketCollector::SetProxyPacketBufferSize(size_t size) {
    std::lock_guard<std::mutex> lock(proxyPacketMutex);
    if (size < 10) size = 10;  // 最小10条
    if (size > 10000) size = 10000;  // 最大10000条
    proxyPacketBufferSize = size;
    proxyPacketBuffer.clear();
    proxyPacketBuffer.resize(proxyPacketBufferSize);
    proxyPacketWriteIndex = 0;
    proxyPacketFirstRecordLogged.store(false);
    AB_LOG_INFO_CAT(
        LOG_CAT_COLLECTOR,
        "[代理数据] 实例=" + (m_instanceId.empty() ? std::string("<default>") : m_instanceId) +
        " 缓冲区已重置, size=" + std::to_string(proxyPacketBufferSize));
}

size_t PacketCollector::GetProxyPacketBufferSize() const {
    return proxyPacketBufferSize;
}
