#include "TcpFileClient.h"
#include "Logger.h"
#include <fstream>
#include <chrono>
#include <ws2tcpip.h>

// ========== 辅助函数：可靠接收 ==========
static bool RecvAll(SOCKET socket, char* buffer, int length) {
    int totalReceived = 0;

    while (totalReceived < length) {
        int bytesReceived = recv(socket, buffer + totalReceived,
            length - totalReceived, 0);

        if (bytesReceived == 0) {
            // 连接被优雅关闭
            AB_LOG_ERROR("[TCP客户端] 连接被服务器关闭，已接收: " +
                std::to_string(totalReceived) + "/" + std::to_string(length) + " 字节");
            return false;
        }

        if (bytesReceived == SOCKET_ERROR) {
            int error = WSAGetLastError();
            AB_LOG_ERROR("[TCP客户端] 接收数据失败，错误码: " + std::to_string(error) +
                " (已接收: " + std::to_string(totalReceived) + "/" + std::to_string(length) + " 字节)");

            // 记录具体错误类型
            switch (error) {
            case WSAETIMEDOUT:
                AB_LOG_ERROR("[TCP客户端] 错误详情: 接收超时");
                break;
            case WSAECONNRESET:
                AB_LOG_ERROR("[TCP客户端] 错误详情: 连接被重置");
                break;
            case WSAECONNABORTED:
                AB_LOG_ERROR("[TCP客户端] 错误详情: 连接被中止");
                break;
            case WSAENETDOWN:
                AB_LOG_ERROR("[TCP客户端] 错误详情: 网络断开");
                break;
            default:
                AB_LOG_ERROR("[TCP客户端] 错误详情: 未知错误");
                break;
            }
            return false;
        }

        totalReceived += bytesReceived;
    }

    return true;
}

TcpFileClient::TcpFileClient(const std::string& host, int port)
    : serverHost(host), serverPort(port),
    isDownloading(false), currentDownloaded(0),
    totalSize(0), downloadSpeed(0.0) {}

TcpFileClient::~TcpFileClient() {}

bool TcpFileClient::DownloadDatabaseFile(const std::string& savePath,
    DownloadProgressCallback progressCallback) {
    std::lock_guard<std::mutex> lock(downloadMutex);

    if (isDownloading) {
        AB_LOG_WARNING("[TCP客户端] 正在下载中，请稍候");
        return false;
    }

    isDownloading = true;
    currentDownloaded = 0;
    totalSize = 0;
    downloadSpeed = 0.0;

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        AB_LOG_ERROR("[TCP客户端] WSAStartup失败");
        isDownloading = false;
        return false;
    }

    SOCKET clientSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (clientSocket == INVALID_SOCKET) {
        AB_LOG_ERROR("[TCP客户端] 创建Socket失败");
        WSACleanup();
        isDownloading = false;
        return false;
    }

    // ✅ 增加超时时间到60秒
    DWORD timeout = 60000;  // 60秒
    setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
    setsockopt(clientSocket, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));

    // ✅ 增加接收缓冲区到256KB
    int recvBufSize = 256 * 1024;  // 256KB
    setsockopt(clientSocket, SOL_SOCKET, SO_RCVBUF, (char*)&recvBufSize, sizeof(recvBufSize));

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(serverPort);

    // 解析主机地址
    if (inet_pton(AF_INET, serverHost.c_str(), &serverAddr.sin_addr) != 1) {
        struct addrinfo hints = { 0 }, * result = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        if (getaddrinfo(serverHost.c_str(), nullptr, &hints, &result) == 0) {
            serverAddr.sin_addr = ((sockaddr_in*)result->ai_addr)->sin_addr;
            freeaddrinfo(result);
        }
        else {
            AB_LOG_ERROR("[TCP客户端] 无法解析主机: " + serverHost);
            closesocket(clientSocket);
            WSACleanup();
            isDownloading = false;
            return false;
        }
    }

    AB_LOG_INFO("[TCP客户端] 正在连接服务器: " + serverHost + ":" + std::to_string(serverPort));

    if (connect(clientSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        int error = WSAGetLastError();
        AB_LOG_ERROR("[TCP客户端] 连接服务器失败，错误码: " + std::to_string(error));
        closesocket(clientSocket);
        WSACleanup();
        isDownloading = false;
        return false;
    }

    AB_LOG_INFO("[TCP客户端] 已连接到服务器");

    // 发送请求
    FileTransferHeader requestHeader = { 0 };
    requestHeader.command = CMD_REQUEST_DB_FILE;

    if (send(clientSocket, (char*)&requestHeader, sizeof(requestHeader), 0) != sizeof(requestHeader)) {
        AB_LOG_ERROR("[TCP客户端] 发送请求失败");
        closesocket(clientSocket);
        WSACleanup();
        isDownloading = false;
        return false;
    }

    AB_LOG_INFO("[TCP客户端] 已发送文件请求");

    bool success = ReceiveFile(clientSocket, savePath, progressCallback);

    closesocket(clientSocket);
    WSACleanup();
    isDownloading = false;

    return success;
}

bool TcpFileClient::ReceiveFile(SOCKET socket, const std::string& savePath,
    DownloadProgressCallback progressCallback) {

    // ✅ 使用可靠接收文件信息
    FileTransferHeader infoHeader;
    AB_LOG_INFO("[TCP客户端] 正在接收文件信息头...");

    if (!RecvAll(socket, (char*)&infoHeader, sizeof(infoHeader))) {
        AB_LOG_ERROR("[TCP客户端] 接收文件信息失败");
        return false;
    }

    if (infoHeader.command == CMD_ERROR) {
        AB_LOG_ERROR("[TCP客户端] 服务器返回错误");
        return false;
    }

    if (infoHeader.command != CMD_FILE_INFO) {
        AB_LOG_ERROR("[TCP客户端] 收到未知命令: " + std::to_string(infoHeader.command) +
            "，期望: " + std::to_string(CMD_FILE_INFO));
        return false;
    }

    totalSize = infoHeader.totalSize;
    uint32_t totalChunks = infoHeader.totalChunks;

    AB_LOG_INFO("[TCP客户端] 文件大小: " + std::to_string(totalSize) + " 字节 (" +
        std::to_string(totalSize / 1024.0 / 1024.0) + " MB), " +
        std::to_string(totalChunks) + " 块");

    // 创建临时文件
    std::string tempPath = savePath + ".tmp";
    std::ofstream file(tempPath, std::ios::binary);
    if (!file.is_open()) {
        AB_LOG_ERROR("[TCP客户端] 无法创建文件: " + tempPath);
        return false;
    }

    // 接收文件数据
    uint32_t receivedChunks = 0;
    currentDownloaded = 0;

    auto startTime = std::chrono::steady_clock::now();
    auto lastUpdateTime = startTime;

    while (receivedChunks < totalChunks) {
        // ✅ 使用可靠接收块头
        FileTransferHeader chunkHeader;

        if (!RecvAll(socket, (char*)&chunkHeader, sizeof(chunkHeader))) {
            AB_LOG_ERROR("[TCP客户端] 接收块头失败，当前进度: " +
                std::to_string(receivedChunks) + "/" + std::to_string(totalChunks) + " 块 (" +
                std::to_string((receivedChunks * 100) / totalChunks) + "%)");
            file.close();
            DeleteFileA(tempPath.c_str());
            return false;
        }

        if (chunkHeader.command == CMD_FILE_COMPLETE) {
            AB_LOG_INFO("[TCP客户端] 收到完成标志，当前已接收: " +
                std::to_string(receivedChunks) + "/" + std::to_string(totalChunks) + " 块");
            break;
        }

        if (chunkHeader.command != CMD_FILE_CHUNK) {
            AB_LOG_ERROR("[TCP客户端] 收到未知块命令: " + std::to_string(chunkHeader.command) +
                "，期望: " + std::to_string(CMD_FILE_CHUNK) +
                "，块索引: " + std::to_string(chunkHeader.chunkIndex));
            file.close();
            DeleteFileA(tempPath.c_str());
            return false;
        }

        // ✅ 使用可靠接收块数据
        std::vector<char> buffer(chunkHeader.dataSize);

        if (!RecvAll(socket, buffer.data(), chunkHeader.dataSize)) {
            AB_LOG_ERROR("[TCP客户端] 接收块数据失败，块索引: " +
                std::to_string(chunkHeader.chunkIndex) + "/" + std::to_string(totalChunks - 1) +
                "，块大小: " + std::to_string(chunkHeader.dataSize) + " 字节");
            file.close();
            DeleteFileA(tempPath.c_str());
            return false;
        }

        // 写入文件
        file.write(buffer.data(), chunkHeader.dataSize);
        if (!file.good()) {
            AB_LOG_ERROR("[TCP客户端] 写入文件失败，块索引: " + std::to_string(chunkHeader.chunkIndex));
            file.close();
            DeleteFileA(tempPath.c_str());
            return false;
        }

        currentDownloaded += chunkHeader.dataSize;
        receivedChunks++;

        // 计算下载速度
        auto currentTime = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            currentTime - startTime).count();

        if (elapsed > 0) {
            downloadSpeed = (currentDownloaded / 1024.0) / (elapsed / 1000.0);
        }

        // 每秒更新一次进度
        auto updateElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            currentTime - lastUpdateTime).count();

        if (updateElapsed >= 1000 || receivedChunks == totalChunks) {
            lastUpdateTime = currentTime;

            if (progressCallback) {
                progressCallback(currentDownloaded, totalSize, downloadSpeed);
            }

            double progress = (currentDownloaded * 100.0) / totalSize;
            AB_LOG_INFO("[TCP客户端] 接收进度: " + std::to_string((int)progress) + "% (" +
                std::to_string(receivedChunks) + "/" + std::to_string(totalChunks) + " 块) - " +
                std::to_string((int)downloadSpeed) + " KB/s");
        }
    }

    file.close();

    // ✅ 验证接收完整性
    if (currentDownloaded != totalSize) {
        AB_LOG_ERROR("[TCP客户端] 文件大小不匹配！接收: " + std::to_string(currentDownloaded) +
            " 字节，期望: " + std::to_string(totalSize) + " 字节");
        DeleteFileA(tempPath.c_str());
        return false;
    }

    // 重命名临时文件
    DeleteFileA(savePath.c_str());
    if (MoveFileA(tempPath.c_str(), savePath.c_str())) {
        AB_LOG_INFO("[TCP客户端] 文件下载完成: " + savePath +
            " (" + std::to_string(totalSize) + " 字节)");
        return true;
    }
    else {
        AB_LOG_ERROR("[TCP客户端] 重命名文件失败，源文件: " + tempPath + "，目标: " + savePath);
        DeleteFileA(tempPath.c_str());
        return false;
    }
}
