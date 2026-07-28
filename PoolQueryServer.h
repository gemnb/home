#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

// Callback: Validate SOCKS account and return bound GameID
using QueryValidateCallback = std::function<bool(const std::string& username,
    const std::string& password, std::string& outGameID)>;

// Callback: Get packet count for a GameID
using GetPacketCountCallback = std::function<size_t(const std::string& gameID)>;

// Callback: Clear packets for a GameID (returns cleared count)
using ClearPacketsByGameIDCallback = std::function<size_t(const std::string& gameID)>;

class PoolQueryServer {
private:
    int port;
    SOCKET listenSocket;
    std::thread serverThread;
    std::atomic<bool> isRunning;

    QueryValidateCallback validateCallback;
    GetPacketCountCallback getPacketCountCallback;
    ClearPacketsByGameIDCallback clearPacketsCallback;

    void ServerLoop();
    void HandleClient(SOCKET clientSocket, const std::string& clientIP);

    // HTTP response
    void SendResponse(SOCKET socket, int statusCode, const std::string& contentType,
        const std::string& body);
    void SendHTML(SOCKET socket, const std::string& html);
    void SendJSON(SOCKET socket, bool success, const std::string& message,
        const std::string& gameID = "", size_t packetCount = 0);
    void SendClearJSON(SOCKET socket, bool success, const std::string& message,
        const std::string& gameID, size_t clearedCount);

    // Request parsing
    std::string ParsePostData(const std::string& request, const std::string& key);
    std::string UrlDecode(const std::string& str);

    // HTML pages
    std::string GetLoginPageHTML();
    std::string GetResultPageHTML(bool success, const std::string& message,
        const std::string& gameID = "", size_t packetCount = 0,
        const std::string& username = "", const std::string& password = "");
    std::string GetClearResultPageHTML(bool success, const std::string& gameID,
        size_t clearedCount, size_t currentCount);

public:
    PoolQueryServer(int port);
    ~PoolQueryServer();

    bool Start();
    void Stop();
    bool IsRunning() const { return isRunning; }
    int GetPort() const { return port; }

    // Set callbacks
    void SetValidateCallback(QueryValidateCallback callback) {
        validateCallback = callback;
    }

    void SetGetPacketCountCallback(GetPacketCountCallback callback) {
        getPacketCountCallback = callback;
    }

    void SetClearPacketsCallback(ClearPacketsByGameIDCallback callback) {
        clearPacketsCallback = callback;
    }
};
