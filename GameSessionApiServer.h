#ifndef GAMESESSIONAPISERVER_H
#define GAMESESSIONAPISERVER_H

#include <winsock2.h>
#include <thread>
#include <string>
#include <mutex>
#include "PacketCollector.h"
#include "DatabaseManager.h"

#pragma comment(lib, "ws2_32.lib")

class GameSessionApiServer {
public:
    GameSessionApiServer(int serverPort, const std::string& domain);
    ~GameSessionApiServer();

    // ===== 主要接口 =====
    bool Start(PacketCollector* collector, DatabaseManager* db);
    void Stop();

    // ===== 🔥 状态查询和配置 =====
    bool IsRunning() const { return isRunning; }
    void SetMonitorDomain(const std::string& domain) {
        std::lock_guard<std::mutex> lock(domainMutex);
        monitorDomain = domain;
    }
    std::string GetMonitorDomain() const {
        std::lock_guard<std::mutex> lock(domainMutex);
        return monitorDomain;
    }
    int GetPort() const { return port; }

private:
    void ServerLoop();
    void HandleRequest(SOCKET clientSocket);
    std::string HandleLatestRequest(const std::string& username);

    // 工具函数
    std::string BuildJsonResponse(bool success, const std::string& data, const std::string& error);
    std::string GetCurrentTimestamp();

    // 编码转换函数
    std::string GBKToUTF8(const std::string& gbkStr);
    std::string ANSIToUTF8(const std::string& ansiStr);
    std::string EnsureUTF8(const std::string& str);

    // ===== 成员变量 =====
    int port;
    std::string monitorDomain;
    mutable std::mutex domainMutex;  // 保护 monitorDomain
    SOCKET listenSocket;
    bool isRunning;
    std::thread serverThread;

    PacketCollector* packetCollector;
    DatabaseManager* database;
};

#endif
