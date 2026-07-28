#pragma once
#include <cstdint>

// 文件传输协议定义（服务端和客户端共用）
enum FileTransferCommand {
    CMD_REQUEST_DB_FILE = 1,      // 请求数据库文件
    CMD_FILE_INFO = 2,            // 文件信息响应
    CMD_FILE_CHUNK = 3,           // 文件数据块
    CMD_FILE_COMPLETE = 4,        // 传输完成
    CMD_ERROR = 5                 // 错误
};

struct FileTransferHeader {
    uint32_t command;
    uint32_t dataSize;
    uint64_t totalSize;
    uint32_t chunkIndex;
    uint32_t totalChunks;
};
