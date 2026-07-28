#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <winsock2.h>
#include <ws2tcpip.h>
#include "PacketCollector.h"
#include <map>
#include <vector>
#include <functional>

#pragma comment(lib, "ws2_32.lib")

// 账号变更回调类型
using AccountChangedCallback = std::function<void()>;

class HttpApiServer {
private:
    int port;
    std::string username;
    std::string password;
    std::string adminPassword;
    SOCKET listenSocket;
    std::thread serverThread;
    std::atomic<bool> isRunning;
    PacketCollector* socks5Server;
    std::string htmlTemplate;

    // 账号变更回调（用于保存到数据库）
    AccountChangedCallback onAccountChanged;

    void ServerLoop();
    void HandleClient(SOCKET clientSocket);

    // HTML渲染
    std::string RenderAccountListHTML();
    std::string LoadHTMLTemplate();
    std::string ReplaceVariables(const std::string& html,
        const std::map<std::string, std::string>& vars);

    // POST处理
    bool HandlePostRequest(const std::string& postData, std::string& errorMsg);
    std::map<std::string, std::string> ParsePostData(const std::string& postData);
    std::string UrlDecode(const std::string& str);

    // 认证
    bool CheckAuth(const std::string& authHeader);
    std::string Base64Decode(const std::string& encoded);

    // ✅ 新增：编码转换
    static std::string GBKToUTF8(const std::string& gbkStr);
    static std::string GetEmbeddedHTMLTemplate();

    // 辅助函数
    std::string GetCurrentDate();
    std::string GetCurrentTime();

public:
    HttpApiServer(int port, const std::string& user, const std::string& pass);
    ~HttpApiServer();

    bool Start(PacketCollector* server);
    void Stop();
    bool IsRunning() const { return isRunning; }
    void SetAuth(const std::string& user, const std::string& pass);

    // 设置账号变更回调（用于保存到数据库）
    void SetAccountChangedCallback(AccountChangedCallback callback) {
        onAccountChanged = callback;
    }
};
