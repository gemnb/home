#include "GameSessionApiServer.h"
#include "Logger.h"
#include <sstream>
#include <json/json.h>
#include <regex>
#include <windows.h>

// ========================================
// 编码转换工具函数
// ========================================

std::string GameSessionApiServer::GBKToUTF8(const std::string& gbkStr) {
    if (gbkStr.empty()) return "";

    // GBK -> Unicode
    int unicodeLen = MultiByteToWideChar(CP_ACP, 0, gbkStr.c_str(), -1, NULL, 0);
    if (unicodeLen <= 0) return gbkStr;

    wchar_t* pUnicode = new wchar_t[unicodeLen + 1];
    memset(pUnicode, 0, (unicodeLen + 1) * sizeof(wchar_t));
    MultiByteToWideChar(CP_ACP, 0, gbkStr.c_str(), -1, pUnicode, unicodeLen);

    // Unicode -> UTF-8
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, pUnicode, -1, NULL, 0, NULL, NULL);
    if (utf8Len <= 0) {
        delete[] pUnicode;
        return gbkStr;
    }

    char* pUtf8 = new char[utf8Len + 1];
    memset(pUtf8, 0, utf8Len + 1);
    WideCharToMultiByte(CP_UTF8, 0, pUnicode, -1, pUtf8, utf8Len, NULL, NULL);

    std::string result(pUtf8);
    delete[] pUnicode;
    delete[] pUtf8;

    return result;
}

std::string GameSessionApiServer::ANSIToUTF8(const std::string& ansiStr) {
    return GBKToUTF8(ansiStr); // Windows ANSI通常就是GBK
}

std::string GameSessionApiServer::EnsureUTF8(const std::string& str) {
    if (str.empty()) return str;

    // 简单判断是否包含非ASCII字符
    bool hasNonAscii = false;
    for (unsigned char c : str) {
        if (c > 127) {
            hasNonAscii = true;
            break;
        }
    }

    // 如果包含非ASCII字符，尝试转换
    if (hasNonAscii) {
        return GBKToUTF8(str);
    }
    return str;
}

// ========================================
// 构造函数和析构函数
// ========================================

GameSessionApiServer::GameSessionApiServer(int serverPort, const std::string& domain)
    : port(serverPort), monitorDomain(domain), listenSocket(INVALID_SOCKET),
    isRunning(false), packetCollector(nullptr), database(nullptr) {
}

GameSessionApiServer::~GameSessionApiServer() {
    Stop();
}

// ========================================
// 启动和停止服务
// ========================================

bool GameSessionApiServer::Start(PacketCollector* collector, DatabaseManager* db) {
    if (isRunning) {
        AB_LOG_WARNING("[游戏会话API] 服务已在运行");
        return false;
    }

    if (!collector) {
        AB_LOG_ERROR_CAT(LOG_CAT_API, "[游戏会话API] PacketCollector 未初始化");
        return false;
    }

    packetCollector = collector;
    database = db;

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        AB_LOG_ERROR_CAT(LOG_CAT_API, "[游戏会话API] WSAStartup 失败");
        return false;
    }

    listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket == INVALID_SOCKET) {
        AB_LOG_ERROR_CAT(LOG_CAT_API, "[游戏会话API] 创建Socket失败");
        WSACleanup();
        return false;
    }

    // 设置地址重用
    int reuse = 1;
    setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));

    // 绑定端口
    sockaddr_in serverAddr = {};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(port);

    if (bind(listenSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        AB_LOG_ERROR_CAT(LOG_CAT_API, "[游戏会话API] 绑定端口失败: " + std::to_string(port));
        closesocket(listenSocket);
        WSACleanup();
        return false;
    }

    if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) {
        AB_LOG_ERROR_CAT(LOG_CAT_API, "[游戏会话API] 监听失败");
        closesocket(listenSocket);
        WSACleanup();
        return false;
    }

    isRunning = true;
    serverThread = std::thread(&GameSessionApiServer::ServerLoop, this);

    AB_LOG_INFO_CAT(LOG_CAT_API, "[游戏会话API] 服务启动成功 | 端口: " + std::to_string(port) +
        " | 监控域名: " + monitorDomain);
    return true;
}

void GameSessionApiServer::Stop() {
    if (!isRunning) return;

    isRunning = false;

    if (listenSocket != INVALID_SOCKET) {
        closesocket(listenSocket);
        listenSocket = INVALID_SOCKET;
    }

    if (serverThread.joinable()) {
        serverThread.join();
    }

    WSACleanup();
    AB_LOG_INFO_CAT(LOG_CAT_API, "[游戏会话API] 服务已停止");
}

// ========================================
// 服务器主循环
// ========================================

void GameSessionApiServer::ServerLoop() {
    AB_LOG_INFO_CAT(LOG_CAT_API, "[游戏会话API] 开始监听客户端连接...");

    while (isRunning) {
        sockaddr_in clientAddr = {};
        int clientAddrLen = sizeof(clientAddr);

        SOCKET clientSocket = accept(listenSocket, (sockaddr*)&clientAddr, &clientAddrLen);

        if (clientSocket == INVALID_SOCKET) {
            if (isRunning) {
                Sleep(100);
            }
            continue;
        }

        // 为每个连接创建独立线程处理
        std::thread([this, clientSocket]() {
            HandleRequest(clientSocket);
            }).detach();
    }

    AB_LOG_INFO_CAT(LOG_CAT_API, "[游戏会话API] 监听循环已退出");
}

// ========================================
// HTTP请求处理
// ========================================

void GameSessionApiServer::HandleRequest(SOCKET clientSocket) {
    char buffer[4096] = {};
    int bytesReceived = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);

    if (bytesReceived <= 0) {
        closesocket(clientSocket);
        return;
    }

    std::string request(buffer, bytesReceived);

    // 解析HTTP请求
    std::istringstream iss(request);
    std::string method, path, httpVersion;
    iss >> method >> path >> httpVersion;

    // 提取Host头
    std::string host;
    std::regex hostRegex("Host: ([^\r\n]+)");
    std::smatch match;
    if (std::regex_search(request, match, hostRegex)) {
        host = match[1].str();
    }

    AB_LOG_INFO_CAT(LOG_CAT_API, "[游戏会话API] 请求: " + method + " " + path + " | Host: " + host);

    std::string response;
    std::string contentType = "application/json; charset=utf-8";
    std::string body;

    // ===== 处理OPTIONS预检请求（CORS）=====
    if (method == "OPTIONS") {
        std::ostringstream oss;
        oss << "HTTP/1.1 204 No Content\r\n";
        oss << "Access-Control-Allow-Origin: *\r\n";
        oss << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
        oss << "Access-Control-Allow-Headers: Content-Type\r\n";
        oss << "Access-Control-Max-Age: 86400\r\n";
        oss << "Connection: close\r\n";
        oss << "\r\n";

        response = oss.str();
        send(clientSocket, response.c_str(), static_cast<int>(response.length()), 0);
        closesocket(clientSocket);
        return;
    }

    // ===== 允许所有来源访问 =====
    AB_LOG_INFO_CAT(LOG_CAT_API, "[游戏会话API] 允许访问 | Host: " + host);

    // ===== 处理 /latest 接口 =====
    if (path.find("/latest") != std::string::npos) {
        std::string targetUsername;

        // 安全获取监控域名
        std::string currentMonitorDomain;
        {
            std::lock_guard<std::mutex> lock(domainMutex);
            currentMonitorDomain = monitorDomain;
        }

        // 核心逻辑：获取最后访问监控域名的用户
        if (!currentMonitorDomain.empty()) {
            targetUsername = packetCollector->GetLastUserForDomain(currentMonitorDomain);

            if (!targetUsername.empty()) {
                AB_LOG_INFO_CAT(LOG_CAT_API, "[游戏会话API] 返回最后访问 [" + currentMonitorDomain + "] 的用户: " + targetUsername);
            }
            else {
                AB_LOG_WARNING("[游戏会话API] 暂无用户访问过 [" + currentMonitorDomain + "]");
            }
        }
        else {
            AB_LOG_WARNING("[游戏会话API] 监控域名未配置");
        }

        // 返回结果
        if (targetUsername.empty()) {
            std::string errorMsg = EnsureUTF8("暂无用户访问过域名: " + currentMonitorDomain);
            body = BuildJsonResponse(false, "", errorMsg);
        }
        else {
            body = HandleLatestRequest(targetUsername);
        }
    }
    // ===== 处理未知路径 =====
    else {
        std::string errorMsg = EnsureUTF8("未知的API路径: " + path);
        body = BuildJsonResponse(false, "", errorMsg);
    }

    // ===== 构建HTTP响应（UTF-8编码）=====
    std::ostringstream oss;
    oss << "HTTP/1.1 200 OK\r\n";
    oss << "Content-Type: " << contentType << "\r\n";
    oss << "Content-Length: " << static_cast<int>(body.length()) << "\r\n";
    oss << "Access-Control-Allow-Origin: *\r\n";
    oss << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
    oss << "Access-Control-Allow-Headers: Content-Type\r\n";
    oss << "Cache-Control: no-cache, no-store, must-revalidate\r\n";
    oss << "Pragma: no-cache\r\n";
    oss << "Expires: 0\r\n";
    oss << "Connection: close\r\n";
    oss << "\r\n";
    oss << body;

    response = oss.str();
    send(clientSocket, response.c_str(), static_cast<int>(response.length()), 0);
    closesocket(clientSocket);
}

// ========================================
// 处理 /latest 请求（添加登录IP）
// ========================================

std::string GameSessionApiServer::HandleLatestRequest(const std::string& username) {
    try {
        Json::Value data;

        if (username.empty()) {
            std::string currentDomain;
            {
                std::lock_guard<std::mutex> lock(domainMutex);
                currentDomain = monitorDomain;
            }
            std::string errorMsg = EnsureUTF8("未找到访问域名的用户: " + currentDomain);
            return BuildJsonResponse(false, "", errorMsg);
        }

        // 获取账号信息
        auto account = packetCollector->GetAccount(username);
        if (account.username.empty()) {
            std::string errorMsg = EnsureUTF8("用户不存在: " + username);
            return BuildJsonResponse(false, "", errorMsg);
        }

        // ===== 基本信息（UTF-8编码）=====
        data["username"] = username;

        // 到期时间
        if (account.expireTime.empty()) {
            data["expireTime"] = "Never";
        }
        else {
            data["expireTime"] = account.expireTime;
        }

        // 获取监控域名
        std::string currentDomain;
        {
            std::lock_guard<std::mutex> lock(domainMutex);
            currentDomain = monitorDomain;
        }
        data["matchedDomain"] = currentDomain;

        data["isOnline"] = account.currentConnections > 0;
        data["activeConnectionCount"] = account.currentConnections;

        // ===== 🔥 新增：返回客户登录IP =====
        data["loginIP"] = account.lastLoginIP.empty() ? "" : account.lastLoginIP;

        // ===== 计算本次在线时间和累计时间 =====
        int64_t currentOnlineSeconds = 0;
        std::string currentOnlineTimeStr = "Offline";
        uint64_t totalOnlineSeconds = account.totalOnlineSeconds;

        if (account.currentConnections > 0) {
            auto now = std::chrono::system_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::seconds>(
                now - account.firstConnectTime);
            currentOnlineSeconds = duration.count();

            // 累计时间要加上当前在线时长
            totalOnlineSeconds += currentOnlineSeconds;

            // 格式化时间
            int hours = static_cast<int>(currentOnlineSeconds / 3600);
            int minutes = static_cast<int>((currentOnlineSeconds % 3600) / 60);
            int seconds = static_cast<int>(currentOnlineSeconds % 60);

            std::ostringstream timeOss;
            if (hours > 0) {
                timeOss << hours << "h " << minutes << "m " << seconds << "s";
            }
            else if (minutes > 0) {
                timeOss << minutes << "m " << seconds << "s";
            }
            else {
                timeOss << seconds << "s";
            }
            currentOnlineTimeStr = timeOss.str();
        }

        data["currentOnlineSeconds"] = static_cast<Json::Int64>(currentOnlineSeconds);
        data["currentOnlineTime"] = currentOnlineTimeStr;
        data["totalOnlineSeconds"] = static_cast<Json::Int64>(totalOnlineSeconds);

        // ===== 游戏会话信息 =====
        std::string gameID = account.currentGameID;
        data["hasSession"] = !gameID.empty();

        if (!gameID.empty()) {
            data["gameType"] = gameID;
            data["sessionId_raw"] = gameID;
            // ===== 🔥 会话ID位数（字符数）=====
            data["sessionIdLength"] = static_cast<int>(gameID.length());
            data["gameIDLength"] = static_cast<int>(gameID.length());  // 保留兼容性

            // 时间信息
            if (account.currentConnections > 0) {
                auto time = std::chrono::system_clock::to_time_t(account.firstConnectTime);
                struct tm timeinfo;
                localtime_s(&timeinfo, &time);
                char buffer[64];
                strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
                data["lastConnectTime"] = std::string(buffer);
                data["lastDisconnectTime"] = "";
            }
            else {
                data["lastConnectTime"] = "";
                if (account.lastDisconnectTime.time_since_epoch().count() > 0) {
                    auto time = std::chrono::system_clock::to_time_t(account.lastDisconnectTime);
                    struct tm timeinfo;
                    localtime_s(&timeinfo, &time);
                    char buffer[64];
                    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
                    data["lastDisconnectTime"] = std::string(buffer);
                }
                else {
                    data["lastDisconnectTime"] = "";
                }
            }
        }
        else {
            data["gameType"] = "";
            data["sessionId_raw"] = "";
            data["sessionIdLength"] = 0;  // 🔥 新增
            data["gameIDLength"] = 0;
            data["lastConnectTime"] = "";
            data["lastDisconnectTime"] = "";
        }

        // ===== 使用UTF-8编码构建JSON字符串 =====
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        builder["commentStyle"] = "None";
        builder["emitUTF8"] = true;

        std::string jsonStr = Json::writeString(builder, data);

        return jsonStr;

    }
    catch (const std::exception& ex) {
        AB_LOG_ERROR_CAT(LOG_CAT_API, "[游戏会话API] 处理失败: " + std::string(ex.what()));
        std::string errorMsg = EnsureUTF8("服务器内部错误");
        return BuildJsonResponse(false, "", errorMsg);
    }
}




// ========================================
// 构建JSON响应
// ========================================

std::string GameSessionApiServer::BuildJsonResponse(bool success, const std::string& data,
    const std::string& error) {
    if (success) {
        return data; // 直接返回数据JSON
    }
    else {
        Json::Value root;
        root["success"] = false;
        // 确保错误消息是UTF-8编码
        root["error"] = EnsureUTF8(error);

        // 使用UTF-8编码构建JSON
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        builder["commentStyle"] = "None";
        builder["emitUTF8"] = true;

        return Json::writeString(builder, root);
    }
}

// ========================================
// 获取当前时间戳
// ========================================

std::string GameSessionApiServer::GetCurrentTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    struct tm timeinfo;
    localtime_s(&timeinfo, &time);
    char buffer[64];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
    return std::string(buffer);
}
