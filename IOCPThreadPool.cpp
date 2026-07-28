#include "IOCPThreadPool.h"
#include "Logger.h"
#include "GlobalAntiCCCoordinator.h"
#include "PacketParser.h"
#include "WPEFilterIntegration.h"
#include "UserFilterManager.h"
#include "UserFilterWebServer.h"
#include "WPEFilter.h"
#include <algorithm>
#include <mstcpip.h>  // 用于 TCP Keep-Alive 设置

// ==================== 辅助函数：设置 TCP Keep-Alive ====================
// 解决长连接在3分钟无数据时被中间设备（防火墙/NAT）断开的问题
static void SetSocketKeepAlive(SOCKET sock) {
    if (sock == INVALID_SOCKET) return;

    // 启用 TCP Keep-Alive
    BOOL keepAlive = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (char*)&keepAlive, sizeof(keepAlive));

    // 设置 Keep-Alive 参数
    struct tcp_keepalive ka = {0};
    ka.onoff = 1;
    ka.keepalivetime = 60000;     // 60秒后开始发送 Keep-Alive 探测
    ka.keepaliveinterval = 10000; // 每10秒发送一次探测
    DWORD bytesReturned = 0;
    WSAIoctl(sock, SIO_KEEPALIVE_VALS, &ka, sizeof(ka), NULL, 0, &bytesReturned, NULL, NULL);
}

// ==================== 构造函数 ====================
IOCPThreadPool::IOCPThreadPool(int workerThreads)
    : iocpHandle(NULL)
    , listenSocket(INVALID_SOCKET)
    , numWorkerThreads(workerThreads)
    , isRunning(false)
    , nextConnectionId(1)
    , whitelistConnCount(0)
    , normalConnCount(0)
    , totalConnCount(0)
    , whitelistProcessed(0)
    , normalProcessed(0)
    , lifecycleCreatedCount(0)
    , lifecycleCloseRequestedCount(0)
    , lifecycleClosedCount(0)
    , totalPackets(0)
    , totalBytes(0)
    , fragmentedPackets(0)
    , multiPackets(0)
    , maxWhitelistConnections(2000)
    , maxNormalConnections(500)
    , whitelistQueueLimit(1000)
    , normalQueueLimit(200)
    , enableSocks5Auth(false)  // 🔥 默认不启用认证
    , lpfnAcceptEx(NULL)
    , lpfnGetAcceptExSockaddrs(NULL)
    , lpfnConnectEx(NULL)
    , acceptPoolSize(10) {

    // 自动检测CPU核心数
    if (numWorkerThreads <= 0) {
        SYSTEM_INFO sysInfo;
        GetSystemInfo(&sysInfo);
        numWorkerThreads = sysInfo.dwNumberOfProcessors * 2;
        if (numWorkerThreads < 4) numWorkerThreads = 4;
        if (numWorkerThreads > 32) numWorkerThreads = 32;
    }
}

// ==================== 析构函数 ====================
IOCPThreadPool::~IOCPThreadPool() {
    Stop();
}

// ==================== 初始化扩展函数 ====================
bool IOCPThreadPool::InitializeExtensionFunctions() {
    // 获取AcceptEx函数指针
    GUID guidAcceptEx = WSAID_ACCEPTEX;
    DWORD dwBytes = 0;
    if (WSAIoctl(listenSocket, SIO_GET_EXTENSION_FUNCTION_POINTER,
        &guidAcceptEx, sizeof(guidAcceptEx),
        &lpfnAcceptEx, sizeof(lpfnAcceptEx),
        &dwBytes, NULL, NULL) == SOCKET_ERROR) {
        AB_LOG_ERROR("[IOCP] Failed to get AcceptEx: " + std::to_string(WSAGetLastError()));
        return false;
    }

    // 获取GetAcceptExSockaddrs函数指针
    GUID guidGetAcceptExSockaddrs = WSAID_GETACCEPTEXSOCKADDRS;
    if (WSAIoctl(listenSocket, SIO_GET_EXTENSION_FUNCTION_POINTER,
        &guidGetAcceptExSockaddrs, sizeof(guidGetAcceptExSockaddrs),
        &lpfnGetAcceptExSockaddrs, sizeof(lpfnGetAcceptExSockaddrs),
        &dwBytes, NULL, NULL) == SOCKET_ERROR) {
        AB_LOG_ERROR("[IOCP] Failed to get GetAcceptExSockaddrs: " + std::to_string(WSAGetLastError()));
        return false;
    }

    // 获取ConnectEx函数指针
    GUID guidConnectEx = WSAID_CONNECTEX;
    if (WSAIoctl(listenSocket, SIO_GET_EXTENSION_FUNCTION_POINTER,
        &guidConnectEx, sizeof(guidConnectEx),
        &lpfnConnectEx, sizeof(lpfnConnectEx),
        &dwBytes, NULL, NULL) == SOCKET_ERROR) {
        AB_LOG_ERROR("[IOCP] Failed to get ConnectEx: " + std::to_string(WSAGetLastError()));
        return false;
    }

    return true;
}

// ==================== 启动线程池 ====================
bool IOCPThreadPool::Start(SOCKET socket) {
    if (isRunning.load()) {
        AB_LOG_WARNING("[IOCP] Thread pool already running");
        return false;
    }

    listenSocket = socket;

    // 初始化扩展函数
    if (!InitializeExtensionFunctions()) {
        return false;
    }

    // 创建IOCP
    iocpHandle = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, numWorkerThreads);
    if (iocpHandle == NULL) {
        AB_LOG_ERROR("[IOCP] Failed to create IOCP: " + std::to_string(GetLastError()));
        return false;
    }

    // 将监听socket关联到IOCP
    if (!AssociateWithIOCP(listenSocket, 0)) {
        CloseHandle(iocpHandle);
        iocpHandle = NULL;
        return false;
    }

    isRunning = true;

    // 创建工作线程
    for (int i = 0; i < numWorkerThreads; i++) {
        workerThreads.emplace_back(&IOCPThreadPool::WorkerThread, this);
    }

    // 投递初始Accept请求
    for (int i = 0; i < acceptPoolSize; i++) {
        if (!PostAccept()) {
            AB_LOG_WARNING("[IOCP] Failed to post Accept request");
        }
    }

    AB_LOG_INFO("[IOCP] Thread pool started, workers: " + std::to_string(numWorkerThreads) +
        ", max whitelist: " + std::to_string(maxWhitelistConnections) +
        ", max normal: " + std::to_string(maxNormalConnections));

    return true;
}

// ==================== 停止线程池 ====================
void IOCPThreadPool::Stop() {
    if (!isRunning.load()) {
        return;
    }

    isRunning = false;

    // 发送退出信号给所有工作线程
    for (size_t i = 0; i < workerThreads.size(); i++) {
        PostQueuedCompletionStatus(iocpHandle, 0, 0, NULL);
    }

    // 等待所有工作线程退出
    for (auto& thread : workerThreads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    workerThreads.clear();

    // 关闭所有连接
    {
        std::lock_guard<std::mutex> lock(connectionsMutex);
        for (auto& pair : connections) {
            auto& conn = pair.second;
            conn->isActive = false;
            if (conn->clientSocket != INVALID_SOCKET) {
                closesocket(conn->clientSocket);
                conn->clientSocket = INVALID_SOCKET;
            }
            if (conn->serverSocket != INVALID_SOCKET) {
                closesocket(conn->serverSocket);
                conn->serverSocket = INVALID_SOCKET;
            }
        }
        connections.clear();
    }

    // 清理Accept socket池
    {
        std::lock_guard<std::mutex> lock(acceptPoolMutex);
        for (SOCKET sock : acceptSocketPool) {
            if (sock != INVALID_SOCKET) {
                closesocket(sock);
            }
        }
        acceptSocketPool.clear();
        acceptContextPool.clear();
    }

    // 关闭IOCP句柄
    if (iocpHandle != NULL) {
        CloseHandle(iocpHandle);
        iocpHandle = NULL;
    }

    AB_LOG_INFO("[IOCP] Thread pool stopped");
}

// ==================== 设置socket为非阻塞 ====================
void IOCPThreadPool::SetSocketNonBlocking(SOCKET sock) {
    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);
}

// ==================== 关联socket到IOCP ====================
bool IOCPThreadPool::AssociateWithIOCP(SOCKET sock, ULONG_PTR completionKey) {
    HANDLE h = CreateIoCompletionPort((HANDLE)sock, iocpHandle, completionKey, 0);
    if (h == NULL) {
        AB_LOG_ERROR("[IOCP] Failed to associate socket: " + std::to_string(GetLastError()));
        return false;
    }
    return true;
}

// ==================== 投递Accept请求 ====================
bool IOCPThreadPool::PostAccept() {
    // 创建新socket用于接受连接
    SOCKET acceptSocket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (acceptSocket == INVALID_SOCKET) {
        AB_LOG_ERROR("[IOCP] Failed to create accept socket: " + std::to_string(WSAGetLastError()));
        return false;
    }

    // 创建IO上下文
    auto ctx = std::make_unique<IOContext>();
    ctx->opType = IOOperationType::IO_ACCEPT;
    ctx->socket = acceptSocket;
    ctx->wsaBuf.len = sizeof(ctx->buffer);

    DWORD bytesReceived = 0;
    BOOL result = lpfnAcceptEx(
        listenSocket,
        acceptSocket,
        ctx->buffer,
        0,  // 不接收初始数据
        sizeof(sockaddr_in) + 16,
        sizeof(sockaddr_in) + 16,
        &bytesReceived,
        ctx.get()
    );

    if (!result && WSAGetLastError() != ERROR_IO_PENDING) {
        AB_LOG_ERROR("[IOCP] AcceptEx failed: " + std::to_string(WSAGetLastError()));
        closesocket(acceptSocket);
        return false;
    }

    // 保存到池中
    {
        std::lock_guard<std::mutex> lock(acceptPoolMutex);
        acceptSocketPool.push_back(acceptSocket);
        acceptContextPool.push_back(std::move(ctx));
    }

    return true;
}

// ==================== 投递Recv请求 ====================
bool IOCPThreadPool::PostRecv(ProxyConnectionInfo* conn, bool isClient) {
    if (!conn || !conn->isActive) return false;

    IOContext* ctx = isClient ? conn->clientRecvCtx.get() : conn->serverRecvCtx.get();
    SOCKET sock = isClient ? conn->clientSocket : conn->serverSocket;

    if (!ctx || sock == INVALID_SOCKET) return false;

    ctx->Reset();
    ctx->opType = isClient ? IOOperationType::IO_RECV_CLIENT : IOOperationType::IO_RECV_SERVER;
    ctx->socket = sock;
    ctx->connectionId = conn->id;
    ctx->wsaBuf.buf = ctx->buffer;
    ctx->wsaBuf.len = sizeof(ctx->buffer);
    ctx->flags = 0;

    AB_LOG_INFO("[调试] PostRecv: connId=" + std::to_string(conn->id) +
        ", isClient=" + std::to_string(isClient) +
        ", state=" + std::to_string(static_cast<int>(conn->state)));

    DWORD bytesRecv = 0;
    int result = WSARecv(sock, &ctx->wsaBuf, 1, &bytesRecv, &ctx->flags, ctx, NULL);

    if (result == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
        int err = WSAGetLastError();
        if (err != WSAECONNRESET && err != WSAECONNABORTED) {
            AB_LOG_ERROR("[IOCP] WSARecv failed: " + std::to_string(err));
        }
        AB_LOG_WARNING("[调试] PostRecv 失败: error=" + std::to_string(err));
        return false;
    }

    AB_LOG_INFO("[调试] PostRecv 成功投递: result=" + std::to_string(result) +
        ", bytesRecv=" + std::to_string(bytesRecv));
    return true;
}

// ==================== 投递Send请求 ====================
bool IOCPThreadPool::PostSend(ProxyConnectionInfo* conn, bool isClient, const std::vector<uint8_t>& data) {
    AB_LOG_INFO("[调试] PostSend 被调用: isClient=" + std::to_string(isClient) +
        ", dataSize=" + std::to_string(data.size()) +
        ", connId=" + std::to_string(conn ? conn->id : 0));

    if (!conn || !conn->isActive || data.empty()) {
        AB_LOG_WARNING("[调试] PostSend 参数检查失败: conn=" + std::string(conn ? "有效" : "空") +
            ", isActive=" + (conn ? std::to_string(conn->isActive) : std::string("N/A")) +
            ", dataEmpty=" + std::to_string(data.empty()));
        return false;
    }

    IOContext* ctx = isClient ? conn->clientSendCtx.get() : conn->serverSendCtx.get();
    SOCKET sock = isClient ? conn->clientSocket : conn->serverSocket;

    if (!ctx || sock == INVALID_SOCKET) return false;

    // 检查是否正在发送
    {
        std::lock_guard<std::mutex> lock(conn->sendMutex);
        bool& sending = isClient ? conn->clientSending : conn->serverSending;
        auto& queue = isClient ? conn->clientSendQueue : conn->serverSendQueue;

        AB_LOG_INFO("[调试] PostSend 检查发送状态: sending=" + std::to_string(sending) +
            ", queueSize=" + std::to_string(queue.size()));

        if (sending) {
            // 🔥 限制发送队列大小，防止内存无限增长
            const size_t MAX_SEND_QUEUE_SIZE = 100;
            if (queue.size() >= MAX_SEND_QUEUE_SIZE) {
                AB_LOG_WARNING("[IOCP] Send queue full, closing connection. connId=" +
                    std::to_string(conn->id) + ", queueSize=" + std::to_string(queue.size()));
                conn->isActive = false;
                return false;
            }

            // 加入发送队列
            queue.push(data);
            AB_LOG_INFO("[调试] PostSend 数据已加入队列: dataSize=" + std::to_string(data.size()) +
                ", queueSize=" + std::to_string(queue.size()) + ", connId=" + std::to_string(conn->id));
            return true;
        }
        sending = true;
        AB_LOG_INFO("[调试] PostSend 设置 sending=true");
    }

    ctx->Reset();
    ctx->opType = isClient ? IOOperationType::IO_SEND_CLIENT : IOOperationType::IO_SEND_SERVER;
    ctx->socket = sock;
    ctx->connectionId = conn->id;

    // 复制数据到缓冲区
    size_t copyLen = (std::min)(data.size(), sizeof(ctx->buffer));
    memcpy(ctx->buffer, data.data(), copyLen);
    ctx->wsaBuf.buf = ctx->buffer;
    ctx->wsaBuf.len = static_cast<ULONG>(copyLen);

    // 🔥 如果数据大于缓冲区，将剩余数据放入发送队列
    if (data.size() > copyLen) {
        std::lock_guard<std::mutex> lock(conn->sendMutex);
        auto& queue = isClient ? conn->clientSendQueue : conn->serverSendQueue;

        // 检查队列大小限制
        const size_t MAX_SEND_QUEUE_SIZE = 100;
        if (queue.size() >= MAX_SEND_QUEUE_SIZE) {
            AB_LOG_WARNING("[IOCP] Send queue full when splitting large data, closing connection. connId=" +
                std::to_string(conn->id) + ", queueSize=" + std::to_string(queue.size()));

            bool& sending = isClient ? conn->clientSending : conn->serverSending;
            sending = false;
            conn->isActive = false;
            return false;
        }

        // 将剩余数据放入队列
        std::vector<uint8_t> remainingData(data.begin() + copyLen, data.end());
        queue.push(std::move(remainingData));

        AB_LOG_INFO("[调试] PostSend 数据过大，已分割: 发送=" + std::to_string(copyLen) +
            "B, 队列=" + std::to_string(data.size() - copyLen) + "B, queueSize=" + std::to_string(queue.size()));
    }

    DWORD bytesSent = 0;
    int result = WSASend(sock, &ctx->wsaBuf, 1, &bytesSent, 0, ctx, NULL);

    if (result == SOCKET_ERROR) {
        int err = WSAGetLastError();

        // WSA_IO_PENDING 是正常的异步操作，不是错误
        if (err != WSA_IO_PENDING) {
            AB_LOG_ERROR("[IOCP] WSASend failed: error=" + std::to_string(err) +
                ", connId=" + std::to_string(conn->id));

            std::lock_guard<std::mutex> lock(conn->sendMutex);
            bool& sending = isClient ? conn->clientSending : conn->serverSending;
            sending = false;

            // 🔥 发送失败，关闭连接
            conn->isActive = false;
            return false;
        }
        // WSA_IO_PENDING: 异步操作已投递，等待 OnSendComplete 回调
        AB_LOG_INFO("[调试] PostSend WSASend 异步投递: copyLen=" + std::to_string(copyLen));
    }
    else {
        // result == 0: 立即完成，不会触发 OnSendComplete
        // 需要手动处理发送完成逻辑
        AB_LOG_INFO("[调试] PostSend WSASend 立即完成: bytesSent=" + std::to_string(bytesSent) +
            ", copyLen=" + std::to_string(copyLen));

        // 更新统计
        if (isClient) {
            conn->bytesToClient += bytesSent;
        }
        else {
            conn->bytesToServer += bytesSent;
        }

        // 检查发送队列，继续发送下一个数据
        std::vector<uint8_t> nextData;
        {
            std::lock_guard<std::mutex> lock(conn->sendMutex);
            bool& sending = isClient ? conn->clientSending : conn->serverSending;
            auto& queue = isClient ? conn->clientSendQueue : conn->serverSendQueue;

            AB_LOG_INFO("[调试] PostSend 立即完成后检查队列: queueSize=" + std::to_string(queue.size()));

            if (!queue.empty()) {
                nextData = std::move(queue.front());
                queue.pop();
                AB_LOG_INFO("[调试] PostSend 立即完成后从队列取出数据: dataSize=" + std::to_string(nextData.size()) +
                    ", 剩余queueSize=" + std::to_string(queue.size()));
                // 保持 sending=true，继续发送队列中的数据
            }
            else {
                sending = false;
                AB_LOG_INFO("[调试] PostSend 立即完成后设置 sending=false");
            }
        }

        // 递归发送队列中的下一个数据
        // 注意：此时 sending 仍然是 true，但我们直接递归调用，会重新检查并发送
        if (!nextData.empty()) {
            // 直接递归调用 PostSend，此时 sending=true，会将数据加入队列
            // 但我们需要直接发送，所以需要特殊处理
            // 实际上，我们应该直接发送而不是调用 PostSend

            // 直接发送，绕过 PostSend 的队列检查
            ctx->Reset();
            ctx->opType = isClient ? IOOperationType::IO_SEND_CLIENT : IOOperationType::IO_SEND_SERVER;
            ctx->socket = sock;
            ctx->connectionId = conn->id;

            size_t copyLen = (std::min)(nextData.size(), sizeof(ctx->buffer));
            memcpy(ctx->buffer, nextData.data(), copyLen);
            ctx->wsaBuf.buf = ctx->buffer;
            ctx->wsaBuf.len = static_cast<ULONG>(copyLen);

            // 如果数据大于缓冲区，将剩余数据放回队列
            if (nextData.size() > copyLen) {
                std::lock_guard<std::mutex> lock(conn->sendMutex);
                auto& queue = isClient ? conn->clientSendQueue : conn->serverSendQueue;
                std::vector<uint8_t> remainingData(nextData.begin() + copyLen, nextData.end());
                queue.push(std::move(remainingData));
                AB_LOG_INFO("[调试] PostSend 立即完成后数据过大，剩余部分放回队列: " + std::to_string(nextData.size() - copyLen) + "B");
            }

            DWORD bytesSent2 = 0;
            int result2 = WSASend(sock, &ctx->wsaBuf, 1, &bytesSent2, 0, ctx, NULL);

            if (result2 == SOCKET_ERROR) {
                int err = WSAGetLastError();
                if (err != WSA_IO_PENDING) {
                    AB_LOG_ERROR("[IOCP] PostSend 立即完成后 WSASend failed: error=" + std::to_string(err));
                    std::lock_guard<std::mutex> lock(conn->sendMutex);
                    bool& sending = isClient ? conn->clientSending : conn->serverSending;
                    sending = false;
                    conn->isActive = false;
                }
                // WSA_IO_PENDING: 异步操作，等待回调
            }
            else {
                // 立即完成，需要递归处理（但要避免无限递归）
                // 这里不再递归，让 OnSendComplete 处理
            }
        }
    }

    return true;
}

// ==================== 投递Connect请求 ====================
bool IOCPThreadPool::PostConnect(ProxyConnectionInfo* conn, const sockaddr_in& addr) {
    if (!conn || !conn->isActive) return false;

    // 创建目标服务器socket
    conn->serverSocket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (conn->serverSocket == INVALID_SOCKET) {
        AB_LOG_ERROR("[IOCP] Failed to create server socket: " + std::to_string(WSAGetLastError()));
        return false;
    }

    // 绑定本地地址（ConnectEx要求）
    sockaddr_in localAddr = {};
    localAddr.sin_family = AF_INET;
    localAddr.sin_addr.s_addr = INADDR_ANY;
    localAddr.sin_port = 0;
    if (bind(conn->serverSocket, (sockaddr*)&localAddr, sizeof(localAddr)) == SOCKET_ERROR) {
        AB_LOG_ERROR("[IOCP] Failed to bind local addr: " + std::to_string(WSAGetLastError()));
        closesocket(conn->serverSocket);
        conn->serverSocket = INVALID_SOCKET;
        return false;
    }

    // 关联到IOCP
    if (!AssociateWithIOCP(conn->serverSocket, conn->id)) {
        closesocket(conn->serverSocket);
        conn->serverSocket = INVALID_SOCKET;
        return false;
    }

    // 🔥 设置 TCP Keep-Alive，防止长连接被防火墙/NAT断开
    SetSocketKeepAlive(conn->serverSocket);

    // 创建服务器IO上下文
    conn->serverRecvCtx = std::make_unique<IOContext>();
    conn->serverSendCtx = std::make_unique<IOContext>();
    conn->serverRecvCtx->connectionId = conn->id;
    conn->serverSendCtx->connectionId = conn->id;

    // 设置连接上下文
    IOContext* ctx = conn->serverRecvCtx.get();
    ctx->Reset();
    ctx->opType = IOOperationType::IO_CONNECT;
    ctx->socket = conn->serverSocket;
    ctx->connectionId = conn->id;

    DWORD bytesSent = 0;
    BOOL result = lpfnConnectEx(
        conn->serverSocket,
        (sockaddr*)&addr,
        sizeof(addr),
        NULL,
        0,
        &bytesSent,
        ctx
    );

    if (!result && WSAGetLastError() != ERROR_IO_PENDING) {
        AB_LOG_ERROR("[IOCP] ConnectEx failed: " + std::to_string(WSAGetLastError()));
        closesocket(conn->serverSocket);
        conn->serverSocket = INVALID_SOCKET;
        return false;
    }

    conn->state = ConnectionState::STATE_CONNECTING;
    return true;
}

// ==================== 工作线程 ====================
void IOCPThreadPool::WorkerThread() {
    AB_LOG_INFO("[IOCP] Worker thread started");

    while (isRunning.load()) {
        DWORD bytesTransferred = 0;
        ULONG_PTR completionKey = 0;
        OVERLAPPED* overlapped = NULL;

        BOOL result = GetQueuedCompletionStatus(
            iocpHandle,
            &bytesTransferred,
            &completionKey,
            &overlapped,
            INFINITE
        );

        // 检查退出信号
        if (!result && overlapped == NULL) {
            DWORD err = GetLastError();
            if (err == WAIT_TIMEOUT) {
                continue;
            }
            break;
        }

        if (overlapped == NULL) {
            // 退出信号
            break;
        }

        IOContext* ctx = static_cast<IOContext*>(overlapped);
        ctx->bytesTransferred = bytesTransferred;

        // 检查操作是否成功
        if (!result) {
            DWORD err = GetLastError();
            if (err != ERROR_NETNAME_DELETED && err != ERROR_CONNECTION_ABORTED) {
                AB_LOG_WARNING("[IOCP] IO operation failed: " + std::to_string(err));
            }

            // 关闭连接
            if (ctx->connectionId > 0) {
                CloseConnection(ctx->connectionId, "IO error");
            }
            continue;
        }

        // 根据操作类型处理
        switch (ctx->opType) {
        case IOOperationType::IO_ACCEPT:
            OnAcceptComplete(ctx, bytesTransferred);
            break;

        case IOOperationType::IO_RECV_CLIENT:
        case IOOperationType::IO_RECV_SERVER:
            OnRecvComplete(ctx, bytesTransferred);
            break;

        case IOOperationType::IO_SEND_CLIENT:
        case IOOperationType::IO_SEND_SERVER:
            OnSendComplete(ctx, bytesTransferred);
            break;

        case IOOperationType::IO_CONNECT:
            OnConnectComplete(ctx, bytesTransferred);
            break;

        default:
            AB_LOG_WARNING("[IOCP] Unknown operation type: " + std::to_string(static_cast<int>(ctx->opType)));
            break;
        }
    }

    AB_LOG_INFO("[IOCP] Worker thread exited");
}

// ==================== Accept完成处理 ====================
void IOCPThreadPool::OnAcceptComplete(IOContext* ctx, DWORD bytesTransferred) {
    SOCKET acceptSocket = ctx->socket;

    // 获取客户端地址
    sockaddr_in* localAddr = NULL;
    sockaddr_in* remoteAddr = NULL;
    int localAddrLen = sizeof(sockaddr_in);
    int remoteAddrLen = sizeof(sockaddr_in);

    lpfnGetAcceptExSockaddrs(
        ctx->buffer,
        0,
        sizeof(sockaddr_in) + 16,
        sizeof(sockaddr_in) + 16,
        (sockaddr**)&localAddr,
        &localAddrLen,
        (sockaddr**)&remoteAddr,
        &remoteAddrLen
    );

    // 获取客户端IP
    char clientIP[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &remoteAddr->sin_addr, clientIP, INET_ADDRSTRLEN);
    std::string clientIPStr = clientIP;
    std::string clientAddr = clientIPStr + ":" + std::to_string(ntohs(remoteAddr->sin_port));

    // 设置socket选项（继承监听socket的选项）
    setsockopt(acceptSocket, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
        (char*)&listenSocket, sizeof(listenSocket));

    // 检查是否白名单
    bool isWhitelisted = false;
    if (whitelistChecker) {
        isWhitelisted = whitelistChecker(clientIPStr);
    }

    // 🔥🔥🔥 防CC检查（与传统模式一致）
    if (antiCCChecker) {
        if (!antiCCChecker(clientIPStr)) {
            AB_LOG_WARNING("[IOCP] 防CC拒绝连接: " + clientIPStr);
            closesocket(acceptSocket);
            // 从Accept池中移除并投递新请求
            {
                std::lock_guard<std::mutex> lock(acceptPoolMutex);
                auto socketIt = std::find(acceptSocketPool.begin(), acceptSocketPool.end(), acceptSocket);
                if (socketIt != acceptSocketPool.end()) {
                    size_t index = socketIt - acceptSocketPool.begin();
                    acceptSocketPool.erase(socketIt);
                    if (index < acceptContextPool.size()) {
                        acceptContextPool.erase(acceptContextPool.begin() + index);
                    }
                }
            }
            PostAccept();
            return;
        }
    }

    // 检查连接数限制
    int currentCount = isWhitelisted ? whitelistConnCount.load() : normalConnCount.load();
    int maxCount = isWhitelisted ? maxWhitelistConnections : maxNormalConnections;

    if (currentCount >= maxCount) {
        // 达到限制，加入等待队列或拒绝
        std::lock_guard<std::mutex> lock(queueMutex);
        auto& queue = isWhitelisted ? whitelistWaitQueue : normalWaitQueue;
        int queueLimit = isWhitelisted ? whitelistQueueLimit : normalQueueLimit;

        if (static_cast<int>(queue.size()) < queueLimit) {
            queue.push(acceptSocket);
            AB_LOG_INFO("[IOCP] Connection queued: " + clientIPStr +
                (isWhitelisted ? " (whitelist)" : " (normal)"));
        }
        else {
            // 队列满，拒绝连接
            closesocket(acceptSocket);
            AB_LOG_WARNING("[IOCP] Connection rejected (queue full): " + clientIPStr);
        }
    }
    else {
        // 创建新连接
        ProxyConnectionInfo* conn = CreateConnection(acceptSocket, clientAddr, clientIPStr);
        if (conn) {
            conn->isWhitelisted = isWhitelisted;
            if (isWhitelisted) {
                whitelistConnCount++;
            }
            else {
                normalConnCount++;
            }
            totalConnCount++;

            // 开始SOCKS5握手
            conn->state = ConnectionState::STATE_SOCKS5_INIT;
            PostRecv(conn, true);

            AB_LOG_INFO("[IOCP] New connection: " + clientAddr +
                (isWhitelisted ? " [whitelist]" : " [normal]") +
                " (total: " + std::to_string(totalConnCount.load()) + ")");
        }
        else {
            closesocket(acceptSocket);
        }
    }

    // 从Accept池中移除
    {
        std::lock_guard<std::mutex> lock(acceptPoolMutex);
        auto socketIt = std::find(acceptSocketPool.begin(), acceptSocketPool.end(), acceptSocket);
        if (socketIt != acceptSocketPool.end()) {
            size_t index = socketIt - acceptSocketPool.begin();
            acceptSocketPool.erase(socketIt);
            if (index < acceptContextPool.size()) {
                acceptContextPool.erase(acceptContextPool.begin() + index);
            }
        }
    }

    // 投递新的Accept请求
    PostAccept();
}

// ==================== Recv完成处理 ====================
void IOCPThreadPool::OnRecvComplete(IOContext* ctx, DWORD bytesTransferred) {
    AB_LOG_INFO("[调试] OnRecvComplete: connId=" + std::to_string(ctx->connectionId) +
        ", bytesTransferred=" + std::to_string(bytesTransferred) +
        ", opType=" + std::to_string(static_cast<int>(ctx->opType)));

    if (bytesTransferred == 0) {
        // 连接关闭
        AB_LOG_INFO("[调试] OnRecvComplete: 连接关闭 (bytesTransferred=0)");
        CloseConnection(ctx->connectionId, "Peer closed");
        return;
    }

    // 查找连接并检查是否仍然活跃
    std::shared_ptr<ProxyConnectionInfo> conn;
    {
        std::lock_guard<std::mutex> lock(connectionsMutex);
        auto it = connections.find(ctx->connectionId);
        if (it == connections.end() || !it->second->isActive) {
            AB_LOG_WARNING("[调试] OnRecvComplete: 连接不存在或已关闭");
            return;
        }
        conn = it->second;
    }

    AB_LOG_INFO("[调试] OnRecvComplete: state=" + std::to_string(static_cast<int>(conn->state)));

    bool isFromClient = (ctx->opType == IOOperationType::IO_RECV_CLIENT);

    // 根据连接状态处理
    switch (conn->state) {
    case ConnectionState::STATE_SOCKS5_INIT:
    case ConnectionState::STATE_SOCKS5_AUTH:
    case ConnectionState::STATE_SOCKS5_REQUEST:
        // SOCKS5握手阶段
        AB_LOG_INFO("[调试] OnRecvComplete: 调用 ProcessSocks5Handshake");
            ProcessSocks5Handshake(conn.get(), (uint8_t*)ctx->buffer, bytesTransferred);
            break;

    case ConnectionState::STATE_FORWARDING:
        // 数据转发阶段
        if (isFromClient) {
            ProcessClientData(conn.get(), (uint8_t*)ctx->buffer, bytesTransferred);
        }
        else {
            ProcessServerData(conn.get(), (uint8_t*)ctx->buffer, bytesTransferred);
        }
        break;

    default:
        break;
    }
}

// ==================== Send完成处理 ====================
void IOCPThreadPool::OnSendComplete(IOContext* ctx, DWORD bytesTransferred) {
    AB_LOG_INFO("[调试] OnSendComplete: bytesTransferred=" + std::to_string(bytesTransferred) +
        ", connId=" + std::to_string(ctx->connectionId) +
        ", opType=" + std::to_string(static_cast<int>(ctx->opType)));

    // 查找连接并检查是否仍然活跃
    std::shared_ptr<ProxyConnectionInfo> conn;
    {
        std::lock_guard<std::mutex> lock(connectionsMutex);
        auto it = connections.find(ctx->connectionId);
        if (it == connections.end() || !it->second->isActive) {
            AB_LOG_WARNING("[调试] OnSendComplete: 连接不存在或已关闭");
            return;
        }
        conn = it->second;
    }

    bool isClient = (ctx->opType == IOOperationType::IO_SEND_CLIENT);

    // 🔥 检查发送是否成功
    if (bytesTransferred == 0) {
        AB_LOG_WARNING("[IOCP] Send failed (0 bytes), closing connection. connId=" +
            std::to_string(ctx->connectionId));

        std::lock_guard<std::mutex> lock(conn->sendMutex);
        bool& sending = isClient ? conn->clientSending : conn->serverSending;
        sending = false;
        conn->isActive = false;
        return;
    }

    // 更新统计
    if (isClient) {
        conn->bytesToClient += bytesTransferred;
    }
    else {
        conn->bytesToServer += bytesTransferred;
    }

    // 检查发送队列
    std::vector<uint8_t> nextData;
    {
        std::lock_guard<std::mutex> lock(conn->sendMutex);
        bool& sending = isClient ? conn->clientSending : conn->serverSending;
        auto& queue = isClient ? conn->clientSendQueue : conn->serverSendQueue;

        AB_LOG_INFO("[调试] OnSendComplete 检查队列: queueSize=" + std::to_string(queue.size()));

        if (!queue.empty()) {
            nextData = std::move(queue.front());
            queue.pop();
            AB_LOG_INFO("[调试] OnSendComplete 从队列取出数据: dataSize=" + std::to_string(nextData.size()) +
                ", 剩余queueSize=" + std::to_string(queue.size()));
            // 保持 sending=true，继续发送队列中的数据
        }
        else {
            sending = false;
            AB_LOG_INFO("[调试] OnSendComplete 设置 sending=false");
        }
    }

    // 发送���列中的下一个数据
    // 注意：此时 sending 仍然是 true，但我们直接发送，不通过 PostSend 的队列逻辑
    if (!nextData.empty() && conn->isActive) {
        // 直接发送，绕过 PostSend 的队列检查
        IOContext* ctx = isClient ? conn->clientSendCtx.get() : conn->serverSendCtx.get();
        SOCKET sock = isClient ? conn->clientSocket : conn->serverSocket;

        if (ctx && sock != INVALID_SOCKET && conn->isActive) {
            ctx->Reset();
            ctx->opType = isClient ? IOOperationType::IO_SEND_CLIENT : IOOperationType::IO_SEND_SERVER;
            ctx->socket = sock;
            ctx->connectionId = conn->id;

            size_t copyLen = (std::min)(nextData.size(), sizeof(ctx->buffer));
            memcpy(ctx->buffer, nextData.data(), copyLen);
            ctx->wsaBuf.buf = ctx->buffer;
            ctx->wsaBuf.len = static_cast<ULONG>(copyLen);

            // 如果数据大于缓冲区，将剩余数据放回队列
            if (nextData.size() > copyLen) {
                std::lock_guard<std::mutex> lock(conn->sendMutex);
                auto& queue = isClient ? conn->clientSendQueue : conn->serverSendQueue;
                std::vector<uint8_t> remainingData(nextData.begin() + copyLen, nextData.end());
                queue.push(std::move(remainingData));
                AB_LOG_INFO("[调试] OnSendComplete 数据过大，剩余部分放回队列: " + std::to_string(nextData.size() - copyLen) + "B");
            }

            DWORD bytesSent = 0;
            int result = WSASend(sock, &ctx->wsaBuf, 1, &bytesSent, 0, ctx, NULL);

            if (result == SOCKET_ERROR) {
                int err = WSAGetLastError();
                if (err != WSA_IO_PENDING) {
                    AB_LOG_ERROR("[IOCP] OnSendComplete WSASend failed: error=" + std::to_string(err));
                    std::lock_guard<std::mutex> lock(conn->sendMutex);
                    bool& sending = isClient ? conn->clientSending : conn->serverSending;
                    sending = false;
                    conn->isActive = false;
                }
            }
        }
    }
}

// ==================== Connect完成处理 ====================
void IOCPThreadPool::OnConnectComplete(IOContext* ctx, DWORD bytesTransferred) {
    // 查找连接并检查是否仍然活跃
    std::shared_ptr<ProxyConnectionInfo> conn;
    {
        std::lock_guard<std::mutex> lock(connectionsMutex);
        auto it = connections.find(ctx->connectionId);
        if (it == connections.end() || !it->second->isActive) {
            return;
        }
        conn = it->second;
    }

    // 更新socket选项
    setsockopt(conn->serverSocket, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0);

    AB_LOG_INFO("[IOCP] Connected to target: " + conn->targetHost + ":" + std::to_string(conn->targetPort) +
        " [" + conn->clientAddr + "]");

    // 发送SOCKS5连接成功响应
    char reply[10] = { 5, 0, 0, 1, 0, 0, 0, 0, 0, 0 };
    std::vector<uint8_t> replyData(reply, reply + 10);
    PostSend(conn.get(), true, replyData);

    // 开始数据转发
    if (conn->isActive) {
        StartForwarding(conn.get());
    }
}

// ==================== 创建连接 ====================
ProxyConnectionInfo* IOCPThreadPool::CreateConnection(SOCKET clientSocket, const std::string& clientAddr, const std::string& clientIP) {
    auto conn = std::make_shared<ProxyConnectionInfo>();
    conn->id = nextConnectionId++;
    conn->clientSocket = clientSocket;
    conn->clientAddr = clientAddr;
    conn->clientIP = clientIP;
    conn->isActive = true;

    // 关联到IOCP
    if (!AssociateWithIOCP(clientSocket, conn->id)) {
        return nullptr;
    }

    // 🔥 设置 TCP Keep-Alive，防止长连接被防火墙/NAT断开
    SetSocketKeepAlive(clientSocket);

    // 创建IO上下文
    conn->clientRecvCtx = std::make_unique<IOContext>();
    conn->clientSendCtx = std::make_unique<IOContext>();
    conn->clientRecvCtx->connectionId = conn->id;
    conn->clientSendCtx->connectionId = conn->id;

    uint64_t connId = conn->id;
    ProxyConnectionInfo* connPtr = conn.get();
    size_t mapSizeAfterInsert = 0;

    {
        std::lock_guard<std::mutex> lock(connectionsMutex);
        connections[connId] = conn;
        mapSizeAfterInsert = connections.size();
    }

    uint64_t created = lifecycleCreatedCount.fetch_add(1, std::memory_order_relaxed) + 1;
    uint64_t closed = lifecycleClosedCount.load(std::memory_order_relaxed);

    if (connectionEvent) {
        connectionEvent(connPtr, "created");
    }

    AB_LOG_INFO("[IOCP-Lifecycle] created connId=" + std::to_string(connId) +
        ", mapSize=" + std::to_string(mapSizeAfterInsert) +
        ", useCount=" + std::to_string(conn.use_count()) +
        ", created=" + std::to_string(created) +
        ", closed=" + std::to_string(closed));

    return connPtr;
}

// ==================== 处理SOCKS5握手 ====================
void IOCPThreadPool::ProcessSocks5Handshake(ProxyConnectionInfo* conn, const uint8_t* data, int len) {
    // 检查连接是否仍然有效
    if (!conn || !conn->isActive) {
        return;
    }

    // 将数据追加到握手缓冲区（如果data不为空）
    if (data != nullptr && len > 0) {
        conn->handshakeBuffer.insert(conn->handshakeBuffer.end(), data, data + len);
    }

    switch (conn->state) {
    case ConnectionState::STATE_SOCKS5_INIT: {
        AB_LOG_INFO("[调试] SOCKS5握手 - 阶段1: 初始化");

        // 🔥 打印接收到的原始数据（十六进制）
        std::string hexData;
        for (size_t i = 0; i < conn->handshakeBuffer.size() && i < 32; i++) {
            char buf[4];
            sprintf_s(buf, sizeof(buf), "%02X ", conn->handshakeBuffer[i]);
            hexData += buf;
        }
        AB_LOG_INFO("[调试] 接收到的握手数据 (" + std::to_string(conn->handshakeBuffer.size()) + " 字节): " + hexData);

        // 阶段1：接收客户端支持的认证方法
        if (conn->handshakeBuffer.size() < 2) {
            PostRecv(conn, true);
            return;
        }

        uint8_t version = conn->handshakeBuffer[0];
        uint8_t nmethods = conn->handshakeBuffer[1];

        // 🔥🔥🔥 非SOCKS5连接检测（与传统模式一致）
        if (version != 5) {
            AB_LOG_WARNING("[IOCP] 检测到非SOCKS5连接: " + conn->clientIP + " (版本字节: " + std::to_string(version) + ")");
            // 调用非SOCKS5检测回调进行封禁
            if (nonSocksChecker) {
                nonSocksChecker(conn->clientSocket, conn->clientIP);
            }
            CloseConnection(conn->id, "Non-SOCKS5 connection");
            return;
        }

        if (conn->handshakeBuffer.size() < 2 + nmethods) {
            AB_LOG_INFO("[调试] 数据不完整，等待更多数据: 当前=" + std::to_string(conn->handshakeBuffer.size()) +
                ", 需要=" + std::to_string(2 + nmethods));
            PostRecv(conn, true);
            return;
        }

        // 🔥 打印客户端支持的所有认证方法
        std::string methodsStr;
        for (int i = 0; i < nmethods; i++) {
            methodsStr += std::to_string(conn->handshakeBuffer[2 + i]) + " ";
        }
        AB_LOG_INFO("[调试] 客户端支持的认证方法: " + methodsStr);

        // 检查是否需要认证（与传统模式一致：同时检查enableSocks5Auth和authCallback）
        bool needAuth = (enableSocks5Auth && authCallback != nullptr);
        uint8_t selectedMethod = needAuth ? 0x02 : 0x00;
        AB_LOG_INFO("[调试] SOCKS5方法选择: enableSocks5Auth=" + std::to_string(enableSocks5Auth) +
            ", authCallback=" + (authCallback ? "已设置" : "未设置") +
            ", needAuth=" + std::to_string(needAuth) +
            ", selectedMethod=" + std::to_string(selectedMethod));

        if (needAuth) {
            bool supportsAuth = false;
            auto globalPressure = instanceId.empty()
                ? AntiCCPressureState::Normal
                : GlobalAntiCCCoordinator::GetInstance().GetGlobalPressure();
            auto instancePressure = instanceId.empty()
                ? AntiCCPressureState::Normal
                : GlobalAntiCCCoordinator::GetInstance().GetInstancePressure(instanceId);
            const bool highPressure =
                (globalPressure >= AntiCCPressureState::Busy ||
                 instancePressure >= AntiCCPressureState::Busy);
            for (int i = 0; i < nmethods; i++) {
                if (conn->handshakeBuffer[2 + i] == 0x02) {
                    supportsAuth = true;
                    break;
                }
            }
            if (supportsAuth && !instanceId.empty()) {
                GlobalAntiCCCoordinator::GetInstance().RecordAuthHint(instanceId, conn->clientIP);
            }
            if (supportsAuth && highPressure && antiCCAuthPriorityAdmissionEnabled.load()) {
                const int currentInFlight = antiCCAuthPriorityInFlight.fetch_add(1) + 1;
                if (currentInFlight > antiCCAuthPriorityQueueLimit.load()) {
                    antiCCAuthPriorityInFlight.fetch_sub(1);
                    AB_LOG_WARNING_CAT(LOG_CAT_ANTICC, "[AntiCC] IOCP认证优先名额已满，拒绝连接: " + conn->clientIP +
                        " instance=" + instanceId);
                    char response[2] = { 5, (char)0xFF };
                    PostSend(conn, true, std::vector<uint8_t>(response, response + 2));
                    CloseConnection(conn->id, "Auth priority slots full");
                    return;
                }
                conn->authPrioritySlotHeld = true;
            }
            if (!supportsAuth) {
                char response[2] = { 5, (char)0xFF };
                PostSend(conn, true, std::vector<uint8_t>(response, response + 2));
                CloseConnection(conn->id, "Client does not support auth");
                return;
            }
        }

        // 发送方法选择响应
        char response[2] = { 5, (char)selectedMethod };
        AB_LOG_INFO("[调试] 发送方法选择响应: version=5, method=" + std::to_string(selectedMethod));
        bool sendSuccess = PostSend(conn, true, std::vector<uint8_t>(response, response + 2));
        AB_LOG_INFO("[调试] PostSend 返回: " + std::string(sendSuccess ? "成功" : "失败"));

        // 🔥 移除已处理的握手数据（前2+nmethods字节），保留可能的流水线数据
        size_t processedBytes = 2 + nmethods;
        if (conn->handshakeBuffer.size() > processedBytes) {
            AB_LOG_INFO("[调试] 检测到流水线数据，保留 " +
                std::to_string(conn->handshakeBuffer.size() - processedBytes) + " 字节");
            conn->handshakeBuffer.erase(conn->handshakeBuffer.begin(),
                conn->handshakeBuffer.begin() + processedBytes);
        } else {
            conn->handshakeBuffer.clear();
        }

        conn->state = needAuth ? ConnectionState::STATE_SOCKS5_AUTH : ConnectionState::STATE_SOCKS5_REQUEST;

        // 🔥 如果缓冲区还有数据，立即处理；否则投递接收
        if (!conn->handshakeBuffer.empty()) {
            AB_LOG_INFO("[调试] 立即处理缓冲区中的剩余数据");
            ProcessSocks5Handshake(conn, nullptr, 0);  // 递归处理
        } else {
            PostRecv(conn, true);
        }
        break;
    }

    case ConnectionState::STATE_SOCKS5_AUTH: {
        AB_LOG_INFO("[调试] SOCKS5握手 - 阶段2: 认证");
        // 阶段2：认证
        if (conn->handshakeBuffer.size() < 2) {
            PostRecv(conn, true);
            return;
        }

        uint8_t authVersion = conn->handshakeBuffer[0];
        uint8_t usernameLen = conn->handshakeBuffer[1];

        if (conn->handshakeBuffer.size() < 2 + usernameLen + 1) {
            PostRecv(conn, true);
            return;
        }

        uint8_t passwordLen = conn->handshakeBuffer[2 + usernameLen];
        if (conn->handshakeBuffer.size() < 2 + usernameLen + 1 + passwordLen) {
            PostRecv(conn, true);
            return;
        }

        std::string username(conn->handshakeBuffer.begin() + 2,
            conn->handshakeBuffer.begin() + 2 + usernameLen);
        std::string password(conn->handshakeBuffer.begin() + 2 + usernameLen + 1,
            conn->handshakeBuffer.begin() + 2 + usernameLen + 1 + passwordLen);

        AB_LOG_INFO("[调试] 认证数据: authVersion=" + std::to_string(authVersion) +
            ", usernameLen=" + std::to_string(usernameLen) +
            ", username=" + username +
            ", passwordLen=" + std::to_string(passwordLen) +
            ", password=" + password);

        bool authSuccess = false;
        if (authCallback) {
            AB_LOG_INFO("[调试] 调用 authCallback 进行认证");
            authSuccess = authCallback(username, password, conn->clientIP, &conn->accountStateOwner);
            AB_LOG_INFO("[调试] authCallback 返回: " + std::string(authSuccess ? "成功" : "失败"));
        } else {
            AB_LOG_WARNING("[调试] authCallback 为空，认证失败");
        }

        char response[2] = { 1, authSuccess ? (char)0 : (char)1 };
        AB_LOG_INFO("[调试] 发送认证响应: version=" + std::to_string((int)response[0]) +
            ", status=" + std::to_string((int)response[1]) +
            " (" + (authSuccess ? "成功" : "失败") + ")");
        PostSend(conn, true, std::vector<uint8_t>(response, response + 2));

        if (!authSuccess) {
            if (conn->authPrioritySlotHeld) {
                antiCCAuthPriorityInFlight.fetch_sub(1);
                conn->authPrioritySlotHeld = false;
            }
            AB_LOG_WARNING("[调试] 认证失败，准备关闭连接");
            // 🔥 调用认证失败回调（与传统模式一致）
            if (authFailure) {
                authFailure(conn->clientIP);
            }
            CloseConnection(conn->id, "Auth failed");
            return;
        }
        AB_LOG_INFO("[调试] SOCKS5认证成功: 用户=" + username);
        if (conn->authPrioritySlotHeld) {
            antiCCAuthPriorityInFlight.fetch_sub(1);
            conn->authPrioritySlotHeld = false;
        }

        conn->authenticatedUser = username;

        // 🔥 移除已处理的认证数据，保留可能的流水线数据
        size_t processedBytes = 2 + usernameLen + 1 + passwordLen;
        if (conn->handshakeBuffer.size() > processedBytes) {
            AB_LOG_INFO("[调试] 认证后检测到流水线数据，保留 " +
                std::to_string(conn->handshakeBuffer.size() - processedBytes) + " 字节");
            conn->handshakeBuffer.erase(conn->handshakeBuffer.begin(),
                conn->handshakeBuffer.begin() + processedBytes);
        } else {
            conn->handshakeBuffer.clear();
        }

        conn->state = ConnectionState::STATE_SOCKS5_REQUEST;

        // 🔥 如果缓冲区还有数据，立即处理；否则投递接收
        if (!conn->handshakeBuffer.empty()) {
            AB_LOG_INFO("[调试] 立即处理认证后的剩余数据");
            ProcessSocks5Handshake(conn, nullptr, 0);  // 递归处理
        } else {
            PostRecv(conn, true);
        }
        break;
    }

    case ConnectionState::STATE_SOCKS5_REQUEST: {
        AB_LOG_INFO("[调试] SOCKS5握手 - 阶段3: 连接请求");
        // 阶段3：连接请求
        if (conn->handshakeBuffer.size() < 4) {
            PostRecv(conn, true);
            return;
        }

        uint8_t ver = conn->handshakeBuffer[0];
        uint8_t cmd = conn->handshakeBuffer[1];
        uint8_t atyp = conn->handshakeBuffer[3];

        if (ver != 5 || cmd != 1) {
            AB_LOG_WARNING("[IOCP] Unsupported SOCKS command");
            CloseConnection(conn->id, "Unsupported command");
            return;
        }

        size_t requiredLen = 4;
        if (atyp == 1) {
            requiredLen += 4 + 2;  // IPv4 + port
        }
        else if (atyp == 3) {
            if (conn->handshakeBuffer.size() < 5) {
                PostRecv(conn, true);
                return;
            }
            uint8_t domainLen = conn->handshakeBuffer[4];
            requiredLen += 1 + domainLen + 2;  // len + domain + port
        }
        else if (atyp == 4) {
            requiredLen += 16 + 2;  // IPv6 + port
        }

        if (conn->handshakeBuffer.size() < requiredLen) {
            PostRecv(conn, true);
            return;
        }

        // 解析目标地址
        if (atyp == 1) {
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &conn->handshakeBuffer[4], ip, INET_ADDRSTRLEN);
            conn->targetHost = ip;
            conn->targetPort = (conn->handshakeBuffer[8] << 8) | conn->handshakeBuffer[9];
        }
        else if (atyp == 3) {
            uint8_t domainLen = conn->handshakeBuffer[4];
            conn->targetHost = std::string(conn->handshakeBuffer.begin() + 5,
                conn->handshakeBuffer.begin() + 5 + domainLen);
            conn->targetPort = (conn->handshakeBuffer[5 + domainLen] << 8) |
                conn->handshakeBuffer[5 + domainLen + 1];
        }
        else {
            CloseConnection(conn->id, "IPv6 not supported");
            return;
        }

        AB_LOG_INFO("[调试] SOCKS5连接请求解析完成: 目标=" + conn->targetHost + ":" + std::to_string(conn->targetPort));

        // 🔥 移除已处理的请求数据（不应该有流水线数据了，但为了一致性还是处理）
        if (conn->handshakeBuffer.size() > requiredLen) {
            AB_LOG_WARNING("[调试] 连接请求后检测到额外数据，保留 " +
                std::to_string(conn->handshakeBuffer.size() - requiredLen) + " 字节");
            conn->handshakeBuffer.erase(conn->handshakeBuffer.begin(),
                conn->handshakeBuffer.begin() + requiredLen);
        } else {
            conn->handshakeBuffer.clear();
        }

        // 🔥🔥🔥 关键：设置 bypassModifier（与传统模式一致）
        if (filterChecker) {
            conn->bypassModifier = filterChecker(conn->targetHost, conn->targetPort);
            if (conn->bypassModifier) {
                AB_LOG_INFO("[IOCP] 过滤: " + conn->targetHost + ":" + std::to_string(conn->targetPort) +
                    " 不符合过滤条件，直接转发");
            }
            else {
                AB_LOG_INFO("[IOCP] 处理: " + conn->targetHost + ":" + std::to_string(conn->targetPort) +
                    " 符合过滤条件，将进行数据处理");
            }
        }
        else {
            conn->bypassModifier = false;  // 默认不跳过
        }

        // 🔥🔥🔥 新增：如果 bypassModifier=true 表示被过滤，直接拒绝连接（与传统模式一致）
        if (conn->bypassModifier) {
            AB_LOG_INFO("[IOCP] 流量过滤拒绝连接: " + conn->targetHost + ":" + std::to_string(conn->targetPort));
            char reply[10] = { 5, 2, 0, 1, 0, 0, 0, 0, 0, 0 };  // 2 = Connection not allowed by ruleset
            PostSend(conn, true, std::vector<uint8_t>(reply, reply + 10));
            CloseConnection(conn->id, "Filtered by traffic rules");
            return;
        }

        // 连接目标服务器
        if (!ConnectToTarget(conn)) {
            // 发送连接失败响应
            char reply[10] = { 5, 4, 0, 1, 0, 0, 0, 0, 0, 0 };  // 主机不可达
            PostSend(conn, true, std::vector<uint8_t>(reply, reply + 10));
            CloseConnection(conn->id, "Failed to connect target");
        }
        break;
    }

    default:
        break;
    }
}

// ==================== 连接目标服务器 ====================
bool IOCPThreadPool::ConnectToTarget(ProxyConnectionInfo* conn) {
    // 🔥 调试日志：检查二级代理连接器状态
    AB_LOG_INFO("[调试] ConnectToTarget 被调用: 目标=" + conn->targetHost + ":" + std::to_string(conn->targetPort) +
        ", secondaryProxyConnector=" + (secondaryProxyConnector ? "已设置" : "未设置"));

    // 🔥🔥🔥 二级代理支持（与传统模式一致）
    if (secondaryProxyConnector) {
        // 使用二级代理连接
        AB_LOG_INFO("[IOCP] 通过二级代理连接: " + conn->targetHost + ":" + std::to_string(conn->targetPort));

        SOCKET proxySocket = secondaryProxyConnector(conn->targetHost, conn->targetPort);
        if (proxySocket == INVALID_SOCKET) {
            AB_LOG_ERROR("[IOCP] 二级代理连接失败: " + conn->targetHost + ":" + std::to_string(conn->targetPort));
            return false;
        }

        // 设置为非阻塞模式
        u_long mode = 1;
        ioctlsocket(proxySocket, FIONBIO, &mode);

        // 关联到IOCP
        if (!AssociateWithIOCP(proxySocket, conn->id)) {
            closesocket(proxySocket);
            return false;
        }

        conn->serverSocket = proxySocket;

        // 🔥 设置 TCP Keep-Alive，防止长连接被防火墙/NAT断开
        SetSocketKeepAlive(proxySocket);

        // 创建服务器IO上下文
        conn->serverRecvCtx = std::make_unique<IOContext>();
        conn->serverSendCtx = std::make_unique<IOContext>();
        conn->serverRecvCtx->connectionId = conn->id;
        conn->serverSendCtx->connectionId = conn->id;

        AB_LOG_INFO("[IOCP] 二级代理连接成功: " + conn->targetHost + ":" + std::to_string(conn->targetPort));

        // 发送SOCKS5连接成功响应
        char reply[10] = { 5, 0, 0, 1, 0, 0, 0, 0, 0, 0 };
        std::vector<uint8_t> replyData(reply, reply + 10);
        PostSend(conn, true, replyData);

        // 开始数据转发
        StartForwarding(conn);
        return true;
    }

    // 直连模式（原有逻辑）
    // 解析目标地址
    sockaddr_in targetAddr = {};
    targetAddr.sin_family = AF_INET;
    targetAddr.sin_port = htons(conn->targetPort);

    if (inet_pton(AF_INET, conn->targetHost.c_str(), &targetAddr.sin_addr) != 1) {
        // DNS解析
        struct addrinfo hints = {}, * result = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        if (getaddrinfo(conn->targetHost.c_str(), nullptr, &hints, &result) == 0) {
            targetAddr.sin_addr = ((sockaddr_in*)result->ai_addr)->sin_addr;
            freeaddrinfo(result);
        }
        else {
            AB_LOG_ERROR("[IOCP] DNS resolve failed: " + conn->targetHost);
            return false;
        }
    }

    AB_LOG_INFO("[IOCP] Connecting to target: " + conn->targetHost + ":" + std::to_string(conn->targetPort));
    return PostConnect(conn, targetAddr);
}

// ==================== 开始数据转发 ====================
void IOCPThreadPool::StartForwarding(ProxyConnectionInfo* conn) {
    conn->state = ConnectionState::STATE_FORWARDING;

    // 更新统计
    if (conn->isWhitelisted) {
        whitelistProcessed++;
    }
    else {
        normalProcessed++;
    }

    if (connectionEvent) {
        connectionEvent(conn, "forwarding");
    }

    // 开始双向数据接收
    PostRecv(conn, true);   // 接收客户端数据
    PostRecv(conn, false);  // 接收服务器数据
}

// ==================== 处理客户端数据 ====================
void IOCPThreadPool::ProcessClientData(ProxyConnectionInfo* conn, const uint8_t* data, int len) {
    // 检查连接是否仍然有效
    if (!conn || !conn->isActive) {
        return;
    }

    conn->bytesFromClient += len;

    // 🔥 更新全局统计（与传统模式一致）
    totalBytes += len;

    // ===== SNI嗅探复检（与传统模式一致：每连接仅执行一次）=====
    if (!conn->sniChecked && sniRecheckCallback) {
        conn->sniChecked = true;
        std::vector<uint8_t> sniProbeData(data, data + len);
        if (!sniRecheckCallback(conn, sniProbeData)) {
            AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[IOCP-SNI] 嗅探复检拒绝连接: " +
                conn->targetHost + ":" + std::to_string(conn->targetPort));
            CloseConnection(conn->id, "SNI mismatch");
            return;
        }
    }

    if (rawDataObserver) {
        std::vector<uint8_t> rawChunk(data, data + len);
        if (!rawDataObserver(conn, rawChunk, true)) {
            return;
        }
        if (!conn->isActive) {
            return;
        }
    }

    // ===== 将新数据追加到缓冲区 =====
    conn->clientDataBuffer.insert(conn->clientDataBuffer.end(), data, data + len);

    // ===== 如果是直通模式或不需要分包处理，直接转发 =====
    bool needPacketSplit = IsPacketSplitEnabledForPort(conn->targetPort);

    if (conn->bypassModifier || !needPacketSplit) {
        // 🔥🔥🔥 调试日志
        if (conn->bypassModifier) {
            AB_LOG_WARNING_CAT(LOG_CAT_COLLECTOR, "[IOCP-bypass] 目标=" + conn->targetHost + ":" +
                std::to_string(conn->targetPort) + " 不匹配过滤条件，跳过数据处理(不触发回调) " +
                std::to_string(len) + "B");
        } else {
            AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[IOCP-直通] 端口 " + std::to_string(conn->targetPort) +
                " 不需要分包处理，直接转发 " + std::to_string(len) + "B");
        }

        std::vector<uint8_t> dataToSend = conn->clientDataBuffer;
        conn->clientDataBuffer.clear();

        // 🔥🔥🔥 调试：检查WPE滤镜条件
        AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[IOCP-直通-调试] bypassModifier=" + std::to_string(conn->bypassModifier) +
            ", applyWpeOnNonSplitTraffic=" + std::to_string(applyWpeOnNonSplitTraffic.load()) +
            ", dataModifier=" + (dataModifier ? "已设置" : "未设置"));

        // 🔥 如果启用了"对不分包流量应用WPE滤镜"，则调用 dataModifier
        if (!conn->bypassModifier && applyWpeOnNonSplitTraffic.load() && dataModifier) {
            AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[IOCP-直通-WPE] 端口 " + std::to_string(conn->targetPort) +
                " 不需要分包但应用WPE滤镜 " + std::to_string(len) + "B, user=" + conn->authenticatedUser);

            DataModifierResult modResult = dataModifier(conn, dataToSend, true);

            // 回调后再次检查连接是否有效
            if (!conn->isActive) {
                return;
            }

            if (!modResult.forwardedData.empty()) {
                dataToSend = modResult.forwardedData;
            }
            // 如果被拦截，不转发
            if (modResult.intercepted) {
                AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[IOCP-直通] 数据被WPE滤镜拦截，不转发");
                PostRecv(conn, true);
                return;
            }
        }

        if (!dataToSend.empty()) {
            PostSend(conn, false, dataToSend);
        }
        PostRecv(conn, true);
        return;
    }

    // ===== 粘包分包处理（与传统模式一致）=====
    std::vector<std::vector<uint8_t>> completePackets;

    // 🔥 记录处理前的缓冲区状态（用于判断处理类型）
    bool hadBufferedData = (conn->clientDataBuffer.size() > static_cast<size_t>(len));
    conn->fragmentCount++;
    int currentFragmentCount = conn->fragmentCount;
    int packetsExtractedThisTime = 0;

    while (true) {
        // 缓冲区数据不足5字节，无法解析包头
        if (conn->clientDataBuffer.size() < 5) {
            break;
        }

        // 解析包头获取完整包长度
        uint32_t totalLength = PacketParser::ParseHeader(conn->clientDataBuffer);

        // 🔥 处理包头解析失败的情况（与传统模式一致）
        if (totalLength == 0) {
            AB_LOG_WARNING_CAT(LOG_CAT_COLLECTOR, "[IOCP] 包头解析失败(长度=0)，直接转发原始数据 " +
                std::to_string(conn->clientDataBuffer.size()) + "B");
            completePackets.push_back(conn->clientDataBuffer);
            conn->clientDataBuffer.clear();
            conn->fragmentCount = 0;
            break;
        }

        // 🔥 如果解析出的长度明显不合理（超过64KB），直接转发
        if (totalLength > 65535) {
            AB_LOG_WARNING_CAT(LOG_CAT_COLLECTOR, "[IOCP] 包头长度异常(" +
                std::to_string(totalLength) + ")，直接转发原始数据 " +
                std::to_string(conn->clientDataBuffer.size()) + "B");
            completePackets.push_back(conn->clientDataBuffer);
            conn->clientDataBuffer.clear();
            conn->fragmentCount = 0;
            break;
        }

        // 等待更多数据（正常的分包情况）
        if (conn->clientDataBuffer.size() < totalLength) {
            break;
        }

        // 提取完整包
        std::vector<uint8_t> completePacket(conn->clientDataBuffer.begin(),
            conn->clientDataBuffer.begin() + totalLength);
        conn->clientDataBuffer.erase(conn->clientDataBuffer.begin(),
            conn->clientDataBuffer.begin() + totalLength);

        // 🔥 解析数据包信息（与传统模式一致）
        PacketInfo packetInfo = PacketParser::ParsePacket(completePacket);

        // 🔥 设置SOCKS用户名（用于按账号索引替换数据）
        packetInfo.socksUsername = conn->authenticatedUser;

        // 🔥 提取GameID（与传统模式一致）
        std::string gameID = PacketParser::ExtractGameID(completePacket);
        if (!gameID.empty() && gameID.find("temp_") != 0) {
            if (conn->gameID.empty() || conn->gameID.find("temp_") == 0 || conn->gameID != gameID) {
                conn->gameID = gameID;
                conn->gameIDExtracted = true;
                AB_LOG_INFO_CAT(LOG_CAT_COLLECTOR, "[IOCP] GameID识别: " + gameID +
                    " (" + conn->clientAddr + ")");
            }
        }

        if (!conn->gameID.empty()) {
            packetInfo.gameID = conn->gameID;
        }

        // 🔥🔥🔥 判断数据包处理类型（与传统模式完全一致）
        bool wasFragmented = false;
        bool wasMultiPacket = false;

        if (hadBufferedData || currentFragmentCount > 1) {
            wasFragmented = true;
        }

        if (packetsExtractedThisTime > 0 || conn->clientDataBuffer.size() > 0) {
            wasMultiPacket = true;
        }

        if (wasFragmented && wasMultiPacket) {
            packetInfo.processType = PACKET_BOTH;
            fragmentedPackets++;
            multiPackets++;
        }
        else if (wasFragmented) {
            packetInfo.processType = PACKET_FRAGMENTED;
            fragmentedPackets++;
        }
        else if (wasMultiPacket) {
            packetInfo.processType = PACKET_MULTI;
            multiPackets++;
        }
        else {
            packetInfo.processType = PACKET_NORMAL;
        }

        packetInfo.fragmentCount = currentFragmentCount;
        packetsExtractedThisTime++;
        totalPackets++;

        // 🔥🔥🔥 用户自定义滤镜模式：设置用户启用的滤镜列表
        if (enableUserFilterMode.load() && !conn->authenticatedUser.empty()) {
            // 需要包含 UserFilterManager.h
            extern UserFilterManager* g_userFilterManager;
            if (g_userFilterManager) {
                packetInfo.userEnabledFilters = g_userFilterManager->GetUserEnabledFilters(
                    instanceId, conn->authenticatedUser
                );
            }
        }

        // 调用数据修改回调
        DataModifierResult modResult;
        modResult.forwardedData = completePacket;
        modResult.callbackData = completePacket;
        if (dataModifier) {
            modResult = dataModifier(conn, completePacket, true);

            // 回调后再次检查连接是否有效
            if (!conn->isActive) {
                return;
            }

            if (modResult.callbackData.empty()) {
                modResult.callbackData = modResult.forwardedData;
            }
        }

        completePackets.push_back(modResult.forwardedData);

        // 🔥🔥🔥 调用packetReceived回调用于UI显示（与传统模式完全一致）
        if (packetReceived) {
            try {
                packetReceived(packetInfo, modResult.forwardedData, modResult.callbackData);

                // 回调后再次检查连接是否有效
                if (!conn->isActive) {
                    return;
                }
            }
            catch (const std::exception& e) {
                AB_LOG_ERROR_CAT(LOG_CAT_COLLECTOR, "[IOCP] 回调执行异常: " + std::string(e.what()));
            }
            catch (...) {
                AB_LOG_ERROR_CAT(LOG_CAT_COLLECTOR, "[IOCP] 回调执行未知异常");
            }
        }
    }

    // 🔥 重置分片计数（与传统模式一致）
    if (conn->clientDataBuffer.size() < 5) {
        conn->fragmentCount = 0;
    }

    // 🔥 如果没有提取到任何包，记录警告
    if (completePackets.empty() && len > 0) {
        AB_LOG_WARNING_CAT(LOG_CAT_COLLECTOR, "[IOCP] 收到" + std::to_string(len) +
            "B数据但未提取到完整包，缓冲区: " + std::to_string(conn->clientDataBuffer.size()) + "B");
    }

    // 发送所有完整的包到服务器
    for (const auto& packet : completePackets) {
        // 每次发送前检查连接是否仍然有效
        if (!conn->isActive) {
            return;
        }
        if (!packet.empty()) {
            PostSend(conn, false, packet);
        }
    }

    // 继续接收客户端数据（最后再次检查）
    if (conn->isActive) {
        PostRecv(conn, true);
    }
}

// ==================== 处理服务器数据 ====================
void IOCPThreadPool::ProcessServerData(ProxyConnectionInfo* conn, const uint8_t* data, int len) {
    // 检查连接是否仍然有效
    if (!conn || !conn->isActive) {
        return;
    }

    conn->bytesFromServer += len;

    // 服务器到客户端：直接转发，不做粘包分包处理（与传统模式一致）
    std::vector<uint8_t> dataToSend(data, data + len);

    // ===== 应用WPE滤镜（响应方向）=====
    if (serverDataCallback) {
        auto callbackResult = serverDataCallback(conn, dataToSend);

        // 回调后再次检查连接是否有效
        if (!conn->isActive) {
            return;
        }

        // 如果被拦截，不转发
        if (callbackResult.intercepted) {
            // 继续接收服务器数据
            PostRecv(conn, false);
            return;
        }

        // 如果数据被修改，使用修改后的数据
        if (callbackResult.modified && !callbackResult.modifiedData.empty()) {
            dataToSend = callbackResult.modifiedData;
        }
    }

    // 发送到客户端
    if (!dataToSend.empty() && conn->isActive) {
        PostSend(conn, true, dataToSend);
    }

    // 继续接收服务器数据（最后再次检查）
    if (conn->isActive) {
        PostRecv(conn, false);
    }
}

// ==================== 获取连接 ====================
std::shared_ptr<ProxyConnectionInfo> IOCPThreadPool::GetConnection(uint64_t id) {
    std::lock_guard<std::mutex> lock(connectionsMutex);
    auto it = connections.find(id);
    if (it != connections.end()) {
        return it->second;
    }
    return std::shared_ptr<ProxyConnectionInfo>();
}

// ==================== 获取所有连接 ====================
std::vector<std::shared_ptr<ProxyConnectionInfo>> IOCPThreadPool::GetAllConnections() {
    std::vector<std::shared_ptr<ProxyConnectionInfo>> result;
    std::lock_guard<std::mutex> lock(connectionsMutex);
    result.reserve(connections.size());
    for (auto& pair : connections) {
        result.push_back(pair.second);
    }
    return result;
}

// ==================== 关闭连接 ====================
void IOCPThreadPool::CloseConnection(uint64_t connId, const std::string& reason) {
    std::shared_ptr<ProxyConnectionInfo> conn;
    uint64_t closeRequest = lifecycleCloseRequestedCount.fetch_add(1, std::memory_order_relaxed) + 1;
    size_t mapSizeAfterErase = 0;

    {
        std::lock_guard<std::mutex> lock(connectionsMutex);
        auto it = connections.find(connId);
        if (it == connections.end()) {
            AB_LOG_INFO("[IOCP-Lifecycle] close_miss connId=" + std::to_string(connId) +
                ", request=" + std::to_string(closeRequest) +
                ", reason=" + reason +
                ", mapSize=" + std::to_string(connections.size()));
            return;
        }
        conn = it->second;
        connections.erase(it);
        mapSizeAfterErase = connections.size();
    }

    if (!conn) return;

    conn->isActive = false;
    if (conn->authPrioritySlotHeld) {
        antiCCAuthPriorityInFlight.fetch_sub(1);
        conn->authPrioritySlotHeld = false;
    }

    // 重置该用户的滤镜状态（如果有用户名）
    if (!conn->authenticatedUser.empty() && g_wpeFilterManager) {
        g_wpeFilterManager->ResetUserFilterStates(this->instanceId, conn->authenticatedUser);
    }

    // 关闭socket
    if (conn->clientSocket != INVALID_SOCKET) {
        closesocket(conn->clientSocket);
        conn->clientSocket = INVALID_SOCKET;
    }
    if (conn->serverSocket != INVALID_SOCKET) {
        closesocket(conn->serverSocket);
        conn->serverSocket = INVALID_SOCKET;
    }

    // 更新计数
    if (conn->isWhitelisted) {
        whitelistConnCount--;
    }
    else {
        normalConnCount--;
    }
    totalConnCount--;

    if (connectionEvent) {
        connectionEvent(conn.get(), "closed:" + reason);
    }

    uint64_t closed = lifecycleClosedCount.fetch_add(1, std::memory_order_relaxed) + 1;
    uint64_t created = lifecycleCreatedCount.load(std::memory_order_relaxed);
    int live = totalConnCount.load(std::memory_order_relaxed);

    AB_LOG_INFO("[IOCP] Connection closed: " + conn->clientAddr + " -> " +
        conn->targetHost + ":" + std::to_string(conn->targetPort) +
        " (" + reason + ")");
    AB_LOG_INFO("[IOCP-Lifecycle] closed connId=" + std::to_string(connId) +
        ", request=" + std::to_string(closeRequest) +
        ", reason=" + reason +
        ", mapSize=" + std::to_string(mapSizeAfterErase) +
        ", useCount=" + std::to_string(conn.use_count()) +
        ", created=" + std::to_string(created) +
        ", closed=" + std::to_string(closed) +
        ", live=" + std::to_string(live));

    // 检查等待队列
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        auto& queue = conn->isWhitelisted ? whitelistWaitQueue : normalWaitQueue;
        if (!queue.empty()) {
            SOCKET waitingSocket = queue.front();
            queue.pop();
            // TODO: 处理等待中的连接
        }
    }
}

// ==================== 移除连接 ====================
void IOCPThreadPool::RemoveConnection(uint64_t connId) {
    CloseConnection(connId, "Manual removal");
}

// ==================== 设置最大连接数 ====================
void IOCPThreadPool::SetMaxConnections(int maxWhitelist, int maxNormal) {
    maxWhitelistConnections = maxWhitelist > 0 ? maxWhitelist : 2000;
    maxNormalConnections = maxNormal > 0 ? maxNormal : 500;
}

// ==================== 设置队列限制 ====================
void IOCPThreadPool::SetQueueLimits(int whitelistLimit, int normalLimit) {
    whitelistQueueLimit = whitelistLimit > 0 ? whitelistLimit : 1000;
    normalQueueLimit = normalLimit > 0 ? normalLimit : 200;
}

// ==================== 获取白名单队列大小 ====================
int IOCPThreadPool::GetWhitelistQueueSize() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(queueMutex));
    return static_cast<int>(whitelistWaitQueue.size());
}

// ==================== 获取普通队列大小 ====================
int IOCPThreadPool::GetNormalQueueSize() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(queueMutex));
    return static_cast<int>(normalWaitQueue.size());
}

// ==================== 分包处理配置接口 ====================
void IOCPThreadPool::SetPacketSplitEnabled(bool enabled) {
    enablePacketSplit = enabled;
    AB_LOG_INFO("[IOCP-分包处理] " + std::string(enabled ? "已启用" : "已禁用"));
}

bool IOCPThreadPool::IsPacketSplitEnabled() const {
    return enablePacketSplit.load();
}

void IOCPThreadPool::SetPacketSplitPorts(const std::vector<int>& ports) {
    std::lock_guard<std::mutex> lock(packetSplitMutex);
    packetSplitPorts = ports;

    std::string portList;
    for (size_t i = 0; i < ports.size(); i++) {
        if (i > 0) portList += ", ";
        portList += std::to_string(ports[i]);
    }
    AB_LOG_INFO("[IOCP-分包处理] 端口列表已更新: " + (portList.empty() ? "无" : portList));
}

std::vector<int> IOCPThreadPool::GetPacketSplitPorts() const {
    std::lock_guard<std::mutex> lock(packetSplitMutex);
    return packetSplitPorts;
}

bool IOCPThreadPool::IsPacketSplitEnabledForPort(int port) const {
    if (!enablePacketSplit.load()) {
        return false;  // 全局禁用分包处理
    }

    std::lock_guard<std::mutex> lock(packetSplitMutex);

    // 如果端口列表为空，表示对所有端口都启用分包处理
    if (packetSplitPorts.empty()) {
        return true;
    }

    // 检查端口是否在列表中
    for (int p : packetSplitPorts) {
        if (p == port) {
            return true;
        }
    }

    return false;
}

// ===== 🔥 对不分包流量应用WPE滤镜 =====
void IOCPThreadPool::SetApplyWpeOnNonSplitTraffic(bool enabled) {
    applyWpeOnNonSplitTraffic = enabled;
    AB_LOG_INFO("[IOCP] 对不分包流量应用WPE滤镜: " + std::string(enabled ? "启用" : "禁用"));
}

bool IOCPThreadPool::IsApplyWpeOnNonSplitTraffic() const {
    return applyWpeOnNonSplitTraffic.load();
}

// ===== 🔥 用户滤镜HTTP服务器管理 =====
bool IOCPThreadPool::StartUserFilterHttpServer() {
    // 🔥 如果对象已存在且正在运行，直接返回
    if (userFilterHttpServer && userFilterHttpServer->IsRunning()) {
        AB_LOG_WARNING("[IOCP] 用户滤镜HTTP服务器已在运行");
        return true;
    }

    // 🔥 销毁旧对象（如果存在），确保使用最新的端口配置
    if (userFilterHttpServer) {
        userFilterHttpServer.reset();
    }

    // 创建新的HTTP服务器对象
    userFilterHttpServer = std::make_unique<UserFilterWebServer>(instanceId, userFilterHttpPort);

    // 设置SOCKS验证回调
    userFilterHttpServer->SetSocksValidator([this](const std::string& username, const std::string& password) {
        if (authCallback) {
            return authCallback(username, password, "", nullptr);
        }
        return false;
    });

    // 设置滤镜列表获取回调
    userFilterHttpServer->SetFilterListGetter([this]() {
        std::vector<std::pair<int, std::string>> filters;
        if (g_wpeFilterManager) {
            auto allFilters = g_wpeFilterManager->GetAllFilters();
            for (const auto& filter : allFilters) {
                const std::string displayName = filter.webDisplayName.empty() ? filter.name : filter.webDisplayName;
                // 🔥 只返回对当前实例生效的滤镜
                if (filter.target.applyToAllInstances) {
                    // 对所有实例生效
                    filters.push_back({filter.id, displayName});
                } else {
                    // 检查是否在目标实例列表中
                    auto& targetIds = filter.target.targetInstanceIds;
                    if (std::find(targetIds.begin(), targetIds.end(), instanceId) != targetIds.end()) {
                        filters.push_back({filter.id, displayName});
                    }
                }
            }
        }
        return filters;
    });

    userFilterHttpServer->SetAuthorizedFilterListGetter([this](const std::string& username) {
        std::vector<AuthorizedWebFilter> result;
        if (!g_wpeFilterManager || !g_userFilterManager) return result;

        const auto state = g_userFilterManager->GetAuthorizedFilters(instanceId, username);
        const auto allFilters = g_wpeFilterManager->GetAllFilters();
        for (const auto& filter : allFilters) {
            if (state.authorizedFilterIds.count(filter.id) == 0) continue;
            if (!filter.target.applyToAllInstances) {
                const auto& ids = filter.target.targetInstanceIds;
                if (!ids.empty() && std::find(ids.begin(), ids.end(), instanceId) == ids.end()) {
                    continue;
                }
            }

            AuthorizedWebFilter item;
            item.id = filter.id;
            item.name = filter.webDisplayName.empty() ? filter.name : filter.webDisplayName;
            item.applyToCollector = filter.target.applyToCollector || filter.target.applyToAllInstances;
            item.applyToHeartbeat = filter.target.applyToHeartbeat || filter.target.applyToAllInstances;
            result.push_back(item);
        }
        return result;
    });

    // 启动HTTP服务器
    bool success = userFilterHttpServer->Start();
    if (success) {
        AB_LOG_INFO("[IOCP] 用户滤镜HTTP服务器启动成功，端口: " + std::to_string(userFilterHttpPort));
    } else {
        AB_LOG_ERROR("[IOCP] 用户滤镜HTTP服务器启动失败，端口: " + std::to_string(userFilterHttpPort));
        // 🔥 启动失败时销毁对象，避免下次启动时使用错误的配置
        userFilterHttpServer.reset();
    }
    return success;
}

void IOCPThreadPool::StopUserFilterHttpServer() {
    if (userFilterHttpServer) {
        if (userFilterHttpServer->IsRunning()) {
            userFilterHttpServer->Stop();
            AB_LOG_INFO("[IOCP] 用户滤镜HTTP服务器已停止");
        }
        // 🔥 销毁对象，这样下次启动时会用新端口重新创建
        userFilterHttpServer.reset();
    }
}

bool IOCPThreadPool::IsUserFilterHttpServerRunning() const {
    return userFilterHttpServer && userFilterHttpServer->IsRunning();
}
