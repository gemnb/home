#include "TcpFileServer.h"
#include "Logger.h"
#include <fstream>
#include <chrono>

const size_t CHUNK_SIZE = 64 * 1024;  // 64KB per chunk

// ========== 辅助函数：可靠发送 ==========
static bool SendAll(SOCKET socket, const char* buffer, int length) {
    int totalSent = 0;

    while (totalSent < length) {
        int bytesSent = send(socket, buffer + totalSent,
            length - totalSent, 0);

        if (bytesSent == SOCKET_ERROR) {
            int error = WSAGetLastError();
            AB_LOG_ERROR("[TCP服务端] 发送数据失败，错误码: " + std::to_string(error) +
                " (已发送: " + std::to_string(totalSent) + "/" + std::to_string(length) + " 字节)");

            // 记录具体错误类型
            switch (error) {
            case WSAETIMEDOUT:
                AB_LOG_ERROR("[TCP服务端] 错误详情: 发送超时");
                break;
            case WSAECONNRESET:
                AB_LOG_ERROR("[TCP服务端] 错误详情: 连接被重置");
                break;
            case WSAECONNABORTED:
                AB_LOG_ERROR("[TCP服务端] 错误详情: 连接被中止");
                break;
            case WSAENETDOWN:
                AB_LOG_ERROR("[TCP服务端] 错误详情: 网络断开");
                break;
            default:
                AB_LOG_ERROR("[TCP服务端] 错误详情: 未知错误");
                break;
            }
            return false;
        }

        totalSent += bytesSent;
    }

    return true;
}

TcpFileServer::TcpFileServer(int port, DatabaseManager* db)
    : port(port), listenSocket(INVALID_SOCKET),
    isRunning(false), database(db),
    totalConnections(0), totalBytesSent(0) {}

TcpFileServer::~TcpFileServer() {
    Stop();
}

bool TcpFileServer::Start() {
    if (isRunning) {
        AB_LOG_WARNING("[TCP服务端] 服务器已在运行");
        return false;
    }

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        AB_LOG_ERROR("[TCP服务端] WSAStartup失败");
        return false;
    }

    listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket == INVALID_SOCKET) {
        AB_LOG_ERROR("[TCP服务端] 创建Socket失败");
        WSACleanup();
        return false;
    }

    int reuse = 1;
    setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));

    // ✅ 增加发送缓冲区
    int sendBufSize = 256 * 1024;  // 256KB
    setsockopt(listenSocket, SOL_SOCKET, SO_SNDBUF, (char*)&sendBufSize, sizeof(sendBufSize));

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(port);

    if (bind(listenSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        AB_LOG_ERROR("[TCP服务端] 绑定端口失败: " + std::to_string(port));
        closesocket(listenSocket);
        WSACleanup();
        return false;
    }

    if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) {
        AB_LOG_ERROR("[TCP服务端] 监听失败");
        closesocket(listenSocket);
        WSACleanup();
        return false;
    }

    isRunning = true;
    serverThread = std::thread(&TcpFileServer::ServerLoop, this);

    AB_LOG_INFO("[TCP服务端] 文件传输服务启动成功，端口: " + std::to_string(port));
    return true;
}

void TcpFileServer::ServerLoop() {
    while (isRunning) {
        sockaddr_in clientAddr;
        int clientAddrSize = sizeof(clientAddr);

        SOCKET clientSocket = accept(listenSocket, (sockaddr*)&clientAddr, &clientAddrSize);

        if (clientSocket == INVALID_SOCKET) {
            if (isRunning) {
                AB_LOG_ERROR("[TCP服务端] 接受连接失败");
            }
            continue;
        }

        // ✅ 为客户端连接设置超时
        DWORD timeout = 60000;  // 60秒
        setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
        setsockopt(clientSocket, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));

        char clientIP[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, clientIP, INET_ADDRSTRLEN);
        std::string clientAddrStr = std::string(clientIP) + ":" +
            std::to_string(ntohs(clientAddr.sin_port));

        totalConnections++;
        AB_LOG_INFO("[TCP服务端] 新连接: " + clientAddrStr + " (总连接数: " +
            std::to_string(totalConnections.load()) + ")");

        std::thread(&TcpFileServer::HandleClient, this, clientSocket, clientAddrStr).detach();
    }
}

void TcpFileServer::HandleClient(SOCKET clientSocket, const std::string& clientAddr) {
    AB_LOG_INFO("[TCP服务端] 开始处理客户端: " + clientAddr);

    // 接收请求头
    FileTransferHeader header;
    int bytesReceived = recv(clientSocket, (char*)&header, sizeof(header), 0);

    if (bytesReceived != sizeof(header)) {
        AB_LOG_ERROR("[TCP服务端] 接收请求头失败: " + clientAddr +
            "，接收字节: " + std::to_string(bytesReceived) +
            "，期望: " + std::to_string(sizeof(header)));
        closesocket(clientSocket);
        return;
    }

    AB_LOG_INFO("[TCP服务端] 收到命令: " + std::to_string(header.command));

    if (header.command == CMD_REQUEST_DB_FILE) {
        AB_LOG_INFO("[TCP服务端] 收到数据库文件请求: " + clientAddr);

        // 检查数据库是否初始化
        if (!database) {
            AB_LOG_ERROR("[TCP服务端] 数据库未初始化");
            FileTransferHeader errorHeader = { 0 };
            errorHeader.command = CMD_ERROR;
            send(clientSocket, (char*)&errorHeader, sizeof(errorHeader), 0);
            closesocket(clientSocket);
            return;
        }

        // ========== 步骤1：生成临时快照路径 ==========
        char tempPath[512];
        DWORD tempPathLen = GetTempPathA(sizeof(tempPath), tempPath);

        if (tempPathLen == 0 || tempPathLen > sizeof(tempPath)) {
            AB_LOG_ERROR("[TCP服务端] 获取临时目录失败");
            FileTransferHeader errorHeader = { 0 };
            errorHeader.command = CMD_ERROR;
            send(clientSocket, (char*)&errorHeader, sizeof(errorHeader), 0);
            closesocket(clientSocket);
            return;
        }

        // 生成唯一的快照文件名
        DWORD threadId = GetCurrentThreadId();
        time_t now = time(NULL);
        std::string snapshotPath = std::string(tempPath) + "db_snapshot_" +
            std::to_string(now) + "_" + std::to_string(threadId) + ".db";

        AB_LOG_INFO("[TCP服务端] 准备创建数据库快照...");
        AB_LOG_INFO("[TCP服务端] 快照路径: " + snapshotPath);

        // ========== 步骤2：创建快照 ==========
        auto snapshotStartTime = std::chrono::steady_clock::now();

        if (!database->CreateSnapshotForTransfer(snapshotPath)) {
            AB_LOG_ERROR("[TCP服务端] 创建快照失败");
            FileTransferHeader errorHeader = { 0 };
            errorHeader.command = CMD_ERROR;
            send(clientSocket, (char*)&errorHeader, sizeof(errorHeader), 0);
            closesocket(clientSocket);
            return;
        }

        auto snapshotEndTime = std::chrono::steady_clock::now();
        auto snapshotDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
            snapshotEndTime - snapshotStartTime).count();

        AB_LOG_INFO("[TCP服务端] 快照创建成功，耗时: " + std::to_string(snapshotDuration) + " 毫秒");

        // ========== 步骤3：发送快照文件 ==========
        AB_LOG_INFO("[TCP服务端] 开始发送快照文件...");

        auto sendStartTime = std::chrono::steady_clock::now();

        bool success = SendDatabaseFile(clientSocket, snapshotPath);

        auto sendEndTime = std::chrono::steady_clock::now();
        auto sendDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
            sendEndTime - sendStartTime).count();

        // ========== 步骤4：删除临时快照 ==========
        AB_LOG_INFO("[TCP服务端] 正在删除临时快照...");

        if (DeleteFileA(snapshotPath.c_str())) {
            AB_LOG_INFO("[TCP服务端] 已删除临时快照: " + snapshotPath);
        }
        else {
            DWORD error = GetLastError();
            AB_LOG_WARNING("[TCP服务端] 删除临时快照失败，错误码: " +
                std::to_string(error) + "，文件: " + snapshotPath);
        }

        // ========== 步骤5：记录结果 ==========
        if (!success) {
            AB_LOG_ERROR("[TCP服务端] 发送文件失败: " + clientAddr +
                "，耗时: " + std::to_string(sendDuration) + " 毫秒");
        }
        else {
            auto totalDuration = snapshotDuration + sendDuration;
            AB_LOG_INFO("[TCP服务端] 文件发送完成: " + clientAddr);
            AB_LOG_INFO("[TCP服务端] 总耗时: " + std::to_string(totalDuration) + " 毫秒 (" +
                std::to_string(totalDuration / 1000.0) + " 秒)");
            AB_LOG_INFO("[TCP服务端] 快照创建: " + std::to_string(snapshotDuration) + " 毫秒，" +
                "文件发送: " + std::to_string(sendDuration) + " 毫秒");
        }
    }
    else {
        AB_LOG_WARNING("[TCP服务端] 未知命令: " + std::to_string(header.command) +
            " (来自: " + clientAddr + ")");

        // 发送错误响应
        FileTransferHeader errorHeader = { 0 };
        errorHeader.command = CMD_ERROR;
        send(clientSocket, (char*)&errorHeader, sizeof(errorHeader), 0);
    }

    AB_LOG_INFO("[TCP服务端] 客户端处理完成: " + clientAddr);
    closesocket(clientSocket);
}





bool TcpFileServer::SendDatabaseFile(SOCKET clientSocket, const std::string& dbPath) {
    // 打开文件
    std::ifstream file(dbPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        AB_LOG_ERROR("[TCP服务端] 无法打开文件: " + dbPath);
        FileTransferHeader errorHeader = { 0 };
        errorHeader.command = CMD_ERROR;
        send(clientSocket, (char*)&errorHeader, sizeof(errorHeader), 0);
        return false;
    }

    // 获取文件大小
    std::streamsize fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    // 计算总块数
    uint32_t totalChunks = static_cast<uint32_t>((fileSize + CHUNK_SIZE - 1) / CHUNK_SIZE);

    // ✅ 使用可靠发送文件信息
    FileTransferHeader infoHeader = { 0 };
    infoHeader.command = CMD_FILE_INFO;
    infoHeader.totalSize = static_cast<uint64_t>(fileSize);
    infoHeader.totalChunks = totalChunks;

    if (!SendAll(clientSocket, (char*)&infoHeader, sizeof(infoHeader))) {
        AB_LOG_ERROR("[TCP服务端] 发送文件信息失败");
        file.close();
        return false;
    }

    AB_LOG_INFO("[TCP服务端] 开始发送文件: " + dbPath +
        " (大小: " + std::to_string(fileSize) + " 字节, " +
        std::to_string(totalChunks) + " 块)");

    // 发送文件数据
    std::vector<char> buffer(CHUNK_SIZE);
    uint32_t chunkIndex = 0;
    uint64_t totalSent = 0;

    while (file.read(buffer.data(), CHUNK_SIZE) || file.gcount() > 0) {
        std::streamsize bytesRead = file.gcount();

        // ✅ 使用可靠发送块头
        FileTransferHeader chunkHeader = { 0 };
        chunkHeader.command = CMD_FILE_CHUNK;
        chunkHeader.dataSize = static_cast<uint32_t>(bytesRead);
        chunkHeader.totalSize = static_cast<uint64_t>(fileSize);
        chunkHeader.chunkIndex = chunkIndex;
        chunkHeader.totalChunks = totalChunks;

        if (!SendAll(clientSocket, (char*)&chunkHeader, sizeof(chunkHeader))) {
            AB_LOG_ERROR("[TCP服务端] 发送块头失败，块索引: " + std::to_string(chunkIndex));
            file.close();
            return false;
        }

        // ✅ 使用可靠发送块数据
        if (!SendAll(clientSocket, buffer.data(), static_cast<int>(bytesRead))) {
            AB_LOG_ERROR("[TCP服务端] 发送块数据失败，块索引: " + std::to_string(chunkIndex) +
                "，块大小: " + std::to_string(bytesRead) + " 字节");
            file.close();
            return false;
        }

        totalSent += bytesRead;
        chunkIndex++;

        // 每100块记录一次进度
        if (chunkIndex % 100 == 0 || chunkIndex == totalChunks) {
            double progress = (totalSent * 100.0) / fileSize;
            AB_LOG_INFO("[TCP服务端] 发送进度: " + std::to_string((int)progress) + "% (" +
                std::to_string(chunkIndex) + "/" + std::to_string(totalChunks) + " 块)");
        }
    }

    file.close();

    // ✅ 使用可靠发送完成标志
    FileTransferHeader completeHeader = { 0 };
    completeHeader.command = CMD_FILE_COMPLETE;
    completeHeader.totalSize = static_cast<uint64_t>(fileSize);

    if (!SendAll(clientSocket, (char*)&completeHeader, sizeof(completeHeader))) {
        AB_LOG_ERROR("[TCP服务端] 发送完成标志失败");
        return false;
    }

    totalBytesSent += totalSent;

    AB_LOG_INFO("[TCP服务端] 文件发送完成: " + std::to_string(totalSent) + " 字节 (" +
        std::to_string(totalSent / 1024.0 / 1024.0) + " MB)");
    return true;
}

void TcpFileServer::Stop() {
    if (!isRunning) return;

    isRunning = false;

    if (listenSocket != INVALID_SOCKET) {
        closesocket(listenSocket);
        listenSocket = INVALID_SOCKET;
    }

    if (serverThread.joinable()) {
        serverThread.join();
    }

    WSACleanup();
    AB_LOG_INFO("[TCP服务端] 文件传输服务已停止");
}
