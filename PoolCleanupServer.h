#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

// Callback: Validate SOCKS account and return bound GameID
using ValidateAndGetGameIDCallback = std::function<bool(const std::string& username,
    const std::string& password, std::string& outGameID)>;

// Callback: Clear memory pool by GameID
using ClearPoolByGameIDCallback = std::function<void(const std::string& gameID)>;

class PoolCleanupServer {
private:
    int port;
    SOCKET listenSocket;
    std::thread serverThread;
    std::atomic<bool> isRunning;

    ValidateAndGetGameIDCallback validateCallback;
    ClearPoolByGameIDCallback clearPoolCallback;

    void ServerLoop();
    void HandleClient(SOCKET clientSocket, const std::string& clientIP);

    // HTTP response
    void SendResponse(SOCKET socket, int statusCode, const std::string& contentType,
        const std::string& body);
    void SendHTML(SOCKET socket, const std::string& html);
    void SendJSON(SOCKET socket, bool success, const std::string& message);

    // Request parsing
    std::string ParsePostData(const std::string& request, const std::string& key);
    std::string UrlDecode(const std::string& str);

    // HTML pages
    std::string GetLoginPageHTML();
    std::string GetResultPageHTML(bool success, const std::string& message,
        const std::string& gameID = "");

public:
    PoolCleanupServer(int port);
    ~PoolCleanupServer();

    bool Start();
    void Stop();
    bool IsRunning() const { return isRunning; }
    int GetPort() const { return port; }

    // Set callbacks
    void SetValidateCallback(ValidateAndGetGameIDCallback callback) {
        validateCallback = callback;
    }

    void SetClearPoolCallback(ClearPoolByGameIDCallback callback) {
        clearPoolCallback = callback;
    }
};
