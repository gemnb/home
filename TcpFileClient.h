#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <winsock2.h>
#include <functional>
#include "FileTransferProtocol.h"
#pragma comment(lib, "ws2_32.lib")


// 下载进度回调：void(uint64_t downloaded, uint64_t total, double speed)
using DownloadProgressCallback = std::function<void(uint64_t, uint64_t, double)>;

class TcpFileClient {
private:
    std::string serverHost;
    int serverPort;

    std::atomic<bool> isDownloading;
    std::atomic<uint64_t> currentDownloaded;
    std::atomic<uint64_t> totalSize;
    std::atomic<double> downloadSpeed;  // KB/s

    std::mutex downloadMutex;

    bool ReceiveFile(SOCKET socket, const std::string& savePath,
        DownloadProgressCallback progressCallback);

public:
    TcpFileClient(const std::string& host, int port);
    ~TcpFileClient();

    bool DownloadDatabaseFile(const std::string& savePath,
        DownloadProgressCallback progressCallback = nullptr);

    bool IsDownloading() const { return isDownloading; }
    uint64_t GetCurrentDownloaded() const { return currentDownloaded; }
    uint64_t GetTotalSize() const { return totalSize; }
    double GetDownloadSpeed() const { return downloadSpeed; }
};
