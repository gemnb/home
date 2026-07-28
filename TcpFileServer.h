#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <mutex>
#include <winsock2.h>
#include <ws2tcpip.h>
#include "DatabaseManager.h"
#include "FileTransferProtocol.h"
#pragma comment(lib, "ws2_32.lib")



class TcpFileServer {
private:
    int port;
    SOCKET listenSocket;
    std::thread serverThread;
    std::atomic<bool> isRunning;
    DatabaseManager* database;

    std::atomic<int> totalConnections;
    std::atomic<uint64_t> totalBytesSent;
    std::mutex statsMutex;

    void ServerLoop();
    void HandleClient(SOCKET clientSocket, const std::string& clientAddr);
    bool SendDatabaseFile(SOCKET clientSocket, const std::string& dbPath);

public:
    TcpFileServer(int port, DatabaseManager* db);
    ~TcpFileServer();

    bool Start();
    void Stop();
    bool IsRunning() const { return isRunning; }

    int GetTotalConnections() const { return totalConnections; }
    uint64_t GetTotalBytesSent() const { return totalBytesSent; }
};
