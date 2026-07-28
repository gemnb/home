#include "UserFilterWebServer.h"
#include "UserFilterManager.h"
#include "DatabaseManager.h"
#include "Logger.h"
#include <sstream>
#include <algorithm>

extern DatabaseManager* g_database;

namespace {
constexpr int kUserFilterClientTimeoutMs = 4000;
constexpr int kUserFilterMaxRequestBytes = 256 * 1024;

struct WebFilterNameView {
    std::string name;
    std::string webDisplayName;
};

static WebFilterNameView SplitWebFilterName(const std::string& filterName) {
    const std::string marker = "\x1F";
    size_t pos = filterName.find(marker);
    if (pos == std::string::npos) {
        return {filterName, ""};
    }
    return {filterName.substr(0, pos), filterName.substr(pos + marker.size())};
}

static std::string ResolveWebFilterName(const std::string& filterName) {
    WebFilterNameView filter = SplitWebFilterName(filterName);
    return filter.webDisplayName.empty() ? filter.name : filter.webDisplayName;
}

static std::string JsonEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

static std::string ExtractJsonString(const std::string& body, const std::string& key) {
    const std::string quotedKey = "\"" + key + "\"";
    size_t keyPos = body.find(quotedKey);
    if (keyPos == std::string::npos) return "";
    size_t colonPos = body.find(":", keyPos);
    size_t quoteStart = body.find("\"", colonPos);
    size_t quoteEnd = body.find("\"", quoteStart + 1);
    if (quoteStart == std::string::npos || quoteEnd == std::string::npos) return "";
    return body.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
}

bool IsRecoverableAcceptError(int error) {
    switch (error) {
        case WSAECONNRESET:
        case WSAEINTR:
        case WSAETIMEDOUT:
        case WSAEWOULDBLOCK:
        case WSAENOBUFS:
            return true;
        default:
            return false;
    }
}

size_t ParseHttpContentLengthValue(const std::string& request) {
    const std::string headerKey = "Content-Length:";
    size_t headerPos = request.find(headerKey);
    if (headerPos == std::string::npos) {
        return 0;
    }

    headerPos += headerKey.length();
    while (headerPos < request.size() && (request[headerPos] == ' ' || request[headerPos] == '\t')) {
        ++headerPos;
    }

    size_t lineEnd = request.find("\r\n", headerPos);
    std::string value = request.substr(headerPos, lineEnd == std::string::npos ? std::string::npos : lineEnd - headerPos);
    try {
        return static_cast<size_t>(std::stoul(value));
    } catch (...) {
        return 0;
    }
}

std::string ReceiveHttpRequestFully(SOCKET clientSocket) {
    int timeoutMs = kUserFilterClientTimeoutMs;
    setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
    setsockopt(clientSocket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

    std::string request;
    request.reserve(4096);
    char buffer[4096] = {};
    size_t expectedTotalSize = 0;

    while (request.size() < kUserFilterMaxRequestBytes) {
        int bytesReceived = recv(clientSocket, buffer, sizeof(buffer), 0);
        if (bytesReceived <= 0) {
            break;
        }

        request.append(buffer, bytesReceived);

        size_t headerEnd = request.find("\r\n\r\n");
        if (headerEnd == std::string::npos) {
            continue;
        }

        if (expectedTotalSize == 0) {
            expectedTotalSize = headerEnd + 4 + ParseHttpContentLengthValue(request);
        }
        if (request.size() >= expectedTotalSize) {
            break;
        }
    }

    return request;
}
} // namespace

UserFilterWebServer::UserFilterWebServer(const std::string& instanceId, int port)
    : instanceId(instanceId)
    , port(port)
    , running(false)
    , listenSocket(INVALID_SOCKET)
    , workerThreadCount(8) {
}

UserFilterWebServer::~UserFilterWebServer() {
    Stop();
}

void UserFilterWebServer::SetSocksValidator(std::function<bool(const std::string&, const std::string&)> validator) {
    socksValidator = validator;
}

void UserFilterWebServer::SetSocksValidatorWithReason(std::function<bool(const std::string&, const std::string&, std::string&)> validator) {
    socksValidatorWithReason = validator;
}

void UserFilterWebServer::SetFilterListGetter(std::function<std::vector<std::pair<int, std::string>>()> getter) {
    filterListGetter = getter;
}

void UserFilterWebServer::SetAuthorizedFilterListGetter(
    std::function<std::vector<AuthorizedWebFilter>(const std::string& username)> getter) {
    authorizedFilterListGetter = getter;
}

void UserFilterWebServer::SetAccountInfoGetter(std::function<std::string(const std::string&)> getter) {
    accountInfoGetter = getter;
}

void UserFilterWebServer::SetUserGameInfoGetter(std::function<UserGameInfo(const std::string& username)> getter) {
    userGameInfoGetter = getter;
}

void UserFilterWebServer::SetUserPoolClearer(std::function<size_t(const std::string& username)> clearer) {
    userPoolClearer = clearer;
}

bool UserFilterWebServer::Start() {
    if (running.load()) {
        AB_LOG_WARNING("[用户滤镜Web] 服务已在运行");
        return false;
    }

    listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket == INVALID_SOCKET) {
        AB_LOG_ERROR("[用户滤镜Web] 创建socket失败: " + std::to_string(WSAGetLastError()));
        return false;
    }

    int reuse = 1;
    setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));

    sockaddr_in serverAddr = {};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(port);

    if (bind(listenSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        int error = WSAGetLastError();
        AB_LOG_ERROR("[用户滤镜Web] 绑定端口失败: " + std::to_string(port) + ", 错误码: " + std::to_string(error));
        closesocket(listenSocket);
        listenSocket = INVALID_SOCKET;
        return false;
    }

    if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) {
        AB_LOG_ERROR("[用户滤镜Web] 监听失败: " + std::to_string(WSAGetLastError()));
        closesocket(listenSocket);
        listenSocket = INVALID_SOCKET;
        return false;
    }

    running = true;
    {
        std::lock_guard<std::mutex> lock(workerMutex);
        while (!pendingClientSockets.empty()) pendingClientSockets.pop();
        workerThreads.clear();
        activeClientSockets.clear();
    }

    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    const size_t cpuCount = (std::max)(static_cast<size_t>(2), static_cast<size_t>(sysInfo.dwNumberOfProcessors));
    workerThreadCount = (std::min)(static_cast<size_t>(16), cpuCount * 2);
    for (size_t i = 0; i < workerThreadCount; ++i) {
        workerThreads.emplace_back(&UserFilterWebServer::WorkerLoop, this);
    }

    serverThread = std::thread(&UserFilterWebServer::ServerLoop, this);

    AB_LOG_INFO("[用户滤镜Web] HTTP服务启动成功: 0.0.0.0:" + std::to_string(port) + " (实例: " + instanceId + ")");
    return true;
}

void UserFilterWebServer::Stop() {
    if (!running.load()) {
        return;
    }

    AB_LOG_INFO("[用户滤镜Web] 正在停止HTTP服务...");
    running = false;

    if (listenSocket != INVALID_SOCKET) {
        shutdown(listenSocket, SD_BOTH);
        closesocket(listenSocket);
        listenSocket = INVALID_SOCKET;
    }

    std::vector<SOCKET> clientSockets;
    {
        std::lock_guard<std::mutex> lock(workerMutex);
        clientSockets = activeClientSockets;
    }
    for (SOCKET clientSocket : clientSockets) {
        if (clientSocket != INVALID_SOCKET) {
            shutdown(clientSocket, SD_BOTH);
        }
    }

    workerCondition.notify_all();

    if (serverThread.joinable()) {
        serverThread.join();
    }

    std::vector<std::thread> workersToJoin;
    {
        std::lock_guard<std::mutex> lock(workerMutex);
        workersToJoin.swap(workerThreads);
        while (!pendingClientSockets.empty()) {
            pendingClientSockets.pop();
        }
        activeClientSockets.clear();
    }
    for (auto& worker : workersToJoin) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    AB_LOG_INFO("[用户滤镜Web] HTTP服务已停止");
}

void UserFilterWebServer::ServerLoop() {
    AB_LOG_INFO("[用户滤镜Web] 服务线程启动");

    while (running.load()) {
        sockaddr_in clientAddr = {};
        int clientAddrLen = sizeof(clientAddr);

        SOCKET clientSocket = accept(listenSocket, (sockaddr*)&clientAddr, &clientAddrLen);
        if (clientSocket == INVALID_SOCKET) {
            const int error = WSAGetLastError();
            if (!running.load()) {
                break;
            }
            if (IsRecoverableAcceptError(error)) {
                AB_LOG_WARNING("[用户滤镜Web] Accept暂时失败，继续监听: " + std::to_string(error));
                Sleep(50);
                continue;
            }

            AB_LOG_ERROR("[用户滤镜Web] Accept致命失败，服务线程退出: " + std::to_string(error));
            running = false;
            break;
        }

        {
            std::lock_guard<std::mutex> lock(workerMutex);
            activeClientSockets.push_back(clientSocket);
            pendingClientSockets.push(clientSocket);
        }
        workerCondition.notify_one();
    }

    running = false;
    workerCondition.notify_all();
    AB_LOG_INFO("[用户滤镜Web] 服务线程退出");
}

void UserFilterWebServer::WorkerLoop() {
    while (true) {
        SOCKET clientSocket = INVALID_SOCKET;
        {
            std::unique_lock<std::mutex> lock(workerMutex);
            workerCondition.wait(lock, [this]() {
                return !running.load() || !pendingClientSockets.empty();
            });

            if (!running.load() && pendingClientSockets.empty()) {
                break;
            }

            if (!pendingClientSockets.empty()) {
                clientSocket = pendingClientSockets.front();
                pendingClientSockets.pop();
            }
        }

        if (clientSocket != INVALID_SOCKET) {
            HandleClientSession(clientSocket);
        }
    }
}

void UserFilterWebServer::HandleClientSession(SOCKET clientSocket) {
    HandleRequest(clientSocket);
    shutdown(clientSocket, SD_BOTH);
    closesocket(clientSocket);
    RemoveActiveClientSocket(clientSocket);
}



void UserFilterWebServer::RemoveActiveClientSocket(SOCKET clientSocket) {
    std::lock_guard<std::mutex> lock(workerMutex);
    activeClientSockets.erase(
        std::remove(activeClientSockets.begin(), activeClientSockets.end(), clientSocket),
        activeClientSockets.end());
}

void UserFilterWebServer::HandleRequest(SOCKET clientSocket) {
    const std::string request = ReceiveHttpRequestFully(clientSocket);
    if (request.empty()) {
        return;
    }

    std::string method = ParseRequestMethod(request);
    std::string path = ParseRequestPath(request);
    std::string response;

    if (method == "GET" && (path == "/" || path.rfind("/?", 0) == 0)) {
        response = BuildHttpResponse(200, "text/html; charset=utf-8", GetHtmlPage());
    }
    else if (method == "GET" && path == "/favicon.ico") {
        response = BuildHttpResponse(204, "text/plain; charset=utf-8", "");
    }
    else if (method == "POST" && path == "/api/login") {
        std::string body = ParseRequestBody(request);
        response = BuildHttpResponse(200, "application/json; charset=utf-8", HandleLogin(body));
    }
    else if (method == "GET" && path.find("/api/filters") == 0) {
        std::string username = ParseQueryParam(path, "username");
        response = BuildHttpResponse(200, "application/json; charset=utf-8", HandleGetFilters(username));
    }
    else if (method == "POST" && path == "/api/update") {
        std::string body = ParseRequestBody(request);
        response = BuildHttpResponse(200, "application/json; charset=utf-8", HandleUpdateFilters(body));
    }
    else if (method == "POST" && path == "/api/reset-counts") {
        std::string body = ParseRequestBody(request);
        response = BuildHttpResponse(200, "application/json; charset=utf-8", HandleResetCounts(body));
    }
    else if (method == "POST" && path == "/api/clear-pool") {
        std::string body = ParseRequestBody(request);
        response = BuildHttpResponse(200, "application/json; charset=utf-8", HandleClearPool(body));
    }
    else {
        response = BuildHttpResponse(404, "text/plain; charset=utf-8", "Not Found");
    }

    send(clientSocket, response.c_str(), static_cast<int>(response.length()), 0);
}

std::string UserFilterWebServer::BuildHttpResponse(int statusCode, const std::string& contentType, const std::string& body) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << statusCode << " " << GetStatusText(statusCode) << "\r\n";
    oss << "Content-Type: " << contentType << "\r\n";
    oss << "Content-Length: " << body.length() << "\r\n";
    oss << "Connection: close\r\n";
    oss << "Access-Control-Allow-Origin: *\r\n";
    oss << "Cache-Control: no-store, no-cache, must-revalidate\r\n";
    oss << "Pragma: no-cache\r\n";
    oss << "Expires: 0\r\n";
    oss << "\r\n";
    oss << body;
    return oss.str();
}
std::string UserFilterWebServer::GetStatusText(int statusCode) {
    switch (statusCode) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 404: return "Not Found";
        case 500: return "Internal Server Error";
        default: return "Unknown";
    }
}

std::string UserFilterWebServer::HandleLogin(const std::string& body) {
    // 简单的JSON解析（手动解析，避免依赖第三方库）
    std::string username, password;

    size_t usernamePos = body.find("\"username\"");
    if (usernamePos != std::string::npos) {
        size_t colonPos = body.find(":", usernamePos);
        size_t quoteStart = body.find("\"", colonPos);
        size_t quoteEnd = body.find("\"", quoteStart + 1);
        if (quoteStart != std::string::npos && quoteEnd != std::string::npos) {
            username = body.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
        }
    }

    size_t passwordPos = body.find("\"password\"");
    if (passwordPos != std::string::npos) {
        size_t colonPos = body.find(":", passwordPos);
        size_t quoteStart = body.find("\"", colonPos);
        size_t quoteEnd = body.find("\"", quoteStart + 1);
        if (quoteStart != std::string::npos && quoteEnd != std::string::npos) {
            password = body.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
        }
    }

    // 验证SOCKS账号
    bool success = false;
    std::string failReason;

    if (!username.empty() && !password.empty()) {
        // 优先使用带失败原因的验证器
        if (socksValidatorWithReason) {
            success = socksValidatorWithReason(username, password, failReason);
        } else if (socksValidator) {
            success = socksValidator(username, password);
            if (!success) {
                failReason = "用户名或密码错误";
            }
        }
    } else {
        failReason = "用户名或密码不能为空";
    }

    if (success) {
        AB_LOG_INFO("[用户滤镜Web] 用户登录成功: " + username);
        return "{\"success\":true,\"message\":\"登录成功\"}";
    } else {
        AB_LOG_WARNING("[用户滤镜Web] 用户登录失败: " + username + ", 原因: " + failReason);
        return "{\"success\":false,\"message\":\"" + failReason + "\"}";
    }
}

std::string UserFilterWebServer::HandleGetFilters(const std::string& username) {
    if (username.empty()) {
        return "{\"success\":false,\"message\":\"用户名不能为空\"}";
    }

    AuthorizedFilterState authState;
    bool useAuthorization = false;
    if (g_userFilterManager) {
        authState = g_userFilterManager->GetAuthorizedFilters(instanceId, username);
        useAuthorization = !authState.authorizedFilterIds.empty() || !authState.groupIds.empty();
    }

    std::vector<AuthorizedWebFilter> allFilters;
    if (authorizedFilterListGetter) {
        allFilters = authorizedFilterListGetter(username);
    } else if (filterListGetter) {
        for (const auto& pair : filterListGetter()) {
            AuthorizedWebFilter item;
            item.id = pair.first;
            item.name = pair.second;
            item.applyToCollector = true;
            item.applyToHeartbeat = true;
            allFilters.push_back(item);
        }
    }

    if (useAuthorization) {
        allFilters.erase(
            std::remove_if(allFilters.begin(), allFilters.end(),
                [&authState](const AuthorizedWebFilter& filter) {
                    return authState.authorizedFilterIds.count(filter.id) == 0;
                }),
            allFilters.end());
    }

    std::set<int> enabledFilters = authState.effectiveFilterIds;
    if (!useAuthorization && g_userFilterManager) {
        enabledFilters = g_userFilterManager->GetEffectiveUserFilters(instanceId, username);
    }

    // 🔥 是否显示执行次数（由实例配置控制）
    bool includeCounts = false;
    std::map<int, int64_t> executionCounts;
    if (g_database) {
        std::string showCountsStr = g_database->GetConfigValue(
            "instance_" + instanceId + "_userFilterShowCounts", "0");
        includeCounts = (showCountsStr == "1" || showCountsStr == "true");
        if (includeCounts) {
            executionCounts = g_database->GetUserFilterExecutionCounts(instanceId, username);
        }
    }

    // 🔥 获取用户到期时间
    std::string expireTime = "未知";
    if (accountInfoGetter) {
        expireTime = accountInfoGetter(username);
    }

    UserGameInfo gameInfo;
    if (userGameInfoGetter) {
        gameInfo = userGameInfoGetter(username);
    }

    auto appendFilterArray = [&](std::ostringstream& out, const std::vector<AuthorizedWebFilter>& filters, bool collectorOnly, bool heartbeatOnly) {
        bool first = true;
        for (const auto& filter : filters) {
            if (collectorOnly && !filter.applyToCollector) continue;
            if (heartbeatOnly && !filter.applyToHeartbeat) continue;

            if (!first) out << ",";
            first = false;

            bool enabled = (enabledFilters.find(filter.id) != enabledFilters.end());
            bool defaultEnabled = (authState.defaultEnabledFilterIds.find(filter.id) != authState.defaultEnabledFilterIds.end());
            out << "{\"id\":" << filter.id
                << ",\"name\":\"" << JsonEscape(ResolveWebFilterName(filter.name))
                << "\",\"enabled\":" << (enabled ? "true" : "false")
                << ",\"defaultEnabled\":" << (defaultEnabled ? "true" : "false");
            if (includeCounts) {
                int64_t execCount = 0;
                auto it = executionCounts.find(filter.id);
                if (it != executionCounts.end()) {
                    execCount = it->second;
                }
                out << ",\"executionCount\":" << execCount;
            }
            out << "}";
        }
    };

    // 构建JSON响应
    std::ostringstream oss;
    oss << "{\"success\":true,\"username\":\"" << JsonEscape(username)
        << "\",\"expireTime\":\"" << JsonEscape(expireTime)
        << R"(,"gameId":")" << JsonEscape(gameInfo.gameId)
        << R"(","poolCount":)" << static_cast<unsigned long long>(gameInfo.poolCount)
        << R"(,"hasUserConfig":)" << (authState.hasUserConfig ? "true" : "false")
        << R"(,"collectorFilters":[)";
    appendFilterArray(oss, allFilters, true, false);
    oss << R"(],"heartbeatFilters":[)";
    appendFilterArray(oss, allFilters, false, true);
    oss << R"(],"filters":[)";
    appendFilterArray(oss, allFilters, false, false);
    oss << "]}";

    return oss.str();
}

std::string UserFilterWebServer::HandleUpdateFilters(const std::string& body) {
    // 解析JSON: {"username":"xxx","filterIds":[1,3,5]}
    std::string username;
    std::vector<int> filterIds;

    size_t usernamePos = body.find("\"username\"");
    if (usernamePos != std::string::npos) {
        size_t colonPos = body.find(":", usernamePos);
        size_t quoteStart = body.find("\"", colonPos);
        size_t quoteEnd = body.find("\"", quoteStart + 1);
        if (quoteStart != std::string::npos && quoteEnd != std::string::npos) {
            username = body.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
        }
    }

    size_t filterIdsPos = body.find("\"filterIds\"");
    if (filterIdsPos != std::string::npos) {
        size_t arrayStart = body.find("[", filterIdsPos);
        size_t arrayEnd = body.find("]", arrayStart);
        if (arrayStart != std::string::npos && arrayEnd != std::string::npos) {
            std::string arrayStr = body.substr(arrayStart + 1, arrayEnd - arrayStart - 1);
            std::stringstream ss(arrayStr);
            std::string item;
            while (std::getline(ss, item, ',')) {
                try {
                    filterIds.push_back(std::stoi(item));
                } catch (...) {}
            }
        }
    }

    if (username.empty()) {
        return "{\"success\":false,\"message\":\"用户名不能为空\"}";
    }

    // 更新用户滤镜配置
    if (g_userFilterManager) {
        auto authState = g_userFilterManager->GetAuthorizedFilters(instanceId, username);
        const bool useAuthorization = !authState.authorizedFilterIds.empty() || !authState.groupIds.empty();
        std::vector<int> sanitized;
        sanitized.reserve(filterIds.size());
        for (int filterId : filterIds) {
            if (!useAuthorization || authState.authorizedFilterIds.count(filterId) > 0) {
                sanitized.push_back(filterId);
            }
        }

        if (g_userFilterManager->UpdateUserFilters(instanceId, username, sanitized)) {
            AB_LOG_INFO("[用户滤镜Web] 更新用户配置成功: " + username + ", 滤镜数: " + std::to_string(sanitized.size()));
            return "{\"success\":true,\"message\":\"保存成功\"}";
        } else {
            AB_LOG_ERROR("[用户滤镜Web] 更新用户配置失败: " + username);
            return "{\"success\":false,\"message\":\"保存失败\"}";
        }
    }

    return "{\"success\":false,\"message\":\"系统错误\"}";
}

std::string UserFilterWebServer::HandleResetCounts(const std::string& body) {
    // 解析JSON: {"username":"xxx"}
    std::string username;

    size_t usernamePos = body.find("\"username\"");
    if (usernamePos != std::string::npos) {
        size_t colonPos = body.find(":", usernamePos);
        size_t quoteStart = body.find("\"", colonPos);
        size_t quoteEnd = body.find("\"", quoteStart + 1);
        if (quoteStart != std::string::npos && quoteEnd != std::string::npos) {
            username = body.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
        }
    }

    if (username.empty()) {
        return "{\"success\":false,\"message\":\"用户名不能为空\"}";
    }

    // 重置用户的所有滤镜执行次数
    if (g_database) {
        if (g_database->ResetUserFilterExecutionCounts(instanceId, username)) {
            AB_LOG_INFO("[用户滤镜Web] 重置执行次数成功: " + username);
            return "{\"success\":true,\"message\":\"重置成功\"}";
        } else {
            AB_LOG_ERROR("[用户滤镜Web] 重置执行次数失败: " + username);
            return "{\"success\":false,\"message\":\"重置失败\"}";
        }
    }

    return "{\"success\":false,\"message\":\"系统错误\"}";
}

std::string UserFilterWebServer::HandleClearPool(const std::string& body) {
    std::string username = ExtractJsonString(body, "username");
    if (username.empty()) {
        return "{\"success\":false,\"message\":\"用户名不能为空\"}";
    }
    if (!userPoolClearer) {
        return "{\"success\":false,\"message\":\"系统未配置数据池清理器\"}";
    }

    size_t cleared = userPoolClearer(username);
    AB_LOG_INFO("[用户滤镜Web] 清空用户数据池: " + username + ", 数量: " + std::to_string(cleared));
    return "{\"success\":true,\"message\":\"清空成功\",\"cleared\":" + std::to_string(cleared) + "}";
}

std::string UserFilterWebServer::ParseRequestMethod(const std::string& request) {
    size_t spacePos = request.find(' ');
    if (spacePos != std::string::npos) {
        return request.substr(0, spacePos);
    }
    return "";
}

std::string UserFilterWebServer::ParseRequestPath(const std::string& request) {
    size_t firstSpace = request.find(' ');
    size_t secondSpace = request.find(' ', firstSpace + 1);
    if (firstSpace != std::string::npos && secondSpace != std::string::npos) {
        return request.substr(firstSpace + 1, secondSpace - firstSpace - 1);
    }
    return "";
}

std::string UserFilterWebServer::ParseRequestBody(const std::string& request) {
    size_t bodyPos = request.find("\r\n\r\n");
    if (bodyPos != std::string::npos) {
        return request.substr(bodyPos + 4);
    }
    return "";
}

std::string UserFilterWebServer::ParseQueryParam(const std::string& path, const std::string& param) {
    size_t queryStart = path.find('?');
    if (queryStart == std::string::npos) {
        return "";
    }

    std::string query = path.substr(queryStart + 1);
    std::string searchKey = param + "=";
    size_t paramPos = query.find(searchKey);
    if (paramPos == std::string::npos) {
        return "";
    }

    size_t valueStart = paramPos + searchKey.length();
    size_t valueEnd = query.find('&', valueStart);
    if (valueEnd == std::string::npos) {
        valueEnd = query.length();
    }

    return UrlDecode(query.substr(valueStart, valueEnd - valueStart));
}

std::string UserFilterWebServer::UrlDecode(const std::string& str) {
    std::string result;
    for (size_t i = 0; i < str.length(); i++) {
        if (str[i] == '%' && i + 2 < str.length()) {
            int value = 0;
            std::istringstream iss(str.substr(i + 1, 2));
            if (iss >> std::hex >> value) {
                result += static_cast<char>(value);
                i += 2;
            } else {
                result += str[i];
            }
        } else if (str[i] == '+') {
            result += ' ';
        } else {
            result += str[i];
        }
    }
    return result;
}

const char* UserFilterWebServer::GetHtmlPage() {
    return R"HTML(<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
    <meta name="apple-mobile-web-app-capable" content="yes">
    <meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
    <title>SOCKS用户滤镜管理</title>
    <style>
        :root {
            --primary: #3b82f6;
            --primary-hover: #2563eb;
            --success: #10b981;
            --warning: #f59e0b;
            --danger: #ef4444;
            --bg: #0a0e1a;
            --bg-secondary: #0f1419;
            --bg-card: rgba(20, 24, 36, 0.84);
            --bg-input: rgba(30, 35, 48, 0.88);
            --bg-hover: rgba(59, 130, 246, 0.08);
            --text: #e2e8f0;
            --text-secondary: #94a3b8;
            --text-muted: #64748b;
            --border: rgba(255, 255, 255, 0.08);
            --border-hover: rgba(59, 130, 246, 0.26);
            --radius: 18px;
            --radius-sm: 12px;
            --shadow: 0 18px 48px rgba(0, 0, 0, 0.42);
        }
        * { margin: 0; padding: 0; box-sizing: border-box; }
        html, body {
            min-height: 100%;
            background: var(--bg);
            color: var(--text);
        }
        body {
            font-family: 'Microsoft YaHei', 'Segoe UI', 'Inter', -apple-system, BlinkMacSystemFont, sans-serif;
            min-height: 100vh;
            display: flex;
            align-items: center;
            justify-content: center;
            padding: 16px;
            -webkit-font-smoothing: antialiased;
            -moz-osx-font-smoothing: grayscale;
            background:
                radial-gradient(circle at 20% 20%, rgba(59,130,246,0.12) 0%, transparent 30%),
                radial-gradient(circle at 80% 15%, rgba(167,139,250,0.10) 0%, transparent 26%),
                radial-gradient(circle at 50% 100%, rgba(16,185,129,0.07) 0%, transparent 24%),
                var(--bg);
        }
        .container {
            width: 100%;
            max-width: 620px;
            padding: 24px 20px;
            border-radius: var(--radius);
            border: 1px solid var(--border);
            background: var(--bg-card);
            box-shadow: var(--shadow);
            backdrop-filter: blur(18px);
        }
        h2 {
            color: var(--text);
            margin-bottom: 22px;
            text-align: center;
            font-size: 22px;
            font-weight: 700;
            letter-spacing: 0.2px;
        }
        .input-group { margin-bottom: 16px; }
        label {
            display: block;
            margin-bottom: 8px;
            color: var(--text-secondary);
            font-weight: 600;
            font-size: 13px;
        }
        input[type="text"], input[type="password"] {
            width: 100%;
            height: 46px;
            padding: 0 14px;
            border: 1px solid var(--border);
            border-radius: var(--radius-sm);
            font-size: 15px;
            background: var(--bg-input);
            color: var(--text);
            transition: border-color 0.2s, box-shadow 0.2s, background 0.2s;
            -webkit-appearance: none;
            appearance: none;
        }
        input::placeholder { color: #6b7280; }
        input[type="text"]:focus, input[type="password"]:focus {
            outline: none;
            border-color: var(--border-hover);
            box-shadow: 0 0 0 4px rgba(59,130,246,0.12);
            background: rgba(30, 35, 48, 0.96);
        }
        button {
            width: 100%;
            min-height: 44px;
            padding: 0 16px;
            border: 1px solid transparent;
            border-radius: var(--radius-sm);
            background: linear-gradient(135deg, #3b82f6 0%, #6366f1 100%);
            color: #fff;
            font-size: 15px;
            font-weight: 700;
            cursor: pointer;
            transition: transform .15s ease, opacity .15s ease, background .2s ease, border-color .2s ease;
            -webkit-tap-highlight-color: transparent;
            touch-action: manipulation;
        }
        button:hover { transform: translateY(-1px); }
        button:active { transform: translateY(0); opacity: 0.92; }
        .message {
            margin-top: 14px;
            padding: 12px 14px;
            border-radius: 12px;
            text-align: center;
            font-size: 13px;
            line-height: 1.6;
            border: 1px solid transparent;
            animation: slideIn 0.2s ease;
        }
        @keyframes slideIn {
            from { opacity: 0; transform: translateY(-6px); }
            to { opacity: 1; transform: translateY(0); }
        }
        .message.error {
            background: rgba(239,68,68,0.12);
            color: #fecaca;
            border-color: rgba(239,68,68,0.22);
        }
        .message.success {
            background: rgba(16,185,129,0.12);
            color: #a7f3d0;
            border-color: rgba(16,185,129,0.24);
        }
        .filter-item {
            padding: 14px;
            border: 1px solid var(--border);
            border-radius: 14px;
            margin-bottom: 10px;
            display: flex;
            align-items: flex-start;
            background: rgba(255,255,255,0.03);
            transition: border-color 0.2s, background 0.2s, transform 0.15s;
            -webkit-tap-highlight-color: transparent;
        }
        .filter-item:hover {
            background: var(--bg-hover);
            border-color: var(--border-hover);
            transform: translateY(-1px);
        }
        .filter-item input[type="checkbox"] {
            width: 18px;
            height: 18px;
            margin-right: 12px;
            margin-top: 2px;
            cursor: pointer;
            flex-shrink: 0;
            accent-color: var(--primary);
        }
        .filter-item label {
            margin: 0;
            cursor: pointer;
            flex: 1;
            font-size: 14px;
            line-height: 1.6;
            user-select: none;
            -webkit-user-select: none;
            color: var(--text);
        }
        .user-info {
            background: rgba(255,255,255,0.025);
            padding: 14px;
            border-radius: 14px;
            margin-bottom: 18px;
            border: 1px solid var(--border);
        }
        .user-info-row {
            display: flex;
            justify-content: space-between;
            align-items: center;
            gap: 12px;
            margin-bottom: 10px;
        }
        .user-info-row:last-child { margin-bottom: 0; }
        .user-info-label {
            color: var(--text-muted);
            font-size: 12px;
            flex-shrink: 0;
        }
        .user-info-value {
            color: var(--text);
            font-weight: 700;
            font-size: 13px;
            text-align: right;
            word-break: break-all;
        }
        .expire-warning { color: #fde68a !important; }
        .expire-danger { color: #fca5a5 !important; }
        .button-group {
            display: flex;
            gap: 10px;
            margin-top: 14px;
        }
        .button-group button { flex: 1; }
        .button-secondary {
            background: rgba(255,255,255,0.04) !important;
            color: var(--text) !important;
            border-color: var(--border) !important;
        }
        .button-warning {
            background: rgba(245,158,11,0.14) !important;
            color: #fde68a !important;
            border-color: rgba(245,158,11,0.24) !important;
        }
        .button-info {
            background: rgba(59,130,246,0.12) !important;
            color: #bfdbfe !important;
            border-color: rgba(59,130,246,0.24) !important;
        }
        #filterBox { display: none; }
        .filter-list-container {
            max-height: 60vh;
            overflow-y: auto;
            -webkit-overflow-scrolling: touch;
            margin-bottom: 10px;
            padding-right: 2px;
        }
        .filter-list-container::-webkit-scrollbar { width: 6px; }
        .filter-list-container::-webkit-scrollbar-thumb {
            background: rgba(255,255,255,0.12);
            border-radius: 3px;
        }
        @media (max-width: 640px) {
            body { padding: 12px; align-items: flex-start; }
            .container {
                padding: 18px 14px;
                border-radius: 16px;
            }
            h2 {
                font-size: 20px;
                margin-bottom: 18px;
            }
            input[type="text"], input[type="password"] {
                font-size: 16px;
            }
            .button-group {
                flex-direction: column;
            }
            .user-info-row {
                flex-direction: column;
                align-items: flex-start;
            }
            .user-info-value {
                text-align: left;
            }
        }
        @media (max-height: 520px) and (orientation: landscape) {
            body { align-items: flex-start; }
            .filter-list-container { max-height: 40vh; }
        }
    </style>)HTML"
R"HTML(
</head>
<body>
    <div class="container">
        <div id="loginBox">
            <h2>SOCKS用户滤镜管理</h2>
            <div class="input-group">
                <label for="username">用户名</label>
                <input type="text" id="username" placeholder="请输入SOCKS用户名">
            </div>
            <div class="input-group">
                <label for="password">密码</label>
                <input type="password" id="password" placeholder="请输入SOCKS密码">
            </div>
            <button onclick="login()">登录</button>
            <div id="loginMsg"></div>
        </div>

        <div id="filterBox">
            <h2>我的滤镜配置</h2>
            <div class="user-info" id="userInfo"></div>
            <div class="filter-list-container">
                <div id="filterList"></div>
            </div>
            <div class="button-group">
                <button onclick="saveConfig()">保存配置</button>
                <button class="button-info" onclick="loadFilters()">刷新</button>
            </div>
            <div class="button-group">
                <button class="button-warning" onclick="resetCounts()">重置次数</button>
                <button class="button-secondary" onclick="logout()">退出</button>
            </div>
            <div id="saveMsg"></div>
        </div>
    </div>

)HTML"
R"HTML(
    <script>
        let currentUser = '';

        function login() {
            const username = document.getElementById('username').value.trim();
            const password = document.getElementById('password').value;

            if (!username || !password) {
                showMessage('loginMsg', '请输入用户名和密码', 'error');
                return;
            }

            fetch('/api/login', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({username, password})
            })
            .then(r => r.json())
            .then(data => {
                if (data.success) {
                    currentUser = username;
                    loadFilters();
                } else {
                    showMessage('loginMsg', data.message || '登录失败', 'error');
                }
            })
            .catch(err => {
                showMessage('loginMsg', '网络错误', 'error');
            });
        }

        function loadFilters() {
            fetch('/api/filters?username=' + encodeURIComponent(currentUser))
            .then(r => r.json())
            .then(data => {
                if (data.success) {
                    document.getElementById('loginBox').style.display = 'none';
                    document.getElementById('filterBox').style.display = 'block';

                    // 🔥 显示用户信息和到期时间
                    let userInfoHtml = '<div class="user-info-row">';
                    userInfoHtml += '<span class="user-info-label">当前用户</span>';
                    userInfoHtml += '<span class="user-info-value">' + data.username + '</span>';
                    userInfoHtml += '</div>';

                    if (data.expireTime) {
                        userInfoHtml += '<div class="user-info-row">';
                        userInfoHtml += '<span class="user-info-label">到期时间</span>';

                        // 计算剩余天数
                        let expireDate = new Date(data.expireTime);
                        let now = new Date();
                        let daysLeft = Math.ceil((expireDate - now) / (1000 * 60 * 60 * 24));

                        let expireClass = '';
                        let expireText = data.expireTime;
                        if (daysLeft <= 0) {
                            expireClass = 'expire-danger';
                            expireText += ' (已过期)';
                        } else if (daysLeft <= 7) {
                            expireClass = 'expire-danger';
                            expireText += ' (剩余' + daysLeft + '天)';
                        } else if (daysLeft <= 30) {
                            expireClass = 'expire-warning';
                            expireText += ' (剩余' + daysLeft + '天)';
                        } else {
                            expireText += ' (剩余' + daysLeft + '天)';
                        }

                        userInfoHtml += '<span class="user-info-value ' + expireClass + '">' + expireText + '</span>';
                        userInfoHtml += '</div>';
                    }

                    document.getElementById('userInfo').innerHTML = userInfoHtml;

                    document.getElementById('filterList').innerHTML =
                        renderFilterSection('采集监听端口滤镜', data.collectorFilters || []) +
                        renderFilterSection('伪心跳监听端口滤镜', data.heartbeatFilters || []);
                } else {
                    showMessage('loginMsg', data.message || '加载失败', 'error');
                }
            })
            .catch(err => {
                showMessage('loginMsg', '网络错误', 'error');
            });
        }

        function renderFilterSection(title, filters) {
            if (!filters || filters.length === 0) {
                return '<div class="user-info">' + title + '：暂无可用滤镜</div>';
            }
            let html = '<h3 class="filter-section-title">' + title + '</h3>';
            filters.forEach(f => {
                html += '<div class="filter-item">';
                html += '<input type="checkbox" class="filter-checkbox" data-auth-filter-id="' + f.id + '" id="filter_' + f.id + '" ' + (f.enabled ? 'checked' : '') + '>';
                html += '<label for="filter_' + f.id + '">' + f.name;
                if (f.defaultEnabled) {
                    html += ' <span class="filter-badge">默认开启</span>';
                }
                if (f.executionCount !== undefined) {
                    var countColor = f.executionCount > 0 ? '#4CAF50' : '#999';
                    html += ' <span class="filter-count" style="color: ' + countColor + '; font-size: 12px;">(执行: ' + f.executionCount + '次)</span>';
                }
                html += '</label>';
                html += '</div>';
            });
            return html;
        }

        function saveConfig() {
            const checkboxes = document.querySelectorAll('.filter-checkbox[data-auth-filter-id]');
            const filterIds = [];
            checkboxes.forEach(cb => {
                if (cb.checked) {
                    filterIds.push(parseInt(cb.getAttribute('data-auth-filter-id')));
                }
            });

            fetch('/api/update', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({username: currentUser, filterIds})
            })
            .then(r => r.json())
            .then(data => {
                showMessage('saveMsg', data.message || (data.success ? '保存成功' : '保存失败'), data.success ? 'success' : 'error');
                setTimeout(() => document.getElementById('saveMsg').innerHTML = '', 3000);
            })
            .catch(err => {
                showMessage('saveMsg', '网络错误', 'error');
            });
        }

        function resetCounts() {
            if (!confirm('确定要重置所有滤镜的执行次数吗？')) return;

            fetch('/api/reset-counts', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({username: currentUser})
            })
            .then(r => r.json())
            .then(data => {
                showMessage('saveMsg', data.message || (data.success ? '重置成功' : '重置失败'), data.success ? 'success' : 'error');
                if (data.success) {
                    loadFilters(); // 刷新显示
                }
                setTimeout(() => document.getElementById('saveMsg').innerHTML = '', 3000);
            })
            .catch(err => {
                showMessage('saveMsg', '网络错误', 'error');
            });
        }

        function logout() {
            currentUser = '';
            document.getElementById('loginBox').style.display = 'block';
            document.getElementById('filterBox').style.display = 'none';
            document.getElementById('username').value = '';
            document.getElementById('password').value = '';
            document.getElementById('loginMsg').innerHTML = '';
        }

        function showMessage(elementId, message, type) {
            const el = document.getElementById(elementId);
            el.innerHTML = '<div class="message ' + type + '">' + message + '</div>';
        }

        // 回车登录
        document.addEventListener('DOMContentLoaded', function() {
            document.getElementById('password').addEventListener('keypress', function(e) {
                if (e.key === 'Enter') login();
            });
        });
    </script>
</body>
</html>)HTML";
}





