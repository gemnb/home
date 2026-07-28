#include "WhitelistThreadPool.h"
#include "Logger.h"

#define NOMINMAX  // 防止Windows.h定义min/max宏
#include <algorithm>

WhitelistThreadPool::WhitelistThreadPool(int whitelistSize, int normalSize)
    : isRunning(false)
    , whitelistPoolSize(whitelistSize)
    , normalPoolSize(normalSize)
    , whitelistProcessed(0)
    , normalProcessed(0)
    , whitelistQueueSize(0)
    , normalQueueSize(0) {
}

WhitelistThreadPool::~WhitelistThreadPool() {
    Stop();
}

bool WhitelistThreadPool::Start(ConnectionHandler handler) {
    if (isRunning.load()) {
        AB_LOG_WARNING("[ThreadPool] Thread pool is already running");
        return false;
    }

    if (!handler) {
        AB_LOG_ERROR("[ThreadPool] Connection handler is null");
        return false;
    }

    connectionHandler = handler;
    isRunning = true;

    // 创建白名单工作线程
    for (int i = 0; i < whitelistPoolSize; i++) {
        whitelistWorkers.emplace_back(&WhitelistThreadPool::WhitelistWorkerThread, this);
    }

    // 创建普通工作线程
    for (int i = 0; i < normalPoolSize; i++) {
        normalWorkers.emplace_back(&WhitelistThreadPool::NormalWorkerThread, this);
    }

    AB_LOG_INFO("[ThreadPool] Started with " + std::to_string(whitelistPoolSize) +
        " whitelist workers and " + std::to_string(normalPoolSize) + " normal workers");

    return true;
}

void WhitelistThreadPool::Stop() {
    if (!isRunning.load()) {
        return;
    }

    isRunning = false;

    // 唤醒所有等待的线程
    whitelistCV.notify_all();
    normalCV.notify_all();

    // 等待白名单线程退出
    for (auto& worker : whitelistWorkers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    whitelistWorkers.clear();

    // 等待普通线程退出
    for (auto& worker : normalWorkers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    normalWorkers.clear();

    // 清理队列中未处理的连接
    {
        std::lock_guard<std::mutex> lock(whitelistMutex);
        while (!whitelistQueue.empty()) {
            ConnectionTask task = whitelistQueue.front();
            whitelistQueue.pop();
            if (task.clientSocket != INVALID_SOCKET) {
                closesocket(task.clientSocket);
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(normalMutex);
        while (!normalQueue.empty()) {
            ConnectionTask task = normalQueue.front();
            normalQueue.pop();
            if (task.clientSocket != INVALID_SOCKET) {
                closesocket(task.clientSocket);
            }
        }
    }

    AB_LOG_INFO("[ThreadPool] Thread pool stopped");
}

void WhitelistThreadPool::WhitelistWorkerThread() {
    AB_LOG_INFO("[ThreadPool] Whitelist worker thread started");

    while (isRunning.load()) {
        ConnectionTask task;
        bool hasTask = false;

        {
            std::unique_lock<std::mutex> lock(whitelistMutex);

            // 等待任务或停止信号
            whitelistCV.wait(lock, [this] {
                return !whitelistQueue.empty() || !isRunning.load();
            });

            // 检查是否应该退出
            if (!isRunning.load() && whitelistQueue.empty()) {
                break;
            }

            // 取出任务
            if (!whitelistQueue.empty()) {
                task = whitelistQueue.front();
                whitelistQueue.pop();
                whitelistQueueSize = static_cast<int>(whitelistQueue.size());
                hasTask = true;
            }
        }

        // 在锁外处理连接
        if (hasTask && connectionHandler) {
            try {
                connectionHandler(task.clientSocket, task.clientAddr, task.clientIP);
                whitelistProcessed++;
            }
            catch (const std::exception& e) {
                AB_LOG_ERROR("[ThreadPool] Whitelist handler exception: " + std::string(e.what()));
                if (task.clientSocket != INVALID_SOCKET) {
                    closesocket(task.clientSocket);
                }
            }
            catch (...) {
                AB_LOG_ERROR("[ThreadPool] Whitelist handler unknown exception");
                if (task.clientSocket != INVALID_SOCKET) {
                    closesocket(task.clientSocket);
                }
            }
        }
    }

    AB_LOG_INFO("[ThreadPool] Whitelist worker thread exited");
}

void WhitelistThreadPool::NormalWorkerThread() {
    AB_LOG_INFO("[ThreadPool] Normal worker thread started");

    while (isRunning.load()) {
        ConnectionTask task;
        bool hasTask = false;

        {
            std::unique_lock<std::mutex> lock(normalMutex);

            // 等待任务或停止信号
            normalCV.wait(lock, [this] {
                return !normalQueue.empty() || !isRunning.load();
            });

            // 检查是否应该退出
            if (!isRunning.load() && normalQueue.empty()) {
                break;
            }

            // 取出任务
            if (!normalQueue.empty()) {
                task = normalQueue.front();
                normalQueue.pop();
                normalQueueSize = static_cast<int>(normalQueue.size());
                hasTask = true;
            }
        }

        // 在锁外处理连接
        if (hasTask && connectionHandler) {
            try {
                connectionHandler(task.clientSocket, task.clientAddr, task.clientIP);
                normalProcessed++;
            }
            catch (const std::exception& e) {
                AB_LOG_ERROR("[ThreadPool] Normal handler exception: " + std::string(e.what()));
                if (task.clientSocket != INVALID_SOCKET) {
                    closesocket(task.clientSocket);
                }
            }
            catch (...) {
                AB_LOG_ERROR("[ThreadPool] Normal handler unknown exception");
                if (task.clientSocket != INVALID_SOCKET) {
                    closesocket(task.clientSocket);
                }
            }
        }
    }

    AB_LOG_INFO("[ThreadPool] Normal worker thread exited");
}

bool WhitelistThreadPool::SubmitConnection(const ConnectionTask& task) {
    if (!isRunning.load()) {
        return false;
    }

    if (task.isWhitelisted) {
        return SubmitWhitelistConnection(task.clientSocket, task.clientAddr, task.clientIP);
    }
    else {
        return SubmitNormalConnection(task.clientSocket, task.clientAddr, task.clientIP);
    }
}

bool WhitelistThreadPool::SubmitWhitelistConnection(SOCKET sock, const std::string& addr, const std::string& ip) {
    if (!isRunning.load()) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(whitelistMutex);

        // 白名单队列大小限制（防止内存溢出，但要比普通队列大）
        const size_t MAX_WHITELIST_QUEUE = 500;
        if (whitelistQueue.size() >= MAX_WHITELIST_QUEUE) {
            AB_LOG_WARNING("[ThreadPool] Whitelist queue full, rejecting: " + ip);
            return false;
        }

        whitelistQueue.push(ConnectionTask(sock, addr, ip, true));
        whitelistQueueSize = static_cast<int>(whitelistQueue.size());
    }

    whitelistCV.notify_one();

    AB_LOG_INFO("[ThreadPool] Whitelist connection queued: " + ip +
        " (queue: " + std::to_string(whitelistQueueSize.load()) + ")");

    return true;
}

bool WhitelistThreadPool::SubmitNormalConnection(SOCKET sock, const std::string& addr, const std::string& ip) {
    if (!isRunning.load()) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(normalMutex);

        // 普通队列大小限制
        const size_t MAX_NORMAL_QUEUE = 200;
        if (normalQueue.size() >= MAX_NORMAL_QUEUE) {
            AB_LOG_WARNING("[ThreadPool] Normal queue full, rejecting: " + ip);
            return false;
        }

        normalQueue.push(ConnectionTask(sock, addr, ip, false));
        normalQueueSize = static_cast<int>(normalQueue.size());
    }

    normalCV.notify_one();

    return true;
}

bool WhitelistThreadPool::IsWhitelistPoolAvailable() const {
    if (!isRunning.load()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(whitelistMutex));
    return whitelistQueue.size() < 500;  // 与MAX_WHITELIST_QUEUE保持一致
}

bool WhitelistThreadPool::IsNormalPoolAvailable() const {
    if (!isRunning.load()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(normalMutex));
    return normalQueue.size() < 200;  // 与MAX_NORMAL_QUEUE保持一致
}

void WhitelistThreadPool::SetPoolSizes(int whitelistSize, int normalSize) {
    if (isRunning.load()) {
        AB_LOG_WARNING("[ThreadPool] Cannot change pool sizes while running");
        return;
    }

    whitelistPoolSize = (std::max)(1, whitelistSize);
    normalPoolSize = (std::max)(1, normalSize);

    AB_LOG_INFO("[ThreadPool] Pool sizes updated: whitelist=" +
        std::to_string(whitelistPoolSize) + ", normal=" + std::to_string(normalPoolSize));
}
