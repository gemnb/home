#pragma once

#include <winsock2.h>
#include <thread>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <string>

// 连接任务结构
struct ConnectionTask {
    SOCKET clientSocket;
    std::string clientAddr;
    std::string clientIP;
    bool isWhitelisted;

    ConnectionTask() : clientSocket(INVALID_SOCKET), isWhitelisted(false) {}
    ConnectionTask(SOCKET sock, const std::string& addr, const std::string& ip, bool whitelist)
        : clientSocket(sock), clientAddr(addr), clientIP(ip), isWhitelisted(whitelist) {}
};

// 连接处理回调类型
using ConnectionHandler = std::function<void(SOCKET, const std::string&, const std::string&)>;

// 白名单专用线程池
class WhitelistThreadPool {
private:
    // 白名单线程池
    std::vector<std::thread> whitelistWorkers;
    std::queue<ConnectionTask> whitelistQueue;
    std::mutex whitelistMutex;
    std::condition_variable whitelistCV;

    // 普通线程池
    std::vector<std::thread> normalWorkers;
    std::queue<ConnectionTask> normalQueue;
    std::mutex normalMutex;
    std::condition_variable normalCV;

    // 运行状态
    std::atomic<bool> isRunning;

    // 线程池大小
    int whitelistPoolSize;
    int normalPoolSize;

    // 连接处理回调
    ConnectionHandler connectionHandler;

    // 统计信息
    std::atomic<int> whitelistProcessed;
    std::atomic<int> normalProcessed;
    std::atomic<int> whitelistQueueSize;
    std::atomic<int> normalQueueSize;

    // 工作线程函数
    void WhitelistWorkerThread();
    void NormalWorkerThread();

public:
    WhitelistThreadPool(int whitelistSize = 10, int normalSize = 50);
    ~WhitelistThreadPool();

    // 启动线程池
    bool Start(ConnectionHandler handler);

    // 停止线程池
    void Stop();

    // 提交连接任务
    bool SubmitConnection(const ConnectionTask& task);

    // 提交白名单连接
    bool SubmitWhitelistConnection(SOCKET sock, const std::string& addr, const std::string& ip);

    // 提交普通连接
    bool SubmitNormalConnection(SOCKET sock, const std::string& addr, const std::string& ip);

    // 获取统计信息
    int GetWhitelistProcessed() const { return whitelistProcessed.load(); }
    int GetNormalProcessed() const { return normalProcessed.load(); }
    int GetWhitelistQueueSize() const { return whitelistQueueSize.load(); }
    int GetNormalQueueSize() const { return normalQueueSize.load(); }

    // 检查线程池状态
    bool IsRunning() const { return isRunning.load(); }
    bool IsWhitelistPoolAvailable() const;
    bool IsNormalPoolAvailable() const;

    // 配置
    void SetPoolSizes(int whitelistSize, int normalSize);
    int GetWhitelistPoolSize() const { return whitelistPoolSize; }
    int GetNormalPoolSize() const { return normalPoolSize; }
};
