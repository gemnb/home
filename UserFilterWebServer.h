#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <vector>
#include <mutex>
#include <queue>
#include <condition_variable>

#pragma comment(lib, "ws2_32.lib")

// 前向声明
class PacketCollector;

struct AuthorizedWebFilter {
    int id = 0;
    std::string name;
    bool applyToCollector = false;
    bool applyToHeartbeat = false;
};

struct UserGameInfo {
    std::string gameId;
    size_t poolCount = 0;
};

// ==================== 用户滤镜Web服务器 ====================
// 提供HTTP接口供SOCKS用户通过网页管理自己的WPE滤镜配置
class UserFilterWebServer {
public:
    UserFilterWebServer(const std::string& instanceId, int port);
    ~UserFilterWebServer();

    bool Start();  // 启动HTTP服务
    void Stop();   // 停止HTTP服务
    bool IsRunning() const { return running.load(); }
    int GetPort() const { return port; }
    std::string GetInstanceId() const { return instanceId; }

    // 设置SOCKS账号验证器（用于登录验证）
    void SetSocksValidator(std::function<bool(const std::string&, const std::string&)> validator);

    // 设置SOCKS账号验证器（带失败原因）
    void SetSocksValidatorWithReason(std::function<bool(const std::string&, const std::string&, std::string&)> validator);

    // 设置滤镜列表获取器（用于获取当前实例的所有滤镜）
    void SetFilterListGetter(std::function<std::vector<std::pair<int, std::string>>()> getter);
    void SetAuthorizedFilterListGetter(
        std::function<std::vector<AuthorizedWebFilter>(const std::string& username)> getter);

    // 设置账号信息获取器（用于获取用户到期时间）
    void SetAccountInfoGetter(std::function<std::string(const std::string&)> getter);
    void SetUserGameInfoGetter(std::function<UserGameInfo(const std::string& username)> getter);
    void SetUserPoolClearer(std::function<size_t(const std::string& username)> clearer);

private:
    std::string instanceId;
    int port;
    std::atomic<bool> running;
    std::thread serverThread;
    SOCKET listenSocket;
    size_t workerThreadCount;
    std::vector<std::thread> workerThreads;
    std::queue<SOCKET> pendingClientSockets;
    std::vector<SOCKET> activeClientSockets;
    mutable std::mutex workerMutex;
    std::condition_variable workerCondition;

    // SOCKS账号验证回调
    std::function<bool(const std::string&, const std::string&)> socksValidator;

    // SOCKS账号验证回调（带失败原因）
    std::function<bool(const std::string&, const std::string&, std::string&)> socksValidatorWithReason;

    // 滤镜列表获取回调
    std::function<std::vector<std::pair<int, std::string>>()> filterListGetter;
    std::function<std::vector<AuthorizedWebFilter>(const std::string& username)> authorizedFilterListGetter;

    // 账号信息获取回调 (返回到期时间)
    std::function<std::string(const std::string&)> accountInfoGetter;
    std::function<UserGameInfo(const std::string& username)> userGameInfoGetter;
    std::function<size_t(const std::string& username)> userPoolClearer;

    // HTTP请求处理
    void ServerLoop();
    void HandleRequest(SOCKET clientSocket);
    void WorkerLoop();
    void HandleClientSession(SOCKET clientSocket);
    void RemoveActiveClientSocket(SOCKET clientSocket);

    // HTTP响应构建
    std::string BuildHttpResponse(int statusCode, const std::string& contentType, const std::string& body);
    std::string GetStatusText(int statusCode);

    // API端点处理
    std::string HandleLogin(const std::string& body);
    std::string HandleGetFilters(const std::string& username);
    std::string HandleUpdateFilters(const std::string& body);
    std::string HandleResetCounts(const std::string& body);
    std::string HandleClearPool(const std::string& body);

    // 辅助函数
    std::string ParseRequestMethod(const std::string& request);
    std::string ParseRequestPath(const std::string& request);
    std::string ParseRequestBody(const std::string& request);
    std::string ParseQueryParam(const std::string& path, const std::string& param);
    std::string UrlDecode(const std::string& str);

    // HTML页面
    static const char* GetHtmlPage();
};
