#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <chrono>
#include <cstdint>
#include <map>
#include "sqlite\sqlite3.h"

// ========== 存储模式枚举 ==========
enum class StorageMode {
    HOURLY = 0,    // 按小时生成db文件
    DAILY = 1,     // 按天生成db文件
    MEMORY = 2     // 内存存储
};

// 心跳数据结构
struct HeartbeatRecord {
    int id;
    std::string gameID;
    std::string socksUsername;  // 🔥 新增：采集时的SOCKS5账号用户名（用于按账号查找）
    std::string timestamp;
    std::vector<uint8_t> pattern23Data;
    std::vector<uint8_t> pattern09Data;
    std::vector<uint8_t> pattern62Data;  // 新增：62特征数据
    std::vector<uint8_t> pattern0A92Data; // 新增：0A92特征数据（4字节）
    std::vector<uint8_t> patternAfter1686Data;  // 新增：16 86之后的所有数据
    int pos1686;  // 新增：16 86在原始数据中的位置
    int pos0952;  // 新增：09 52在原始数据中的位置
    std::vector<uint8_t> rawData;
    int packetSize;
    bool isComplete;
    int processType;
    int fragmentCount;
    std::string createdAt;
    int packetType;
    int gameIDLength;
    std::chrono::system_clock::time_point loadTime;

    HeartbeatRecord() : id(0), packetSize(0), isComplete(false),
        processType(0), fragmentCount(0), packetType(-1), gameIDLength(0),
        pos1686(-1), pos0952(-1) {}

    // 🔥 新增：重载 == 运算符，以便 std::find 函数能够比较两个 HeartbeatRecord 对象
    bool operator==(const HeartbeatRecord& other) const {
        return id == other.id &&
            gameID == other.gameID &&
            socksUsername == other.socksUsername &&
            timestamp == other.timestamp &&
            pattern23Data == other.pattern23Data &&
            pattern09Data == other.pattern09Data &&
            pattern62Data == other.pattern62Data &&
            pattern0A92Data == other.pattern0A92Data &&
            patternAfter1686Data == other.patternAfter1686Data &&
            pos1686 == other.pos1686 &&
            pos0952 == other.pos0952 &&
            rawData == other.rawData &&
            packetSize == other.packetSize &&
            isComplete == other.isComplete &&
            processType == other.processType &&
            fragmentCount == other.fragmentCount &&
            createdAt == other.createdAt &&
            packetType == other.packetType &&
            gameIDLength == other.gameIDLength;
    }
};

// ========== 新增：应用配置结构体 ==========
struct AppConfig {
    // ===== 🔥 存储模式配置 =====
    StorageMode storageMode = StorageMode::HOURLY;
    std::string customSavePath;  // 用于内存模式的另存为路径

    // ===== 🔥 伪心跳自动加载模式 =====
    StorageMode heartbeatLoadMode = StorageMode::HOURLY;  // 伪心跳自动加载从按小时/按天DB读取

    // 采集逻辑配置
    int collectorPort = 1080;
    bool enableRecording = true;
    int filterType = 0;
    std::string filterValue;
    bool disablePacketHeaderFilter = false;
    bool allowCollect00ID = false;
    bool allowCollectOBID = false;
    bool enableCollect62Pattern = false;  // 新增：是否启用62特征采集
    bool enableDisconnectAutoClear = true;  // 断开连接时是否自动清理内存池数据
    bool enableSecondaryProxy = false;
    std::string secondaryProxyHost = "127.0.0.1";
    std::string secondaryProxyPort = "10808";
    bool enablePos10Replace = false;
    bool enablePos0109Minus8Replace = false;
    std::string collectorPacketTypes;
    std::map<int, bool> collectorTypeEnabled;
    std::map<int, bool> collectorDisplayFilter;

    // 伪心跳配置
    int heartbeatPort = 1081;
    bool enableHeartbeatRecording = true;
    bool enableHeartbeatForward = true;
    bool disableHeartbeatHeaderFilter = false;
    bool allowHeartbeat00ID = false;
    bool allowHeartbeatOBID = false;
    bool enableHeartbeat62Pattern = false;  // 新增：是否启用62特征伪心跳
    int pattern23Mode = 0;
    int pattern23DynamicOffset = 0;
    bool enable0109FrontReplace = false;
    bool enable0109BackReplace = false;
    bool enable0109Bytes567Replace = false;
    bool useSingleHeartbeatPacket = false;
    bool enableVTDFilter = false;
    int heartbeatFilterType = 0;
    std::string heartbeatFilterValue;
    std::string heartbeatPacketTypes;
    std::map<int, bool> heartbeatTypeEnabled;

    // 🔥 新增：算法选择配置
    bool useSameAlgorithm = true;  // true=同号算法(按游戏ID读取), false=异号算法(按SOCKS账号读取)

    // 🔥 新增：数据包读取顺序模式配置
    int packetOrderMode = 0;  // 0=升序模式(FIFO先采集先使用), 1=降序模式(LIFO最新数据优先)

    // 🔥 新增：伪心跳计数限制配置
    int heartbeatReplaceLimit = 0;  // 每个SOCKS账号或游戏ID的替换数量限制（0表示无限制）
    bool enableLimitCleanup = false;  // 达到限制后是否清理对应SOCKS账号或游戏ID的数据

    // 时间配置
    int heartbeatLoadInterval = 180;
    int heartbeatExpireTime = 1200;
    int heartbeatCleanupInterval = 60;

    // 清理配置
    bool cleanupIDLength17 = false;
    bool cleanupIDLength18 = false;
    bool cleanupIDLength19 = true;
    bool cleanupIDLength20 = true;
    bool cleanupIDLength21 = false;
    bool cleanupIDLength22 = false;

    // 日志配置
    bool autoScrollLog = true;
    bool enableLogging = true;  // 是否启用日志记录

    // ===== 🔥 新增：TCP文件服务器配置 =====
    int tcpFileServerPort = 9000;

    // ===== 🔥 新增：远程同步配置 =====
    bool enableRemoteSync = false;
    std::string remoteServerHost = "127.0.0.1";
    int remoteServerPort = 9000;
    int remoteSyncInterval = 300;
};

struct WPEFilterGroupItemRecord {
    int filterId = 0;
    bool defaultEnabled = false;
};

struct WPEFilterGroupRecord {
    int id = 0;
    std::string name;
    std::string description;
    bool enabled = true;
    std::vector<WPEFilterGroupItemRecord> items;
};





class DatabaseManager {
private:
    sqlite3* db;
    std::string currentDbPath;
    std::string currentHour;
    std::mutex dbMutex;

    // 数据库文件目录（默认 ./db，可按实例隔离为 ./db/<instanceId> 等）
    std::string dataDir = "db";

    // 配置数据库
    sqlite3* configDb;
    std::string configDbPath;
    std::mutex configMutex;

    // ===== 🔥 新增：存储模式和内存存储 =====
    StorageMode currentMode;
    std::vector<HeartbeatRecord> memoryRecords;  // 内存模式存储
    mutable std::mutex memoryMutex;  // 🔥 声明为mutable以便在const函数中使用
    int nextMemoryId;  // 内存模式的自增ID

    std::string GetBeijingHour();
    std::string GetBeijingTimestamp();
    std::string GetBeijingDay();  // 🔥 新增：获取天（YYYY-MM-DD）
    std::string GetTimeStringByMode();  // 🔥 新增：根据模式返回时间字符串
    bool CreateTable();
    void CheckAndSwitchDatabase();
    bool CreateConfigTable();

public:
    DatabaseManager();
    ~DatabaseManager();

    // 设置数据目录（影响按小时/按天生成的 heartbeat_*.db 的落盘位置）
    void SetDataDirectory(const std::string& dir) { dataDir = dir.empty() ? "db" : dir; }
    std::string GetDataDirectory() const { return dataDir; }

    bool Initialize();
    bool InsertHeartbeatData(const HeartbeatRecord& record);
    std::vector<HeartbeatRecord> GetRecentHeartbeats(int limit = 100);
    std::vector<HeartbeatRecord> GetHeartbeatsByGameID(const std::string& gameID);
    std::string GetCurrentDatabasePath() const { return currentDbPath; }
    void Close();

    std::vector<HeartbeatRecord> GetHeartbeatsFromId(int startId, int limit = 1000);
    int GetMaxHeartbeatId();
    static std::vector<HeartbeatRecord> LoadFromExternalDatabase(const std::string& dbPath);

    // ===== 按长度查询 =====
    int GetHeartbeatCountByLength(int length);  // 获取指定长度的数据量
    std::vector<HeartbeatRecord> GetHeartbeatsByLength(int length, int startId = 1, int endId = INT_MAX);  // 获取指定长度范围内的数据

    // ===== 🔥 新增：存储模式管理 =====
    void SetStorageMode(StorageMode mode);
    StorageMode GetStorageMode() const { return currentMode; }

    // ===== 🔥 新增：内存存储功能 =====
    bool SaveMemoryToFile(const std::string& dbPath);  // 另存为功能
    std::vector<HeartbeatRecord> GetMemoryRecords(int limit = 15000) const;  // 获取内存数据
    std::vector<HeartbeatRecord> GetMemoryRecordsFromId(int startId, int limit = 15000) const;  // 从指定ID获取
    int GetMemoryMaxId() const;  // 获取内存最大ID
    // 将外部加载的记录追加到内存池并尽量保留record.id（用于增量加载/固定包选择等）
    void AppendMemoryRecordsPreserveId(const std::vector<HeartbeatRecord>& records);
    void ClearMemoryRecords();  // 清空内存数据

    // ========== 配置管理功能 ==========
    bool InitializeConfig(const std::string& configPath = "config.db");

    // 基础配置读写
    bool SetConfigValue(const std::string& key, const std::string& value);
    std::string GetConfigValue(const std::string& key, const std::string& defaultValue = "");
    bool DeleteConfigValue(const std::string& key);

    //创建数据库快照用于传输（不阻塞写入）
    bool CreateSnapshotForTransfer(const std::string& snapshotPath);

    // 高级配置管理
    bool SaveAppConfig(const AppConfig& config);
    AppConfig LoadAppConfig();

    // ===== 删除指定数据库中的记录 =====
    static bool DeleteRecordFromDatabase(const std::string& dbPath, int recordId);

    // ========== 🔥 用户滤镜配置管理 ==========
    bool CreateUserFilterConfigTable();

    // ===== WPE filter group permission management =====
    bool CreateWPEFilterGroupTables();
    int SaveWPEFilterGroup(const WPEFilterGroupRecord& group);
    bool DeleteWPEFilterGroup(int groupId);
    std::vector<WPEFilterGroupRecord> LoadWPEFilterGroups(bool includeDisabled = false);
    WPEFilterGroupRecord LoadWPEFilterGroup(int groupId);

    bool SaveUserWPEFilterGroups(const std::string& instanceId, const std::string& username,
                                 const std::vector<int>& groupIds);
    std::vector<int> LoadUserWPEFilterGroups(const std::string& instanceId, const std::string& username);
    std::map<std::string, std::vector<int>> LoadAllUserWPEFilterGroups(const std::string& instanceId);

    bool SaveCardWPEFilterGroups(const std::string& cardKey, const std::vector<int>& groupIds);
    std::vector<int> LoadCardWPEFilterGroups(const std::string& cardKey);
    bool DeleteCardWPEFilterGroups(const std::string& cardKey);

    // 保存用户滤镜配置
    bool SaveUserFilterConfig(const std::string& instanceId, const std::string& username,
                              const std::vector<int>& enabledFilterIds);

    // 加载用户滤镜配置
    std::vector<int> LoadUserFilterConfig(const std::string& instanceId, const std::string& username);

    // 加载指定实例的所有用户配置
    std::map<std::string, std::vector<int>> LoadAllUserFilterConfigs(const std::string& instanceId);

    // 删除用户滤镜配置
    bool DeleteUserFilterConfig(const std::string& instanceId, const std::string& username);

    // 清空指定实例的所有用户配置
    bool ClearInstanceUserFilterConfigs(const std::string& instanceId);

    // ===== 默认滤镜配置管理 =====
    // 保存实例的默认滤镜配置（对所有账号生效）
    bool SaveDefaultFilterConfig(const std::string& instanceId, const std::vector<int>& filterIds);

    // 加载实例的默认滤镜配置
    std::vector<int> LoadDefaultFilterConfig(const std::string& instanceId);

    // 删除实例的默认滤镜配置
    bool DeleteDefaultFilterConfig(const std::string& instanceId);

    // ===== 🔥 滤镜执行次数统计 =====
    // 创建滤镜执行次数表
    bool CreateFilterExecutionStatsTable();

    // 增加滤镜执行次数（内存缓存，异步批量写入）
    void IncrementFilterExecutionCount(const std::string& instanceId, const std::string& username, int filterId);

    // 强制刷新缓存到数据库
    void FlushFilterExecutionStats();

    // 获取用户所有滤镜的执行次数
    std::map<int, int64_t> GetUserFilterExecutionCounts(const std::string& instanceId, const std::string& username);

    // 重置用户滤镜执行次数
    bool ResetUserFilterExecutionCounts(const std::string& instanceId, const std::string& username);

    // 重置指定滤镜的执行次数
    bool ResetFilterExecutionCount(const std::string& instanceId, const std::string& username, int filterId);

private:
    // 🔥 滤镜执行次数缓存
    struct FilterExecutionKey {
        std::string instanceId;
        std::string username;
        int filterId;

        bool operator<(const FilterExecutionKey& other) const {
            if (instanceId != other.instanceId) return instanceId < other.instanceId;
            if (username != other.username) return username < other.username;
            return filterId < other.filterId;
        }
    };
    std::map<FilterExecutionKey, int64_t> filterExecutionCache;  // 内存缓存
    std::map<std::string, std::map<std::string, std::map<int, int64_t>>> filterExecutionSnapshot;
    std::mutex filterExecutionMutex;
    std::mutex filterExecutionSnapshotMutex;
    std::chrono::steady_clock::time_point lastFlushTime;  // 上次刷新时间
    const int FLUSH_INTERVAL_SECONDS = 10;  // 每10秒刷新一次
    const int FLUSH_THRESHOLD = 1000;  // 缓存超过1000条记录时立即刷新

    // 🔥 后台刷新线程
    std::thread flushThread;
    std::atomic<bool> flushThreadRunning{false};
    std::condition_variable flushCondition;
    std::mutex flushMutex;

    void FlushThreadWorker();  // 后台刷新线程工作函数
};
