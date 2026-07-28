#include "DatabaseManager.h"
#include <sstream>
#include <iomanip>
#include <chrono>
#include <iostream>
#include <Windows.h>
#include <fstream>
#include <filesystem>
#include "Logger.h"


namespace {
void DbLogImpl(const std::string& msg) {
    // 组合完整消息
    std::string fullMsg = "[数据库] " + msg;

    // UTF-8 转 GBK 后输出到控制台
    int wlen = MultiByteToWideChar(CP_UTF8, 0, fullMsg.c_str(), -1, NULL, 0);
    if (wlen > 0) {
        std::vector<wchar_t> wstr(wlen);
        MultiByteToWideChar(CP_UTF8, 0, fullMsg.c_str(), -1, wstr.data(), wlen);

        int gbkLen = WideCharToMultiByte(CP_ACP, 0, wstr.data(), -1, NULL, 0, NULL, NULL);
        if (gbkLen > 0) {
            std::vector<char> gbkStr(gbkLen);
            WideCharToMultiByte(CP_ACP, 0, wstr.data(), -1, gbkStr.data(), gbkLen, NULL, NULL);
            std::cout << gbkStr.data() << std::endl;
            return;
        }
    }

    // 转换失败时直接输出
    std::cout << fullMsg << std::endl;
}
}  // namespace

#define DbLog(message) do { if (Logger::IsEnabled()) { DbLogImpl((message)); } } while (0)


DatabaseManager::DatabaseManager()
    : db(nullptr), configDb(nullptr),
      currentMode(StorageMode::HOURLY), nextMemoryId(1),
      lastFlushTime(std::chrono::steady_clock::now()) {
    // 🔥 启动后台刷新线程
    flushThreadRunning = true;
    flushThread = std::thread(&DatabaseManager::FlushThreadWorker, this);
}


DatabaseManager::~DatabaseManager() {
    // 🔥 停止后台刷新线程
    flushThreadRunning = false;
    flushCondition.notify_all();
    if (flushThread.joinable()) {
        flushThread.join();
    }

    // 🔥 析构前刷新缓存到数据库
    FlushFilterExecutionStats();
    Close();
}

// 修改：获取北京时间的小时字符串（格式：YYYY-MM-DD-HH）
std::string DatabaseManager::GetBeijingHour() {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
    seconds += 8 * 3600; // 北京时间 UTC+8

    time_t beijing_time = seconds;
    struct tm timeinfo;
    gmtime_s(&timeinfo, &beijing_time);

    std::stringstream ss;
    ss << std::put_time(&timeinfo, "%Y-%m-%d-%H");  // 修改：增加小时，格式为 YYYY-MM-DD-HH
    return ss.str();
}

// 🔥 新增：获取北京时间的天字符串（格式：YYYY-MM-DD）
std::string DatabaseManager::GetBeijingDay() {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
    seconds += 8 * 3600; // 北京时间 UTC+8

    time_t beijing_time = seconds;
    struct tm timeinfo;
    gmtime_s(&timeinfo, &beijing_time);

    std::stringstream ss;
    ss << std::put_time(&timeinfo, "%Y-%m-%d");  // 格式为 YYYY-MM-DD
    return ss.str();
}

// 🔥 新增：根据存储模式返回时间字符串
std::string DatabaseManager::GetTimeStringByMode() {
    switch (currentMode) {
    case StorageMode::HOURLY:
        return GetBeijingHour();
    case StorageMode::DAILY:
        return GetBeijingDay();
    case StorageMode::MEMORY:
        return "memory";  // 内存模式不需要时间字符串
    default:
        return GetBeijingHour();
    }
}



// 获取北京时间的时间戳字符串
std::string DatabaseManager::GetBeijingTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count() % 1000;
    seconds += 8 * 3600;

    time_t beijing_time = seconds;
    struct tm timeinfo;
    gmtime_s(&timeinfo, &beijing_time);

    std::stringstream ss;
    ss << std::put_time(&timeinfo, "%Y-%m-%d %H:%M:%S");
    ss << '.' << std::setfill('0') << std::setw(3) << ms;
    return ss.str();
}

bool DatabaseManager::CreateTable() {
    const char* createTableSQL = R"(
        CREATE TABLE IF NOT EXISTS heartbeat_data (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            game_id TEXT NOT NULL,
            timestamp TEXT NOT NULL,
            pattern_23_data BLOB,
            pattern_09_data BLOB,
            pattern_62_data BLOB,
            raw_data BLOB,
            packet_size INTEGER,
            is_complete INTEGER,
            process_type INTEGER,
            fragment_count INTEGER,
            packet_type INTEGER,
            game_id_length INTEGER,
            created_at TEXT NOT NULL
        );
    )";

    char* errMsg = nullptr;
    int rc = sqlite3_exec(db, createTableSQL, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        DbLog("创建表失败: " + std::string(errMsg));
        sqlite3_free(errMsg);
        return false;
    }

    DbLog("表创建成功");

    const char* createIndex1SQL = "CREATE INDEX IF NOT EXISTS idx_game_id ON heartbeat_data(game_id);";
    rc = sqlite3_exec(db, createIndex1SQL, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        DbLog("创建 game_id 索引失败: " + std::string(errMsg));
        sqlite3_free(errMsg);
    }

    const char* createIndex2SQL = "CREATE INDEX IF NOT EXISTS idx_timestamp ON heartbeat_data(timestamp);";
    rc = sqlite3_exec(db, createIndex2SQL, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        DbLog("创建 timestamp 索引失败: " + std::string(errMsg));
        sqlite3_free(errMsg);
    }

    const char* createIndex3SQL = "CREATE INDEX IF NOT EXISTS idx_packet_type ON heartbeat_data(packet_type);";
    rc = sqlite3_exec(db, createIndex3SQL, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        DbLog("创建 packet_type 索引失败: " + std::string(errMsg));
        sqlite3_free(errMsg);
    }

    return true;
}

void DatabaseManager::CheckAndSwitchDatabase() {
    // 内存模式不需要切换数据库
    if (currentMode == StorageMode::MEMORY) {
        return;
    }

    std::string newTimeStr = GetTimeStringByMode();  // 🔥 修改：根据模式获取时间字符串

    if (newTimeStr != currentHour) {  // 检查时间是否变化（小时或天）
        std::string modeDesc = (currentMode == StorageMode::HOURLY) ? "整点" : "日期";
        DbLog("检测到" + modeDesc + "变更: " + currentHour + " -> " + newTimeStr);

        if (db != nullptr) {
            sqlite3_close(db);
            db = nullptr;
        }
        currentHour = newTimeStr;  // 更新当前时间字符串

        // 确保数据库目录存在（默认 ./db，可按实例隔离到其它目录）
        if (!std::filesystem::exists(dataDir)) {
            if (std::filesystem::create_directories(dataDir)) {
                DbLog("已创建数据库目录: " + dataDir);
            }
            else {
                DbLog("创建数据库目录失败: " + dataDir);
                return;
            }
        }

        currentDbPath = dataDir + "/heartbeat_" + currentHour + ".db";  // 文件存储在 dataDir 中
        int rc = sqlite3_open(currentDbPath.c_str(), &db);
        if (rc != SQLITE_OK) {
            DbLog("创建新数据库失败: " + currentDbPath);
            return;
        }
        CreateTable();
        DbLog("已切换到新数据库: " + currentDbPath);
    }
}






bool DatabaseManager::Initialize() {
    // 内存模式不需要初始化数据库
    if (currentMode == StorageMode::MEMORY) {
        DbLog("初始化内存存储模式");
        std::lock_guard<std::mutex> lock(memoryMutex);
        memoryRecords.clear();
        nextMemoryId = 1;
        DbLog("内存存储模式初始化成功");
        return true;
    }

    std::lock_guard<std::mutex> lock(dbMutex);

    // 创建数据库目录（如果不存在）
    if (!std::filesystem::exists(dataDir)) {
        if (std::filesystem::create_directories(dataDir)) {
            DbLog("已创建数据库目录: " + dataDir);
        }
        else {
            DbLog("创建数据库目录失败: " + dataDir);
            return false;
        }
    }

    currentHour = GetTimeStringByMode();  // 🔥 修改：根据模式获取时间字符串
    currentDbPath = dataDir + "/heartbeat_" + currentHour + ".db";  // 文件存储在 dataDir 中
    std::string modeDesc = (currentMode == StorageMode::HOURLY) ? "按小时" : "按天";
    DbLog("初始化数据库（" + modeDesc + "）: " + currentDbPath);

    int rc = sqlite3_open(currentDbPath.c_str(), &db);
    if (rc != SQLITE_OK) {
        DbLog("打开数据库失败: " + std::string(sqlite3_errmsg(db)));
        return false;
    }

    sqlite3_exec(db, "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA synchronous = NORMAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA journal_mode = WAL;", nullptr, nullptr, nullptr);

    if (!CreateTable()) {
        return false;
    }

    DbLog("数据库初始化成功（" + modeDesc + "）");
    return true;
}






bool DatabaseManager::InsertHeartbeatData(const HeartbeatRecord& record) {
    // 🔥 内存模式：存储到内存向量
    if (currentMode == StorageMode::MEMORY) {
        std::lock_guard<std::mutex> lock(memoryMutex);
        HeartbeatRecord memRecord = record;
        memRecord.id = nextMemoryId++;
        memRecord.createdAt = GetBeijingTimestamp();
        memoryRecords.push_back(memRecord);
        return true;
    }

    // 文件模式：存储到数据库
    std::lock_guard<std::mutex> lock(dbMutex);
    CheckAndSwitchDatabase();
    if (db == nullptr) {
        DbLog("数据库未初始化");
        return false;
    }

    const char* sql = R"(
        INSERT INTO heartbeat_data
        (game_id, timestamp, pattern_23_data, pattern_09_data, pattern_62_data, raw_data,
         packet_size, is_complete, process_type, fragment_count,
         packet_type, game_id_length, created_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
    )";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        DbLog("准备SQL语句失败: " + std::string(sqlite3_errmsg(db)));
        return false;
    }

    sqlite3_bind_text(stmt, 1, record.gameID.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, record.timestamp.c_str(), -1, SQLITE_TRANSIENT);

    if (!record.pattern23Data.empty()) {
        sqlite3_bind_blob(stmt, 3, record.pattern23Data.data(),
            static_cast<int>(record.pattern23Data.size()), SQLITE_TRANSIENT);
    }
    else {
        sqlite3_bind_null(stmt, 3);
    }

    if (!record.pattern09Data.empty()) {
        sqlite3_bind_blob(stmt, 4, record.pattern09Data.data(),
            static_cast<int>(record.pattern09Data.size()), SQLITE_TRANSIENT);
    }
    else {
        sqlite3_bind_null(stmt, 4);
    }

    // 新增：绑定62特征数据
    if (!record.pattern62Data.empty()) {
        sqlite3_bind_blob(stmt, 5, record.pattern62Data.data(),
            static_cast<int>(record.pattern62Data.size()), SQLITE_TRANSIENT);
    }
    else {
        sqlite3_bind_null(stmt, 5);
    }

    if (!record.rawData.empty()) {
        sqlite3_bind_blob(stmt, 6, record.rawData.data(),
            static_cast<int>(record.rawData.size()), SQLITE_TRANSIENT);
    }
    else {
        sqlite3_bind_null(stmt, 6);
    }

    sqlite3_bind_int(stmt, 7, record.packetSize);
    sqlite3_bind_int(stmt, 8, record.isComplete ? 1 : 0);
    sqlite3_bind_int(stmt, 9, record.processType);
    sqlite3_bind_int(stmt, 10, record.fragmentCount);
    sqlite3_bind_int(stmt, 11, record.packetType);
    sqlite3_bind_int(stmt, 12, record.gameIDLength);

    std::string createdAt = GetBeijingTimestamp();
    sqlite3_bind_text(stmt, 13, createdAt.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        DbLog("插入数据失败: " + std::string(sqlite3_errmsg(db)));
        return false;
    }
    return true;
}

std::vector<HeartbeatRecord> DatabaseManager::GetRecentHeartbeats(int limit) {
    std::lock_guard<std::mutex> lock(dbMutex);
    std::vector<HeartbeatRecord> results;
    if (db == nullptr) {
        DbLog("数据库未初始化");
        return results;
    }

    std::string sql = "SELECT id, game_id, timestamp, pattern_23_data, pattern_09_data, pattern_62_data, "
        "raw_data, packet_size, is_complete, process_type, fragment_count, "
        "packet_type, game_id_length, created_at "
        "FROM heartbeat_data ORDER BY id DESC LIMIT ?;";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        DbLog("查询准备失败: " + std::string(sqlite3_errmsg(db)));
        return results;
    }

    sqlite3_bind_int(stmt, 1, limit);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        HeartbeatRecord record;
        record.id = sqlite3_column_int(stmt, 0);
        record.gameID = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        record.timestamp = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));

        const void* blob23 = sqlite3_column_blob(stmt, 3);
        int blob23Size = sqlite3_column_bytes(stmt, 3);
        if (blob23 && blob23Size > 0) {
            const uint8_t* data = static_cast<const uint8_t*>(blob23);
            record.pattern23Data.assign(data, data + blob23Size);
        }

        const void* blob09 = sqlite3_column_blob(stmt, 4);
        int blob09Size = sqlite3_column_bytes(stmt, 4);
        if (blob09 && blob09Size > 0) {
            const uint8_t* data = static_cast<const uint8_t*>(blob09);
            record.pattern09Data.assign(data, data + blob09Size);
        }

        // 新增：读取62特征数据
        const void* blob62 = sqlite3_column_blob(stmt, 5);
        int blob62Size = sqlite3_column_bytes(stmt, 5);
        if (blob62 && blob62Size > 0) {
            const uint8_t* data = static_cast<const uint8_t*>(blob62);
            record.pattern62Data.assign(data, data + blob62Size);
        }

        const void* blobRaw = sqlite3_column_blob(stmt, 6);
        int blobRawSize = sqlite3_column_bytes(stmt, 6);
        if (blobRaw && blobRawSize > 0) {
            const uint8_t* data = static_cast<const uint8_t*>(blobRaw);
            record.rawData.assign(data, data + blobRawSize);
        }

        record.packetSize = sqlite3_column_int(stmt, 7);
        record.isComplete = sqlite3_column_int(stmt, 8) == 1;
        record.processType = sqlite3_column_int(stmt, 9);
        record.fragmentCount = sqlite3_column_int(stmt, 10);
        record.packetType = sqlite3_column_int(stmt, 11);
        record.gameIDLength = sqlite3_column_int(stmt, 12);
        record.createdAt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 13));

        results.push_back(record);
    }

    sqlite3_finalize(stmt);
    return results;
}

std::vector<HeartbeatRecord> DatabaseManager::GetHeartbeatsByGameID(const std::string& gameID) {
    std::lock_guard<std::mutex> lock(dbMutex);
    std::vector<HeartbeatRecord> results;
    if (db == nullptr) {
        DbLog("数据库未初始化");
        return results;
    }

    std::string sql = "SELECT id, game_id, timestamp, pattern_23_data, pattern_09_data, pattern_62_data, "
        "raw_data, packet_size, is_complete, process_type, fragment_count, "
        "packet_type, game_id_length, created_at "
        "FROM heartbeat_data WHERE game_id = ? ORDER BY id DESC;";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        DbLog("查询准备失败: " + std::string(sqlite3_errmsg(db)));
        return results;
    }

    sqlite3_bind_text(stmt, 1, gameID.c_str(), -1, SQLITE_TRANSIENT);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        HeartbeatRecord record;
        record.id = sqlite3_column_int(stmt, 0);
        record.gameID = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        record.timestamp = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));

        const void* blob23 = sqlite3_column_blob(stmt, 3);
        int blob23Size = sqlite3_column_bytes(stmt, 3);
        if (blob23 && blob23Size > 0) {
            const uint8_t* data = static_cast<const uint8_t*>(blob23);
            record.pattern23Data.assign(data, data + blob23Size);
        }

        const void* blob09 = sqlite3_column_blob(stmt, 4);
        int blob09Size = sqlite3_column_bytes(stmt, 4);
        if (blob09 && blob09Size > 0) {
            const uint8_t* data = static_cast<const uint8_t*>(blob09);
            record.pattern09Data.assign(data, data + blob09Size);
        }

        // 新增：读取62特征数据
        const void* blob62 = sqlite3_column_blob(stmt, 5);
        int blob62Size = sqlite3_column_bytes(stmt, 5);
        if (blob62 && blob62Size > 0) {
            const uint8_t* data = static_cast<const uint8_t*>(blob62);
            record.pattern62Data.assign(data, data + blob62Size);
        }

        const void* blobRaw = sqlite3_column_blob(stmt, 6);
        int blobRawSize = sqlite3_column_bytes(stmt, 6);
        if (blobRaw && blobRawSize > 0) {
            const uint8_t* data = static_cast<const uint8_t*>(blobRaw);
            record.rawData.assign(data, data + blobRawSize);
        }

        record.packetSize = sqlite3_column_int(stmt, 7);
        record.isComplete = sqlite3_column_int(stmt, 8) == 1;
        record.processType = sqlite3_column_int(stmt, 9);
        record.fragmentCount = sqlite3_column_int(stmt, 10);
        record.packetType = sqlite3_column_int(stmt, 11);
        record.gameIDLength = sqlite3_column_int(stmt, 12);
        record.createdAt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 13));

        results.push_back(record);
    }

    sqlite3_finalize(stmt);
    return results;
}

std::vector<HeartbeatRecord> DatabaseManager::GetHeartbeatsFromId(int startId, int limit) {
    // 🔥 内存模式：从内存获取
    if (currentMode == StorageMode::MEMORY) {
        return GetMemoryRecordsFromId(startId, limit);
    }

    std::lock_guard<std::mutex> lock(dbMutex);
    std::vector<HeartbeatRecord> results;
    if (db == nullptr) {
        DbLog("数据库未初始化");
        return results;
    }

    std::string sql = "SELECT id, game_id, timestamp, pattern_23_data, pattern_09_data, pattern_62_data, "
        "raw_data, packet_size, is_complete, process_type, fragment_count, "
        "packet_type, game_id_length, created_at "
        "FROM heartbeat_data WHERE id > ? ORDER BY id ASC LIMIT ?;";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        DbLog("查询准备失败: " + std::string(sqlite3_errmsg(db)));
        return results;
    }

    sqlite3_bind_int(stmt, 1, startId);
    sqlite3_bind_int(stmt, 2, limit);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        HeartbeatRecord record;
        record.id = sqlite3_column_int(stmt, 0);
        record.gameID = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        record.timestamp = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));

        const void* blob23 = sqlite3_column_blob(stmt, 3);
        int blob23Size = sqlite3_column_bytes(stmt, 3);
        if (blob23 && blob23Size > 0) {
            const uint8_t* data = static_cast<const uint8_t*>(blob23);
            record.pattern23Data.assign(data, data + blob23Size);
        }

        const void* blob09 = sqlite3_column_blob(stmt, 4);
        int blob09Size = sqlite3_column_bytes(stmt, 4);
        if (blob09 && blob09Size > 0) {
            const uint8_t* data = static_cast<const uint8_t*>(blob09);
            record.pattern09Data.assign(data, data + blob09Size);
        }

        // 新增：读取62特征数据
        const void* blob62 = sqlite3_column_blob(stmt, 5);
        int blob62Size = sqlite3_column_bytes(stmt, 5);
        if (blob62 && blob62Size > 0) {
            const uint8_t* data = static_cast<const uint8_t*>(blob62);
            record.pattern62Data.assign(data, data + blob62Size);
        }

        const void* blobRaw = sqlite3_column_blob(stmt, 6);
        int blobRawSize = sqlite3_column_bytes(stmt, 6);
        if (blobRaw && blobRawSize > 0) {
            const uint8_t* data = static_cast<const uint8_t*>(blobRaw);
            record.rawData.assign(data, data + blobRawSize);
        }

        record.packetSize = sqlite3_column_int(stmt, 7);
        record.isComplete = sqlite3_column_int(stmt, 8) == 1;
        record.processType = sqlite3_column_int(stmt, 9);
        record.fragmentCount = sqlite3_column_int(stmt, 10);
        record.packetType = sqlite3_column_int(stmt, 11);
        record.gameIDLength = sqlite3_column_int(stmt, 12);
        record.createdAt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 13));

        results.push_back(record);
    }

    sqlite3_finalize(stmt);
    return results;
}

int DatabaseManager::GetMaxHeartbeatId() {
    // 🔥 内存模式：从内存获取
    if (currentMode == StorageMode::MEMORY) {
        return GetMemoryMaxId();
    }

    std::lock_guard<std::mutex> lock(dbMutex);
    if (db == nullptr) {
        DbLog("数据库未初始化");
        return 0;
    }

    const char* sql = "SELECT IFNULL(MAX(id), 0) FROM heartbeat_data;";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        DbLog("查询最大ID失败: " + std::string(sqlite3_errmsg(db)));
        return 0;
    }

    int maxId = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        maxId = sqlite3_column_int(stmt, 0);
    }

    sqlite3_finalize(stmt);
    return maxId;
}

int DatabaseManager::GetHeartbeatCountByLength(int length) {
    // 内存模式
    if (currentMode == StorageMode::MEMORY) {
        std::lock_guard<std::mutex> lock(memoryMutex);
        int count = 0;
        for (const auto& record : memoryRecords) {
            if (static_cast<int>(record.gameID.length()) == length) {
                count++;
            }
        }
        return count;
    }

    // 数据库模式
    std::lock_guard<std::mutex> lock(dbMutex);
    if (db == nullptr) {
        return 0;
    }

    const char* sql = "SELECT COUNT(*) FROM heartbeat_data WHERE LENGTH(game_id) = ?;";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return 0;
    }

    sqlite3_bind_int(stmt, 1, length);

    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }

    sqlite3_finalize(stmt);
    return count;
}

std::vector<HeartbeatRecord> DatabaseManager::GetHeartbeatsByLength(int length, int startId, int endId) {
    std::vector<HeartbeatRecord> results;

    // 内存模式
    if (currentMode == StorageMode::MEMORY) {
        std::lock_guard<std::mutex> lock(memoryMutex);
        for (const auto& record : memoryRecords) {
            if (static_cast<int>(record.gameID.length()) == length &&
                record.id >= startId && record.id <= endId) {
                results.push_back(record);
            }
        }
        return results;
    }

    // 数据库模式
    std::lock_guard<std::mutex> lock(dbMutex);
    if (db == nullptr) {
        return results;
    }

    const char* sql = "SELECT id, game_id, timestamp, pattern_23_data, pattern_09_data, pattern_62_data, raw_data, "
                      "packet_size, is_complete, process_type, fragment_count, packet_type, game_id_length, created_at "
                      "FROM heartbeat_data WHERE LENGTH(game_id) = ? AND id >= ? AND id <= ? "
                      "ORDER BY id ASC;";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return results;
    }

    sqlite3_bind_int(stmt, 1, length);
    sqlite3_bind_int(stmt, 2, startId);
    sqlite3_bind_int(stmt, 3, endId);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        HeartbeatRecord record;
        record.id = sqlite3_column_int(stmt, 0);
        record.gameID = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        record.timestamp = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));

        const void* blob23 = sqlite3_column_blob(stmt, 3);
        int blob23Size = sqlite3_column_bytes(stmt, 3);
        if (blob23 && blob23Size > 0) {
            const uint8_t* data = static_cast<const uint8_t*>(blob23);
            record.pattern23Data.assign(data, data + blob23Size);
        }

        const void* blob09 = sqlite3_column_blob(stmt, 4);
        int blob09Size = sqlite3_column_bytes(stmt, 4);
        if (blob09 && blob09Size > 0) {
            const uint8_t* data = static_cast<const uint8_t*>(blob09);
            record.pattern09Data.assign(data, data + blob09Size);
        }

        const void* blob62 = sqlite3_column_blob(stmt, 5);
        int blob62Size = sqlite3_column_bytes(stmt, 5);
        if (blob62 && blob62Size > 0) {
            const uint8_t* data = static_cast<const uint8_t*>(blob62);
            record.pattern62Data.assign(data, data + blob62Size);
        }

        const void* blobRaw = sqlite3_column_blob(stmt, 6);
        int blobRawSize = sqlite3_column_bytes(stmt, 6);
        if (blobRaw && blobRawSize > 0) {
            const uint8_t* data = static_cast<const uint8_t*>(blobRaw);
            record.rawData.assign(data, data + blobRawSize);
        }

        record.packetSize = sqlite3_column_int(stmt, 7);
        record.isComplete = sqlite3_column_int(stmt, 8) == 1;
        record.processType = sqlite3_column_int(stmt, 9);
        record.fragmentCount = sqlite3_column_int(stmt, 10);
        record.packetType = sqlite3_column_int(stmt, 11);
        record.gameIDLength = sqlite3_column_int(stmt, 12);
        record.createdAt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 13));
        results.push_back(record);
    }

    sqlite3_finalize(stmt);
    return results;
}

void DatabaseManager::Close() {
    std::lock_guard<std::mutex> lock(dbMutex);
    if (db != nullptr) {
        sqlite3_close(db);
        db = nullptr;
        DbLog("数据库已关闭");
    }
}




// ===== 从外部数据库文件加载数据 =====
std::vector<HeartbeatRecord> DatabaseManager::LoadFromExternalDatabase(const std::string& dbPath) {
    std::vector<HeartbeatRecord> results;

    // 检查文件是否存在
    std::ifstream fileCheck(dbPath);
    if (!fileCheck.good()) {
        DbLog("外部数据库文件不存在: " + dbPath);
        return results;
    }
    fileCheck.close();

    // 打开外部数据库
    sqlite3* externalDb = nullptr;
    int rc = sqlite3_open(dbPath.c_str(), &externalDb);

    if (rc != SQLITE_OK) {
        DbLog("无法打开外部数据库: " + dbPath + " - " +
            std::string(sqlite3_errmsg(externalDb)));
        if (externalDb) sqlite3_close(externalDb);
        return results;
    }

    DbLog("成功打开外部数据库: " + dbPath);

    // 尝试使用新的SQL（包含pattern_62_data），如果失败则回退到旧的SQL
    std::string sql = "SELECT id, game_id, timestamp, pattern_23_data, pattern_09_data, pattern_62_data, "
        "raw_data, packet_size, is_complete, process_type, fragment_count, "
        "packet_type, game_id_length, created_at "
        "FROM heartbeat_data ORDER BY id DESC LIMIT 15000;";

    sqlite3_stmt* stmt = nullptr;
    rc = sqlite3_prepare_v2(externalDb, sql.c_str(), -1, &stmt, nullptr);

    bool hasPattern62Column = (rc == SQLITE_OK);

    if (!hasPattern62Column) {
        // 回退到旧的SQL（不包含pattern_62_data）
        sql = "SELECT id, game_id, timestamp, pattern_23_data, pattern_09_data, "
            "raw_data, packet_size, is_complete, process_type, fragment_count, "
            "packet_type, game_id_length, created_at "
            "FROM heartbeat_data ORDER BY id DESC LIMIT 15000;";
        rc = sqlite3_prepare_v2(externalDb, sql.c_str(), -1, &stmt, nullptr);
    }

    if (rc != SQLITE_OK) {
        DbLog("查询外部数据库失败: " + std::string(sqlite3_errmsg(externalDb)));
        sqlite3_close(externalDb);
        return results;
    }

    // 读取数据
    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        HeartbeatRecord record;
        record.id = sqlite3_column_int(stmt, 0);
        record.gameID = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        record.timestamp = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));

        // 读取 pattern_23_data
        const void* blob23 = sqlite3_column_blob(stmt, 3);
        int blob23Size = sqlite3_column_bytes(stmt, 3);
        if (blob23 && blob23Size > 0) {
            const uint8_t* data = static_cast<const uint8_t*>(blob23);
            record.pattern23Data.assign(data, data + blob23Size);
        }

        // 读取 pattern_09_data
        const void* blob09 = sqlite3_column_blob(stmt, 4);
        int blob09Size = sqlite3_column_bytes(stmt, 4);
        if (blob09 && blob09Size > 0) {
            const uint8_t* data = static_cast<const uint8_t*>(blob09);
            record.pattern09Data.assign(data, data + blob09Size);
        }

        if (hasPattern62Column) {
            // 读取 pattern_62_data（仅当列存在时）
            const void* blob62 = sqlite3_column_blob(stmt, 5);
            int blob62Size = sqlite3_column_bytes(stmt, 5);
            if (blob62 && blob62Size > 0) {
                const uint8_t* data = static_cast<const uint8_t*>(blob62);
                record.pattern62Data.assign(data, data + blob62Size);
            }

            // 读取 raw_data（索引+1）
            const void* blobRaw = sqlite3_column_blob(stmt, 6);
            int blobRawSize = sqlite3_column_bytes(stmt, 6);
            if (blobRaw && blobRawSize > 0) {
                const uint8_t* data = static_cast<const uint8_t*>(blobRaw);
                record.rawData.assign(data, data + blobRawSize);
            }

            record.packetSize = sqlite3_column_int(stmt, 7);
            record.isComplete = sqlite3_column_int(stmt, 8) == 1;
            record.processType = sqlite3_column_int(stmt, 9);
            record.fragmentCount = sqlite3_column_int(stmt, 10);
            record.packetType = sqlite3_column_int(stmt, 11);
            record.gameIDLength = sqlite3_column_int(stmt, 12);
            record.createdAt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 13));
        }
        else {
            // 旧数据库格式（无pattern_62_data列）
            const void* blobRaw = sqlite3_column_blob(stmt, 5);
            int blobRawSize = sqlite3_column_bytes(stmt, 5);
            if (blobRaw && blobRawSize > 0) {
                const uint8_t* data = static_cast<const uint8_t*>(blobRaw);
                record.rawData.assign(data, data + blobRawSize);
            }

            record.packetSize = sqlite3_column_int(stmt, 6);
            record.isComplete = sqlite3_column_int(stmt, 7) == 1;
            record.processType = sqlite3_column_int(stmt, 8);
            record.fragmentCount = sqlite3_column_int(stmt, 9);
            record.packetType = sqlite3_column_int(stmt, 10);
            record.gameIDLength = sqlite3_column_int(stmt, 11);
            record.createdAt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 12));
        }

        results.push_back(record);
        count++;
    }

    sqlite3_finalize(stmt);
    sqlite3_close(externalDb);

    DbLog("从外部数据库读取了 " + std::to_string(count) + " 条记录");

    return results;
}

// ========== 配置管理实现 ==========

bool DatabaseManager::InitializeConfig(const std::string& configPath) {
    std::lock_guard<std::mutex> lock(configMutex);

    configDbPath = configPath;

    int rc = sqlite3_open(configDbPath.c_str(), &configDb);
    if (rc != SQLITE_OK) {
        DbLog("无法打开配置数据库: " + std::string(sqlite3_errmsg(configDb)));
        configDb = nullptr;
        return false;
    }

    // 优化SQLite性能
    sqlite3_exec(configDb, "PRAGMA synchronous = NORMAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(configDb, "PRAGMA journal_mode = WAL;", nullptr, nullptr, nullptr);

    if (!CreateConfigTable()) {
        sqlite3_close(configDb);
        configDb = nullptr;
        return false;
    }

    // 🔥 创建用户滤镜配置表
    if (!CreateUserFilterConfigTable()) {
        DbLog("创建用户滤镜配置表失败，但继续运行");
    }

    if (!CreateWPEFilterGroupTables()) {
        DbLog("创建WPE滤镜组表失败，但继续运行");
    }

    // 🔥 创建滤镜执行次数统计表
    if (!CreateFilterExecutionStatsTable()) {
        DbLog("创建滤镜执行次数统计表失败，但继续运行");
    }

    DbLog("配置数据库初始化成功: " + configDbPath);
    return true;
}

bool DatabaseManager::CreateConfigTable() {
    if (!configDb) return false;

    const char* createTableSQL = R"(
        CREATE TABLE IF NOT EXISTS config_settings (
            key TEXT PRIMARY KEY,
            value TEXT NOT NULL,
            updated_at TEXT NOT NULL
        );
    )";

    char* errMsg = nullptr;
    int rc = sqlite3_exec(configDb, createTableSQL, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        DbLog("创建配置表失败: " + std::string(errMsg));
        sqlite3_free(errMsg);
        return false;
    }

    return true;
}

bool DatabaseManager::SetConfigValue(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(configMutex);
    if (!configDb) return false;

    const char* sql = R"(
        INSERT OR REPLACE INTO config_settings (key, value, updated_at)
        VALUES (?, ?, datetime('now', '+8 hours'));
    )";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(configDb, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return (rc == SQLITE_DONE);
}

std::string DatabaseManager::GetConfigValue(const std::string& key, const std::string& defaultValue) {
    std::lock_guard<std::mutex> lock(configMutex);
    if (!configDb) return defaultValue;

    const char* sql = "SELECT value FROM config_settings WHERE key = ?;";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(configDb, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return defaultValue;
    }

    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);

    std::string result = defaultValue;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* value = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (value) {
            result = value;
        }
    }

    sqlite3_finalize(stmt);
    return result;
}

bool DatabaseManager::DeleteConfigValue(const std::string& key) {
    std::lock_guard<std::mutex> lock(configMutex);
    if (!configDb) return false;

    const char* sql = "DELETE FROM config_settings WHERE key = ?;";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(configDb, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return (rc == SQLITE_DONE);
}

// ========== 保存完整应用配置 ==========
bool DatabaseManager::SaveAppConfig(const AppConfig& config) {
    if (!configDb) return false;

    // 开启事务提高性能
    sqlite3_exec(configDb, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);

    // ===== 🔥 存储模式配置 =====
    SetConfigValue("storage_mode", std::to_string(static_cast<int>(config.storageMode)));
    SetConfigValue("custom_save_path", config.customSavePath);

    // ===== 🔥 伪心跳自动加载模式 =====
    SetConfigValue("heartbeat_load_mode", std::to_string(static_cast<int>(config.heartbeatLoadMode)));

    // 采集逻辑配置
    SetConfigValue("collector_port", std::to_string(config.collectorPort));
    SetConfigValue("enable_recording", config.enableRecording ? "1" : "0");
    SetConfigValue("filter_type", std::to_string(config.filterType));
    SetConfigValue("filter_value", config.filterValue);
    SetConfigValue("disable_packet_header_filter", config.disablePacketHeaderFilter ? "1" : "0");
    SetConfigValue("allow_collect_00id", config.allowCollect00ID ? "1" : "0");
    SetConfigValue("allow_collect_obid", config.allowCollectOBID ? "1" : "0");  // ===== 🔥 新增 =====
    SetConfigValue("enable_collect_62_pattern", config.enableCollect62Pattern ? "1" : "0");  // ===== 🔥 新增：62特征采集开关 =====
    SetConfigValue("enable_disconnect_auto_clear", config.enableDisconnectAutoClear ? "1" : "0");  // 断开自动清理
    SetConfigValue("enable_secondary_proxy", config.enableSecondaryProxy ? "1" : "0");
    SetConfigValue("secondary_proxy_host", config.secondaryProxyHost);
    SetConfigValue("secondary_proxy_port", config.secondaryProxyPort);
    SetConfigValue("collector_packet_types", config.collectorPacketTypes);
    SetConfigValue("enable_pos10_replace", config.enablePos10Replace ? "1" : "0");  // ===== 🔥 新增 =====
    SetConfigValue("enable_pos0109_minus8_replace", config.enablePos0109Minus8Replace ? "1" : "0");  // ===== 🔥 新增 =====

    // 保存采集包头启用状态
    for (const auto& pair : config.collectorTypeEnabled) {
        std::string key = "collector_type_enabled_" + std::to_string(pair.first);
        SetConfigValue(key, pair.second ? "1" : "0");
    }

    // 保存采集显示过滤状态
    for (const auto& pair : config.collectorDisplayFilter) {
        std::string key = "collector_display_filter_" + std::to_string(pair.first);
        SetConfigValue(key, pair.second ? "1" : "0");
    }

    // 伪心跳配置
    SetConfigValue("heartbeat_port", std::to_string(config.heartbeatPort));
    SetConfigValue("enable_heartbeat_recording", config.enableHeartbeatRecording ? "1" : "0");
    SetConfigValue("enable_heartbeat_forward", config.enableHeartbeatForward ? "1" : "0");
    SetConfigValue("disable_heartbeat_header_filter", config.disableHeartbeatHeaderFilter ? "1" : "0");
    SetConfigValue("allow_heartbeat_00id", config.allowHeartbeat00ID ? "1" : "0");
    SetConfigValue("allow_heartbeat_obid", config.allowHeartbeatOBID ? "1" : "0");  // ===== 🔥 新增 =====
    SetConfigValue("enable_heartbeat_62_pattern", config.enableHeartbeat62Pattern ? "1" : "0");  // ===== 🔥 新增：62特征伪心跳开关 =====
    SetConfigValue("pattern23_mode", std::to_string(config.pattern23Mode));
    SetConfigValue("pattern23_dynamic_offset", std::to_string(config.pattern23DynamicOffset));
    SetConfigValue("enable_0109_front_replace", config.enable0109FrontReplace ? "1" : "0");
    SetConfigValue("enable_0109_back_replace", config.enable0109BackReplace ? "1" : "0");
    SetConfigValue("enable_0109_bytes567_replace", config.enable0109Bytes567Replace ? "1" : "0");
    SetConfigValue("use_single_heartbeat_packet", config.useSingleHeartbeatPacket ? "1" : "0");
    SetConfigValue("enable_vtd_filter", config.enableVTDFilter ? "1" : "0");
    SetConfigValue("heartbeat_filter_type", std::to_string(config.heartbeatFilterType));
    SetConfigValue("heartbeat_filter_value", config.heartbeatFilterValue);
    SetConfigValue("heartbeat_packet_types", config.heartbeatPacketTypes);
    SetConfigValue("use_same_algorithm", config.useSameAlgorithm ? "1" : "0");  // 🔥 新增：算法选择配置
    SetConfigValue("packet_order_mode", std::to_string(config.packetOrderMode));  // 🔥 新增：数据包读取顺序模式
    SetConfigValue("heartbeat_replace_limit", std::to_string(config.heartbeatReplaceLimit));  // 🔥 新增：替换数量限制
    SetConfigValue("enable_limit_cleanup", config.enableLimitCleanup ? "1" : "0");  // 🔥 新增：达到限制后清理数据

    // 保存伪心跳包头启用状态
    for (const auto& pair : config.heartbeatTypeEnabled) {
        std::string key = "heartbeat_type_enabled_" + std::to_string(pair.first);
        SetConfigValue(key, pair.second ? "1" : "0");
    }

    // 时间配置
    SetConfigValue("heartbeat_load_interval", std::to_string(config.heartbeatLoadInterval));
    SetConfigValue("heartbeat_expire_time", std::to_string(config.heartbeatExpireTime));
    SetConfigValue("heartbeat_cleanup_interval", std::to_string(config.heartbeatCleanupInterval));

    // 清理配置
    SetConfigValue("cleanup_id_length_17", config.cleanupIDLength17 ? "1" : "0");
    SetConfigValue("cleanup_id_length_18", config.cleanupIDLength18 ? "1" : "0");
    SetConfigValue("cleanup_id_length_19", config.cleanupIDLength19 ? "1" : "0");
    SetConfigValue("cleanup_id_length_20", config.cleanupIDLength20 ? "1" : "0");
    SetConfigValue("cleanup_id_length_21", config.cleanupIDLength21 ? "1" : "0");
    SetConfigValue("cleanup_id_length_22", config.cleanupIDLength22 ? "1" : "0");

    // 日志配置
    SetConfigValue("auto_scroll_log", config.autoScrollLog ? "1" : "0");
    SetConfigValue("enable_logging", config.enableLogging ? "1" : "0");

    // ===== 🔥 新增：TCP文件服务器配置 =====
    SetConfigValue("tcp_file_server_port", std::to_string(config.tcpFileServerPort));

    // ===== 🔥 新增：远程同步配置 =====
    SetConfigValue("enable_remote_sync", config.enableRemoteSync ? "1" : "0");
    SetConfigValue("remote_server_host", config.remoteServerHost);
    SetConfigValue("remote_server_port", std::to_string(config.remoteServerPort));
    SetConfigValue("remote_sync_interval", std::to_string(config.remoteSyncInterval));

    // 提交事务
    sqlite3_exec(configDb, "COMMIT;", nullptr, nullptr, nullptr);

    DbLog("应用配置已保存到: " + configDbPath);
    return true;
}




// ========== 辅助函数：安全字符串转换 ==========
static int SafeStringToInt(const std::string& str, int defaultValue) {
    if (str.empty()) return defaultValue;
    try {
        return std::stoi(str);
    }
    catch (...) {
        return defaultValue;
    }
}

static bool SafeStringToBool(const std::string& str, bool defaultValue) {
    if (str.empty()) return defaultValue;
    return (str == "1" || str == "true");
}

// ========== 加载完整应用配置 ==========
// ========== 加载完整应用配置 ==========
AppConfig DatabaseManager::LoadAppConfig() {
    AppConfig config;

    if (!configDb) {
        DbLog("配置数据库未初始化，返回默认配置");
        return config;
    }

    // ===== 🔥 存储模式配置 =====
    int storageModeInt = SafeStringToInt(GetConfigValue("storage_mode", "0"), 0);
    config.storageMode = static_cast<StorageMode>(storageModeInt);
    config.customSavePath = GetConfigValue("custom_save_path", "");

    // ===== 🔥 伪心跳自动加载模式 =====
    int heartbeatLoadModeInt = SafeStringToInt(GetConfigValue("heartbeat_load_mode", "0"), 0);
    config.heartbeatLoadMode = static_cast<StorageMode>(heartbeatLoadModeInt);

    // 采集逻辑配置
    config.collectorPort = SafeStringToInt(GetConfigValue("collector_port", "1080"), 1080);
    config.enableRecording = SafeStringToBool(GetConfigValue("enable_recording", "1"), true);
    config.filterType = SafeStringToInt(GetConfigValue("filter_type", "0"), 0);
    config.filterValue = GetConfigValue("filter_value", "");
    config.disablePacketHeaderFilter = SafeStringToBool(GetConfigValue("disable_packet_header_filter", "0"), false);
    config.allowCollect00ID = SafeStringToBool(GetConfigValue("allow_collect_00id", "0"), false);
    config.allowCollectOBID = SafeStringToBool(GetConfigValue("allow_collect_obid", "0"), false);  // ===== 🔥 新增 =====
    config.enableCollect62Pattern = SafeStringToBool(GetConfigValue("enable_collect_62_pattern", "0"), false);  // ===== 🔥 新增：62特征采集开关 =====
    config.enableDisconnectAutoClear = SafeStringToBool(GetConfigValue("enable_disconnect_auto_clear", "1"), true);  // 断开自动清理
    config.enableSecondaryProxy = SafeStringToBool(GetConfigValue("enable_secondary_proxy", "0"), false);
    config.secondaryProxyHost = GetConfigValue("secondary_proxy_host", "127.0.0.1");
    config.secondaryProxyPort = GetConfigValue("secondary_proxy_port", "10808");
    config.collectorPacketTypes = GetConfigValue("collector_packet_types", "1,2,3,4");
    config.enablePos10Replace = SafeStringToBool(GetConfigValue("enable_pos10_replace", "0"), false);  // ===== 🔥 新增 =====
    config.enablePos0109Minus8Replace = SafeStringToBool(GetConfigValue("enable_pos0109_minus8_replace", "0"), false);  // ===== 🔥 新增 =====

    // 解析采集包头类型
    std::vector<int> collectorTypes;
    std::stringstream ss1(config.collectorPacketTypes);
    std::string item;
    while (std::getline(ss1, item, ',')) {
        int type = SafeStringToInt(item, -1);
        if (type >= 0 && type <= 255) {
            collectorTypes.push_back(type);
            std::string key = "collector_type_enabled_" + std::to_string(type);
            config.collectorTypeEnabled[type] = SafeStringToBool(GetConfigValue(key, "1"), true);

            key = "collector_display_filter_" + std::to_string(type);
            config.collectorDisplayFilter[type] = SafeStringToBool(GetConfigValue(key, "1"), true);
        }
    }

    // 伪心跳配置
    config.heartbeatPort = SafeStringToInt(GetConfigValue("heartbeat_port", "1081"), 1081);
    config.enableHeartbeatRecording = SafeStringToBool(GetConfigValue("enable_heartbeat_recording", "1"), true);
    config.enableHeartbeatForward = SafeStringToBool(GetConfigValue("enable_heartbeat_forward", "0"), false);
    config.disableHeartbeatHeaderFilter = SafeStringToBool(GetConfigValue("disable_heartbeat_header_filter", "0"), false);
    config.allowHeartbeat00ID = SafeStringToBool(GetConfigValue("allow_heartbeat_00id", "0"), false);
    config.allowHeartbeatOBID = SafeStringToBool(GetConfigValue("allow_heartbeat_obid", "0"), false);  // ===== 🔥 新增 =====
    config.enableHeartbeat62Pattern = SafeStringToBool(GetConfigValue("enable_heartbeat_62_pattern", "0"), false);  // ===== 🔥 新增：62特征伪心跳开关 =====
    config.pattern23Mode = SafeStringToInt(GetConfigValue("pattern23_mode", "0"), 0);
    config.pattern23DynamicOffset = SafeStringToInt(GetConfigValue("pattern23_dynamic_offset", "0"), 0);
    config.enable0109FrontReplace = SafeStringToBool(GetConfigValue("enable_0109_front_replace", "0"), false);
    config.enable0109BackReplace = SafeStringToBool(GetConfigValue("enable_0109_back_replace", "0"), false);
    config.enable0109Bytes567Replace = SafeStringToBool(GetConfigValue("enable_0109_bytes567_replace", "0"), false);
    config.useSingleHeartbeatPacket = SafeStringToBool(GetConfigValue("use_single_heartbeat_packet", "0"), false);
    config.enableVTDFilter = SafeStringToBool(GetConfigValue("enable_vtd_filter", "0"), false);
    config.heartbeatFilterType = SafeStringToInt(GetConfigValue("heartbeat_filter_type", "0"), 0);
    config.heartbeatFilterValue = GetConfigValue("heartbeat_filter_value", "");
    config.heartbeatPacketTypes = GetConfigValue("heartbeat_packet_types", "1,2,3,4");
    config.useSameAlgorithm = SafeStringToBool(GetConfigValue("use_same_algorithm", "1"), true);  // 🔥 新增：算法选择配置
    config.packetOrderMode = SafeStringToInt(GetConfigValue("packet_order_mode", "0"), 0);  // 🔥 新增：数据包读取顺序模式
    config.heartbeatReplaceLimit = SafeStringToInt(GetConfigValue("heartbeat_replace_limit", "0"), 0);  // 🔥 新增：替换数量限制
    config.enableLimitCleanup = SafeStringToBool(GetConfigValue("enable_limit_cleanup", "0"), false);  // 🔥 新增：达到限制后清理数据

    // 解析伪心跳包头类型
    std::stringstream ss2(config.heartbeatPacketTypes);
    while (std::getline(ss2, item, ',')) {
        int type = SafeStringToInt(item, -1);
        if (type >= 0 && type <= 255) {
            std::string key = "heartbeat_type_enabled_" + std::to_string(type);
            config.heartbeatTypeEnabled[type] = SafeStringToBool(GetConfigValue(key, "1"), true);
        }
    }

    // 时间配置
    config.heartbeatLoadInterval = SafeStringToInt(GetConfigValue("heartbeat_load_interval", "180"), 180);
    config.heartbeatExpireTime = SafeStringToInt(GetConfigValue("heartbeat_expire_time", "1200"), 1200);
    config.heartbeatCleanupInterval = SafeStringToInt(GetConfigValue("heartbeat_cleanup_interval", "60"), 60);

    // 清理配置
    config.cleanupIDLength17 = SafeStringToBool(GetConfigValue("cleanup_id_length_17", "0"), false);
    config.cleanupIDLength18 = SafeStringToBool(GetConfigValue("cleanup_id_length_18", "0"), false);
    config.cleanupIDLength19 = SafeStringToBool(GetConfigValue("cleanup_id_length_19", "1"), true);
    config.cleanupIDLength20 = SafeStringToBool(GetConfigValue("cleanup_id_length_20", "1"), true);
    config.cleanupIDLength21 = SafeStringToBool(GetConfigValue("cleanup_id_length_21", "0"), false);
    config.cleanupIDLength22 = SafeStringToBool(GetConfigValue("cleanup_id_length_22", "0"), false);

    // 日志配置
    config.autoScrollLog = SafeStringToBool(GetConfigValue("auto_scroll_log", "1"), true);
    config.enableLogging = SafeStringToBool(GetConfigValue("enable_logging", "1"), true);

    // ===== 🔥 新增：TCP文件服务器配置 =====
    config.tcpFileServerPort = SafeStringToInt(GetConfigValue("tcp_file_server_port", "9000"), 9000);

    // ===== 🔥 新增：远程同步配置 =====
    config.enableRemoteSync = SafeStringToBool(GetConfigValue("enable_remote_sync", "0"), false);
    config.remoteServerHost = GetConfigValue("remote_server_host", "127.0.0.1");
    config.remoteServerPort = SafeStringToInt(GetConfigValue("remote_server_port", "9000"), 9000);
    config.remoteSyncInterval = SafeStringToInt(GetConfigValue("remote_sync_interval", "300"), 300);

    DbLog("应用配置已从 " + configDbPath + " 加载");
    return config;
}



// 创建数据库快照用于传输（使用 SQLite Backup API）
bool DatabaseManager::CreateSnapshotForTransfer(const std::string& snapshotPath) {
    // 🔥 内存模式：先将内存数据保存到快照文件
    if (currentMode == StorageMode::MEMORY) {
        DbLog("数据库快照 - 内存模式，直接保存内存数据到快照");
        return SaveMemoryToFile(snapshotPath);
    }

    // 1. 只在获取数据库指针时短暂加锁
    sqlite3* srcDb = nullptr;
    {
        std::lock_guard<std::mutex> lock(dbMutex);
        if (!db) {
            DbLog("数据库快照 - 数据库未打开");
            return false;
        }
        srcDb = db;
    }
    // 锁已释放，后续操作不会阻塞写入

    // 删除旧的快照文件（如果存在）
    if (std::filesystem::exists(snapshotPath)) {
        DbLog("数据库快照 - 删除旧快照文件: " + snapshotPath);
        if (!DeleteFileA(snapshotPath.c_str())) {
            DWORD error = GetLastError();
            DbLog("数据库快照 - 删除旧快照失败，错误码: " + std::to_string(error));
        }
    }

    DbLog("数据库快照 - 开始创建快照: " + snapshotPath);
    DbLog("数据库快照 - 使用无阻塞模式，采集写入可以继续");

    // 记录开始时间
    auto startTime = std::chrono::steady_clock::now();

    // 2. 打开目标数据库（不需要锁）
    sqlite3* pDestDb = nullptr;
    int rc = sqlite3_open(snapshotPath.c_str(), &pDestDb);

    if (rc != SQLITE_OK) {
        std::string errorMsg = pDestDb ? sqlite3_errmsg(pDestDb) : "无法创建数据库文件";
        DbLog("数据库快照 - 无法创建目标数据库: " + errorMsg);
        if (pDestDb) {
            sqlite3_close(pDestDb);
        }
        return false;
    }

    DbLog("数据库快照 - 目标数据库已打开");

    // 3. 初始化备份（SQLite Backup API 内部处理并发）
    sqlite3_backup* pBackup = sqlite3_backup_init(pDestDb, "main", srcDb, "main");

    if (!pBackup) {
        std::string errorMsg = sqlite3_errmsg(pDestDb);
        DbLog("数据库快照 - 备份初始化失败: " + errorMsg);
        sqlite3_close(pDestDb);
        DeleteFileA(snapshotPath.c_str());
        return false;
    }

    DbLog("数据库快照 - 备份已初始化，开始复制数据...");

    // 4. 分步备份 - 每次复制少量页面，允许写入操作插入
    int lastLoggedPercent = -1;
    int retryCount = 0;
    const int maxRetries = 100;  // 最大重试次数

    do {
        // 每次复制 50 页（减少单次操作时间，提高并发性）
        rc = sqlite3_backup_step(pBackup, 50);

        int remaining = sqlite3_backup_remaining(pBackup);
        int total = sqlite3_backup_pagecount(pBackup);

        if (total > 0) {
            int copied = total - remaining;
            double progress = (copied * 100.0) / total;
            int currentPercent = static_cast<int>(progress);

            // 每进步10%记录一次日志
            if (currentPercent >= lastLoggedPercent + 10) {
                DbLog("数据库快照 - 复制进度: " + std::to_string(currentPercent) +
                    "% (" + std::to_string(copied) + "/" + std::to_string(total) + " 页)");
                lastLoggedPercent = currentPercent;
            }
        }

        // 处理不同返回码
        if (rc == SQLITE_OK) {
            // 正常进行中，短暂休眠让出CPU给写入操作
            Sleep(1);
            retryCount = 0;
        }
        else if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
            // 数据库忙（有写入操作），等待后重试
            retryCount++;
            if (retryCount > maxRetries) {
                DbLog("数据库快照 - 数据库持续繁忙，已重试 " + std::to_string(maxRetries) + " 次");
                // 不中断，继续尝试
                retryCount = 0;
            }
            Sleep(10);  // 等待10毫秒后重试
        }
        else if (rc == SQLITE_DONE) {
            // 备份完成
            break;
        }
        else {
            // 其他错误
            std::string errorMsg = sqlite3_errmsg(pDestDb);
            DbLog("数据库快照 - 备份步骤失败: " + errorMsg + " (错误码: " + std::to_string(rc) + ")");
            break;
        }

    } while (rc == SQLITE_OK || rc == SQLITE_BUSY || rc == SQLITE_LOCKED);

    // 5. 检查备份结果
    if (rc != SQLITE_DONE) {
        std::string errorMsg = sqlite3_errmsg(pDestDb);
        DbLog("数据库快照 - 备份失败: " + errorMsg + " (错误码: " + std::to_string(rc) + ")");
        sqlite3_backup_finish(pBackup);
        sqlite3_close(pDestDb);
        DeleteFileA(snapshotPath.c_str());
        return false;
    }

    DbLog("数据库快照 - 数据复制完成，正在完成备份...");

    // 6. 完成备份
    int finishResult = sqlite3_backup_finish(pBackup);

    if (finishResult != SQLITE_OK) {
        std::string errorMsg = sqlite3_errmsg(pDestDb);
        DbLog("数据库快照 - 备份完成时出现警告: " + errorMsg + " (错误码: " + std::to_string(finishResult) + ")");
    }

    // 7. 关闭目标数据库
    rc = sqlite3_close(pDestDb);
    if (rc != SQLITE_OK) {
        DbLog("数据库快照 - 关闭目标数据库时出现警告 (错误码: " + std::to_string(rc) + ")");
    }

    // 8. 计算耗时
    auto endTime = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();

    // 9. 验证快照文件
    std::ifstream file(snapshotPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        DbLog("数据库快照 - 无法验证快照文件");
        DeleteFileA(snapshotPath.c_str());
        return false;
    }

    std::streamsize fileSize = file.tellg();
    file.close();

    if (fileSize <= 0) {
        DbLog("数据库快照 - 快照文件大小为0或异常: " + std::to_string(fileSize));
        DeleteFileA(snapshotPath.c_str());
        return false;
    }

    DbLog("数据库快照 - 快照创建成功！");
    DbLog("数据库快照 - 文件大小: " + std::to_string(fileSize) + " 字节 (" +
        std::to_string(fileSize / 1024.0 / 1024.0) + " MB)");
    DbLog("数据库快照 - 耗时: " + std::to_string(duration) + " 毫秒 (" +
        std::to_string(duration / 1000.0) + " 秒)");
    DbLog("数据库快照 - 备份期间采集写入未被阻塞");

    return true;
}



// ===== 🔥 新增：从指定数据库删除记录 =====
bool DatabaseManager::DeleteRecordFromDatabase(const std::string& dbPath, int recordId) {
    // 验证文件是否存在
    std::ifstream fileCheck(dbPath);
    if (!fileCheck.good()) {
        DbLog("删除失败：数据库文件不存在: " + dbPath);
        return false;
    }
    fileCheck.close();

    // 打开数据库
    sqlite3* externalDb = nullptr;
    int rc = sqlite3_open(dbPath.c_str(), &externalDb);

    if (rc != SQLITE_OK) {
        std::string errorMsg = externalDb ? sqlite3_errmsg(externalDb) : "无法打开数据库";
        DbLog("删除失败：无法打开数据库: " + dbPath + " - " + errorMsg);
        if (externalDb) sqlite3_close(externalDb);
        return false;
    }

    DbLog("成功打开数据库进行删除操作: " + dbPath);

    // 执行删除SQL
    std::string sql = "DELETE FROM heartbeat_data WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    rc = sqlite3_prepare_v2(externalDb, sql.c_str(), -1, &stmt, nullptr);

    if (rc != SQLITE_OK) {
        std::string errorMsg = sqlite3_errmsg(externalDb);
        DbLog("删除失败：SQL准备失败: " + errorMsg);
        sqlite3_close(externalDb);
        return false;
    }

    // 绑定参数
    sqlite3_bind_int(stmt, 1, recordId);

    // 执行删除
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        std::string errorMsg = sqlite3_errmsg(externalDb);
        DbLog("删除失败：执行SQL失败: " + errorMsg);
        sqlite3_close(externalDb);
        return false;
    }

    // 检查是否真的删除了数据
    int changes = sqlite3_changes(externalDb);

    sqlite3_close(externalDb);

    if (changes > 0) {
        DbLog("成功从数据库删除记录 ID: " + std::to_string(recordId) +
            " (删除了 " + std::to_string(changes) + " 条记录)");
        return true;
    }
    else {
        DbLog("删除失败：未找到ID为 " + std::to_string(recordId) + " 的记录");
        return false;
    }
}


// ========== 🔥 新增：存储模式管理 ==========
void DatabaseManager::SetStorageMode(StorageMode mode) {
    if (currentMode == mode) {
        return;  // 模式相同，无需更改
    }

    std::string modeNames[] = { "按小时", "按天", "内存" };
    DbLog("切换存储模式: " + modeNames[static_cast<int>(currentMode)] + " -> " + modeNames[static_cast<int>(mode)]);

    // 关闭当前数据库（如果打开）
    if (db != nullptr && currentMode != StorageMode::MEMORY) {
        std::lock_guard<std::mutex> lock(dbMutex);
        sqlite3_close(db);
        db = nullptr;
        DbLog("已关闭当前数据库");
    }

    currentMode = mode;

    // 初始化新模式
    if (mode == StorageMode::MEMORY) {
        std::lock_guard<std::mutex> lock(memoryMutex);
        memoryRecords.clear();
        nextMemoryId = 1;
        DbLog("已初始化内存存储模式");
    }
    else {
        // 重新初始化数据库
        Initialize();
    }
}

// ========== 🔥 新增：内存存储功能 ==========
bool DatabaseManager::SaveMemoryToFile(const std::string& dbPath) {
    if (currentMode != StorageMode::MEMORY) {
        DbLog("另存为失败：当前不是内存存储模式");
        return false;
    }

    std::lock_guard<std::mutex> lock(memoryMutex);

    if (memoryRecords.empty()) {
        DbLog("另存为失败：内存中没有数据");
        return false;
    }

    // 转换中文路径为宽字符
    int wlen = MultiByteToWideChar(CP_UTF8, 0, dbPath.c_str(), -1, NULL, 0);
    if (wlen <= 0) {
        DbLog("另存为失败：路径转换失败");
        return false;
    }
    std::vector<wchar_t> wpath(wlen);
    MultiByteToWideChar(CP_UTF8, 0, dbPath.c_str(), -1, wpath.data(), wlen);

    // 创建父目录（如果不存在）
    std::filesystem::path fsPath(wpath.data());
    if (fsPath.has_parent_path()) {
        std::filesystem::path parentPath = fsPath.parent_path();
        if (!std::filesystem::exists(parentPath)) {
            std::filesystem::create_directories(parentPath);
            DbLog("已创建目录: " + parentPath.string());
        }
    }

    // 删除已存在的文件
    if (std::filesystem::exists(fsPath)) {
        std::filesystem::remove(fsPath);
        DbLog("已删除旧文件: " + dbPath);
    }

    // 创建新数据库
    sqlite3* newDb = nullptr;
    std::string utf8Path = fsPath.string();
    int rc = sqlite3_open(utf8Path.c_str(), &newDb);

    if (rc != SQLITE_OK) {
        DbLog("另存为失败：无法创建数据库: " + std::string(sqlite3_errmsg(newDb)));
        if (newDb) sqlite3_close(newDb);
        return false;
    }

    DbLog("另存为：已创建数据库文件: " + dbPath);

    // 优化SQLite性能
    sqlite3_exec(newDb, "PRAGMA synchronous = NORMAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(newDb, "PRAGMA journal_mode = WAL;", nullptr, nullptr, nullptr);

    // 创建表结构
    const char* createTableSQL = R"(
        CREATE TABLE IF NOT EXISTS heartbeat_data (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            game_id TEXT NOT NULL,
            timestamp TEXT NOT NULL,
            pattern_23_data BLOB,
            pattern_09_data BLOB,
            pattern_62_data BLOB,
            raw_data BLOB,
            packet_size INTEGER,
            is_complete INTEGER,
            process_type INTEGER,
            fragment_count INTEGER,
            packet_type INTEGER,
            game_id_length INTEGER,
            created_at TEXT NOT NULL
        );
    )";

    char* errMsg = nullptr;
    rc = sqlite3_exec(newDb, createTableSQL, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        DbLog("另存为失败：创建表失败: " + std::string(errMsg));
        sqlite3_free(errMsg);
        sqlite3_close(newDb);
        return false;
    }

    // 创建索引
    sqlite3_exec(newDb, "CREATE INDEX IF NOT EXISTS idx_game_id ON heartbeat_data(game_id);", nullptr, nullptr, nullptr);
    sqlite3_exec(newDb, "CREATE INDEX IF NOT EXISTS idx_timestamp ON heartbeat_data(timestamp);", nullptr, nullptr, nullptr);
    sqlite3_exec(newDb, "CREATE INDEX IF NOT EXISTS idx_packet_type ON heartbeat_data(packet_type);", nullptr, nullptr, nullptr);

    // 开启事务提高性能
    sqlite3_exec(newDb, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);

    // 插入内存数据
    const char* insertSQL = R"(
        INSERT INTO heartbeat_data
        (game_id, timestamp, pattern_23_data, pattern_09_data, pattern_62_data, raw_data,
         packet_size, is_complete, process_type, fragment_count,
         packet_type, game_id_length, created_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
    )";

    sqlite3_stmt* stmt = nullptr;
    rc = sqlite3_prepare_v2(newDb, insertSQL, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        DbLog("另存为失败：准备插入语句失败");
        sqlite3_close(newDb);
        return false;
    }

    int successCount = 0;
    for (const auto& record : memoryRecords) {
        sqlite3_bind_text(stmt, 1, record.gameID.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, record.timestamp.c_str(), -1, SQLITE_TRANSIENT);

        if (!record.pattern23Data.empty()) {
            sqlite3_bind_blob(stmt, 3, record.pattern23Data.data(),
                static_cast<int>(record.pattern23Data.size()), SQLITE_TRANSIENT);
        }
        else {
            sqlite3_bind_null(stmt, 3);
        }

        if (!record.pattern09Data.empty()) {
            sqlite3_bind_blob(stmt, 4, record.pattern09Data.data(),
                static_cast<int>(record.pattern09Data.size()), SQLITE_TRANSIENT);
        }
        else {
            sqlite3_bind_null(stmt, 4);
        }

        // 新增：绑定62特征数据
        if (!record.pattern62Data.empty()) {
            sqlite3_bind_blob(stmt, 5, record.pattern62Data.data(),
                static_cast<int>(record.pattern62Data.size()), SQLITE_TRANSIENT);
        }
        else {
            sqlite3_bind_null(stmt, 5);
        }

        if (!record.rawData.empty()) {
            sqlite3_bind_blob(stmt, 6, record.rawData.data(),
                static_cast<int>(record.rawData.size()), SQLITE_TRANSIENT);
        }
        else {
            sqlite3_bind_null(stmt, 6);
        }

        sqlite3_bind_int(stmt, 7, record.packetSize);
        sqlite3_bind_int(stmt, 8, record.isComplete ? 1 : 0);
        sqlite3_bind_int(stmt, 9, record.processType);
        sqlite3_bind_int(stmt, 10, record.fragmentCount);
        sqlite3_bind_int(stmt, 11, record.packetType);
        sqlite3_bind_int(stmt, 12, record.gameIDLength);
        sqlite3_bind_text(stmt, 13, record.createdAt.c_str(), -1, SQLITE_TRANSIENT);

        rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) {
            successCount++;
        }
        sqlite3_reset(stmt);
    }

    sqlite3_finalize(stmt);

    // 提交事务
    sqlite3_exec(newDb, "COMMIT;", nullptr, nullptr, nullptr);

    sqlite3_close(newDb);

    DbLog("另存为成功：已保存 " + std::to_string(successCount) + " 条记录到 " + dbPath);
    return true;
}

std::vector<HeartbeatRecord> DatabaseManager::GetMemoryRecords(int limit) const {
    if (currentMode != StorageMode::MEMORY) {
        return std::vector<HeartbeatRecord>();
    }

    std::lock_guard<std::mutex> lock(memoryMutex);
    std::vector<HeartbeatRecord> results;

    int startIdx = (std::max)(0, static_cast<int>(memoryRecords.size()) - limit);
    for (size_t i = memoryRecords.size(); i > static_cast<size_t>(startIdx); i--) {
        results.push_back(memoryRecords[i - 1]);
    }

    return results;
}

std::vector<HeartbeatRecord> DatabaseManager::GetMemoryRecordsFromId(int startId, int limit) const {
    if (currentMode != StorageMode::MEMORY) {
        return std::vector<HeartbeatRecord>();
    }

    std::lock_guard<std::mutex> lock(memoryMutex);
    std::vector<HeartbeatRecord> results;

    for (const auto& record : memoryRecords) {
        if (record.id > startId) {
            results.push_back(record);
            if (results.size() >= static_cast<size_t>(limit)) {
                break;
            }
        }
    }

    return results;
}

int DatabaseManager::GetMemoryMaxId() const {
    if (currentMode != StorageMode::MEMORY) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(memoryMutex);
    return nextMemoryId - 1;
}

void DatabaseManager::AppendMemoryRecordsPreserveId(const std::vector<HeartbeatRecord>& records) {
    if (currentMode != StorageMode::MEMORY) {
        return;
    }

    if (records.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(memoryMutex);

    int maxId = nextMemoryId - 1;
    for (const auto& r : records) {
        memoryRecords.push_back(r);
        if (r.id > maxId) maxId = r.id;
    }

    // nextMemoryId 用于内存模式下 InsertHeartbeatData 的自增；保证不会与已保留ID冲突
    if (maxId >= nextMemoryId) {
        nextMemoryId = maxId + 1;
    }
}

void DatabaseManager::ClearMemoryRecords() {
    if (currentMode != StorageMode::MEMORY) {
        return;
    }

    std::lock_guard<std::mutex> lock(memoryMutex);
    memoryRecords.clear();
    nextMemoryId = 1;
    DbLog("已清空内存数据");
}

// ========== 🔥 用户滤镜配置管理 ==========

bool DatabaseManager::CreateUserFilterConfigTable() {
    // 注意：调用者已持有 configMutex 锁，这里不再加锁
    if (!configDb) return false;

    const char* createTableSQL = R"(
        CREATE TABLE IF NOT EXISTS user_filter_config (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            instance_id TEXT NOT NULL,
            username TEXT NOT NULL,
            enabled_filter_ids TEXT NOT NULL,
            last_modified INTEGER NOT NULL,
            UNIQUE(instance_id, username)
        );
        CREATE INDEX IF NOT EXISTS idx_user_filter_instance ON user_filter_config(instance_id);
    )";

    char* errMsg = nullptr;
    int rc = sqlite3_exec(configDb, createTableSQL, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        DbLog("创建用户滤镜配置表失败: " + std::string(errMsg));
        sqlite3_free(errMsg);
        return false;
    }

    DbLog("用户滤镜配置表创建成功");
    return true;
}

bool DatabaseManager::CreateWPEFilterGroupTables() {
    if (!configDb) return false;

    const char* groupsSql = R"SQL(
        CREATE TABLE IF NOT EXISTS wpe_filter_groups (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL,
            description TEXT DEFAULT '',
            enabled INTEGER NOT NULL DEFAULT 1,
            created_at TEXT DEFAULT CURRENT_TIMESTAMP,
            updated_at TEXT DEFAULT CURRENT_TIMESTAMP
        );
        CREATE INDEX IF NOT EXISTS idx_wpe_filter_groups_enabled ON wpe_filter_groups(enabled);
    )SQL";

    const char* itemsSql = R"SQL(
        CREATE TABLE IF NOT EXISTS wpe_filter_group_items (
            group_id INTEGER NOT NULL,
            filter_id INTEGER NOT NULL,
            default_enabled INTEGER NOT NULL DEFAULT 0,
            PRIMARY KEY(group_id, filter_id)
        );
        CREATE INDEX IF NOT EXISTS idx_wpe_filter_group_items_filter ON wpe_filter_group_items(filter_id);
    )SQL";

    const char* userSql = R"SQL(
        CREATE TABLE IF NOT EXISTS user_wpe_filter_groups (
            instance_id TEXT NOT NULL,
            username TEXT NOT NULL,
            group_id INTEGER NOT NULL,
            PRIMARY KEY(instance_id, username, group_id)
        );
        CREATE INDEX IF NOT EXISTS idx_user_wpe_filter_groups_user
            ON user_wpe_filter_groups(instance_id, username);
    )SQL";

    const char* cardSql = R"SQL(
        CREATE TABLE IF NOT EXISTS card_wpe_filter_groups (
            card_key TEXT NOT NULL,
            group_id INTEGER NOT NULL,
            PRIMARY KEY(card_key, group_id)
        );
        CREATE INDEX IF NOT EXISTS idx_card_wpe_filter_groups_card
            ON card_wpe_filter_groups(card_key);
    )SQL";

    const char* statements[] = {groupsSql, itemsSql, userSql, cardSql};
    for (const char* sql : statements) {
        char* errMsg = nullptr;
        int rc = sqlite3_exec(configDb, sql, nullptr, nullptr, &errMsg);
        if (rc != SQLITE_OK) {
            DbLog("创建WPE滤镜组表失败: " + std::string(errMsg ? errMsg : sqlite3_errmsg(configDb)));
            sqlite3_free(errMsg);
            return false;
        }
    }

    DbLog("WPE滤镜组表创建成功");
    return true;
}

int DatabaseManager::SaveWPEFilterGroup(const WPEFilterGroupRecord& group) {
    std::lock_guard<std::mutex> lock(configMutex);
    if (!configDb) return 0;

    char* errMsg = nullptr;
    if (sqlite3_exec(configDb, "BEGIN IMMEDIATE TRANSACTION;", nullptr, nullptr, &errMsg) != SQLITE_OK) {
        DbLog("开始保存WPE滤镜组事务失败: " + std::string(errMsg ? errMsg : sqlite3_errmsg(configDb)));
        sqlite3_free(errMsg);
        return 0;
    }

    auto rollback = [this]() {
        sqlite3_exec(configDb, "ROLLBACK;", nullptr, nullptr, nullptr);
    };

    int groupId = group.id;
    sqlite3_stmt* stmt = nullptr;
    const char* groupSql = groupId > 0
        ? "UPDATE wpe_filter_groups SET name=?, description=?, enabled=?, updated_at=CURRENT_TIMESTAMP WHERE id=?;"
        : "INSERT INTO wpe_filter_groups (name, description, enabled) VALUES (?, ?, ?);";

    int rc = sqlite3_prepare_v2(configDb, groupSql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        DbLog("准备保存WPE滤镜组失败: " + std::string(sqlite3_errmsg(configDb)));
        rollback();
        return 0;
    }

    sqlite3_bind_text(stmt, 1, group.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, group.description.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, group.enabled ? 1 : 0);
    if (groupId > 0) {
        sqlite3_bind_int(stmt, 4, groupId);
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        DbLog("保存WPE滤镜组失败: " + std::string(sqlite3_errmsg(configDb)));
        rollback();
        return 0;
    }

    if (groupId <= 0) {
        groupId = static_cast<int>(sqlite3_last_insert_rowid(configDb));
    }

    sqlite3_stmt* delStmt = nullptr;
    rc = sqlite3_prepare_v2(configDb, "DELETE FROM wpe_filter_group_items WHERE group_id=?;", -1, &delStmt, nullptr);
    if (rc != SQLITE_OK) {
        DbLog("准备清理WPE滤镜组条目失败: " + std::string(sqlite3_errmsg(configDb)));
        rollback();
        return 0;
    }
    sqlite3_bind_int(delStmt, 1, groupId);
    rc = sqlite3_step(delStmt);
    sqlite3_finalize(delStmt);
    if (rc != SQLITE_DONE) {
        DbLog("清理WPE滤镜组条目失败: " + std::string(sqlite3_errmsg(configDb)));
        rollback();
        return 0;
    }

    sqlite3_stmt* itemStmt = nullptr;
    rc = sqlite3_prepare_v2(
        configDb,
        "INSERT OR REPLACE INTO wpe_filter_group_items (group_id, filter_id, default_enabled) VALUES (?, ?, ?);",
        -1,
        &itemStmt,
        nullptr);
    if (rc != SQLITE_OK) {
        DbLog("准备保存WPE滤镜组条目失败: " + std::string(sqlite3_errmsg(configDb)));
        rollback();
        return 0;
    }

    for (const auto& item : group.items) {
        if (item.filterId <= 0) continue;

        sqlite3_reset(itemStmt);
        sqlite3_clear_bindings(itemStmt);
        sqlite3_bind_int(itemStmt, 1, groupId);
        sqlite3_bind_int(itemStmt, 2, item.filterId);
        sqlite3_bind_int(itemStmt, 3, item.defaultEnabled ? 1 : 0);

        rc = sqlite3_step(itemStmt);
        if (rc != SQLITE_DONE) {
            DbLog("保存WPE滤镜组条目失败: " + std::string(sqlite3_errmsg(configDb)));
            sqlite3_finalize(itemStmt);
            rollback();
            return 0;
        }
    }
    sqlite3_finalize(itemStmt);

    if (sqlite3_exec(configDb, "COMMIT;", nullptr, nullptr, &errMsg) != SQLITE_OK) {
        DbLog("提交WPE滤镜组事务失败: " + std::string(errMsg ? errMsg : sqlite3_errmsg(configDb)));
        sqlite3_free(errMsg);
        rollback();
        return 0;
    }

    return groupId;
}

bool DatabaseManager::DeleteWPEFilterGroup(int groupId) {
    std::lock_guard<std::mutex> lock(configMutex);
    if (!configDb || groupId <= 0) return false;

    char* errMsg = nullptr;
    if (sqlite3_exec(configDb, "BEGIN IMMEDIATE TRANSACTION;", nullptr, nullptr, &errMsg) != SQLITE_OK) {
        DbLog("开始删除WPE滤镜组事务失败: " + std::string(errMsg ? errMsg : sqlite3_errmsg(configDb)));
        sqlite3_free(errMsg);
        return false;
    }

    const char* sqlList[] = {
        "DELETE FROM wpe_filter_group_items WHERE group_id=?;",
        "DELETE FROM user_wpe_filter_groups WHERE group_id=?;",
        "DELETE FROM card_wpe_filter_groups WHERE group_id=?;",
        "DELETE FROM wpe_filter_groups WHERE id=?;"
    };

    for (const char* sql : sqlList) {
        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(configDb, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            DbLog("准备删除WPE滤镜组失败: " + std::string(sqlite3_errmsg(configDb)));
            sqlite3_exec(configDb, "ROLLBACK;", nullptr, nullptr, nullptr);
            return false;
        }

        sqlite3_bind_int(stmt, 1, groupId);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) {
            DbLog("删除WPE滤镜组失败: " + std::string(sqlite3_errmsg(configDb)));
            sqlite3_exec(configDb, "ROLLBACK;", nullptr, nullptr, nullptr);
            return false;
        }
    }

    if (sqlite3_exec(configDb, "COMMIT;", nullptr, nullptr, &errMsg) != SQLITE_OK) {
        DbLog("提交删除WPE滤镜组事务失败: " + std::string(errMsg ? errMsg : sqlite3_errmsg(configDb)));
        sqlite3_free(errMsg);
        sqlite3_exec(configDb, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    return true;
}

std::vector<WPEFilterGroupRecord> DatabaseManager::LoadWPEFilterGroups(bool includeDisabled) {
    std::lock_guard<std::mutex> lock(configMutex);
    std::vector<WPEFilterGroupRecord> result;
    if (!configDb) return result;

    const char* groupSql = includeDisabled
        ? "SELECT id, name, description, enabled FROM wpe_filter_groups ORDER BY id;"
        : "SELECT id, name, description, enabled FROM wpe_filter_groups WHERE enabled=1 ORDER BY id;";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(configDb, groupSql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return result;
    }

    std::map<int, size_t> groupIndex;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        WPEFilterGroupRecord group;
        group.id = sqlite3_column_int(stmt, 0);
        const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const char* description = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        group.name = name ? name : "";
        group.description = description ? description : "";
        group.enabled = sqlite3_column_int(stmt, 3) != 0;
        groupIndex[group.id] = result.size();
        result.push_back(group);
    }
    sqlite3_finalize(stmt);

    if (result.empty()) {
        return result;
    }

    const char* itemSql =
        "SELECT group_id, filter_id, default_enabled FROM wpe_filter_group_items ORDER BY group_id, filter_id;";
    stmt = nullptr;
    rc = sqlite3_prepare_v2(configDb, itemSql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return result;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int groupId = sqlite3_column_int(stmt, 0);
        auto it = groupIndex.find(groupId);
        if (it == groupIndex.end()) continue;

        WPEFilterGroupItemRecord item;
        item.filterId = sqlite3_column_int(stmt, 1);
        item.defaultEnabled = sqlite3_column_int(stmt, 2) != 0;
        result[it->second].items.push_back(item);
    }

    sqlite3_finalize(stmt);
    return result;
}

WPEFilterGroupRecord DatabaseManager::LoadWPEFilterGroup(int groupId) {
    WPEFilterGroupRecord result;
    if (groupId <= 0) return result;

    auto groups = LoadWPEFilterGroups(true);
    for (const auto& group : groups) {
        if (group.id == groupId) {
            return group;
        }
    }

    return result;
}

bool DatabaseManager::SaveUserWPEFilterGroups(
    const std::string& instanceId,
    const std::string& username,
    const std::vector<int>& groupIds) {
    std::lock_guard<std::mutex> lock(configMutex);
    if (!configDb || instanceId.empty() || username.empty()) return false;

    char* errMsg = nullptr;
    if (sqlite3_exec(configDb, "BEGIN IMMEDIATE TRANSACTION;", nullptr, nullptr, &errMsg) != SQLITE_OK) {
        DbLog("开始保存用户WPE滤镜组事务失败: " + std::string(errMsg ? errMsg : sqlite3_errmsg(configDb)));
        sqlite3_free(errMsg);
        return false;
    }

    sqlite3_stmt* delStmt = nullptr;
    int rc = sqlite3_prepare_v2(
        configDb,
        "DELETE FROM user_wpe_filter_groups WHERE instance_id=? AND username=?;",
        -1,
        &delStmt,
        nullptr);
    if (rc != SQLITE_OK) {
        DbLog("准备清理用户WPE滤镜组失败: " + std::string(sqlite3_errmsg(configDb)));
        sqlite3_exec(configDb, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    sqlite3_bind_text(delStmt, 1, instanceId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(delStmt, 2, username.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(delStmt);
    sqlite3_finalize(delStmt);
    if (rc != SQLITE_DONE) {
        DbLog("清理用户WPE滤镜组失败: " + std::string(sqlite3_errmsg(configDb)));
        sqlite3_exec(configDb, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    sqlite3_stmt* insStmt = nullptr;
    rc = sqlite3_prepare_v2(
        configDb,
        "INSERT OR IGNORE INTO user_wpe_filter_groups (instance_id, username, group_id) VALUES (?, ?, ?);",
        -1,
        &insStmt,
        nullptr);
    if (rc != SQLITE_OK) {
        DbLog("准备保存用户WPE滤镜组失败: " + std::string(sqlite3_errmsg(configDb)));
        sqlite3_exec(configDb, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    for (int groupId : groupIds) {
        if (groupId <= 0) continue;

        sqlite3_reset(insStmt);
        sqlite3_clear_bindings(insStmt);
        sqlite3_bind_text(insStmt, 1, instanceId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insStmt, 2, username.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(insStmt, 3, groupId);

        rc = sqlite3_step(insStmt);
        if (rc != SQLITE_DONE) {
            DbLog("保存用户WPE滤镜组失败: " + std::string(sqlite3_errmsg(configDb)));
            sqlite3_finalize(insStmt);
            sqlite3_exec(configDb, "ROLLBACK;", nullptr, nullptr, nullptr);
            return false;
        }
    }
    sqlite3_finalize(insStmt);

    if (sqlite3_exec(configDb, "COMMIT;", nullptr, nullptr, &errMsg) != SQLITE_OK) {
        DbLog("提交用户WPE滤镜组事务失败: " + std::string(errMsg ? errMsg : sqlite3_errmsg(configDb)));
        sqlite3_free(errMsg);
        sqlite3_exec(configDb, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    return true;
}

std::vector<int> DatabaseManager::LoadUserWPEFilterGroups(
    const std::string& instanceId,
    const std::string& username) {
    std::lock_guard<std::mutex> lock(configMutex);
    std::vector<int> result;
    if (!configDb || instanceId.empty() || username.empty()) return result;

    const char* sql =
        "SELECT group_id FROM user_wpe_filter_groups WHERE instance_id=? AND username=? ORDER BY group_id;";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(configDb, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return result;
    }

    sqlite3_bind_text(stmt, 1, instanceId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, username.c_str(), -1, SQLITE_TRANSIENT);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        result.push_back(sqlite3_column_int(stmt, 0));
    }

    sqlite3_finalize(stmt);
    return result;
}

std::map<std::string, std::vector<int>> DatabaseManager::LoadAllUserWPEFilterGroups(
    const std::string& instanceId) {
    std::lock_guard<std::mutex> lock(configMutex);
    std::map<std::string, std::vector<int>> result;
    if (!configDb || instanceId.empty()) return result;

    const char* sql =
        "SELECT username, group_id FROM user_wpe_filter_groups WHERE instance_id=? ORDER BY username, group_id;";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(configDb, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return result;
    }

    sqlite3_bind_text(stmt, 1, instanceId.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* username = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        int groupId = sqlite3_column_int(stmt, 1);
        if (username && groupId > 0) {
            result[username].push_back(groupId);
        }
    }

    sqlite3_finalize(stmt);
    return result;
}

bool DatabaseManager::SaveCardWPEFilterGroups(const std::string& cardKey, const std::vector<int>& groupIds) {
    std::lock_guard<std::mutex> lock(configMutex);
    if (!configDb || cardKey.empty()) return false;

    char* errMsg = nullptr;
    if (sqlite3_exec(configDb, "BEGIN IMMEDIATE TRANSACTION;", nullptr, nullptr, &errMsg) != SQLITE_OK) {
        DbLog("开始保存卡密WPE滤镜组事务失败: " + std::string(errMsg ? errMsg : sqlite3_errmsg(configDb)));
        sqlite3_free(errMsg);
        return false;
    }

    sqlite3_stmt* delStmt = nullptr;
    int rc = sqlite3_prepare_v2(
        configDb,
        "DELETE FROM card_wpe_filter_groups WHERE card_key=?;",
        -1,
        &delStmt,
        nullptr);
    if (rc != SQLITE_OK) {
        DbLog("准备清理卡密WPE滤镜组失败: " + std::string(sqlite3_errmsg(configDb)));
        sqlite3_exec(configDb, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    sqlite3_bind_text(delStmt, 1, cardKey.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(delStmt);
    sqlite3_finalize(delStmt);
    if (rc != SQLITE_DONE) {
        DbLog("清理卡密WPE滤镜组失败: " + std::string(sqlite3_errmsg(configDb)));
        sqlite3_exec(configDb, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    sqlite3_stmt* insStmt = nullptr;
    rc = sqlite3_prepare_v2(
        configDb,
        "INSERT OR IGNORE INTO card_wpe_filter_groups (card_key, group_id) VALUES (?, ?);",
        -1,
        &insStmt,
        nullptr);
    if (rc != SQLITE_OK) {
        DbLog("准备保存卡密WPE滤镜组失败: " + std::string(sqlite3_errmsg(configDb)));
        sqlite3_exec(configDb, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    for (int groupId : groupIds) {
        if (groupId <= 0) continue;

        sqlite3_reset(insStmt);
        sqlite3_clear_bindings(insStmt);
        sqlite3_bind_text(insStmt, 1, cardKey.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(insStmt, 2, groupId);

        rc = sqlite3_step(insStmt);
        if (rc != SQLITE_DONE) {
            DbLog("保存卡密WPE滤镜组失败: " + std::string(sqlite3_errmsg(configDb)));
            sqlite3_finalize(insStmt);
            sqlite3_exec(configDb, "ROLLBACK;", nullptr, nullptr, nullptr);
            return false;
        }
    }
    sqlite3_finalize(insStmt);

    if (sqlite3_exec(configDb, "COMMIT;", nullptr, nullptr, &errMsg) != SQLITE_OK) {
        DbLog("提交卡密WPE滤镜组事务失败: " + std::string(errMsg ? errMsg : sqlite3_errmsg(configDb)));
        sqlite3_free(errMsg);
        sqlite3_exec(configDb, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    return true;
}

std::vector<int> DatabaseManager::LoadCardWPEFilterGroups(const std::string& cardKey) {
    std::lock_guard<std::mutex> lock(configMutex);
    std::vector<int> result;
    if (!configDb || cardKey.empty()) return result;

    const char* sql = "SELECT group_id FROM card_wpe_filter_groups WHERE card_key=? ORDER BY group_id;";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(configDb, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return result;
    }

    sqlite3_bind_text(stmt, 1, cardKey.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        result.push_back(sqlite3_column_int(stmt, 0));
    }

    sqlite3_finalize(stmt);
    return result;
}

bool DatabaseManager::DeleteCardWPEFilterGroups(const std::string& cardKey) {
    std::lock_guard<std::mutex> lock(configMutex);
    if (!configDb || cardKey.empty()) return false;

    const char* sql = "DELETE FROM card_wpe_filter_groups WHERE card_key=?;";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(configDb, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(stmt, 1, cardKey.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool DatabaseManager::SaveUserFilterConfig(const std::string& instanceId, const std::string& username,
                                           const std::vector<int>& enabledFilterIds) {
    std::lock_guard<std::mutex> lock(configMutex);
    if (!configDb) return false;

    // 将滤镜ID列表转换为JSON字符串 "[1,3,5]"
    std::string filterIdsJson = "[";
    for (size_t i = 0; i < enabledFilterIds.size(); i++) {
        if (i > 0) filterIdsJson += ",";
        filterIdsJson += std::to_string(enabledFilterIds[i]);
    }
    filterIdsJson += "]";

    const char* sql = R"(
        INSERT OR REPLACE INTO user_filter_config (instance_id, username, enabled_filter_ids, last_modified)
        VALUES (?, ?, ?, strftime('%s', 'now'));
    )";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(configDb, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        DbLog("准备保存用户滤镜配置失败: " + std::string(sqlite3_errmsg(configDb)));
        return false;
    }

    sqlite3_bind_text(stmt, 1, instanceId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, filterIdsJson.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc == SQLITE_DONE) {
        DbLog("保存用户滤镜配置成功: " + instanceId + "/" + username);
        return true;
    } else {
        DbLog("保存用户滤镜配置失败: " + std::string(sqlite3_errmsg(configDb)));
        return false;
    }
}

std::vector<int> DatabaseManager::LoadUserFilterConfig(const std::string& instanceId, const std::string& username) {
    std::lock_guard<std::mutex> lock(configMutex);
    std::vector<int> result;
    if (!configDb) return result;

    const char* sql = "SELECT enabled_filter_ids FROM user_filter_config WHERE instance_id = ? AND username = ?;";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(configDb, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return result;
    }

    sqlite3_bind_text(stmt, 1, instanceId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, username.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* filterIdsJson = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (filterIdsJson) {
            // 解析JSON数组 "[1,3,5]"
            std::string jsonStr = filterIdsJson;
            if (jsonStr.length() >= 2 && jsonStr[0] == '[' && jsonStr[jsonStr.length() - 1] == ']') {
                jsonStr = jsonStr.substr(1, jsonStr.length() - 2);  // 去掉 [ ]
                if (!jsonStr.empty()) {
                    std::stringstream ss(jsonStr);
                    std::string item;
                    while (std::getline(ss, item, ',')) {
                        try {
                            result.push_back(std::stoi(item));
                        } catch (...) {}
                    }
                }
            }
        }
    }

    sqlite3_finalize(stmt);
    return result;
}

std::map<std::string, std::vector<int>> DatabaseManager::LoadAllUserFilterConfigs(const std::string& instanceId) {
    std::lock_guard<std::mutex> lock(configMutex);
    std::map<std::string, std::vector<int>> result;
    if (!configDb) return result;

    const char* sql = "SELECT username, enabled_filter_ids FROM user_filter_config WHERE instance_id = ?;";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(configDb, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return result;
    }

    sqlite3_bind_text(stmt, 1, instanceId.c_str(), -1, SQLITE_TRANSIENT);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* username = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* filterIdsJson = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));

        if (username && filterIdsJson) {
            std::vector<int> filterIds;
            std::string jsonStr = filterIdsJson;
            if (jsonStr.length() >= 2 && jsonStr[0] == '[' && jsonStr[jsonStr.length() - 1] == ']') {
                jsonStr = jsonStr.substr(1, jsonStr.length() - 2);
                if (!jsonStr.empty()) {
                    std::stringstream ss(jsonStr);
                    std::string item;
                    while (std::getline(ss, item, ',')) {
                        try {
                            filterIds.push_back(std::stoi(item));
                        } catch (...) {}
                    }
                }
            }
            result[username] = filterIds;
        }
    }

    sqlite3_finalize(stmt);
    DbLog("加载实例用户滤镜配置: " + instanceId + ", 用户数: " + std::to_string(result.size()));
    return result;
}

bool DatabaseManager::DeleteUserFilterConfig(const std::string& instanceId, const std::string& username) {
    std::lock_guard<std::mutex> lock(configMutex);
    if (!configDb) return false;

    const char* sql = "DELETE FROM user_filter_config WHERE instance_id = ? AND username = ?;";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(configDb, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(stmt, 1, instanceId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, username.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return (rc == SQLITE_DONE);
}

bool DatabaseManager::ClearInstanceUserFilterConfigs(const std::string& instanceId) {
    std::lock_guard<std::mutex> lock(configMutex);
    if (!configDb) return false;

    const char* sql = "DELETE FROM user_filter_config WHERE instance_id = ?;";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(configDb, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(stmt, 1, instanceId.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc == SQLITE_DONE) {
        DbLog("清空实例用户滤镜配置: " + instanceId);
        return true;
    }
    return false;
}

// ========== 默认滤镜配置管理 ==========
bool DatabaseManager::SaveDefaultFilterConfig(const std::string& instanceId, const std::vector<int>& filterIds) {
    // 🔥 不需要加锁，因为 SetConfigValue 内部已经有锁保护
    if (!configDb) return false;

    // 将滤镜ID列表转换为逗号分隔的字符串
    std::string filterIdsStr;
    for (size_t i = 0; i < filterIds.size(); ++i) {
        if (i > 0) filterIdsStr += ",";
        filterIdsStr += std::to_string(filterIds[i]);
    }

    // 使用配置键存储
    std::string key = "instance_" + instanceId + "_defaultFilters";
    return SetConfigValue(key, filterIdsStr);
}

std::vector<int> DatabaseManager::LoadDefaultFilterConfig(const std::string& instanceId) {
    // 🔥 不需要加锁，因为 GetConfigValue 内部已经有锁保护
    std::vector<int> result;
    if (!configDb) return result;

    std::string key = "instance_" + instanceId + "_defaultFilters";
    std::string filterIdsStr = GetConfigValue(key, "");

    if (filterIdsStr.empty()) {
        return result;
    }

    // 解析逗号分隔的字符串
    std::stringstream ss(filterIdsStr);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (!item.empty()) {
            try {
                result.push_back(std::stoi(item));
            } catch (...) {
                // 忽略解析错误
            }
        }
    }

    return result;
}

bool DatabaseManager::DeleteDefaultFilterConfig(const std::string& instanceId) {
    // 🔥 不需要加锁，因为 SetConfigValue 内部已经有锁保护
    if (!configDb) return false;

    std::string key = "instance_" + instanceId + "_defaultFilters";
    return SetConfigValue(key, "");
}

// ========== 🔥 滤镜执行次数统计 ==========
bool DatabaseManager::CreateFilterExecutionStatsTable() {
    // 注意：调用者已持有 configMutex 锁，这里不再加锁
    if (!configDb) return false;

    const char* createTableSQL = R"(
        CREATE TABLE IF NOT EXISTS filter_execution_stats (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            instance_id TEXT NOT NULL,
            username TEXT NOT NULL,
            filter_id INTEGER NOT NULL,
            execution_count INTEGER NOT NULL DEFAULT 0,
            last_executed INTEGER NOT NULL,
            UNIQUE(instance_id, username, filter_id)
        );
        CREATE INDEX IF NOT EXISTS idx_filter_stats_instance_user ON filter_execution_stats(instance_id, username);
    )";

    char* errMsg = nullptr;
    int rc = sqlite3_exec(configDb, createTableSQL, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        DbLog("创建滤镜执行次数统计表失败: " + std::string(errMsg));
        sqlite3_free(errMsg);
        return false;
    }

    DbLog("滤镜执行次数统计表创建成功");
    return true;
}

void DatabaseManager::IncrementFilterExecutionCount(const std::string& instanceId, const std::string& username, int filterId) {
    std::lock_guard<std::mutex> lock(filterExecutionMutex);

    // 🔥 增加内存缓存计数
    FilterExecutionKey key{instanceId, username, filterId};
    filterExecutionCache[key]++;

    {
        std::lock_guard<std::mutex> snapshotLock(filterExecutionSnapshotMutex);
        auto instanceIt = filterExecutionSnapshot.find(instanceId);
        if (instanceIt != filterExecutionSnapshot.end()) {
            auto userIt = instanceIt->second.find(username);
            if (userIt != instanceIt->second.end()) {
                userIt->second[filterId] += 1;
            }
        }
    }

    // 🔥 检查是否需要立即刷新（仅在缓存过大时通知后台线程）
    if (filterExecutionCache.size() >= FLUSH_THRESHOLD) {
        flushCondition.notify_one();  // 通知后台线程立即刷新
    }
}

void DatabaseManager::FlushFilterExecutionStats() {
    // 🔥 复制缓存数据并清空（减少锁持有时间）
    std::map<FilterExecutionKey, int64_t> cacheSnapshot;
    {
        std::lock_guard<std::mutex> lock(filterExecutionMutex);
        if (filterExecutionCache.empty()) return;

        cacheSnapshot = filterExecutionCache;
        filterExecutionCache.clear();
    }

    // 🔥 批量写入数据库（不持有 filterExecutionMutex）
    std::lock_guard<std::mutex> dbLock(configMutex);
    if (!configDb) return;

    // 开启事务，提升批量写入性能
    sqlite3_exec(configDb, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);

    const char* sql = R"(
        INSERT INTO filter_execution_stats (instance_id, username, filter_id, execution_count, last_executed)
        VALUES (?, ?, ?, ?, strftime('%s', 'now'))
        ON CONFLICT(instance_id, username, filter_id)
        DO UPDATE SET
            execution_count = execution_count + ?,
            last_executed = strftime('%s', 'now');
    )";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(configDb, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        DbLog("准备批量写入滤镜执行次数失败: " + std::string(sqlite3_errmsg(configDb)));
        sqlite3_exec(configDb, "ROLLBACK;", nullptr, nullptr, nullptr);
        return;
    }

    int successCount = 0;
    for (const auto& [key, count] : cacheSnapshot) {
        sqlite3_reset(stmt);
        sqlite3_bind_text(stmt, 1, key.instanceId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, key.username.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, key.filterId);
        sqlite3_bind_int64(stmt, 4, count);
        sqlite3_bind_int64(stmt, 5, count);

        rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) {
            successCount++;
        }
    }

    sqlite3_finalize(stmt);
    sqlite3_exec(configDb, "COMMIT;", nullptr, nullptr, nullptr);

    DbLog("批量写入滤镜执行次数: " + std::to_string(successCount) + "/" + std::to_string(cacheSnapshot.size()) + " 条记录");
}

// 🔥 后台刷新线程工作函数
void DatabaseManager::FlushThreadWorker() {
    while (flushThreadRunning) {
        std::unique_lock<std::mutex> lock(flushMutex);

        // 等待10秒或被提前唤醒（缓存过大时）
        flushCondition.wait_for(lock, std::chrono::seconds(FLUSH_INTERVAL_SECONDS), [this]() {
            return !flushThreadRunning || filterExecutionCache.size() >= FLUSH_THRESHOLD;
        });

        if (!flushThreadRunning) break;

        // 执行刷新
        FlushFilterExecutionStats();
    }
}


std::map<int, int64_t> DatabaseManager::GetUserFilterExecutionCounts(const std::string& instanceId, const std::string& username) {
    {
        std::lock_guard<std::mutex> snapshotLock(filterExecutionSnapshotMutex);
        auto instanceIt = filterExecutionSnapshot.find(instanceId);
        if (instanceIt != filterExecutionSnapshot.end()) {
            auto userIt = instanceIt->second.find(username);
            if (userIt != instanceIt->second.end()) {
                return userIt->second;
            }
        }
    }

    std::map<int, int64_t> result;
    {
        std::lock_guard<std::mutex> lock(configMutex);
        if (!configDb) return result;

        const char* sql = R"(
            SELECT filter_id, execution_count
            FROM filter_execution_stats
            WHERE instance_id = ? AND username = ?;
        )";

        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(configDb, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            DbLog("准备查询滤镜执行次数失败: " + std::string(sqlite3_errmsg(configDb)));
            return result;
        }

        sqlite3_bind_text(stmt, 1, instanceId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, username.c_str(), -1, SQLITE_TRANSIENT);

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            int filterId = sqlite3_column_int(stmt, 0);
            int64_t count = sqlite3_column_int64(stmt, 1);
            result[filterId] = count;
        }

        sqlite3_finalize(stmt);
    }

    {
        std::lock_guard<std::mutex> lock(filterExecutionMutex);
        for (const auto& [key, count] : filterExecutionCache) {
            if (key.instanceId == instanceId && key.username == username) {
                result[key.filterId] += count;
            }
        }
    }

    {
        std::lock_guard<std::mutex> snapshotLock(filterExecutionSnapshotMutex);
        filterExecutionSnapshot[instanceId][username] = result;
    }

    return result;
}

bool DatabaseManager::ResetUserFilterExecutionCounts(const std::string& instanceId, const std::string& username) {
    std::lock_guard<std::mutex> lock(configMutex);
    if (!configDb) return false;

    const char* sql = "DELETE FROM filter_execution_stats WHERE instance_id = ? AND username = ?;";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(configDb, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        DbLog("准备重置用户滤镜执行次数失败: " + std::string(sqlite3_errmsg(configDb)));
        return false;
    }

    sqlite3_bind_text(stmt, 1, instanceId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, username.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        DbLog("重置用户滤镜执行次数失败: " + std::string(sqlite3_errmsg(configDb)));
        return false;
    }

    // 🔥 清空内存缓存中对应的记录
    {
        std::lock_guard<std::mutex> cacheLock(filterExecutionMutex);
        auto it = filterExecutionCache.begin();
        while (it != filterExecutionCache.end()) {
            if (it->first.instanceId == instanceId && it->first.username == username) {
                it = filterExecutionCache.erase(it);
            } else {
                ++it;
            }
        }
    }

    {
        std::lock_guard<std::mutex> snapshotLock(filterExecutionSnapshotMutex);
        auto instanceIt = filterExecutionSnapshot.find(instanceId);
        if (instanceIt != filterExecutionSnapshot.end()) {
            instanceIt->second.erase(username);
        }
    }

    return true;
}

bool DatabaseManager::ResetFilterExecutionCount(const std::string& instanceId, const std::string& username, int filterId) {
    std::lock_guard<std::mutex> lock(configMutex);
    if (!configDb) return false;

    const char* sql = "DELETE FROM filter_execution_stats WHERE instance_id = ? AND username = ? AND filter_id = ?;";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(configDb, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        DbLog("准备重置滤镜执行次数失败: " + std::string(sqlite3_errmsg(configDb)));
        return false;
    }

    sqlite3_bind_text(stmt, 1, instanceId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, filterId);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        DbLog("重置滤镜执行次数失败: " + std::string(sqlite3_errmsg(configDb)));
        return false;
    }

    // 🔥 清空内存缓存中对应的记录
    {
        std::lock_guard<std::mutex> cacheLock(filterExecutionMutex);
        FilterExecutionKey key{instanceId, username, filterId};
        filterExecutionCache.erase(key);
    }

    {
        std::lock_guard<std::mutex> snapshotLock(filterExecutionSnapshotMutex);
        auto instanceIt = filterExecutionSnapshot.find(instanceId);
        if (instanceIt != filterExecutionSnapshot.end()) {
            auto userIt = instanceIt->second.find(username);
            if (userIt != instanceIt->second.end()) {
                userIt->second.erase(filterId);
            }
        }
    }

    return true;
}

