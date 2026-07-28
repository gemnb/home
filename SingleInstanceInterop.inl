// SingleInstanceInterop.inl
// 说明：
// - 该文件被 `新伪心跳.cpp` include，因此与主程序处于同一翻译单元
// - 目标：把"单伪项目"的采集/伪心跳核心与UI按多实例隔离方式接入 InstanceManager
// - 方案：完整复制原生单伪项目的UI和逻辑，每个实例有独立的上下文

#pragma once

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <json/json.h>

// Windows头文件（用于目录选择对话框）
#include <Windows.h>
#include <shlobj.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

#include "SingleInstanceInterop.h"
#include "InstanceManager.h"
#include "Logger.h"
#include "TcpFileServer.h"

namespace SingleInstanceInterop {
namespace {

// ==================== 枚举定义（与原生单伪项目一致）====================

// 数据包读取顺序模式
enum class PacketOrderMode {
    ASCENDING = 0,   // 升序模式（先采集先使用）
    DESCENDING = 1   // 降序模式（最新数据优先）
};

// 采集子菜单
enum CollectorSubMenu {
    COLLECTOR_SUBMENU_MAIN = 0,        // 主页（控制面板+表格）
    COLLECTOR_SUBMENU_STORAGE,         // 存储模式配置
    COLLECTOR_SUBMENU_SECONDARY_PROXY, // 二级代理配置
    COLLECTOR_SUBMENU_PACKET_TYPE,     // 包头类型管理
    COLLECTOR_SUBMENU_SPECIAL_ID,      // 特殊ID采集控制
    COLLECTOR_SUBMENU_DISPLAY_FILTER,  // 包头显示过滤
    COLLECTOR_SUBMENU_TARGET_FILTER,   // 目标地址过滤
    COLLECTOR_SUBMENU_WHITELIST,       // 采集白名单
    COLLECTOR_SUBMENU_BLACKLIST,       // 采集黑名单
    COLLECTOR_SUBMENU_TCP_SHARE,       // TCP文件共享服务
    COLLECTOR_SUBMENU_SOCKS5_ACCOUNTS  // SOCKS5账号管理
};

// 伪心跳子菜单
enum HeartbeatSubMenu {
    HEARTBEAT_SUBMENU_MAIN = 0,        // 主页
    HEARTBEAT_SUBMENU_REPLACE_MODE,    // 替换模式
    HEARTBEAT_SUBMENU_SPECIAL_ID,      // 特殊ID
    HEARTBEAT_SUBMENU_VTD_FILTER,      // VTD滤镜
    HEARTBEAT_SUBMENU_PATTERN23,       // 23偏移
    HEARTBEAT_SUBMENU_PATTERN09,       // 09偏移
    HEARTBEAT_SUBMENU_WHITELIST,       // 白名单
    HEARTBEAT_SUBMENU_BLACKLIST,       // 黑名单
    HEARTBEAT_SUBMENU_FIXED_PACKET,    // 固定包
    HEARTBEAT_SUBMENU_SECONDARY_PROXY, // 二级代理
    HEARTBEAT_SUBMENU_FORWARD_CONTROL, // 转发控制
    HEARTBEAT_SUBMENU_PACKET_TYPE,     // 包头类型
    HEARTBEAT_SUBMENU_DATA_POOL,       // 数据池
    HEARTBEAT_SUBMENU_TARGET_FILTER,   // 目标过滤
    HEARTBEAT_SUBMENU_SOCKS5_ACCOUNTS  // SOCKS5账号管理
};

// 悬浮窗口结构
struct SingleFloatingWindow {
    bool isOpen = false;
    int menuType = 0;
    bool isCollector = true;
    int windowId = 0;
};

// GUI状态
struct SingleGUIState {
    int leftSelectedId = -1;
    int rightSelectedId = -1;
    bool showLeftDetail = false;
    bool showRightDetail = false;
    bool leftMinimized = false;
    bool rightMinimized = false;
    bool autoScroll = true;
};

// 白名单规则
struct SingleWhitelistRule {
    int id = 0;
    std::string name;
    std::string pattern;
    int offset = 0;
    bool isHex = true;
    bool enabled = true;
};

// 黑名单规则
struct SingleBlacklistRule {
    int id = 0;
    std::string name;
    std::string pattern;
    int offset = 0;
    bool isHex = true;
    bool enabled = true;
};

// 偏移替换规则
struct SingleOffsetRule {
    int id = 0;
    std::string name;
    int offset = 0;
    int length = 0;
    std::string replaceValue;
    bool isHex = true;
    bool enabled = true;
};

// 替换数量规则 - 到达后策略枚举
enum class SinglePostLimitStrategy {
    SendOriginal = 0,             // 达到后：后续只发原包
    CycleFakeThenOriginal = 1     // 达到后：按比例循环（先发N条伪包，再发M条原包）
};

// 替换数量规则（与原生项目对齐）
struct SingleReplaceCountRule {
    int id = 0;
    bool enabled = true;
    std::string name;

    // 触发阈值：成功替换(发伪包)达到 N 次后进入"到达后策略"
    int replaceLimit = 1;

    SinglePostLimitStrategy postLimitStrategy = SinglePostLimitStrategy::SendOriginal;

    // 到达阈值时是否清理数据池中该 GameID 的数据
    bool clearPoolOnReach = false;

    // 到达后"原包段"是否仍执行 WPE 滤镜
    bool applyWpeOnOriginalSegment = true;

    // 断开连接后是否重置计数
    bool resetCountOnDisconnect = false;

    // 仅当 postLimitStrategy == CycleFakeThenOriginal 生效
    int postLimitFakeN = 1;
    int postLimitOriginalN = 1;
};

// 替换数量规则运行时状态
struct SingleReplaceRuntimeState {
    int ruleId = 0;
    uint64_t rulesVersion = 0;
    int replacementsDone = 0;      // 已成功替换(发伪包)次数
    bool reached = false;          // 是否已到达阈值
    bool reachCleanupDone = false; // 到达时清理是否已执行

    // post-limit cycle 状态（仅 CycleFakeThenOriginal 使用）
    bool cycleSendFake = true;
    int cycleRemaining = 0;
};

// 顺序模式配置（按ID长度）
struct SingleSequentialConfig {
    int startId = 1;          // 起始ID
    int endId = 1000;         // 结束ID
    bool enabled = true;      // 是否启用
    int currentCount = 0;     // 当前该长度的数据量（只读，用于显示）

    SingleSequentialConfig() : startId(1), endId(1000), enabled(true), currentCount(0) {}
    SingleSequentialConfig(int start, int end, bool en = true) : startId(start), endId(end), enabled(en), currentCount(0) {}
};

// ==================== 采集实例上下文 ====================
struct SingleCollectorCtx {
    std::string instanceId;
    std::string configDbPath;

    // 基础配置
    int listenPort = 1080;
    char portBuffer[16] = "1080";
    bool portInitializedFromInstance = false;

    // 记录控制
    bool enableRecording = true;
    bool enableVerboseLogging = false;

    // 数据包列表
    std::vector<DisplayPacket> packets;
    std::mutex packetsMutex;
    int nextPacketId = 1;

    // GUI状态
    SingleGUIState guiState;
    int collectorSubMenu = COLLECTOR_SUBMENU_MAIN;

    // 悬浮窗口
    std::vector<SingleFloatingWindow> floatingWindows;
    int floatingWindowIdCounter = 0;

    // 存储模式
    StorageMode storageMode = StorageMode::MEMORY;

    // 数据库目录配置（按小时/按天写入时使用）
    char dbDirectory[512] = "";           // 数据库存储目录
    char dbNameBuffer[128] = "";          // 新建数据库名称输入
    std::string selectedDbPath;           // 当前选中的数据库完整路径
    std::vector<std::string> dbFileList;  // 目录下的数据库文件列表
    int selectedDbIndex = -1;             // 选中的数据库索引
    bool showCreateDbDialog = false;      // 是否显示创建数据库对话框

    // 二级代理
    bool enableSecondaryProxy = false;
    char secondaryProxyHost[256] = "";
    char secondaryProxyPort[16] = "1081";
    char secondaryProxyUsername[128] = "";
    char secondaryProxyPassword[128] = "";
    bool secondaryProxyShowPassword = false;

    // 包头类型
    std::vector<int> packetTypes = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x62};
    std::map<int, bool> packetTypeEnabled;
    char customTypeBuffer[16] = "";
    bool disablePacketHeaderFilter = false;

    // 特殊ID
    bool allowCollect00ID = false;
    bool allowCollectOBID = false;
    bool enableCollect62Pattern = true;

    // 显示过滤
    std::map<int, bool> displayFilter;

    // 目标过滤
    int filterType = 0;
    char filterValue[256] = "";

    // 白名单
    bool enableWhitelist = false;
    std::vector<SingleWhitelistRule> whitelist;
    int nextWhitelistId = 1;
    bool showWhitelistEditWindow = false;
    int editingWhitelistIndex = -1;
    char whitelistEditName[128] = "";
    char whitelistEditPattern[512] = "";
    char whitelistEditOffset[16] = "0";
    bool whitelistEditIsHex = true;

    // 黑名单
    bool enableBlacklist = false;
    std::vector<SingleBlacklistRule> blacklist;
    int nextBlacklistId = 1;
    bool showBlacklistEditWindow = false;
    int editingBlacklistIndex = -1;
    char blacklistEditName[128] = "";
    char blacklistEditPattern[512] = "";
    char blacklistEditOffset[16] = "0";
    bool blacklistEditIsHex = true;

    // 统计
    std::atomic<int> packetCounter{0};
    std::atomic<int> whitelistHitCounter{0};
    std::atomic<int> blacklistHitCounter{0};

    // 断开自动清理
    bool enableDisconnectAutoClear = true;

    // SOCKS认证
    bool enableSocksAuth = false;

    // ===== 绑定SOCKS5账号库实例 =====
    std::string boundSocks5PoolId;  // 绑定的SOCKS5账号库实例ID（空=使用全局账号）

    // TCP文件共享服务
    int tcpFileServerPort = 9000;
    char tcpFileServerPortBuffer[16] = "9000";
    TcpFileServer* tcpFileServer = nullptr;

    // 数据库
    std::unique_ptr<DatabaseManager> db;

    SingleCollectorCtx() {
        // 初始化包头类型启用状态
        for (int type : packetTypes) {
            packetTypeEnabled[type] = true;
        }
    }

    ~SingleCollectorCtx() {
        // 清理TCP文件服务器
        if (tcpFileServer) {
            tcpFileServer->Stop();
            delete tcpFileServer;
            tcpFileServer = nullptr;
        }
    }
};

// ==================== 伪心跳实例上下文 ====================
struct SingleHeartbeatCtx {
    std::string instanceId;
    std::string configDbPath;

    // 基础配置
    int heartbeatPort = 1081;
    char heartbeatPortBuffer[16] = "1081";
    bool portInitializedFromInstance = false;

    // 转发控制
    bool enableHeartbeatForward = true;
    bool enableHeartbeatRecording = true;

    // 数据包列表
    std::vector<DisplayPacket> heartbeatPackets;
    std::mutex heartbeatPacketsMutex;
    int nextHeartbeatPacketId = 1;

    // GUI状态
    SingleGUIState guiState;
    int heartbeatSubMenu = HEARTBEAT_SUBMENU_MAIN;

    // 悬浮窗口
    std::vector<SingleFloatingWindow> floatingWindows;
    int floatingWindowIdCounter = 0;

    // 替换模式: 0=随机, 1=顺序, 2=固定
    int replaceMode = 0;

    // 数据包读取顺序模式
    PacketOrderMode packetOrderMode = PacketOrderMode::ASCENDING;

    // CRC32自动计算
    bool enableCRC32AutoCalc = true;

    // 顺序模式配置（按ID长度）
    std::map<int, SingleSequentialConfig> sequentialConfigs;
    std::mutex sequentialConfigMutex;
    char newSeqLengthBuffer[8] = "19";
    char newSeqStartIdBuffer[16] = "1";
    char newSeqEndIdBuffer[16] = "1000";

    // 顺序模式位置追踪
    std::map<std::string, int> gameIdSequentialPosition;  // GameID -> 当前位置
    std::mutex sequentialPositionMutex;

    // 23模式: 0=静态, 1=动态
    int pattern23Mode = 0;
    int pattern23DynamicOffset = 0;

    // VTD滤镜
    bool enableVTDFilter = false;
    bool enable03Filter = false;

    // 特殊ID
    bool allowHeartbeat00ID = false;
    bool allowHeartbeatOBID = false;
    bool enableHeartbeat62Pattern = true;

    // 包头类型
    std::vector<int> packetTypes = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x62};
    std::map<int, bool> packetTypeEnabled;
    char customTypeBuffer[16] = "";
    bool disableHeartbeatHeaderFilter = false;

    // 23偏移替换
    bool enablePattern23OffsetReplace = false;
    std::vector<SingleOffsetRule> pattern23OffsetRules;
    int nextPattern23RuleId = 1;
    bool showPattern23EditWindow = false;
    int editingPattern23Index = -1;
    char pattern23EditName[128] = "";
    char pattern23EditOffset[16] = "0";
    char pattern23EditLength[16] = "0";
    char pattern23EditValue[512] = "";
    bool pattern23EditIsHex = true;

    // 09偏移替换
    bool enablePattern09OffsetReplace = false;
    std::vector<SingleOffsetRule> pattern09OffsetRules;
    int nextPattern09RuleId = 1;
    bool showPattern09EditWindow = false;
    int editingPattern09Index = -1;
    char pattern09EditName[128] = "";
    char pattern09EditOffset[16] = "0";
    char pattern09EditLength[16] = "0";
    char pattern09EditValue[512] = "";
    bool pattern09EditIsHex = true;

    // 白名单
    bool enableWhitelist = false;
    std::vector<SingleWhitelistRule> whitelist;
    int nextWhitelistId = 1;
    bool showWhitelistEditWindow = false;
    int editingWhitelistIndex = -1;
    char whitelistEditName[128] = "";
    char whitelistEditPattern[512] = "";
    char whitelistEditOffset[16] = "0";
    bool whitelistEditIsHex = true;

    // 黑名单
    bool enableBlacklist = false;
    std::vector<SingleBlacklistRule> blacklist;
    int nextBlacklistId = 1;
    bool showBlacklistEditWindow = false;
    int editingBlacklistIndex = -1;
    char blacklistEditName[128] = "";
    char blacklistEditPattern[512] = "";
    char blacklistEditOffset[16] = "0";
    bool blacklistEditIsHex = true;

    // 固定包
    bool enableFixedPacket = false;
    std::vector<uint8_t> fixedPacketData;
    char fixedPacketHex[4096] = "";

    // 二级代理
    bool enableSecondaryProxy = false;
    char secondaryProxyHost[256] = "";
    char secondaryProxyPort[16] = "1081";
    char secondaryProxyUsername[128] = "";
    char secondaryProxyPassword[128] = "";
    bool secondaryProxyShowPassword = false;

    // 目标过滤
    int filterType = 0;
    char filterValue[256] = "";

    // 替换数量限制
    std::vector<SingleReplaceCountRule> replaceCountRules;
    int nextReplaceCountRuleId = 1;
    bool showReplaceCountEditWindow = false;
    int editingReplaceCountIndex = -1;
    std::atomic<uint64_t> replaceCountRulesVersion{1};
    std::map<std::string, SingleReplaceRuntimeState> replaceRuntimeStates;  // key=G:gameID 或 U:username
    std::mutex replaceRuntimeMutex;

    // 替换数量规则编辑数据
    char replaceCountEditName[128] = "";
    char replaceCountEditLimit[16] = "1";
    int replaceCountEditStrategy = 0;  // 0=SendOriginal, 1=CycleFakeThenOriginal
    bool replaceCountEditClearPool = false;
    bool replaceCountEditApplyWpe = true;
    bool replaceCountEditResetOnDisconnect = false;
    char replaceCountEditFakeN[16] = "1";
    char replaceCountEditOriginalN[16] = "1";

    // 数据池配置 - 三种模式: 0=内存池, 1=数据库读取, 2=远程同步
    int dataSourceType = 0;  // 0=内存池(绑定采集实例), 1=数据库读取, 2=远程同步
    std::string boundCollectorId;
    bool useMemoryPool = true;
    char localDbPath[512] = "";  // 本地数据库路径

    // 远程同步配置
    char remoteHost[256] = "127.0.0.1";
    char remotePort[16] = "9000";
    bool enableAutoSync = false;
    int syncIntervalSeconds = 300;
    char syncIntervalBuffer[16] = "300";
    bool isSyncing = false;
    std::string syncStatus;
    float syncProgress = 0.0f;
    uint64_t syncTotalBytes = 0;
    uint64_t syncDownloadedBytes = 0;
    std::atomic<bool> syncRunning{false};
    std::thread syncThread;
    int lastSyncedId = 0;  // 远程同步的最后ID

    // 数据库目录配置（数据库读取模式使用）
    char dbDirectory[512] = "";           // 数据库存储目录
    char dbNameBuffer[128] = "";          // 新建数据库名称输入
    std::string selectedDbPath;           // 当前选中的数据库完整路径
    std::vector<std::string> dbFileList;  // 目录下的数据库文件列表
    int selectedDbIndex = -1;             // 选中的数据库索引
    bool showCreateDbDialog = false;      // 是否显示创建数据库对话框

    // 自动加载配置
    int autoLoadMode = 0;  // 0=按小时增量加载, 1=按天增量加载
    bool autoLoadRunning = false;
    int lastLoadedId = 0;
    std::string dbLoadStatus;
    bool isLoadingDb = false;

    // 自动管理时间配置
    char loadIntervalBuffer[16] = "300";
    char expireTimeBuffer[16] = "3600";
    char cleanupIntervalBuffer[16] = "600";
    int loadInterval = 300;
    int expireTime = 3600;
    int cleanupInterval = 600;

    // ID长度手动清理
    bool cleanupIDLength17 = false;
    bool cleanupIDLength18 = false;
    bool cleanupIDLength19 = true;
    bool cleanupIDLength20 = true;
    bool cleanupIDLength21 = false;
    bool cleanupIDLength22 = false;
    std::vector<int> customCleanupIDLengths;
    std::map<int, bool> customCleanupIDLengthEnabled;
    char customCleanupIDLengthBuffer[8] = "";

    // 包头手动清理
    std::vector<int> manualCleanupPacketTypes;
    std::map<int, bool> manualCleanupTypeEnabled;
    char manualCleanupCustomTypeBuffer[8] = "";

    // 数据池统计
    int loadedPacketCount = 0;
    int availableGameIdCount = 0;
    int maxRecordId = 0;
    std::string lastUpdateTime;
    std::map<int, int> lengthCounts;  // 按长度统计
    std::map<int, int> packetTypeCounts;  // 按包头类型统计

    // 数据池内存存储
    std::vector<HeartbeatRecord> dataPool;
    std::mutex dataPoolMutex;

    // ===== 绑定SOCKS5账号库实例 =====
    std::string boundSocks5PoolId;  // 绑定的SOCKS5账号库实例ID（空=使用全局账号）
    bool enableSocksAuth = false;   // 是否启用SOCKS5认证

    // 数据库
    std::unique_ptr<DatabaseManager> db;

    SingleHeartbeatCtx() {
        // 初始化包头类型启用状态
        for (int type : packetTypes) {
            packetTypeEnabled[type] = true;
        }
    }
};

// ==================== 全局上下文管理 ====================
static std::mutex g_singleCtxMutex;
static std::unordered_map<std::string, std::shared_ptr<SingleCollectorCtx>> g_singleCollectorCtxMap;
static std::unordered_map<std::string, std::shared_ptr<SingleHeartbeatCtx>> g_singleHeartbeatCtxMap;
static std::unordered_map<std::string, std::shared_ptr<CollectedPacketPool>> g_singleCollectorPoolMap;

// ==================== 辅助函数 ====================

static std::string MakeSingleInstanceConfigDbPath(const std::string& instanceId, const char* kind) {
    std::filesystem::create_directories("config/single_instances");
    return std::string("config/single_instances/") + kind + "_" + instanceId + ".db";
}

static const char* GetCollectorSubMenuName(int menuType) {
    switch (menuType) {
    case COLLECTOR_SUBMENU_MAIN: return "主页";
    case COLLECTOR_SUBMENU_STORAGE: return "存储模式";
    case COLLECTOR_SUBMENU_SECONDARY_PROXY: return "二级代理";
    case COLLECTOR_SUBMENU_PACKET_TYPE: return "包头类型";
    case COLLECTOR_SUBMENU_SPECIAL_ID: return "特殊ID";
    case COLLECTOR_SUBMENU_DISPLAY_FILTER: return "显示过滤";
    case COLLECTOR_SUBMENU_TARGET_FILTER: return "目标过滤";
    case COLLECTOR_SUBMENU_WHITELIST: return "白名单";
    case COLLECTOR_SUBMENU_BLACKLIST: return "黑名单";
    case COLLECTOR_SUBMENU_TCP_SHARE: return "TCP共享";
    case COLLECTOR_SUBMENU_SOCKS5_ACCOUNTS: return "SOCKS5账号";
    default: return "未知";
    }
}

static const char* GetHeartbeatSubMenuName(int menuType) {
    switch (menuType) {
    case HEARTBEAT_SUBMENU_MAIN: return "主页";
    case HEARTBEAT_SUBMENU_REPLACE_MODE: return "替换模式";
    case HEARTBEAT_SUBMENU_SPECIAL_ID: return "特殊ID";
    case HEARTBEAT_SUBMENU_VTD_FILTER: return "VTD滤镜";
    case HEARTBEAT_SUBMENU_PATTERN23: return "23偏移";
    case HEARTBEAT_SUBMENU_PATTERN09: return "09偏移";
    case HEARTBEAT_SUBMENU_WHITELIST: return "白名单";
    case HEARTBEAT_SUBMENU_BLACKLIST: return "黑名单";
    case HEARTBEAT_SUBMENU_FIXED_PACKET: return "固定包";
    case HEARTBEAT_SUBMENU_SECONDARY_PROXY: return "二级代理";
    case HEARTBEAT_SUBMENU_FORWARD_CONTROL: return "转发控制";
    case HEARTBEAT_SUBMENU_PACKET_TYPE: return "包头类型";
    case HEARTBEAT_SUBMENU_DATA_POOL: return "数据池";
    case HEARTBEAT_SUBMENU_TARGET_FILTER: return "目标过滤";
    case HEARTBEAT_SUBMENU_SOCKS5_ACCOUNTS: return "SOCKS5账号";
    default: return "未知";
    }
}

// ==================== 前向声明 ====================
static std::vector<std::string> ScanDbFilesInDirectory(const std::string& directory);

// ==================== 上下文获取/创建 ====================

static std::shared_ptr<SingleCollectorCtx> GetOrCreateSingleCollectorCtx(const std::string& instanceId) {
    std::lock_guard<std::mutex> lock(g_singleCtxMutex);
    auto it = g_singleCollectorCtxMap.find(instanceId);
    if (it != g_singleCollectorCtxMap.end()) return it->second;

    auto ctx = std::make_shared<SingleCollectorCtx>();
    ctx->instanceId = instanceId;
    ctx->configDbPath = MakeSingleInstanceConfigDbPath(instanceId, "collector");

    // 初始化配置数据库
    ctx->db = std::make_unique<DatabaseManager>();
    ctx->db->SetStorageMode(StorageMode::MEMORY);
    ctx->db->InitializeConfig(ctx->configDbPath);

    // 加载已保存的配置
    std::string val;
    val = ctx->db->GetConfigValue("enable_recording");
    if (!val.empty()) ctx->enableRecording = (val == "1");
    val = ctx->db->GetConfigValue("enable_verbose_logging");
    if (!val.empty()) ctx->enableVerboseLogging = (val == "1");
    val = ctx->db->GetConfigValue("storage_mode");
    if (!val.empty()) ctx->storageMode = static_cast<StorageMode>(std::stoi(val));
    val = ctx->db->GetConfigValue("enable_secondary_proxy");
    if (!val.empty()) ctx->enableSecondaryProxy = (val == "1");
    val = ctx->db->GetConfigValue("secondary_proxy_host");
    if (!val.empty()) strncpy(ctx->secondaryProxyHost, val.c_str(), sizeof(ctx->secondaryProxyHost) - 1);
    val = ctx->db->GetConfigValue("secondary_proxy_port");
    if (!val.empty()) strncpy(ctx->secondaryProxyPort, val.c_str(), sizeof(ctx->secondaryProxyPort) - 1);
    val = ctx->db->GetConfigValue("allow_collect_00id");
    if (!val.empty()) ctx->allowCollect00ID = (val == "1");
    val = ctx->db->GetConfigValue("allow_collect_obid");
    if (!val.empty()) ctx->allowCollectOBID = (val == "1");
    val = ctx->db->GetConfigValue("enable_collect_62_pattern");
    if (!val.empty()) ctx->enableCollect62Pattern = (val == "1");
    val = ctx->db->GetConfigValue("enable_whitelist");
    if (!val.empty()) ctx->enableWhitelist = (val == "1");
    val = ctx->db->GetConfigValue("enable_blacklist");
    if (!val.empty()) ctx->enableBlacklist = (val == "1");
    val = ctx->db->GetConfigValue("enable_disconnect_auto_clear");
    if (!val.empty()) ctx->enableDisconnectAutoClear = (val == "1");
    val = ctx->db->GetConfigValue("enable_socks_auth");
    if (!val.empty()) ctx->enableSocksAuth = (val == "1");
    val = ctx->db->GetConfigValue("tcp_file_server_port");
    if (!val.empty()) {
        ctx->tcpFileServerPort = std::stoi(val);
        sprintf_s(ctx->tcpFileServerPortBuffer, "%d", ctx->tcpFileServerPort);
    }

    // 加载数据库目录配置
    val = ctx->db->GetConfigValue("db_directory");
    if (!val.empty()) {
        strncpy(ctx->dbDirectory, val.c_str(), sizeof(ctx->dbDirectory) - 1);
        // 设置DatabaseManager的数据目录
        ctx->db->SetDataDirectory(ctx->dbDirectory);
        // 扫描目录下的数据库文件
        ctx->dbFileList = ScanDbFilesInDirectory(ctx->dbDirectory);
    }
    val = ctx->db->GetConfigValue("selected_db_path");
    if (!val.empty()) {
        ctx->selectedDbPath = val;
        // 找到选中文件的索引
        std::string fileName = std::filesystem::path(val).filename().string();
        for (size_t i = 0; i < ctx->dbFileList.size(); i++) {
            if (ctx->dbFileList[i] == fileName) {
                ctx->selectedDbIndex = (int)i;
                break;
            }
        }
    }

    // ===== 加载绑定的SOCKS5账号库实例ID =====
    val = ctx->db->GetConfigValue("bound_socks5_pool_id");
    if (!val.empty()) ctx->boundSocks5PoolId = val;

    g_singleCollectorCtxMap.emplace(instanceId, ctx);
    return ctx;
}

static std::shared_ptr<SingleHeartbeatCtx> GetOrCreateSingleHeartbeatCtx(const std::string& instanceId) {
    std::lock_guard<std::mutex> lock(g_singleCtxMutex);
    auto it = g_singleHeartbeatCtxMap.find(instanceId);
    if (it != g_singleHeartbeatCtxMap.end()) return it->second;

    auto ctx = std::make_shared<SingleHeartbeatCtx>();
    ctx->instanceId = instanceId;
    ctx->configDbPath = MakeSingleInstanceConfigDbPath(instanceId, "heartbeat");

    // 初始化配置数据库
    ctx->db = std::make_unique<DatabaseManager>();
    ctx->db->SetStorageMode(StorageMode::MEMORY);
    ctx->db->InitializeConfig(ctx->configDbPath);

    // 加载已保存的配置
    std::string val;
    val = ctx->db->GetConfigValue("enable_heartbeat_forward");
    if (!val.empty()) ctx->enableHeartbeatForward = (val == "1");
    val = ctx->db->GetConfigValue("enable_heartbeat_recording");
    if (!val.empty()) ctx->enableHeartbeatRecording = (val == "1");
    val = ctx->db->GetConfigValue("replace_mode");
    if (!val.empty()) ctx->replaceMode = std::stoi(val);
    val = ctx->db->GetConfigValue("pattern23_mode");
    if (!val.empty()) ctx->pattern23Mode = std::stoi(val);
    val = ctx->db->GetConfigValue("pattern23_dynamic_offset");
    if (!val.empty()) ctx->pattern23DynamicOffset = std::stoi(val);
    val = ctx->db->GetConfigValue("enable_vtd_filter");
    if (!val.empty()) ctx->enableVTDFilter = (val == "1");
    val = ctx->db->GetConfigValue("enable_03_filter");
    if (!val.empty()) ctx->enable03Filter = (val == "1");
    val = ctx->db->GetConfigValue("allow_heartbeat_00id");
    if (!val.empty()) ctx->allowHeartbeat00ID = (val == "1");
    val = ctx->db->GetConfigValue("allow_heartbeat_obid");
    if (!val.empty()) ctx->allowHeartbeatOBID = (val == "1");
    val = ctx->db->GetConfigValue("enable_heartbeat_62_pattern");
    if (!val.empty()) ctx->enableHeartbeat62Pattern = (val == "1");
    val = ctx->db->GetConfigValue("enable_whitelist");
    if (!val.empty()) ctx->enableWhitelist = (val == "1");
    val = ctx->db->GetConfigValue("enable_blacklist");
    if (!val.empty()) ctx->enableBlacklist = (val == "1");
    val = ctx->db->GetConfigValue("enable_secondary_proxy");
    if (!val.empty()) ctx->enableSecondaryProxy = (val == "1");
    val = ctx->db->GetConfigValue("secondary_proxy_host");
    if (!val.empty()) strncpy(ctx->secondaryProxyHost, val.c_str(), sizeof(ctx->secondaryProxyHost) - 1);
    val = ctx->db->GetConfigValue("secondary_proxy_port");
    if (!val.empty()) strncpy(ctx->secondaryProxyPort, val.c_str(), sizeof(ctx->secondaryProxyPort) - 1);

    // 加载CRC32自动计算配置
    val = ctx->db->GetConfigValue("enable_crc32_auto_calc");
    if (!val.empty()) ctx->enableCRC32AutoCalc = (val == "1");

    // 加载数据包读取顺序模式
    val = ctx->db->GetConfigValue("packet_order_mode");
    if (!val.empty()) ctx->packetOrderMode = static_cast<PacketOrderMode>(std::stoi(val));

    // 加载全伪模式
    val = ctx->db->GetConfigValue("disable_heartbeat_header_filter");
    if (!val.empty()) ctx->disableHeartbeatHeaderFilter = (val == "1");

    // 加载顺序模式配置
    val = ctx->db->GetConfigValue("sequential_configs");
    if (!val.empty()) {
        Json::CharReaderBuilder builder;
        Json::Value root;
        std::string errs;
        std::istringstream iss(val);
        if (Json::parseFromStream(builder, iss, &root, &errs) && root.isArray()) {
            std::lock_guard<std::mutex> seqLock(ctx->sequentialConfigMutex);
            ctx->sequentialConfigs.clear();
            for (const auto& item : root) {
                int length = item.get("length", 0).asInt();
                if (length > 0) {
                    SingleSequentialConfig cfg;
                    cfg.startId = item.get("startId", 1).asInt();
                    cfg.endId = item.get("endId", 1000).asInt();
                    cfg.enabled = item.get("enabled", true).asBool();
                    ctx->sequentialConfigs[length] = cfg;
                }
            }
        }
    }

    // 加载替换数量规则
    val = ctx->db->GetConfigValue("replace_count_rules");
    if (!val.empty()) {
        Json::CharReaderBuilder builder;
        Json::Value root;
        std::string errs;
        std::istringstream iss(val);
        if (Json::parseFromStream(builder, iss, &root, &errs) && root.isArray()) {
            ctx->replaceCountRules.clear();
            int maxId = 0;
            for (const auto& r : root) {
                SingleReplaceCountRule rule;
                rule.id = r.get("id", 0).asInt();
                rule.enabled = r.get("enabled", true).asBool();
                rule.name = r.get("name", "").asString();
                rule.replaceLimit = std::max(1, r.get("replaceLimit", 1).asInt());
                rule.postLimitStrategy = static_cast<SinglePostLimitStrategy>(r.get("postLimitStrategy", 0).asInt());
                rule.clearPoolOnReach = r.get("clearPoolOnReach", false).asBool();
                rule.applyWpeOnOriginalSegment = r.get("applyWpeOnOriginalSegment", true).asBool();
                rule.resetCountOnDisconnect = r.get("resetCountOnDisconnect", false).asBool();
                rule.postLimitFakeN = std::max(0, r.get("postLimitFakeN", 1).asInt());
                rule.postLimitOriginalN = std::max(0, r.get("postLimitOriginalN", 1).asInt());
                if (rule.id > 0) {
                    ctx->replaceCountRules.push_back(rule);
                    if (rule.id > maxId) maxId = rule.id;
                }
            }
            ctx->nextReplaceCountRuleId = maxId + 1;
        }
    }

    // 加载数据源配置
    val = ctx->db->GetConfigValue("data_source_type");
    if (!val.empty()) ctx->dataSourceType = std::stoi(val);
    val = ctx->db->GetConfigValue("bound_collector_id");
    if (!val.empty()) ctx->boundCollectorId = val;
    val = ctx->db->GetConfigValue("use_memory_pool");
    if (!val.empty()) ctx->useMemoryPool = (val == "1");
    val = ctx->db->GetConfigValue("local_db_path");
    if (!val.empty()) strncpy(ctx->localDbPath, val.c_str(), sizeof(ctx->localDbPath) - 1);
    val = ctx->db->GetConfigValue("remote_host");
    if (!val.empty()) strncpy(ctx->remoteHost, val.c_str(), sizeof(ctx->remoteHost) - 1);
    val = ctx->db->GetConfigValue("remote_port");
    if (!val.empty()) strncpy(ctx->remotePort, val.c_str(), sizeof(ctx->remotePort) - 1);
    val = ctx->db->GetConfigValue("enable_auto_sync");
    if (!val.empty()) ctx->enableAutoSync = (val == "1");
    val = ctx->db->GetConfigValue("sync_interval_seconds");
    if (!val.empty()) ctx->syncIntervalSeconds = std::stoi(val);

    // 加载数据库目录配置（数据库读取模式）
    val = ctx->db->GetConfigValue("hb_db_directory");
    if (!val.empty()) strncpy(ctx->dbDirectory, val.c_str(), sizeof(ctx->dbDirectory) - 1);
    val = ctx->db->GetConfigValue("hb_selected_db_path");
    if (!val.empty()) ctx->selectedDbPath = val;
    val = ctx->db->GetConfigValue("hb_auto_load_mode");
    if (!val.empty()) ctx->autoLoadMode = std::stoi(val);

    // 加载远程同步配置
    val = ctx->db->GetConfigValue("hb_remote_host");
    if (!val.empty()) strncpy(ctx->remoteHost, val.c_str(), sizeof(ctx->remoteHost) - 1);
    val = ctx->db->GetConfigValue("hb_remote_port");
    if (!val.empty()) strncpy(ctx->remotePort, val.c_str(), sizeof(ctx->remotePort) - 1);
    val = ctx->db->GetConfigValue("hb_enable_auto_sync");
    if (!val.empty()) ctx->enableAutoSync = (val == "1");
    val = ctx->db->GetConfigValue("hb_sync_interval");
    if (!val.empty()) {
        ctx->syncIntervalSeconds = std::stoi(val);
        sprintf_s(ctx->syncIntervalBuffer, "%d", ctx->syncIntervalSeconds);
    }

    // 如果有保存的目录，扫描数据库文件列表
    if (strlen(ctx->dbDirectory) > 0) {
        ctx->dbFileList = ScanDbFilesInDirectory(ctx->dbDirectory);
        // 找到之前选中的数据库索引
        if (!ctx->selectedDbPath.empty()) {
            std::filesystem::path selPath(ctx->selectedDbPath);
            std::string selFilename = selPath.filename().string();
            for (size_t i = 0; i < ctx->dbFileList.size(); i++) {
                if (ctx->dbFileList[i] == selFilename) {
                    ctx->selectedDbIndex = (int)i;
                    break;
                }
            }
        }
    }

    // ===== 加载绑定的SOCKS5账号库实例ID =====
    val = ctx->db->GetConfigValue("bound_socks5_pool_id");
    if (!val.empty()) ctx->boundSocks5PoolId = val;
    val = ctx->db->GetConfigValue("enable_socks_auth");
    if (!val.empty()) ctx->enableSocksAuth = (val == "1");

    g_singleHeartbeatCtxMap.emplace(instanceId, ctx);
    return ctx;
}

static std::shared_ptr<CollectedPacketPool> GetOrCreateSingleCollectorPool(const std::string& instanceId) {
    std::lock_guard<std::mutex> lock(g_singleCtxMutex);
    auto it = g_singleCollectorPoolMap.find(instanceId);
    if (it != g_singleCollectorPoolMap.end()) return it->second;

    auto pool = std::make_shared<CollectedPacketPool>();
    g_singleCollectorPoolMap.emplace(instanceId, pool);
    return pool;
}

// ==================== 前向声明 ====================
static void RenderSingleCollectorSubMenuContent(SingleCollectorCtx* ctx, PacketCollector* collector,
    CollectorInstance* instance, int menuType, bool& needsRedraw);
static void RenderSingleHeartbeatSubMenuContent(SingleHeartbeatCtx* ctx, PacketCollector* forwarder,
    HeartbeatInstance* instance, int menuType, bool& needsRedraw);
static void RenderSingleCollectorFloatingWindows(SingleCollectorCtx* ctx, PacketCollector* collector,
    CollectorInstance* instance, bool& needsRedraw);
static void RenderSingleHeartbeatFloatingWindows(SingleHeartbeatCtx* ctx, PacketCollector* forwarder,
    HeartbeatInstance* instance, bool& needsRedraw);
static void RenderSingleCollectorLeftDetailWindow(SingleCollectorCtx* ctx, bool& needsRedraw);
static void RenderSingleHeartbeatLeftDetailWindow(SingleHeartbeatCtx* ctx, bool& needsRedraw);
static void RenderSingleHeartbeatRightDetailWindow(SingleHeartbeatCtx* ctx, bool& needsRedraw);

// ==================== 辅助函数实现 ====================

// 同步采集实例的SOCKS5账号到PacketCollector
static void SyncCollectorSocks5Accounts(SingleCollectorCtx* ctx, PacketCollector* collector) {
    if (!ctx || !collector) return;

    // 先从旧的账号库取消注册（如果之前绑定过）
    // 遍历所有账号库，取消该 collector 的注册
    auto allPools = InstanceManager::GetInstance().GetSocks5PoolInstances();
    for (auto* p : allPools) {
        p->UnregisterBoundCollector(collector);
    }

    // 先清空collector中的所有账号
    auto existingAccounts = collector->GetAllAccounts();
    for (const auto& acc : existingAccounts) {
        collector->RemoveAccount(acc.username);
    }

    // 从绑定的SOCKS5账号库实例获取账号，并注册到账号库（后续变更自动同步）
    if (!ctx->boundSocks5PoolId.empty()) {
        auto* pool = InstanceManager::GetInstance().GetSocks5PoolInstance(ctx->boundSocks5PoolId);
        if (pool) {
            auto accounts = pool->GetAllAccounts();
            for (const auto& acc : accounts) {
                if (collector->AddAccount(acc.username, acc.password, acc.expireTime, acc.maxConnections)) {
                    Socks5Account updatedAcc = collector->GetAccount(acc.username);
                    updatedAcc.isEnabled = acc.isEnabled;
                    collector->UpdateAccountEx(updatedAcc);
                }
            }
            // 注册到账号库，后续账号变更时自动同步
            pool->RegisterBoundCollector(collector);
        }
    }
}

// 同步伪心跳实例的SOCKS5账号到PacketCollector
static void SyncHeartbeatSocks5Accounts(SingleHeartbeatCtx* ctx, PacketCollector* forwarder) {
    if (!ctx || !forwarder) return;

    // 先从旧的账号库取消注册（如果之前绑定过）
    auto allPools = InstanceManager::GetInstance().GetSocks5PoolInstances();
    for (auto* p : allPools) {
        p->UnregisterBoundCollector(forwarder);
    }

    // 先清空forwarder中的所有账号
    auto existingAccounts = forwarder->GetAllAccounts();
    for (const auto& acc : existingAccounts) {
        forwarder->RemoveAccount(acc.username);
    }

    // 从绑定的SOCKS5账号库实例获取账号，并注册到账号库（后续变更自动同步）
    if (!ctx->boundSocks5PoolId.empty()) {
        auto* pool = InstanceManager::GetInstance().GetSocks5PoolInstance(ctx->boundSocks5PoolId);
        if (pool) {
            auto accounts = pool->GetAllAccounts();
            for (const auto& acc : accounts) {
                if (forwarder->AddAccount(acc.username, acc.password, acc.expireTime, acc.maxConnections)) {
                    Socks5Account updatedAcc = forwarder->GetAccount(acc.username);
                    updatedAcc.isEnabled = acc.isEnabled;
                    forwarder->UpdateAccountEx(updatedAcc);
                }
            }
            // 注册到账号库，后续账号变更时自动同步
            pool->RegisterBoundCollector(forwarder);
        }
    }
}

static std::string BytesToHexPreview(const std::vector<uint8_t>& data, size_t maxBytes = 16) {
    std::stringstream ss;
    size_t len = std::min(data.size(), maxBytes);
    for (size_t i = 0; i < len; i++) {
        ss << std::hex << std::uppercase << std::setfill('0') << std::setw(2) << (int)data[i];
        if (i < len - 1) ss << " ";
    }
    if (data.size() > maxBytes) ss << " ...";
    return ss.str();
}

// 扫描目录下的数据库文件
static std::vector<std::string> ScanDbFilesInDirectory(const std::string& directory) {
    std::vector<std::string> result;
    if (directory.empty()) return result;

    try {
        std::filesystem::path dirPath(directory);
        if (!std::filesystem::exists(dirPath) || !std::filesystem::is_directory(dirPath)) {
            return result;
        }

        for (const auto& entry : std::filesystem::directory_iterator(dirPath)) {
            if (entry.is_regular_file()) {
                std::string ext = entry.path().extension().string();
                // 转换为小写比较
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == ".db" || ext == ".sqlite" || ext == ".sqlite3") {
                    result.push_back(entry.path().filename().string());
                }
            }
        }

        // 按文件名排序
        std::sort(result.begin(), result.end());
    } catch (const std::exception& e) {
        AB_LOG_ERROR("[目录扫描] 扫描目录失败: " + std::string(e.what()));
    }

    return result;
}

// 选择目录对话框（Windows）
static bool SelectDirectoryDialog(char* outPath, size_t pathSize) {
    BROWSEINFOA bi = { 0 };
    bi.lpszTitle = "选择数据库存储目录";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (pidl != nullptr) {
        char path[MAX_PATH];
        if (SHGetPathFromIDListA(pidl, path)) {
            strncpy(outPath, path, pathSize - 1);
            outPath[pathSize - 1] = '\0';
            CoTaskMemFree(pidl);
            return true;
        }
        CoTaskMemFree(pidl);
    }
    return false;
}

// 创建数据库文件
static bool CreateDatabaseFile(const std::string& directory, const std::string& dbName, std::string& outFullPath) {
    if (directory.empty() || dbName.empty()) return false;

    std::string fileName = dbName;
    // 确保有.db扩展名
    if (fileName.size() < 3 || fileName.substr(fileName.size() - 3) != ".db") {
        fileName += ".db";
    }

    std::filesystem::path fullPath = std::filesystem::path(directory) / fileName;
    outFullPath = fullPath.string();

    try {
        // 创建空的SQLite数据库
        sqlite3* db = nullptr;
        int rc = sqlite3_open(outFullPath.c_str(), &db);
        if (rc != SQLITE_OK) {
            AB_LOG_ERROR("[数据库] 创建数据库失败: " + std::string(sqlite3_errmsg(db)));
            if (db) sqlite3_close(db);
            return false;
        }

        // 创建心跳数据表
        const char* createTableSQL = R"(
            CREATE TABLE IF NOT EXISTS heartbeat_records (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                game_id TEXT NOT NULL,
                socks_username TEXT,
                pattern23_data BLOB,
                pattern09_data BLOB,
                pattern62_data BLOB,
                raw_data BLOB,
                packet_size INTEGER,
                packet_type INTEGER,
                is_complete INTEGER DEFAULT 0,
                created_at DATETIME DEFAULT CURRENT_TIMESTAMP
            );
            CREATE INDEX IF NOT EXISTS idx_game_id ON heartbeat_records(game_id);
            CREATE INDEX IF NOT EXISTS idx_socks_username ON heartbeat_records(socks_username);
            CREATE INDEX IF NOT EXISTS idx_created_at ON heartbeat_records(created_at);
        )";

        char* errMsg = nullptr;
        rc = sqlite3_exec(db, createTableSQL, nullptr, nullptr, &errMsg);
        if (rc != SQLITE_OK) {
            AB_LOG_ERROR("[数据库] 创建表失败: " + std::string(errMsg ? errMsg : "未知错误"));
            if (errMsg) sqlite3_free(errMsg);
            sqlite3_close(db);
            return false;
        }

        sqlite3_close(db);
        AB_LOG_INFO("[数据库] 成功创建数据库: " + outFullPath);
        return true;
    } catch (const std::exception& e) {
        AB_LOG_ERROR("[数据库] 创建数据库异常: " + std::string(e.what()));
        return false;
    }
}

// 获取伪心跳实例绑定的采集实例数据池
// 返回nullptr表示未绑定或绑定的采集实例不存在
static std::shared_ptr<CollectedPacketPool> GetBoundCollectorPool(SingleHeartbeatCtx* ctx) {
    if (!ctx || ctx->boundCollectorId.empty()) return nullptr;
    if (ctx->dataSourceType != 0 || !ctx->useMemoryPool) return nullptr;

    std::lock_guard<std::mutex> lock(g_singleCtxMutex);
    auto it = g_singleCollectorPoolMap.find(ctx->boundCollectorId);
    if (it != g_singleCollectorPoolMap.end()) {
        return it->second;
    }
    return nullptr;
}

// 检查伪心跳实例是否可以使用指定采集实例的数据
// 只有当伪心跳绑定了该采集实例时才返回true
static bool CanUseCollectorData(SingleHeartbeatCtx* hbCtx, const std::string& collectorId) {
    if (!hbCtx || collectorId.empty()) return false;
    if (hbCtx->dataSourceType != 0 || !hbCtx->useMemoryPool) return false;
    return hbCtx->boundCollectorId == collectorId;
}

// 从绑定的采集实例获取心跳数据（用于伪心跳替换）
// gameId: 要匹配的游戏ID
// 返回: 是否成功获取数据，数据通过outRecord返回
static bool GetHeartbeatDataFromBoundCollector(
    SingleHeartbeatCtx* ctx, const std::string& gameId, HeartbeatRecord& outRecord) {

    auto pool = GetBoundCollectorPool(ctx);
    if (!pool) return false;

    // 根据替换模式选择不同的获取方式
    if (ctx->replaceMode == 0) {
        // 随机模式 - 使用Peek获取但不移除
        return pool->PeekPacketByGameID(gameId, outRecord);
    } else if (ctx->replaceMode == 1) {
        // 顺序模式 - Pop获取并移除（FIFO）
        return pool->PopPacketByGameID(gameId, outRecord, true);
    } else {
        // 固定模式 - 使用Peek获取但不移除
        return pool->PeekPacketByGameID(gameId, outRecord);
    }
}

// 更新伪心跳实例的数据池统计信息（从绑定的采集实例）
static void UpdateHeartbeatPoolStatsFromBoundCollector(SingleHeartbeatCtx* ctx) {
    if (!ctx) return;

    auto pool = GetBoundCollectorPool(ctx);
    if (pool) {
        auto stats = pool->GetStats();
        ctx->loadedPacketCount = stats.totalPackets;
        ctx->availableGameIdCount = stats.gameIDCount;

        // 更新时间
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S");
        ctx->lastUpdateTime = ss.str();
    } else {
        ctx->loadedPacketCount = 0;
        ctx->availableGameIdCount = 0;
    }
}

static void SaveSingleCollectorConfig(SingleCollectorCtx* ctx) {
    if (!ctx || !ctx->db) return;
    ctx->db->SetConfigValue("enable_recording", ctx->enableRecording ? "1" : "0");
    ctx->db->SetConfigValue("enable_verbose_logging", ctx->enableVerboseLogging ? "1" : "0");
    ctx->db->SetConfigValue("storage_mode", std::to_string(static_cast<int>(ctx->storageMode)));
    ctx->db->SetConfigValue("enable_secondary_proxy", ctx->enableSecondaryProxy ? "1" : "0");
    ctx->db->SetConfigValue("secondary_proxy_host", ctx->secondaryProxyHost);
    ctx->db->SetConfigValue("secondary_proxy_port", ctx->secondaryProxyPort);
    ctx->db->SetConfigValue("allow_collect_00id", ctx->allowCollect00ID ? "1" : "0");
    ctx->db->SetConfigValue("allow_collect_obid", ctx->allowCollectOBID ? "1" : "0");
    ctx->db->SetConfigValue("enable_collect_62_pattern", ctx->enableCollect62Pattern ? "1" : "0");
    ctx->db->SetConfigValue("enable_whitelist", ctx->enableWhitelist ? "1" : "0");
    ctx->db->SetConfigValue("enable_blacklist", ctx->enableBlacklist ? "1" : "0");
    ctx->db->SetConfigValue("enable_disconnect_auto_clear", ctx->enableDisconnectAutoClear ? "1" : "0");
    ctx->db->SetConfigValue("enable_socks_auth", ctx->enableSocksAuth ? "1" : "0");
    ctx->db->SetConfigValue("tcp_file_server_port", std::to_string(ctx->tcpFileServerPort));

    // 数据库目录配置
    ctx->db->SetConfigValue("db_directory", ctx->dbDirectory);
    ctx->db->SetConfigValue("selected_db_path", ctx->selectedDbPath);

    // ===== 保存绑定的SOCKS5账号库实例ID =====
    ctx->db->SetConfigValue("bound_socks5_pool_id", ctx->boundSocks5PoolId);
}

static void SaveSingleHeartbeatConfig(SingleHeartbeatCtx* ctx) {
    if (!ctx || !ctx->db) return;
    ctx->db->SetConfigValue("enable_heartbeat_forward", ctx->enableHeartbeatForward ? "1" : "0");
    ctx->db->SetConfigValue("enable_heartbeat_recording", ctx->enableHeartbeatRecording ? "1" : "0");
    ctx->db->SetConfigValue("replace_mode", std::to_string(ctx->replaceMode));
    ctx->db->SetConfigValue("pattern23_mode", std::to_string(ctx->pattern23Mode));
    ctx->db->SetConfigValue("pattern23_dynamic_offset", std::to_string(ctx->pattern23DynamicOffset));
    ctx->db->SetConfigValue("enable_vtd_filter", ctx->enableVTDFilter ? "1" : "0");
    ctx->db->SetConfigValue("enable_03_filter", ctx->enable03Filter ? "1" : "0");
    ctx->db->SetConfigValue("allow_heartbeat_00id", ctx->allowHeartbeat00ID ? "1" : "0");
    ctx->db->SetConfigValue("allow_heartbeat_obid", ctx->allowHeartbeatOBID ? "1" : "0");
    ctx->db->SetConfigValue("enable_heartbeat_62_pattern", ctx->enableHeartbeat62Pattern ? "1" : "0");
    ctx->db->SetConfigValue("enable_whitelist", ctx->enableWhitelist ? "1" : "0");
    ctx->db->SetConfigValue("enable_blacklist", ctx->enableBlacklist ? "1" : "0");
    ctx->db->SetConfigValue("enable_secondary_proxy", ctx->enableSecondaryProxy ? "1" : "0");
    ctx->db->SetConfigValue("secondary_proxy_host", ctx->secondaryProxyHost);
    ctx->db->SetConfigValue("secondary_proxy_port", ctx->secondaryProxyPort);

    // 新增：CRC32自动计算
    ctx->db->SetConfigValue("enable_crc32_auto_calc", ctx->enableCRC32AutoCalc ? "1" : "0");

    // 新增：数据包读取顺序模式
    ctx->db->SetConfigValue("packet_order_mode", std::to_string(static_cast<int>(ctx->packetOrderMode)));

    // 新增：全伪模式
    ctx->db->SetConfigValue("disable_heartbeat_header_filter", ctx->disableHeartbeatHeaderFilter ? "1" : "0");

    // 新增：顺序模式配置（JSON格式）
    {
        std::lock_guard<std::mutex> lock(ctx->sequentialConfigMutex);
        Json::Value seqRoot(Json::arrayValue);
        for (const auto& pair : ctx->sequentialConfigs) {
            Json::Value item;
            item["length"] = pair.first;
            item["startId"] = pair.second.startId;
            item["endId"] = pair.second.endId;
            item["enabled"] = pair.second.enabled;
            seqRoot.append(item);
        }
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        ctx->db->SetConfigValue("sequential_configs", Json::writeString(builder, seqRoot));
    }

    // 新增：替换数量规则（JSON格式）
    {
        Json::Value rulesRoot(Json::arrayValue);
        for (const auto& rule : ctx->replaceCountRules) {
            Json::Value r;
            r["id"] = rule.id;
            r["enabled"] = rule.enabled;
            r["name"] = rule.name;
            r["replaceLimit"] = rule.replaceLimit;
            r["postLimitStrategy"] = static_cast<int>(rule.postLimitStrategy);
            r["clearPoolOnReach"] = rule.clearPoolOnReach;
            r["applyWpeOnOriginalSegment"] = rule.applyWpeOnOriginalSegment;
            r["resetCountOnDisconnect"] = rule.resetCountOnDisconnect;
            r["postLimitFakeN"] = rule.postLimitFakeN;
            r["postLimitOriginalN"] = rule.postLimitOriginalN;
            rulesRoot.append(r);
        }
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        ctx->db->SetConfigValue("replace_count_rules", Json::writeString(builder, rulesRoot));
    }

    // 数据源配置
    ctx->db->SetConfigValue("data_source_type", std::to_string(ctx->dataSourceType));
    ctx->db->SetConfigValue("bound_collector_id", ctx->boundCollectorId);
    ctx->db->SetConfigValue("use_memory_pool", ctx->useMemoryPool ? "1" : "0");
    ctx->db->SetConfigValue("local_db_path", ctx->localDbPath);
    ctx->db->SetConfigValue("remote_host", ctx->remoteHost);
    ctx->db->SetConfigValue("remote_port", ctx->remotePort);
    ctx->db->SetConfigValue("enable_auto_sync", ctx->enableAutoSync ? "1" : "0");
    ctx->db->SetConfigValue("sync_interval_seconds", std::to_string(ctx->syncIntervalSeconds));

    // 数据库目录配置（数据库读取模式）
    ctx->db->SetConfigValue("hb_db_directory", ctx->dbDirectory);
    ctx->db->SetConfigValue("hb_selected_db_path", ctx->selectedDbPath);
    ctx->db->SetConfigValue("hb_auto_load_mode", std::to_string(ctx->autoLoadMode));

    // 远程同步配置
    ctx->db->SetConfigValue("hb_remote_host", ctx->remoteHost);
    ctx->db->SetConfigValue("hb_remote_port", ctx->remotePort);
    ctx->db->SetConfigValue("hb_enable_auto_sync", ctx->enableAutoSync ? "1" : "0");
    ctx->db->SetConfigValue("hb_sync_interval", std::to_string(ctx->syncIntervalSeconds));

    // ===== 保存绑定的SOCKS5账号库实例ID =====
    ctx->db->SetConfigValue("bound_socks5_pool_id", ctx->boundSocks5PoolId);
    ctx->db->SetConfigValue("enable_socks_auth", ctx->enableSocksAuth ? "1" : "0");
}

// ==================== 采集实例子菜单内容渲染 ====================
static void RenderSingleCollectorSubMenuContent(SingleCollectorCtx* ctx, PacketCollector* collector,
    CollectorInstance* instance, int menuType, bool& needsRedraw) {

    if (!ctx) return;
    bool running = collector && collector->IsRunning();

    switch (menuType) {
    case COLLECTOR_SUBMENU_MAIN:
    {
        ImGuiGBK::TextColored(ImVec4(0.5f, 1.0f, 1.0f, 1.0f), "采集主页");
        ImGui::Separator();
        ImGuiGBK::Text("主页内容请在主界面查看");
    }
    break;

    case COLLECTOR_SUBMENU_STORAGE:
    {
        ImGuiGBK::TextColored(ImVec4(0.5f, 1.0f, 1.0f, 1.0f), "存储模式配置");
        ImGui::Separator();
        ImGui::Spacing();

        const char* storageModeItems[] = {
            ImGuiText::U("按小时写入DB"),
            ImGuiText::U("按天写入DB"),
            ImGuiText::U("内存存储")
        };
        int currentStorageMode = static_cast<int>(ctx->storageMode);

        ImGuiGBK::Text("数据存储方式:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200);
        std::string comboId = "##storageMode_" + ctx->instanceId;
        if (ImGui::Combo(comboId.c_str(), &currentStorageMode, storageModeItems, 3)) {
            ctx->storageMode = static_cast<StorageMode>(currentStorageMode);
            if (ctx->db) {
                ctx->db->SetStorageMode(ctx->storageMode);
            }
            SaveSingleCollectorConfig(ctx);
            needsRedraw = true;
        }

        // 按小时或按天模式时显示目录选择
        if (ctx->storageMode == StorageMode::HOURLY || ctx->storageMode == StorageMode::DAILY) {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGuiGBK::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "数据库存储目录:");
            ImGui::Spacing();

            // 目录路径显示和选择
            ImGuiGBK::Text("当前目录:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(350);
            std::string dirInputId = "##coll_db_dir_" + ctx->instanceId;
            ImGui::InputText(dirInputId.c_str(), ctx->dbDirectory, sizeof(ctx->dbDirectory), ImGuiInputTextFlags_ReadOnly);

            ImGui::SameLine();
            std::string browseDirId = "选择目录##coll_browse_dir_" + ctx->instanceId;
            if (ImGuiGBK::Button(browseDirId.c_str(), ImVec2(80, 0))) {
                if (SelectDirectoryDialog(ctx->dbDirectory, sizeof(ctx->dbDirectory))) {
                    // 设置DatabaseManager的数据目录
                    if (ctx->db) {
                        ctx->db->SetDataDirectory(ctx->dbDirectory);
                    }
                    // 扫描目录下的数据库文件
                    ctx->dbFileList = ScanDbFilesInDirectory(ctx->dbDirectory);
                    ctx->selectedDbIndex = -1;
                    ctx->selectedDbPath = "";
                    SaveSingleCollectorConfig(ctx);
                    needsRedraw = true;
                    AB_LOG_INFO("[单伪采集][" + ctx->instanceId + "] 选择数据库目录: " + std::string(ctx->dbDirectory));
                }
            }

            ImGui::Spacing();

            // 刷新文件列表按钮
            std::string refreshListId = "刷新列表##coll_refresh_db_" + ctx->instanceId;
            if (ImGuiGBK::Button(refreshListId.c_str(), ImVec2(80, 0))) {
                ctx->dbFileList = ScanDbFilesInDirectory(ctx->dbDirectory);
                needsRedraw = true;
            }

            ImGui::SameLine();
            std::string createDbId = "新建数据库##coll_create_db_" + ctx->instanceId;
            if (ImGuiGBK::Button(createDbId.c_str(), ImVec2(100, 0))) {
                if (strlen(ctx->dbDirectory) > 0) {
                    ctx->showCreateDbDialog = true;
                    memset(ctx->dbNameBuffer, 0, sizeof(ctx->dbNameBuffer));
                } else {
                    AB_LOG_WARNING("[单伪采集][" + ctx->instanceId + "] 请先选择数据库目录");
                }
            }

            ImGui::Spacing();

            // 数据库文件列表
            if (strlen(ctx->dbDirectory) > 0) {
                ImGuiGBK::Text("目录下的数据库文件 (%d个):", (int)ctx->dbFileList.size());

                std::string listBoxId = "##coll_db_list_" + ctx->instanceId;
                if (ImGui::BeginListBox(listBoxId.c_str(), ImVec2(-1, 150))) {
                    for (size_t i = 0; i < ctx->dbFileList.size(); i++) {
                        bool isSelected = (ctx->selectedDbIndex == (int)i);
                        if (ImGui::Selectable(ctx->dbFileList[i].c_str(), isSelected)) {
                            ctx->selectedDbIndex = (int)i;
                            ctx->selectedDbPath = (std::filesystem::path(ctx->dbDirectory) / ctx->dbFileList[i]).string();
                            SaveSingleCollectorConfig(ctx);
                            needsRedraw = true;
                            AB_LOG_INFO("[单伪采集][" + ctx->instanceId + "] 选中数据库: " + ctx->selectedDbPath);
                        }
                        if (isSelected) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndListBox();
                }

                // 显示当前选中的数据库
                if (!ctx->selectedDbPath.empty()) {
                    ImGui::Spacing();
                    ImGuiGBK::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "当前选中: %s", ctx->selectedDbPath.c_str());
                }
            } else {
                ImGuiGBK::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "请先选择数据库存储目录");
            }

            // 新建数据库对话框
            if (ctx->showCreateDbDialog) {
                std::string modalId = "新建数据库##coll_create_modal_" + ctx->instanceId;
                ImGui::OpenPopup(ImGuiText::U(modalId.c_str()));

                if (ImGui::BeginPopupModal(ImGuiText::U(modalId.c_str()), &ctx->showCreateDbDialog, ImGuiWindowFlags_AlwaysAutoResize)) {
                    ImGuiGBK::Text("数据库名称:");
                    ImGui::SameLine();
                    std::string nameInputId = "##coll_new_db_name_" + ctx->instanceId;
                    ImGui::SetNextItemWidth(200);
                    ImGui::InputText(nameInputId.c_str(), ctx->dbNameBuffer, sizeof(ctx->dbNameBuffer));

                    ImGui::Spacing();
                    ImGuiGBK::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "提示: 不需要输入.db扩展名");

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    std::string confirmId = "创建##coll_confirm_create_" + ctx->instanceId;
                    if (ImGuiGBK::Button(confirmId.c_str(), ImVec2(100, 0))) {
                        if (strlen(ctx->dbNameBuffer) > 0) {
                            std::string newDbPath;
                            if (CreateDatabaseFile(ctx->dbDirectory, ctx->dbNameBuffer, newDbPath)) {
                                // 刷新列表并选中新创建的数据库
                                ctx->dbFileList = ScanDbFilesInDirectory(ctx->dbDirectory);
                                ctx->selectedDbPath = newDbPath;
                                // 找到新文件的索引
                                std::string newFileName = std::filesystem::path(newDbPath).filename().string();
                                for (size_t i = 0; i < ctx->dbFileList.size(); i++) {
                                    if (ctx->dbFileList[i] == newFileName) {
                                        ctx->selectedDbIndex = (int)i;
                                        break;
                                    }
                                }
                                SaveSingleCollectorConfig(ctx);
                                ctx->showCreateDbDialog = false;
                                needsRedraw = true;
                            }
                        }
                    }

                    ImGui::SameLine();
                    std::string cancelId = "取消##coll_cancel_create_" + ctx->instanceId;
                    if (ImGuiGBK::Button(cancelId.c_str(), ImVec2(100, 0))) {
                        ctx->showCreateDbDialog = false;
                    }

                    ImGui::EndPopup();
                }
            }
        } else {
            // 内存存储模式说明
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
            ImGuiGBK::TextWrapped(
                "内存存储模式: 数据仅保存在内存中，程序关闭后数据将丢失。"
                "适合临时采集或配合伪心跳实时使用。"
            );
            ImGui::PopStyleColor();
        }
    }
    break;

    case COLLECTOR_SUBMENU_SECONDARY_PROXY:
    {
        ImGuiGBK::TextColored(ImVec4(0.5f, 1.0f, 1.0f, 1.0f), "二级代理配置");
        ImGui::Separator();
        ImGui::Spacing();

        std::string checkboxId = "启用二级代理##coll_proxy_" + ctx->instanceId;
        if (ImGuiGBK::Checkbox(checkboxId.c_str(), &ctx->enableSecondaryProxy)) {
            if (running && collector) {
                if (ctx->enableSecondaryProxy) {
                    int port = atoi(ctx->secondaryProxyPort);
                    collector->SetSecondaryProxy(true, ctx->secondaryProxyHost, port,
                        ctx->secondaryProxyUsername, ctx->secondaryProxyPassword);
                } else {
                    collector->SetSecondaryProxy(false, "", 0, "", "");
                }
            }
            SaveSingleCollectorConfig(ctx);
            needsRedraw = true;
        }

        ImGui::Spacing();
        ImGuiGBK::Text("地址:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150);
        std::string hostId = "##c_proxy_host_" + ctx->instanceId;
        ImGui::InputText(hostId.c_str(), ctx->secondaryProxyHost, sizeof(ctx->secondaryProxyHost));

        ImGui::SameLine();
        ImGuiGBK::Text("端口:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        std::string portId = "##c_proxy_port_" + ctx->instanceId;
        ImGui::InputText(portId.c_str(), ctx->secondaryProxyPort, sizeof(ctx->secondaryProxyPort));
    }
    break;

    case COLLECTOR_SUBMENU_PACKET_TYPE:
    {
        ImGuiGBK::TextColored(ImVec4(0.5f, 1.0f, 1.0f, 1.0f), "包头类型管理");
        ImGui::Separator();
        ImGui::Spacing();

        ImGuiGBK::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "当前包头类型:");
        ImGui::Spacing();

        int displayedCount = 0;
        for (int type : ctx->packetTypes) {
            if (displayedCount > 0 && displayedCount % 8 == 0) {
            } else if (displayedCount > 0) {
                ImGui::SameLine();
            }

            char hexBuffer[16];
            sprintf_s(hexBuffer, "0x%02X", type);
            std::string labelId = std::string(hexBuffer) + "##cpt_" + ctx->instanceId + "_" + std::to_string(type);

            bool enabled = ctx->packetTypeEnabled[type];
            if (ImGui::Checkbox(ImGuiText::U(labelId.c_str()), &enabled)) {
                ctx->packetTypeEnabled[type] = enabled;
                needsRedraw = true;
            }
            displayedCount++;
        }
    }
    break;

    case COLLECTOR_SUBMENU_SPECIAL_ID:
    {
        ImGuiGBK::TextColored(ImVec4(0.5f, 1.0f, 1.0f, 1.0f), "特殊ID采集控制");
        ImGui::Separator();
        ImGui::Spacing();

        std::string id00 = "采集00开头ID##coll_" + ctx->instanceId;
        std::string idOB = "采集_OB结尾ID##coll_" + ctx->instanceId;
        std::string id62 = "采集62特征数据##coll_" + ctx->instanceId;

        if (ImGuiGBK::Checkbox(id00.c_str(), &ctx->allowCollect00ID)) {
            SaveSingleCollectorConfig(ctx);
            needsRedraw = true;
        }
        if (ImGuiGBK::Checkbox(idOB.c_str(), &ctx->allowCollectOBID)) {
            SaveSingleCollectorConfig(ctx);
            needsRedraw = true;
        }
        if (ImGuiGBK::Checkbox(id62.c_str(), &ctx->enableCollect62Pattern)) {
            SaveSingleCollectorConfig(ctx);
            needsRedraw = true;
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ===== 断开自动清理 =====
        ImGuiGBK::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "连接管理:");
        ImGui::Spacing();

        std::string disconnectClearId = "断开连接时自动清理数据##coll_disconnect_" + ctx->instanceId;
        if (ImGuiGBK::Checkbox(disconnectClearId.c_str(), &ctx->enableDisconnectAutoClear)) {
            SaveSingleCollectorConfig(ctx);
            if (collector) {
                collector->SetDisconnectClearEnabled(ctx->enableDisconnectAutoClear);
            }
            needsRedraw = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGuiGBK::Text("勾选后，用户断开连接时自动解绑GameID并清理内存池数据");
            ImGuiGBK::Text("取消勾选可保留断开用户的绑定关系和数据包");
            ImGui::EndTooltip();
        }

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
        ImGuiGBK::TextWrapped("提示: 当用户切换到新GameID时，旧GameID的数据包会自动清理");
        ImGui::PopStyleColor();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ===== SOCKS账号认证 =====
        ImGuiGBK::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "SOCKS认证:");
        ImGui::Spacing();

        std::string socksAuthId = "启用SOCKS5账号认证##coll_socks_" + ctx->instanceId;
        if (ImGuiGBK::Checkbox(socksAuthId.c_str(), &ctx->enableSocksAuth)) {
            SaveSingleCollectorConfig(ctx);
            if (collector) {
                collector->SetSocks5Auth(ctx->enableSocksAuth);
            }
            needsRedraw = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGuiGBK::Text("勾选后采集端将启用SOCKS5账号认证");
            ImGuiGBK::Text("需要配置账号才能连接");
            ImGui::EndTooltip();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.15f, 0.15f, 0.2f, 1.0f));
        std::string helpId = "SpecialIDHelp_" + ctx->instanceId;
        ImGui::BeginChild(helpId.c_str(), ImVec2(0, 80), true);
        ImGuiGBK::TextColored(ImVec4(1.0f, 1.0f, 0.5f, 1.0f), "说明:");
        ImGui::Spacing();
        ImGuiGBK::TextWrapped("勾选后将采集对应开头的特殊ID数据包。");
        ImGuiGBK::TextWrapped("取消勾选可过滤掉这些特殊ID，减少无效数据。");
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    break;

    case COLLECTOR_SUBMENU_DISPLAY_FILTER:
    {
        ImGuiGBK::TextColored(ImVec4(0.5f, 1.0f, 1.0f, 1.0f), "包头显示过滤");
        ImGui::Separator();
        ImGui::Spacing();

        ImGuiGBK::TextColored(ImVec4(1.0f, 0.8f, 0.5f, 1.0f), "显示过滤:");
        ImGui::SameLine();
        ImGuiGBK::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "(勾选要显示的包头)");

        ImGui::Spacing();

        int displayedFilterTypes = 0;
        for (int type : ctx->packetTypes) {
            if (displayedFilterTypes > 0 && displayedFilterTypes % 8 == 0) {
            } else if (displayedFilterTypes > 0) {
                ImGui::SameLine();
            }

            char hexBuffer[16];
            sprintf_s(hexBuffer, "0x%02X", type);
            std::string labelId = std::string("显示") + hexBuffer + "##disp_" + ctx->instanceId + "_" + std::to_string(type);

            bool enabled = ctx->displayFilter[type];
            if (ImGui::Checkbox(ImGuiText::U(labelId.c_str()), &enabled)) {
                ctx->displayFilter[type] = enabled;
                needsRedraw = true;
            }
            displayedFilterTypes++;
        }

        ImGui::Spacing();

        std::string allShowId = "全部显示##disp_" + ctx->instanceId;
        if (ImGuiGBK::Button(allShowId.c_str(), ImVec2(100, 0))) {
            for (auto& pair : ctx->displayFilter) {
                pair.second = true;
            }
            needsRedraw = true;
        }

        ImGui::SameLine();
        std::string allHideId = "全不显示##disp_" + ctx->instanceId;
        if (ImGuiGBK::Button(allHideId.c_str(), ImVec2(100, 0))) {
            for (auto& pair : ctx->displayFilter) {
                pair.second = false;
            }
            needsRedraw = true;
        }
    }
    break;

    case COLLECTOR_SUBMENU_TARGET_FILTER:
    {
        ImGuiGBK::TextColored(ImVec4(0.5f, 1.0f, 1.0f, 1.0f), "目标地址过滤");
        ImGui::Separator();
        ImGui::Spacing();

        std::string noFilterId = "不过滤##c_" + ctx->instanceId;
        std::string byIpId = "按IP##c_" + ctx->instanceId;
        std::string byDomainId = "按域名##c_" + ctx->instanceId;
        std::string byPortId = "按端口##c_" + ctx->instanceId;

        if (ImGui::RadioButton(ImGuiText::U(noFilterId.c_str()), &ctx->filterType, 0)) needsRedraw = true;
        ImGui::SameLine();
        if (ImGui::RadioButton(ImGuiText::U(byIpId.c_str()), &ctx->filterType, 1)) needsRedraw = true;
        ImGui::SameLine();
        if (ImGui::RadioButton(ImGuiText::U(byDomainId.c_str()), &ctx->filterType, 2)) needsRedraw = true;
        ImGui::SameLine();
        if (ImGui::RadioButton(ImGuiText::U(byPortId.c_str()), &ctx->filterType, 3)) needsRedraw = true;

        ImGui::Spacing();

        ImGuiGBK::Text("值:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200);
        std::string filterValId = "##cFilterValue_" + ctx->instanceId;
        ImGui::InputText(filterValId.c_str(), ctx->filterValue, sizeof(ctx->filterValue));

        ImGui::SameLine();
        std::string applyId = "应用##c_filter_" + ctx->instanceId;
        if (ImGuiGBK::Button(applyId.c_str(), ImVec2(80, 0))) {
            if (collector) {
                if (ctx->filterType == 0) {
                    collector->ClearFilter();
                } else {
                    FilterType ft = static_cast<FilterType>(ctx->filterType);
                    collector->SetFilter(ft, ctx->filterValue);
                }
            }
            needsRedraw = true;
        }

        ImGui::SameLine();
        std::string clearId = "清除##c_filter_" + ctx->instanceId;
        if (ImGuiGBK::Button(clearId.c_str(), ImVec2(80, 0))) {
            if (collector) {
                collector->ClearFilter();
                ctx->filterType = 0;
                ctx->filterValue[0] = '\0';
            }
            needsRedraw = true;
        }
    }
    break;

    case COLLECTOR_SUBMENU_WHITELIST:
    {
        ImGuiGBK::TextColored(ImVec4(0.5f, 1.0f, 1.0f, 1.0f), "16进制白名单（匹配则不写入数据库）");
        ImGui::Separator();
        ImGui::Spacing();

        std::string enableWlId = "启用白名单功能##coll_whitelist_" + ctx->instanceId;
        if (ImGuiGBK::Checkbox(enableWlId.c_str(), &ctx->enableWhitelist)) {
            SaveSingleCollectorConfig(ctx);
            needsRedraw = true;
        }

        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGuiGBK::Text("启用后，匹配白名单规则的数据将不写入数据库");
            ImGuiGBK::Text("适用于过滤不需要记录的特定数据包");
            ImGui::EndTooltip();
        }

        ImGui::SameLine();
        if (ctx->enableWhitelist) {
            ImGuiGBK::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "[已启用]");
        } else {
            ImGuiGBK::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "[已禁用]");
        }

        ImGui::SameLine(0, 30);
        std::string newRuleId = "新建规则##coll_wl_new_" + ctx->instanceId;
        if (ImGuiGBK::Button(newRuleId.c_str(), ImVec2(100, 0))) {
            ctx->editingWhitelistIndex = -1;
            memset(ctx->whitelistEditName, 0, sizeof(ctx->whitelistEditName));
            strcpy_s(ctx->whitelistEditName, "新规则");
            memset(ctx->whitelistEditPattern, 0, sizeof(ctx->whitelistEditPattern));
            strcpy_s(ctx->whitelistEditOffset, "0");
            ctx->whitelistEditIsHex = true;
            ctx->showWhitelistEditWindow = true;
            needsRedraw = true;
        }

        ImGui::Spacing();

        if (ctx->whitelist.empty()) {
            ImGuiGBK::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "暂无白名单规则，点击「新建规则」添加");
        } else {
            std::string tableId = "CollectorWhitelistTable_" + ctx->instanceId;
            if (ImGui::BeginTable(tableId.c_str(), 5,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                ImVec2(0, 150))) {

                ImGui::TableSetupColumn(ImGuiText::U("启用"), ImGuiTableColumnFlags_WidthFixed, 50);
                ImGui::TableSetupColumn(ImGuiText::U("规则名称"), ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn(ImGuiText::U("匹配规则"), ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn(ImGuiText::U("偏移"), ImGuiTableColumnFlags_WidthFixed, 60);
                ImGui::TableSetupColumn(ImGuiText::U("操作"), ImGuiTableColumnFlags_WidthFixed, 120);
                ImGui::TableHeadersRow();

                int deleteIdx = -1;
                for (size_t i = 0; i < ctx->whitelist.size(); i++) {
                    auto& rule = ctx->whitelist[i];
                    ImGui::TableNextRow();

                    ImGui::TableSetColumnIndex(0);
                    std::string cbId = "##coll_wl_en_" + ctx->instanceId + "_" + std::to_string(i);
                    if (ImGui::Checkbox(cbId.c_str(), &rule.enabled)) {
                        needsRedraw = true;
                    }

                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%s", rule.name.c_str());

                    ImGui::TableSetColumnIndex(2);
                    if (rule.pattern.empty()) {
                        ImGuiGBK::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "(未设置)");
                    } else {
                        std::string displayPattern = rule.pattern;
                        if (displayPattern.length() > 30) {
                            displayPattern = displayPattern.substr(0, 30) + "...";
                        }
                        ImGui::Text("%s", displayPattern.c_str());
                    }

                    ImGui::TableSetColumnIndex(3);
                    ImGui::Text("%d", rule.offset);

                    ImGui::TableSetColumnIndex(4);
                    std::string editBtnId = "编辑##coll_wl_" + ctx->instanceId + "_" + std::to_string(i);
                    std::string deleteBtnId = "删除##coll_wl_" + ctx->instanceId + "_" + std::to_string(i);

                    if (ImGuiGBK::Button(editBtnId.c_str(), ImVec2(50, 0))) {
                        ctx->editingWhitelistIndex = static_cast<int>(i);
                        strcpy_s(ctx->whitelistEditName, rule.name.c_str());
                        strcpy_s(ctx->whitelistEditPattern, rule.pattern.c_str());
                        snprintf(ctx->whitelistEditOffset, sizeof(ctx->whitelistEditOffset), "%d", rule.offset);
                        ctx->whitelistEditIsHex = rule.isHex;
                        ctx->showWhitelistEditWindow = true;
                        needsRedraw = true;
                    }

                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.2f, 0.2f, 1.0f));
                    if (ImGuiGBK::Button(deleteBtnId.c_str(), ImVec2(50, 0))) {
                        deleteIdx = static_cast<int>(i);
                    }
                    ImGui::PopStyleColor();
                }

                ImGui::EndTable();

                if (deleteIdx >= 0 && deleteIdx < static_cast<int>(ctx->whitelist.size())) {
                    ctx->whitelist.erase(ctx->whitelist.begin() + deleteIdx);
                    needsRedraw = true;
                }
            }
        }

        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
        ImGuiGBK::TextWrapped(
            "说明: 白名单规则用于过滤不需要写入数据库的数据包。"
            "当数据包匹配任意一条启用的规则时，将跳过数据库写入。"
            "支持使用??作为通配符匹配任意字节。"
        );
        ImGui::PopStyleColor();
    }
    break;

    case COLLECTOR_SUBMENU_BLACKLIST:
    {
        ImGuiGBK::TextColored(ImVec4(0.5f, 1.0f, 1.0f, 1.0f), "16进制黑名单（匹配则禁止写入数据库）");
        ImGui::Separator();
        ImGui::Spacing();

        std::string enableBlId = "启用黑名单功能##coll_blacklist_" + ctx->instanceId;
        if (ImGuiGBK::Checkbox(enableBlId.c_str(), &ctx->enableBlacklist)) {
            SaveSingleCollectorConfig(ctx);
            needsRedraw = true;
        }

        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGuiGBK::Text("启用后，匹配黑名单规则的数据将禁止写入数据库");
            ImGuiGBK::Text("黑名单优先级高于白名单");
            ImGui::EndTooltip();
        }

        ImGui::SameLine();
        if (ctx->enableBlacklist) {
            ImGuiGBK::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "[已启用]");
        } else {
            ImGuiGBK::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "[已禁用]");
        }

        ImGui::SameLine(0, 30);
        std::string newBlRuleId = "新建规则##coll_bl_new_" + ctx->instanceId;
        if (ImGuiGBK::Button(newBlRuleId.c_str(), ImVec2(100, 0))) {
            ctx->editingBlacklistIndex = -1;
            memset(ctx->blacklistEditName, 0, sizeof(ctx->blacklistEditName));
            strcpy_s(ctx->blacklistEditName, "新规则");
            memset(ctx->blacklistEditPattern, 0, sizeof(ctx->blacklistEditPattern));
            strcpy_s(ctx->blacklistEditOffset, "0");
            ctx->blacklistEditIsHex = true;
            ctx->showBlacklistEditWindow = true;
            needsRedraw = true;
        }

        ImGui::Spacing();

        if (ctx->blacklist.empty()) {
            ImGuiGBK::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "暂无黑名单规则，点击「新建规则」添加");
        } else {
            std::string tableId = "CollectorBlacklistTable_" + ctx->instanceId;
            if (ImGui::BeginTable(tableId.c_str(), 5,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                ImVec2(0, 150))) {

                ImGui::TableSetupColumn(ImGuiText::U("启用"), ImGuiTableColumnFlags_WidthFixed, 50);
                ImGui::TableSetupColumn(ImGuiText::U("规则名称"), ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn(ImGuiText::U("匹配规则"), ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn(ImGuiText::U("偏移"), ImGuiTableColumnFlags_WidthFixed, 60);
                ImGui::TableSetupColumn(ImGuiText::U("操作"), ImGuiTableColumnFlags_WidthFixed, 120);
                ImGui::TableHeadersRow();

                int deleteIdx = -1;
                for (size_t i = 0; i < ctx->blacklist.size(); i++) {
                    auto& rule = ctx->blacklist[i];
                    ImGui::TableNextRow();

                    ImGui::TableSetColumnIndex(0);
                    std::string cbId = "##coll_bl_en_" + ctx->instanceId + "_" + std::to_string(i);
                    if (ImGui::Checkbox(cbId.c_str(), &rule.enabled)) {
                        needsRedraw = true;
                    }

                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%s", rule.name.c_str());

                    ImGui::TableSetColumnIndex(2);
                    if (rule.pattern.empty()) {
                        ImGuiGBK::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "(未设置)");
                    } else {
                        std::string displayPattern = rule.pattern;
                        if (displayPattern.length() > 30) {
                            displayPattern = displayPattern.substr(0, 30) + "...";
                        }
                        ImGui::Text("%s", displayPattern.c_str());
                    }

                    ImGui::TableSetColumnIndex(3);
                    ImGui::Text("%d", rule.offset);

                    ImGui::TableSetColumnIndex(4);
                    std::string editBtnId = "编辑##coll_bl_" + ctx->instanceId + "_" + std::to_string(i);
                    std::string deleteBtnId = "删除##coll_bl_" + ctx->instanceId + "_" + std::to_string(i);

                    if (ImGuiGBK::Button(editBtnId.c_str(), ImVec2(50, 0))) {
                        ctx->editingBlacklistIndex = static_cast<int>(i);
                        strcpy_s(ctx->blacklistEditName, rule.name.c_str());
                        strcpy_s(ctx->blacklistEditPattern, rule.pattern.c_str());
                        snprintf(ctx->blacklistEditOffset, sizeof(ctx->blacklistEditOffset), "%d", rule.offset);
                        ctx->blacklistEditIsHex = rule.isHex;
                        ctx->showBlacklistEditWindow = true;
                        needsRedraw = true;
                    }

                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.2f, 0.2f, 1.0f));
                    if (ImGuiGBK::Button(deleteBtnId.c_str(), ImVec2(50, 0))) {
                        deleteIdx = static_cast<int>(i);
                    }
                    ImGui::PopStyleColor();
                }

                ImGui::EndTable();

                if (deleteIdx >= 0 && deleteIdx < static_cast<int>(ctx->blacklist.size())) {
                    ctx->blacklist.erase(ctx->blacklist.begin() + deleteIdx);
                    needsRedraw = true;
                }
            }
        }

        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
        ImGuiGBK::TextWrapped(
            "说明: 黑名单规则用于禁止特定数据包写入数据库。"
            "当数据包匹配任意一条启用的规则时，将禁止写入。"
            "黑名单优先级高于白名单，支持使用??作为通配符匹配任意字节。"
        );
        ImGui::PopStyleColor();
    }
    break;

    case COLLECTOR_SUBMENU_TCP_SHARE:
    {
        ImGuiGBK::TextColored(ImVec4(0.5f, 1.0f, 1.0f, 1.0f), "TCP文件共享服务（供其他服务器同步）");
        ImGui::Separator();
        ImGui::Spacing();

        // 端口配置
        ImGuiGBK::Text("监听端口:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        std::string portInputId = "##tcp_file_port_" + ctx->instanceId;
        if (ImGui::InputText(portInputId.c_str(), ctx->tcpFileServerPortBuffer,
            sizeof(ctx->tcpFileServerPortBuffer), ImGuiInputTextFlags_CharsDecimal)) {
            needsRedraw = true;
        }

        ImGui::SameLine();

        bool tcpServerRunning = ctx->tcpFileServer && ctx->tcpFileServer->IsRunning();

        std::string btnId = (tcpServerRunning ? "停止共享服务##tcp_" : "启动共享服务##tcp_") + ctx->instanceId;
        if (ImGuiGBK::Button(btnId.c_str(), ImVec2(150, 0))) {
            if (tcpServerRunning) {
                ctx->tcpFileServer->Stop();
                AB_LOG_INFO("[单伪采集][" + ctx->instanceId + "] TCP共享服务已停止");
            } else {
                ctx->tcpFileServerPort = atoi(ctx->tcpFileServerPortBuffer);

                if (ctx->tcpFileServer) {
                    ctx->tcpFileServer->Stop();
                    delete ctx->tcpFileServer;
                    ctx->tcpFileServer = nullptr;
                }

                ctx->tcpFileServer = new TcpFileServer(ctx->tcpFileServerPort, ctx->db.get());

                if (ctx->tcpFileServer->Start()) {
                    AB_LOG_INFO("[单伪采集][" + ctx->instanceId + "] TCP共享服务启动成功，端口: " +
                        std::to_string(ctx->tcpFileServerPort));
                } else {
                    AB_LOG_ERROR("[单伪采集][" + ctx->instanceId + "] TCP共享服务启动失败");
                    delete ctx->tcpFileServer;
                    ctx->tcpFileServer = nullptr;
                }
            }
            needsRedraw = true;
        }

        ImGui::SameLine();
        if (tcpServerRunning) {
            ImGuiGBK::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "● 运行中");
        } else {
            ImGuiGBK::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "● 已停止");
        }

        ImGui::Spacing();

        if (tcpServerRunning) {
            ImGui::Separator();
            ImGuiGBK::TextColored(ImVec4(0.5f, 1.0f, 1.0f, 1.0f), "统计信息:");

            int totalConn = ctx->tcpFileServer->GetTotalConnections();
            uint64_t totalSent = ctx->tcpFileServer->GetTotalBytesSent();
            double sentMB = totalSent / 1024.0 / 1024.0;

            ImGuiGBK::Text("总连接数: %d", totalConn);
            ImGui::SameLine(200);
            ImGuiGBK::Text("总发送: %.2f MB", sentMB);

            ImGui::Spacing();
            ImGui::Separator();
        }

        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
        ImGuiGBK::TextWrapped(
            "说明: 启动此服务后，其他服务器可以通过TCP连接下载当前数据库文件。"
            "在其他服务器的伪心跳页面配置远程同步即可自动获取数据。"
        );
        ImGui::PopStyleColor();
    }
    break;

    case COLLECTOR_SUBMENU_SOCKS5_ACCOUNTS:
    {
        ImGuiGBK::TextColored(ImVec4(0.5f, 1.0f, 1.0f, 1.0f), "SOCKS5账号库绑定");
        ImGui::Separator();
        ImGui::Spacing();

        // 启用SOCKS5认证
        std::string authCheckId = "启用SOCKS5认证##socks_auth_" + ctx->instanceId;
        if (ImGuiGBK::Checkbox(authCheckId.c_str(), &ctx->enableSocksAuth)) {
            if (collector) {
                collector->SetSocks5Auth(ctx->enableSocksAuth);
            }
            SaveSingleCollectorConfig(ctx);
            needsRedraw = true;
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // 账号库绑定选择
        ImGuiGBK::Text("绑定账号库:");
        ImGui::SameLine();

        auto& instMgr = InstanceManager::GetInstance();
        auto pools = instMgr.GetSocks5PoolInstances();

        std::vector<std::string> poolNames;
        std::vector<const char*> poolNamesCStr;
        poolNames.push_back("(未绑定)");

        int currentIndex = 0;
        for (size_t i = 0; i < pools.size(); i++) {
            std::string name = pools[i]->GetName() + " (" + std::to_string(pools[i]->GetAccountCount()) + "个账号)";
            poolNames.push_back(name);
            if (pools[i]->GetId() == ctx->boundSocks5PoolId) {
                currentIndex = static_cast<int>(i + 1);
            }
        }

        for (const auto& name : poolNames) {
            poolNamesCStr.push_back(name.c_str());
        }

        ImGui::SetNextItemWidth(250);
        std::string comboId = "##socks5_pool_" + ctx->instanceId;
        if (ImGui::Combo(comboId.c_str(), &currentIndex, poolNamesCStr.data(), static_cast<int>(poolNamesCStr.size()))) {
            if (currentIndex == 0) {
                ctx->boundSocks5PoolId = "";
            } else {
                ctx->boundSocks5PoolId = pools[currentIndex - 1]->GetId();
            }
            SaveSingleCollectorConfig(ctx);

            // 同步账号到collector
            if (collector) {
                SyncCollectorSocks5Accounts(ctx, collector);
            }

            needsRedraw = true;
        }

        ImGui::Spacing();

        // 显示当前绑定状态
        if (ctx->boundSocks5PoolId.empty()) {
            ImGuiGBK::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "未绑定账号库，SOCKS5认证将无账号可用");
        } else {
            auto* pool = instMgr.GetSocks5PoolInstance(ctx->boundSocks5PoolId);
            if (pool) {
                ImGuiGBK::TextColored(ImVec4(0.0f, 1.0f, 0.5f, 1.0f), "已绑定: %s", pool->GetName().c_str());
                ImGuiGBK::Text("账号数量: %d", pool->GetAccountCount());
            } else {
                ImGuiGBK::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "绑定的账号库已不存在");
                ctx->boundSocks5PoolId = "";
                SaveSingleCollectorConfig(ctx);
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
        ImGuiGBK::TextWrapped(
            "说明: 在主菜单的 [SOCKS5账号] 页面创建和管理账号库实例。"
            "每个采集/伪心跳实例可以绑定一个账号库，启用认证后将使用该库中的账号进行验证。"
        );
        ImGui::PopStyleColor();
    }
    break;

    default:
        ImGuiGBK::Text("子菜单: %s", GetCollectorSubMenuName(menuType));
        break;
    }
}

// ==================== 伪心跳实例子菜单内容渲染 ====================
static void RenderSingleHeartbeatSubMenuContent(SingleHeartbeatCtx* ctx, PacketCollector* forwarder,
    HeartbeatInstance* instance, int menuType, bool& needsRedraw) {

    if (!ctx) return;
    bool running = forwarder && forwarder->IsRunning();

    switch (menuType) {
    case HEARTBEAT_SUBMENU_MAIN:
    {
        ImGuiGBK::TextColored(ImVec4(0.5f, 1.0f, 1.0f, 1.0f), "伪心跳主页");
        ImGui::Separator();
        ImGuiGBK::Text("主页内容请在主界面查看");
    }
    break;

    case HEARTBEAT_SUBMENU_REPLACE_MODE:
    {
        ImGuiGBK::TextColored(ImVec4(0.5f, 1.0f, 1.0f, 1.0f), "替换模式配置");
        ImGui::Separator();
        ImGui::Spacing();

        std::string rmRandom = "随机##rm_" + ctx->instanceId;
        std::string rmSeq = "顺序##rm_" + ctx->instanceId;
        std::string rmFixed = "固定##rm_" + ctx->instanceId;

        if (ImGui::RadioButton(ImGuiText::U(rmRandom.c_str()), &ctx->replaceMode, 0)) {
            SaveSingleHeartbeatConfig(ctx);
            needsRedraw = true;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton(ImGuiText::U(rmSeq.c_str()), &ctx->replaceMode, 1)) {
            SaveSingleHeartbeatConfig(ctx);
            needsRedraw = true;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton(ImGuiText::U(rmFixed.c_str()), &ctx->replaceMode, 2)) {
            SaveSingleHeartbeatConfig(ctx);
            needsRedraw = true;
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // CRC32自动计算
        std::string crc32Id = "启用CRC32自动计算##crc32_" + ctx->instanceId;
        if (ImGuiGBK::Checkbox(crc32Id.c_str(), &ctx->enableCRC32AutoCalc)) {
            SaveSingleHeartbeatConfig(ctx);
            needsRedraw = true;
            if (ctx->enableCRC32AutoCalc) {
                AB_LOG_INFO("[单伪心跳][" + ctx->instanceId + "] CRC32自动计算已启用");
            } else {
                AB_LOG_INFO("[单伪心跳][" + ctx->instanceId + "] CRC32自动计算已禁用");
            }
        }
        if (ctx->enableCRC32AutoCalc) {
            ImGui::SameLine();
            ImGuiGBK::TextColored(ImVec4(0.0f, 1.0f, 0.5f, 1.0f), "(替换后自动重算CRC32)");
        }

        ImGui::Spacing();

        // 数据包读取顺序模式
        ImGuiGBK::Text("读取顺序:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200);
        const char* orderModeItems[] = { "升序模式(先采集先使用)", "降序模式(最新数据优先)" };
        int currentOrderMode = static_cast<int>(ctx->packetOrderMode);
        std::string orderComboId = "##orderMode_" + ctx->instanceId;
        if (ImGui::Combo(orderComboId.c_str(), &currentOrderMode, orderModeItems, IM_ARRAYSIZE(orderModeItems))) {
            ctx->packetOrderMode = static_cast<PacketOrderMode>(currentOrderMode);
            SaveSingleHeartbeatConfig(ctx);
            needsRedraw = true;
        }
        if (ctx->packetOrderMode == PacketOrderMode::ASCENDING) {
            ImGuiGBK::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "注：按采集顺序使用数据包，先采集的先使用");
        } else {
            ImGuiGBK::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "注：优先使用最新采集的数据包（ID最大的）");
        }

        // 顺序模式配置（仅当选择顺序模式时显示）
        if (ctx->replaceMode == 1) {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGuiGBK::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "顺序替换配置（按ID长度）");
            ImGui::Spacing();

            // 添加新配置
            ImGuiGBK::Text("添加ID长度配置:");
            ImGui::SameLine();
            std::string lenInputId = "##new_seq_len_" + ctx->instanceId;
            ImGui::SetNextItemWidth(60);
            ImGui::InputText(lenInputId.c_str(), ctx->newSeqLengthBuffer, sizeof(ctx->newSeqLengthBuffer), ImGuiInputTextFlags_CharsDecimal);
            ImGui::SameLine();
            ImGuiGBK::Text("起始:");
            ImGui::SameLine();
            std::string startInputId = "##new_seq_start_" + ctx->instanceId;
            ImGui::SetNextItemWidth(80);
            ImGui::InputText(startInputId.c_str(), ctx->newSeqStartIdBuffer, sizeof(ctx->newSeqStartIdBuffer), ImGuiInputTextFlags_CharsDecimal);
            ImGui::SameLine();
            ImGuiGBK::Text("结束:");
            ImGui::SameLine();
            std::string endInputId = "##new_seq_end_" + ctx->instanceId;
            ImGui::SetNextItemWidth(80);
            ImGui::InputText(endInputId.c_str(), ctx->newSeqEndIdBuffer, sizeof(ctx->newSeqEndIdBuffer), ImGuiInputTextFlags_CharsDecimal);
            ImGui::SameLine();
            std::string addBtnId = "添加##add_seq_cfg_" + ctx->instanceId;
            if (ImGuiGBK::Button(addBtnId.c_str())) {
                int newLen = atoi(ctx->newSeqLengthBuffer);
                int newStart = atoi(ctx->newSeqStartIdBuffer);
                int newEnd = atoi(ctx->newSeqEndIdBuffer);
                if (newLen > 0 && newEnd >= newStart) {
                    std::lock_guard<std::mutex> lock(ctx->sequentialConfigMutex);
                    ctx->sequentialConfigs[newLen] = SingleSequentialConfig(newStart, newEnd, true);
                    SaveSingleHeartbeatConfig(ctx);
                    needsRedraw = true;
                }
            }

            ImGui::Spacing();

            // 批量操作按钮
            std::string enableAllId = "全部启用##seq_enable_all_" + ctx->instanceId;
            if (ImGuiGBK::Button(enableAllId.c_str())) {
                std::lock_guard<std::mutex> lock(ctx->sequentialConfigMutex);
                for (auto& pair : ctx->sequentialConfigs) {
                    pair.second.enabled = true;
                }
                SaveSingleHeartbeatConfig(ctx);
                needsRedraw = true;
            }
            ImGui::SameLine();
            std::string disableAllId = "全部禁用##seq_disable_all_" + ctx->instanceId;
            if (ImGuiGBK::Button(disableAllId.c_str())) {
                std::lock_guard<std::mutex> lock(ctx->sequentialConfigMutex);
                for (auto& pair : ctx->sequentialConfigs) {
                    pair.second.enabled = false;
                }
                SaveSingleHeartbeatConfig(ctx);
                needsRedraw = true;
            }
            ImGui::SameLine();
            std::string clearPosId = "清空位置记录##seq_clear_pos_" + ctx->instanceId;
            if (ImGuiGBK::Button(clearPosId.c_str())) {
                std::lock_guard<std::mutex> lock(ctx->sequentialPositionMutex);
                ctx->gameIdSequentialPosition.clear();
                needsRedraw = true;
            }

            ImGui::Spacing();

            // 配置表格
            std::lock_guard<std::mutex> lock(ctx->sequentialConfigMutex);
            if (!ctx->sequentialConfigs.empty()) {
                std::string tableId = "SeqConfigTable_" + ctx->instanceId;
                if (ImGui::BeginTable(tableId.c_str(), 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
                    ImGui::TableSetupColumn("ID长度", ImGuiTableColumnFlags_WidthFixed, 60);
                    ImGui::TableSetupColumn("启用", ImGuiTableColumnFlags_WidthFixed, 50);
                    ImGui::TableSetupColumn("起始ID", ImGuiTableColumnFlags_WidthFixed, 80);
                    ImGui::TableSetupColumn("结束ID", ImGuiTableColumnFlags_WidthFixed, 80);
                    ImGui::TableSetupColumn("数据量", ImGuiTableColumnFlags_WidthFixed, 60);
                    ImGui::TableSetupColumn("范围", ImGuiTableColumnFlags_WidthFixed, 60);
                    ImGui::TableSetupColumn("操作", ImGuiTableColumnFlags_WidthFixed, 60);
                    ImGui::TableHeadersRow();

                    std::vector<int> toRemove;
                    for (auto& pair : ctx->sequentialConfigs) {
                        int length = pair.first;
                        auto& cfg = pair.second;

                        ImGui::TableNextRow();

                        // ID长度
                        ImGui::TableNextColumn();
                        ImGuiGBK::Text("%d", length);

                        // 启用
                        ImGui::TableNextColumn();
                        std::string enableId = "##seq_en_" + ctx->instanceId + "_" + std::to_string(length);
                        if (ImGui::Checkbox(enableId.c_str(), &cfg.enabled)) {
                            SaveSingleHeartbeatConfig(ctx);
                            needsRedraw = true;
                        }

                        // 起始ID
                        ImGui::TableNextColumn();
                        char startBuf[16];
                        snprintf(startBuf, sizeof(startBuf), "%d", cfg.startId);
                        std::string startId = "##seq_start_" + ctx->instanceId + "_" + std::to_string(length);
                        ImGui::SetNextItemWidth(70);
                        if (ImGui::InputText(startId.c_str(), startBuf, sizeof(startBuf), ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue)) {
                            int newStart = atoi(startBuf);
                            if (newStart > 0) {
                                cfg.startId = newStart;
                                SaveSingleHeartbeatConfig(ctx);
                                needsRedraw = true;
                            }
                        }

                        // 结束ID
                        ImGui::TableNextColumn();
                        char endBuf[16];
                        snprintf(endBuf, sizeof(endBuf), "%d", cfg.endId);
                        std::string endId = "##seq_end_" + ctx->instanceId + "_" + std::to_string(length);
                        ImGui::SetNextItemWidth(70);
                        if (ImGui::InputText(endId.c_str(), endBuf, sizeof(endBuf), ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue)) {
                            int newEnd = atoi(endBuf);
                            if (newEnd >= cfg.startId) {
                                cfg.endId = newEnd;
                                SaveSingleHeartbeatConfig(ctx);
                                needsRedraw = true;
                            }
                        }

                        // 数据量
                        ImGui::TableNextColumn();
                        ImGuiGBK::Text("%d", cfg.currentCount);

                        // 范围
                        ImGui::TableNextColumn();
                        int range = cfg.endId - cfg.startId + 1;
                        ImGuiGBK::Text("%d", range);

                        // 操作
                        ImGui::TableNextColumn();
                        std::string delId = "删除##seq_del_" + ctx->instanceId + "_" + std::to_string(length);
                        if (ImGuiGBK::Button(delId.c_str())) {
                            toRemove.push_back(length);
                        }
                    }

                    ImGui::EndTable();

                    // 删除标记的配置
                    for (int len : toRemove) {
                        ctx->sequentialConfigs.erase(len);
                        SaveSingleHeartbeatConfig(ctx);
                        needsRedraw = true;
                    }
                }
            } else {
                ImGuiGBK::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "暂无顺序配置，请添加ID长度配置");
            }
        }

        // ========== 替换数量规则 ==========
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGuiGBK::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "替换数量规则");
        ImGui::Spacing();

        // 添加规则按钮
        std::string addRuleId = "添加规则##add_replace_rule_" + ctx->instanceId;
        if (ImGuiGBK::Button(addRuleId.c_str())) {
            ctx->editingReplaceCountIndex = -1;
            memset(ctx->replaceCountEditName, 0, sizeof(ctx->replaceCountEditName));
            strcpy(ctx->replaceCountEditLimit, "1");
            ctx->replaceCountEditStrategy = 0;
            ctx->replaceCountEditClearPool = false;
            ctx->replaceCountEditApplyWpe = true;
            ctx->replaceCountEditResetOnDisconnect = false;
            strcpy(ctx->replaceCountEditFakeN, "1");
            strcpy(ctx->replaceCountEditOriginalN, "1");
            ctx->showReplaceCountEditWindow = true;
        }

        ImGui::SameLine();
        std::string clearRulesId = "清空规则##clear_replace_rules_" + ctx->instanceId;
        if (ImGuiGBK::Button(clearRulesId.c_str())) {
            ctx->replaceCountRules.clear();
            ctx->replaceCountRulesVersion.fetch_add(1);
            {
                std::lock_guard<std::mutex> lock(ctx->replaceRuntimeMutex);
                ctx->replaceRuntimeStates.clear();
            }
            SaveSingleHeartbeatConfig(ctx);
            needsRedraw = true;
        }

        ImGui::SameLine();
        std::string resetStatesId = "重置状态##reset_replace_states_" + ctx->instanceId;
        if (ImGuiGBK::Button(resetStatesId.c_str())) {
            ctx->replaceCountRulesVersion.fetch_add(1);
            std::lock_guard<std::mutex> lock(ctx->replaceRuntimeMutex);
            ctx->replaceRuntimeStates.clear();
            needsRedraw = true;
        }

        ImGui::Spacing();

        // 规则列表表格
        if (!ctx->replaceCountRules.empty()) {
            std::string tableId = "ReplaceCountRulesTable_" + ctx->instanceId;
            if (ImGui::BeginTable(tableId.c_str(), 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
                ImGui::TableSetupColumn("启用", ImGuiTableColumnFlags_WidthFixed, 40);
                ImGui::TableSetupColumn("名称", ImGuiTableColumnFlags_WidthFixed, 100);
                ImGui::TableSetupColumn("阈值", ImGuiTableColumnFlags_WidthFixed, 50);
                ImGui::TableSetupColumn("到达后策略", ImGuiTableColumnFlags_WidthFixed, 120);
                ImGui::TableSetupColumn("循环比例", ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableSetupColumn("清理池", ImGuiTableColumnFlags_WidthFixed, 50);
                ImGui::TableSetupColumn("操作", ImGuiTableColumnFlags_WidthFixed, 100);
                ImGui::TableHeadersRow();

                int toRemoveIdx = -1;
                for (size_t i = 0; i < ctx->replaceCountRules.size(); i++) {
                    auto& rule = ctx->replaceCountRules[i];
                    ImGui::TableNextRow();

                    // 启用
                    ImGui::TableNextColumn();
                    std::string enableId = "##rcr_en_" + ctx->instanceId + "_" + std::to_string(i);
                    if (ImGui::Checkbox(enableId.c_str(), &rule.enabled)) {
                        ctx->replaceCountRulesVersion.fetch_add(1);
                        SaveSingleHeartbeatConfig(ctx);
                        needsRedraw = true;
                    }

                    // 名称
                    ImGui::TableNextColumn();
                    ImGuiGBK::Text("%s", rule.name.c_str());

                    // 阈值
                    ImGui::TableNextColumn();
                    ImGuiGBK::Text("%d", rule.replaceLimit);

                    // 到达后策略
                    ImGui::TableNextColumn();
                    if (rule.postLimitStrategy == SinglePostLimitStrategy::SendOriginal) {
                        ImGuiGBK::Text("到达后只发原包");
                    } else {
                        ImGuiGBK::Text("到达后按比例循环");
                    }

                    // 循环比例
                    ImGui::TableNextColumn();
                    if (rule.postLimitStrategy == SinglePostLimitStrategy::CycleFakeThenOriginal) {
                        ImGuiGBK::Text("%d:%d", rule.postLimitFakeN, rule.postLimitOriginalN);
                    } else {
                        ImGuiGBK::Text("-");
                    }

                    // 清理池
                    ImGui::TableNextColumn();
                    ImGuiGBK::Text("%s", rule.clearPoolOnReach ? "是" : "否");

                    // 操作
                    ImGui::TableNextColumn();
                    std::string editId = "编辑##rcr_edit_" + ctx->instanceId + "_" + std::to_string(i);
                    if (ImGuiGBK::Button(editId.c_str())) {
                        ctx->editingReplaceCountIndex = (int)i;
                        strncpy(ctx->replaceCountEditName, rule.name.c_str(), sizeof(ctx->replaceCountEditName) - 1);
                        snprintf(ctx->replaceCountEditLimit, sizeof(ctx->replaceCountEditLimit), "%d", rule.replaceLimit);
                        ctx->replaceCountEditStrategy = static_cast<int>(rule.postLimitStrategy);
                        ctx->replaceCountEditClearPool = rule.clearPoolOnReach;
                        ctx->replaceCountEditApplyWpe = rule.applyWpeOnOriginalSegment;
                        ctx->replaceCountEditResetOnDisconnect = rule.resetCountOnDisconnect;
                        snprintf(ctx->replaceCountEditFakeN, sizeof(ctx->replaceCountEditFakeN), "%d", rule.postLimitFakeN);
                        snprintf(ctx->replaceCountEditOriginalN, sizeof(ctx->replaceCountEditOriginalN), "%d", rule.postLimitOriginalN);
                        ctx->showReplaceCountEditWindow = true;
                    }
                    ImGui::SameLine();
                    std::string delId = "删除##rcr_del_" + ctx->instanceId + "_" + std::to_string(i);
                    if (ImGuiGBK::Button(delId.c_str())) {
                        toRemoveIdx = (int)i;
                    }
                }

                ImGui::EndTable();

                if (toRemoveIdx >= 0) {
                    ctx->replaceCountRules.erase(ctx->replaceCountRules.begin() + toRemoveIdx);
                    ctx->replaceCountRulesVersion.fetch_add(1);
                    SaveSingleHeartbeatConfig(ctx);
                    needsRedraw = true;
                }
            }
        } else {
            ImGuiGBK::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "暂无替换数量规则");
        }

        // 替换数量规则编辑弹窗
        if (ctx->showReplaceCountEditWindow) {
            std::string modalId = "编辑替换数量规则##rcr_modal_" + ctx->instanceId;
            ImGui::OpenPopup(ImGuiText::U(modalId.c_str()));
            if (ImGui::BeginPopupModal(ImGuiText::U(modalId.c_str()), &ctx->showReplaceCountEditWindow, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGuiGBK::Text("规则名称:");
                ImGui::SameLine();
                std::string nameInputId = "##rcr_name_" + ctx->instanceId;
                ImGui::SetNextItemWidth(200);
                ImGui::InputText(nameInputId.c_str(), ctx->replaceCountEditName, sizeof(ctx->replaceCountEditName));

                ImGuiGBK::Text("替换阈值:");
                ImGui::SameLine();
                std::string limitInputId = "##rcr_limit_" + ctx->instanceId;
                ImGui::SetNextItemWidth(100);
                ImGui::InputText(limitInputId.c_str(), ctx->replaceCountEditLimit, sizeof(ctx->replaceCountEditLimit), ImGuiInputTextFlags_CharsDecimal);
                ImGui::SameLine();
                ImGuiGBK::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "(成功替换N次后触发)");

                ImGuiGBK::Text("到达后策略:");
                const char* strategyItems[] = { "到达后只发原包", "到达后按比例循环" };
                std::string strategyComboId = "##rcr_strategy_" + ctx->instanceId;
                ImGui::SetNextItemWidth(200);
                ImGui::Combo(ImGuiText::U(strategyComboId.c_str()), &ctx->replaceCountEditStrategy, strategyItems, 2);

                // 循环比例（仅当选择按比例循环时显示）
                if (ctx->replaceCountEditStrategy == 1) {
                    ImGuiGBK::Text("循环比例:");
                    ImGui::SameLine();
                    ImGuiGBK::Text("伪包:");
                    ImGui::SameLine();
                    std::string fakeNId = "##rcr_fakeN_" + ctx->instanceId;
                    ImGui::SetNextItemWidth(60);
                    ImGui::InputText(fakeNId.c_str(), ctx->replaceCountEditFakeN, sizeof(ctx->replaceCountEditFakeN), ImGuiInputTextFlags_CharsDecimal);
                    ImGui::SameLine();
                    ImGuiGBK::Text("原包:");
                    ImGui::SameLine();
                    std::string origNId = "##rcr_origN_" + ctx->instanceId;
                    ImGui::SetNextItemWidth(60);
                    ImGui::InputText(origNId.c_str(), ctx->replaceCountEditOriginalN, sizeof(ctx->replaceCountEditOriginalN), ImGuiInputTextFlags_CharsDecimal);
                }

                std::string clearPoolId = "到达时清理数据池##rcr_clear_" + ctx->instanceId;
                ImGuiGBK::Checkbox(clearPoolId.c_str(), &ctx->replaceCountEditClearPool);

                std::string applyWpeId = "原包段执行WPE滤镜##rcr_wpe_" + ctx->instanceId;
                ImGuiGBK::Checkbox(applyWpeId.c_str(), &ctx->replaceCountEditApplyWpe);

                std::string resetId = "断开后重置计数##rcr_reset_" + ctx->instanceId;
                ImGuiGBK::Checkbox(resetId.c_str(), &ctx->replaceCountEditResetOnDisconnect);

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                std::string saveId = "保存##rcr_save_" + ctx->instanceId;
                if (ImGuiGBK::Button(saveId.c_str(), ImVec2(100, 0))) {
                    SingleReplaceCountRule rule;
                    rule.name = ctx->replaceCountEditName;
                    if (rule.name.empty()) rule.name = "规则#" + std::to_string(ctx->nextReplaceCountRuleId);
                    rule.replaceLimit = std::max(1, atoi(ctx->replaceCountEditLimit));
                    rule.postLimitStrategy = static_cast<SinglePostLimitStrategy>(ctx->replaceCountEditStrategy);
                    rule.clearPoolOnReach = ctx->replaceCountEditClearPool;
                    rule.applyWpeOnOriginalSegment = ctx->replaceCountEditApplyWpe;
                    rule.resetCountOnDisconnect = ctx->replaceCountEditResetOnDisconnect;
                    rule.postLimitFakeN = std::max(0, atoi(ctx->replaceCountEditFakeN));
                    rule.postLimitOriginalN = std::max(0, atoi(ctx->replaceCountEditOriginalN));

                    if (ctx->editingReplaceCountIndex >= 0 && ctx->editingReplaceCountIndex < (int)ctx->replaceCountRules.size()) {
                        rule.id = ctx->replaceCountRules[ctx->editingReplaceCountIndex].id;
                        ctx->replaceCountRules[ctx->editingReplaceCountIndex] = rule;
                    } else {
                        rule.id = ctx->nextReplaceCountRuleId++;
                        rule.enabled = true;
                        ctx->replaceCountRules.push_back(rule);
                    }

                    ctx->replaceCountRulesVersion.fetch_add(1);
                    SaveSingleHeartbeatConfig(ctx);
                    ctx->showReplaceCountEditWindow = false;
                    needsRedraw = true;
                }
                ImGui::SameLine();
                std::string cancelId = "取消##rcr_cancel_" + ctx->instanceId;
                if (ImGuiGBK::Button(cancelId.c_str(), ImVec2(100, 0))) {
                    ctx->showReplaceCountEditWindow = false;
                }

                ImGui::EndPopup();
            }
        }

        // ========== 23特征替换模式 ==========
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGuiGBK::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "01 0A 00 23 替换模式");
        ImGui::Spacing();

        ImGuiGBK::TextColored(ImVec4(0.5f, 1.0f, 1.0f, 1.0f), "选择23特征替换模式:");
        ImGui::Spacing();

        std::string staticModeId = "静态替换##rm23_" + ctx->instanceId;
        if (ImGui::RadioButton(ImGuiText::U(staticModeId.c_str()), &ctx->pattern23Mode, 0)) {
            SaveSingleHeartbeatConfig(ctx);
            AB_LOG_INFO("[单伪心跳][" + ctx->instanceId + "] 已切换到静态替换模式");
            needsRedraw = true;
        }
        ImGui::SameLine();
        if (ctx->pattern23Mode == 0) {
            ImGuiGBK::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "[当前模式]");
        }

        ImGui::Spacing();

        std::string dynamicModeId = "动态计算##rm23_" + ctx->instanceId;
        if (ImGui::RadioButton(ImGuiText::U(dynamicModeId.c_str()), &ctx->pattern23Mode, 1)) {
            SaveSingleHeartbeatConfig(ctx);
            AB_LOG_INFO("[单伪心跳][" + ctx->instanceId + "] 已切换到动态计算模式");
            needsRedraw = true;
        }
        ImGui::SameLine();
        if (ctx->pattern23Mode == 1) {
            ImGuiGBK::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "[当前模式]");
        }

        ImGui::Spacing();

        // 动态模式配置
        if (ctx->pattern23Mode == 1) {
            ImGui::Indent();

            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.15f, 0.15f, 0.2f, 1.0f));
            std::string dynamicConfigId = "DynamicModeConfig_" + ctx->instanceId;
            ImGui::BeginChild(dynamicConfigId.c_str(), ImVec2(0, 120), true);

            ImGuiGBK::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "动态模式配置:");
            ImGui::Spacing();

            ImGuiGBK::Text("第一处写入偏移位置:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(150);
            std::string offsetSliderId = "##dynamicOffset_" + ctx->instanceId;
            if (ImGui::SliderInt(offsetSliderId.c_str(), &ctx->pattern23DynamicOffset, 0, 6)) {
                SaveSingleHeartbeatConfig(ctx);
                AB_LOG_INFO("[单伪心跳][" + ctx->instanceId + "] 动态偏移位置设置为: " + std::to_string(ctx->pattern23DynamicOffset));
                needsRedraw = true;
            }

            ImGui::SameLine();
            ImGuiGBK::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                "(前8字节的第%d-%d个字节)",
                ctx->pattern23DynamicOffset + 1,
                ctx->pattern23DynamicOffset + 2);

            ImGui::Spacing();

            ImGuiGBK::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "固定写入位置:");
            ImGui::SameLine();
            ImGuiGBK::Text("偏移6 (前8字节的第7-8个字节)");

            ImGui::EndChild();
            ImGui::PopStyleColor();

            ImGui::Unindent();
        }
    }
    break;

    case HEARTBEAT_SUBMENU_SPECIAL_ID:
    {
        ImGuiGBK::TextColored(ImVec4(0.5f, 1.0f, 1.0f, 1.0f), "特殊ID控制");
        ImGui::Separator();
        ImGui::Spacing();

        std::string id00 = "允许00开头ID##hb_" + ctx->instanceId;
        std::string idOB = "允许_OB结尾ID##hb_" + ctx->instanceId;
        std::string id62 = "启用62特征伪心跳##hb_" + ctx->instanceId;

        if (ImGuiGBK::Checkbox(id00.c_str(), &ctx->allowHeartbeat00ID)) {
            SaveSingleHeartbeatConfig(ctx);
            needsRedraw = true;
        }
        if (ImGuiGBK::Checkbox(idOB.c_str(), &ctx->allowHeartbeatOBID)) {
            SaveSingleHeartbeatConfig(ctx);
            needsRedraw = true;
        }
        if (ImGuiGBK::Checkbox(id62.c_str(), &ctx->enableHeartbeat62Pattern)) {
            SaveSingleHeartbeatConfig(ctx);
            needsRedraw = true;
        }
    }
    break;

    case HEARTBEAT_SUBMENU_VTD_FILTER:
    {
        ImGuiGBK::TextColored(ImVec4(0.5f, 1.0f, 1.0f, 1.0f), "VTD专属滤镜");
        ImGui::Separator();
        ImGui::Spacing();

        std::string vtdId = "启用VTD滤镜##vtd_" + ctx->instanceId;
        std::string f03Id = "启用03滤镜##f03_" + ctx->instanceId;

        if (ImGuiGBK::Checkbox(vtdId.c_str(), &ctx->enableVTDFilter)) {
            SaveSingleHeartbeatConfig(ctx);
            needsRedraw = true;
        }
        if (ImGuiGBK::Checkbox(f03Id.c_str(), &ctx->enable03Filter)) {
            SaveSingleHeartbeatConfig(ctx);
            needsRedraw = true;
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.15f, 0.15f, 0.2f, 1.0f));
        std::string helpId = "VTDFilterHelp_" + ctx->instanceId;
        ImGui::BeginChild(helpId.c_str(), ImVec2(0, 60), true);
        ImGuiGBK::TextColored(ImVec4(1.0f, 1.0f, 0.5f, 1.0f), "说明:");
        ImGui::Spacing();
        ImGuiGBK::TextWrapped("VTD滤镜用于过滤特定格式的数据包。");
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    break;

    case HEARTBEAT_SUBMENU_PATTERN23:
    {
        ImGuiGBK::TextColored(ImVec4(0.5f, 1.0f, 1.0f, 1.0f), "23特征偏移替换");
        ImGui::Separator();
        ImGui::Spacing();

        std::string enableId = "启用23偏移替换##enable_p23_" + ctx->instanceId;
        if (ImGuiGBK::Checkbox(enableId.c_str(), &ctx->enablePattern23OffsetReplace)) {
            SaveSingleHeartbeatConfig(ctx);
            needsRedraw = true;
        }
        ImGui::SameLine();
        if (ctx->enablePattern23OffsetReplace) {
            ImGuiGBK::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "[已启用]");
        } else {
            ImGuiGBK::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "[未启用]");
        }

        ImGui::SameLine(300);
        ImGuiGBK::Text("规则数: %d", (int)ctx->pattern23OffsetRules.size());

        ImGui::Spacing();
        ImGuiGBK::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f),
            "以 01 0A 00 23 特征为基准点，根据偏移值替换数据池中对应位置的数据");
        ImGuiGBK::Text("偏移 -43 = 00 00 0A 92 后4字节 | 偏移 -8 = 23特征前8字节");

        ImGui::Spacing();
        ImGui::Separator();

        std::string addRuleId = "添加规则##add_p23_rule_" + ctx->instanceId;
        if (ImGuiGBK::Button(addRuleId.c_str(), ImVec2(100, 0))) {
            ctx->editingPattern23Index = -1;
            memset(ctx->pattern23EditName, 0, sizeof(ctx->pattern23EditName));
            strcpy_s(ctx->pattern23EditOffset, "-43");
            strcpy_s(ctx->pattern23EditLength, "4");
            ctx->showPattern23EditWindow = true;
        }

        ImGui::SameLine();
        std::string clearRulesId = "清空规则##clear_p23_rules_" + ctx->instanceId;
        if (ImGuiGBK::Button(clearRulesId.c_str(), ImVec2(100, 0))) {
            ctx->pattern23OffsetRules.clear();
            needsRedraw = true;
        }

        ImGui::SameLine();
        ImGuiGBK::Text("  快捷:");
        ImGui::SameLine();
        std::string preset43Id = "-43##preset_43_" + ctx->instanceId;
        if (ImGuiGBK::Button(preset43Id.c_str(), ImVec2(40, 0))) {
            SingleOffsetRule rule;
            rule.id = ctx->nextPattern23RuleId++;
            rule.name = "0A92后4字节";
            rule.offset = -43;
            rule.length = 4;
            rule.enabled = true;
            ctx->pattern23OffsetRules.push_back(rule);
            needsRedraw = true;
        }

        ImGui::SameLine();
        std::string preset8Id = "-8##preset_8_" + ctx->instanceId;
        if (ImGuiGBK::Button(preset8Id.c_str(), ImVec2(40, 0))) {
            SingleOffsetRule rule;
            rule.id = ctx->nextPattern23RuleId++;
            rule.name = "23特征前8字节";
            rule.offset = -8;
            rule.length = 8;
            rule.enabled = true;
            ctx->pattern23OffsetRules.push_back(rule);
            needsRedraw = true;
        }

        ImGui::Spacing();
        ImGui::Separator();

        if (!ctx->pattern23OffsetRules.empty()) {
            ImGuiGBK::Text("当前规则列表:");
            std::string tableId = "##p23_offset_table_" + ctx->instanceId;
            if (ImGui::BeginTable(tableId.c_str(), 5,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                ImVec2(0, 200))) {

                ImGui::TableSetupColumn(ImGuiText::U("启用"), ImGuiTableColumnFlags_WidthFixed, 40);
                ImGui::TableSetupColumn(ImGuiText::U("名称"), ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn(ImGuiText::U("偏移"), ImGuiTableColumnFlags_WidthFixed, 60);
                ImGui::TableSetupColumn(ImGuiText::U("长度"), ImGuiTableColumnFlags_WidthFixed, 50);
                ImGui::TableSetupColumn(ImGuiText::U("操作"), ImGuiTableColumnFlags_WidthFixed, 100);
                ImGui::TableHeadersRow();

                int deleteIdx = -1;
                for (size_t i = 0; i < ctx->pattern23OffsetRules.size(); i++) {
                    auto& rule = ctx->pattern23OffsetRules[i];
                    ImGui::TableNextRow();

                    ImGui::TableSetColumnIndex(0);
                    std::string cbId = "##p23_en_" + ctx->instanceId + "_" + std::to_string(i);
                    if (ImGui::Checkbox(cbId.c_str(), &rule.enabled)) {
                        needsRedraw = true;
                    }

                    ImGui::TableSetColumnIndex(1);
                    if (rule.enabled) {
                        ImGuiGBK::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "%s", rule.name.c_str());
                    } else {
                        ImGuiGBK::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%s", rule.name.c_str());
                    }

                    ImGui::TableSetColumnIndex(2);
                    ImGuiGBK::Text("%d", rule.offset);

                    ImGui::TableSetColumnIndex(3);
                    ImGuiGBK::Text("%d", rule.length);

                    ImGui::TableSetColumnIndex(4);
                    std::string editId = "编辑##edit_p23_" + ctx->instanceId + "_" + std::to_string(i);
                    if (ImGuiGBK::Button(editId.c_str(), ImVec2(40, 0))) {
                        ctx->editingPattern23Index = static_cast<int>(i);
                        strcpy_s(ctx->pattern23EditName, rule.name.c_str());
                        snprintf(ctx->pattern23EditOffset, sizeof(ctx->pattern23EditOffset), "%d", rule.offset);
                        snprintf(ctx->pattern23EditLength, sizeof(ctx->pattern23EditLength), "%d", rule.length);
                        ctx->showPattern23EditWindow = true;
                    }
                    ImGui::SameLine();
                    std::string delId = "删除##del_p23_" + ctx->instanceId + "_" + std::to_string(i);
                    if (ImGuiGBK::Button(delId.c_str(), ImVec2(40, 0))) {
                        deleteIdx = static_cast<int>(i);
                    }
                }

                ImGui::EndTable();

                if (deleteIdx >= 0 && deleteIdx < static_cast<int>(ctx->pattern23OffsetRules.size())) {
                    ctx->pattern23OffsetRules.erase(ctx->pattern23OffsetRules.begin() + deleteIdx);
                    needsRedraw = true;
                }
            }
        } else {
            ImGuiGBK::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "暂无规则，请点击\"添加规则\"按钮添加");
        }
    }
    break;

    case HEARTBEAT_SUBMENU_PATTERN09:
    {
        ImGuiGBK::TextColored(ImVec4(0.5f, 1.0f, 1.0f, 1.0f), "09特征偏移替换");
        ImGui::Separator();
        ImGui::Spacing();

        std::string enableId = "启用09偏移替换##enable_p09_" + ctx->instanceId;
        if (ImGuiGBK::Checkbox(enableId.c_str(), &ctx->enablePattern09OffsetReplace)) {
            SaveSingleHeartbeatConfig(ctx);
            needsRedraw = true;
        }
        ImGui::SameLine();
        if (ctx->enablePattern09OffsetReplace) {
            ImGuiGBK::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "[已启用]");
        } else {
            ImGuiGBK::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "[未启用]");
        }

        ImGui::SameLine(300);
        ImGuiGBK::Text("规则数: %d", (int)ctx->pattern09OffsetRules.size());

        ImGui::Spacing();
        ImGui::Separator();

        std::string addRuleId = "添加规则##add_p09_rule_" + ctx->instanceId;
        if (ImGuiGBK::Button(addRuleId.c_str(), ImVec2(100, 0))) {
            ctx->editingPattern09Index = -1;
            memset(ctx->pattern09EditName, 0, sizeof(ctx->pattern09EditName));
            strcpy_s(ctx->pattern09EditOffset, "0");
            strcpy_s(ctx->pattern09EditLength, "4");
            ctx->showPattern09EditWindow = true;
        }

        ImGui::SameLine();
        std::string clearRulesId = "清空规则##clear_p09_rules_" + ctx->instanceId;
        if (ImGuiGBK::Button(clearRulesId.c_str(), ImVec2(100, 0))) {
            ctx->pattern09OffsetRules.clear();
            needsRedraw = true;
        }

        ImGui::Spacing();
        ImGui::Separator();

        if (!ctx->pattern09OffsetRules.empty()) {
            ImGuiGBK::Text("当前规则列表:");
            std::string tableId = "##p09_offset_table_" + ctx->instanceId;
            if (ImGui::BeginTable(tableId.c_str(), 5,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                ImVec2(0, 200))) {

                ImGui::TableSetupColumn(ImGuiText::U("启用"), ImGuiTableColumnFlags_WidthFixed, 40);
                ImGui::TableSetupColumn(ImGuiText::U("名称"), ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn(ImGuiText::U("偏移"), ImGuiTableColumnFlags_WidthFixed, 60);
                ImGui::TableSetupColumn(ImGuiText::U("长度"), ImGuiTableColumnFlags_WidthFixed, 50);
                ImGui::TableSetupColumn(ImGuiText::U("操作"), ImGuiTableColumnFlags_WidthFixed, 100);
                ImGui::TableHeadersRow();

                int deleteIdx = -1;
                for (size_t i = 0; i < ctx->pattern09OffsetRules.size(); i++) {
                    auto& rule = ctx->pattern09OffsetRules[i];
                    ImGui::TableNextRow();

                    ImGui::TableSetColumnIndex(0);
                    std::string cbId = "##p09_en_" + ctx->instanceId + "_" + std::to_string(i);
                    if (ImGui::Checkbox(cbId.c_str(), &rule.enabled)) {
                        needsRedraw = true;
                    }

                    ImGui::TableSetColumnIndex(1);
                    if (rule.enabled) {
                        ImGuiGBK::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "%s", rule.name.c_str());
                    } else {
                        ImGuiGBK::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%s", rule.name.c_str());
                    }

                    ImGui::TableSetColumnIndex(2);
                    ImGuiGBK::Text("%d", rule.offset);

                    ImGui::TableSetColumnIndex(3);
                    ImGuiGBK::Text("%d", rule.length);

                    ImGui::TableSetColumnIndex(4);
                    std::string editId = "编辑##edit_p09_" + ctx->instanceId + "_" + std::to_string(i);
                    if (ImGuiGBK::Button(editId.c_str(), ImVec2(40, 0))) {
                        ctx->editingPattern09Index = static_cast<int>(i);
                        strcpy_s(ctx->pattern09EditName, rule.name.c_str());
                        snprintf(ctx->pattern09EditOffset, sizeof(ctx->pattern09EditOffset), "%d", rule.offset);
                        snprintf(ctx->pattern09EditLength, sizeof(ctx->pattern09EditLength), "%d", rule.length);
                        ctx->showPattern09EditWindow = true;
                    }
                    ImGui::SameLine();
                    std::string delId = "删除##del_p09_" + ctx->instanceId + "_" + std::to_string(i);
                    if (ImGuiGBK::Button(delId.c_str(), ImVec2(40, 0))) {
                        deleteIdx = static_cast<int>(i);
                    }
                }

                ImGui::EndTable();

                if (deleteIdx >= 0 && deleteIdx < static_cast<int>(ctx->pattern09OffsetRules.size())) {
                    ctx->pattern09OffsetRules.erase(ctx->pattern09OffsetRules.begin() + deleteIdx);
                    needsRedraw = true;
                }
            }
        } else {
            ImGuiGBK::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "暂无规则，请点击\"添加规则\"按钮添加");
        }
    }
    break;

    case HEARTBEAT_SUBMENU_WHITELIST:
    {
        ImGuiGBK::TextColored(ImVec4(0.5f, 1.0f, 1.0f, 1.0f), "伪心跳白名单");
        ImGui::Separator();
        ImGui::Spacing();

        std::string enableWlId = "启用白名单##hb_whitelist_" + ctx->instanceId;
        if (ImGuiGBK::Checkbox(enableWlId.c_str(), &ctx->enableWhitelist)) {
            SaveSingleHeartbeatConfig(ctx);
            needsRedraw = true;
        }
        ImGui::SameLine();
        if (ctx->enableWhitelist) {
            ImGuiGBK::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "[已启用]");
        } else {
            ImGuiGBK::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "[未启用]");
        }

        ImGui::SameLine(300);
        ImGuiGBK::Text("规则数: %d", (int)ctx->whitelist.size());

        ImGui::Spacing();

        std::string addRuleId = "添加规则##add_wl_rule_" + ctx->instanceId;
        if (ImGuiGBK::Button(addRuleId.c_str(), ImVec2(100, 0))) {
            ctx->editingWhitelistIndex = -1;
            memset(ctx->whitelistEditName, 0, sizeof(ctx->whitelistEditName));
            strcpy_s(ctx->whitelistEditName, "新规则");
            memset(ctx->whitelistEditPattern, 0, sizeof(ctx->whitelistEditPattern));
            strcpy_s(ctx->whitelistEditOffset, "0");
            ctx->whitelistEditIsHex = true;
            ctx->showWhitelistEditWindow = true;
        }

        ImGui::SameLine();
        std::string clearRulesId = "清空规则##clear_wl_rules_" + ctx->instanceId;
        if (ImGuiGBK::Button(clearRulesId.c_str(), ImVec2(100, 0))) {
            ctx->whitelist.clear();
            needsRedraw = true;
        }

        if (!ctx->whitelist.empty()) {
            ImGui::Spacing();
            std::string tableId = "WhitelistRules_" + ctx->instanceId;
            if (ImGui::BeginTable(tableId.c_str(), 5,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                ImVec2(0, 200))) {

                ImGui::TableSetupColumn(ImGuiText::U("启用"), ImGuiTableColumnFlags_WidthFixed, 40);
                ImGui::TableSetupColumn(ImGuiText::U("规则名称"), ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn(ImGuiText::U("搜索模式"), ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn(ImGuiText::U("编辑"), ImGuiTableColumnFlags_WidthFixed, 50);
                ImGui::TableSetupColumn(ImGuiText::U("删除"), ImGuiTableColumnFlags_WidthFixed, 50);
                ImGui::TableHeadersRow();

                int deleteIdx = -1;
                for (size_t i = 0; i < ctx->whitelist.size(); i++) {
                    auto& rule = ctx->whitelist[i];
                    ImGui::TableNextRow();
                    ImGui::PushID(static_cast<int>(i) + 15000);

                    ImGui::TableSetColumnIndex(0);
                    if (ImGui::Checkbox("##enable_wl", &rule.enabled)) {
                        needsRedraw = true;
                    }

                    ImGui::TableSetColumnIndex(1);
                    ImGuiGBK::Text("%s", rule.name.c_str());

                    ImGui::TableSetColumnIndex(2);
                    std::string patternPreview = rule.pattern;
                    if (patternPreview.length() > 30) {
                        patternPreview = patternPreview.substr(0, 27) + "...";
                    }
                    ImGui::Text("%s", patternPreview.c_str());

                    ImGui::TableSetColumnIndex(3);
                    if (ImGuiGBK::SmallButton("编辑##edit_wl")) {
                        ctx->editingWhitelistIndex = static_cast<int>(i);
                        strcpy_s(ctx->whitelistEditName, rule.name.c_str());
                        strcpy_s(ctx->whitelistEditPattern, rule.pattern.c_str());
                        snprintf(ctx->whitelistEditOffset, sizeof(ctx->whitelistEditOffset), "%d", rule.offset);
                        ctx->whitelistEditIsHex = rule.isHex;
                        ctx->showWhitelistEditWindow = true;
                    }

                    ImGui::TableSetColumnIndex(4);
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.2f, 0.2f, 1.0f));
                    if (ImGuiGBK::SmallButton("删除##delete_wl")) {
                        deleteIdx = static_cast<int>(i);
                    }
                    ImGui::PopStyleColor();

                    ImGui::PopID();
                }

                ImGui::EndTable();

                if (deleteIdx >= 0 && deleteIdx < static_cast<int>(ctx->whitelist.size())) {
                    ctx->whitelist.erase(ctx->whitelist.begin() + deleteIdx);
                    needsRedraw = true;
                }
            }
        } else {
            ImGuiGBK::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "暂无白名单规则");
        }
    }
    break;

    case HEARTBEAT_SUBMENU_BLACKLIST:
    {
        ImGuiGBK::TextColored(ImVec4(0.5f, 1.0f, 1.0f, 1.0f), "伪心跳黑名单");
        ImGui::Separator();
        ImGui::Spacing();

        std::string enableBlId = "启用黑名单##hb_blacklist_" + ctx->instanceId;
        if (ImGuiGBK::Checkbox(enableBlId.c_str(), &ctx->enableBlacklist)) {
            SaveSingleHeartbeatConfig(ctx);
            needsRedraw = true;
        }
        ImGui::SameLine();
        if (ctx->enableBlacklist) {
            ImGuiGBK::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "[已启用]");
        } else {
            ImGuiGBK::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "[未启用]");
        }

        ImGui::SameLine(300);
        ImGuiGBK::Text("规则数: %d", (int)ctx->blacklist.size());

        ImGui::Spacing();

        std::string newBlRuleId = "新建规则##hb_bl_add_" + ctx->instanceId;
        if (ImGuiGBK::Button(newBlRuleId.c_str(), ImVec2(100, 0))) {
            ctx->editingBlacklistIndex = -1;
            memset(ctx->blacklistEditName, 0, sizeof(ctx->blacklistEditName));
            strcpy_s(ctx->blacklistEditName, "新规则");
            memset(ctx->blacklistEditPattern, 0, sizeof(ctx->blacklistEditPattern));
            strcpy_s(ctx->blacklistEditOffset, "0");
            ctx->blacklistEditIsHex = true;
            ctx->showBlacklistEditWindow = true;
            needsRedraw = true;
        }

        ImGui::SameLine();
        std::string clearBlRulesId = "清空规则##hb_bl_clear_" + ctx->instanceId;
        if (ImGuiGBK::Button(clearBlRulesId.c_str(), ImVec2(100, 0))) {
            ctx->blacklist.clear();
            needsRedraw = true;
        }

        if (!ctx->blacklist.empty()) {
            ImGui::Spacing();
            std::string tableId = "HeartbeatBlacklistRules_" + ctx->instanceId;
            if (ImGui::BeginTable(tableId.c_str(), 5,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                ImVec2(0, 200))) {

                ImGui::TableSetupColumn(ImGuiText::U("启用"), ImGuiTableColumnFlags_WidthFixed, 40);
                ImGui::TableSetupColumn(ImGuiText::U("规则名称"), ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn(ImGuiText::U("搜索模式"), ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn(ImGuiText::U("编辑"), ImGuiTableColumnFlags_WidthFixed, 50);
                ImGui::TableSetupColumn(ImGuiText::U("删除"), ImGuiTableColumnFlags_WidthFixed, 50);
                ImGui::TableHeadersRow();

                int deleteIdx = -1;
                for (size_t i = 0; i < ctx->blacklist.size(); i++) {
                    auto& rule = ctx->blacklist[i];
                    ImGui::TableNextRow();
                    ImGui::PushID(static_cast<int>(i) + 16000);

                    ImGui::TableSetColumnIndex(0);
                    if (ImGui::Checkbox("##enable_bl", &rule.enabled)) {
                        needsRedraw = true;
                    }

                    ImGui::TableSetColumnIndex(1);
                    ImGuiGBK::Text("%s", rule.name.c_str());

                    ImGui::TableSetColumnIndex(2);
                    std::string patternPreview = rule.pattern;
                    if (patternPreview.length() > 30) {
                        patternPreview = patternPreview.substr(0, 27) + "...";
                    }
                    ImGui::Text("%s", patternPreview.c_str());

                    ImGui::TableSetColumnIndex(3);
                    if (ImGuiGBK::SmallButton("编辑##edit_bl")) {
                        ctx->editingBlacklistIndex = static_cast<int>(i);
                        strcpy_s(ctx->blacklistEditName, rule.name.c_str());
                        strcpy_s(ctx->blacklistEditPattern, rule.pattern.c_str());
                        snprintf(ctx->blacklistEditOffset, sizeof(ctx->blacklistEditOffset), "%d", rule.offset);
                        ctx->blacklistEditIsHex = rule.isHex;
                        ctx->showBlacklistEditWindow = true;
                    }

                    ImGui::TableSetColumnIndex(4);
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.2f, 0.2f, 1.0f));
                    if (ImGuiGBK::SmallButton("删除##delete_bl")) {
                        deleteIdx = static_cast<int>(i);
                    }
                    ImGui::PopStyleColor();

                    ImGui::PopID();
                }

                ImGui::EndTable();

                if (deleteIdx >= 0 && deleteIdx < static_cast<int>(ctx->blacklist.size())) {
                    ctx->blacklist.erase(ctx->blacklist.begin() + deleteIdx);
                    needsRedraw = true;
                }
            }
        } else {
            ImGuiGBK::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "暂无黑名单规则");
        }
    }
    break;

    case HEARTBEAT_SUBMENU_FIXED_PACKET:
    {
        ImGuiGBK::TextColored(ImVec4(0.5f, 1.0f, 1.0f, 1.0f), "固定伪心跳模式");
        ImGui::Separator();
        ImGui::Spacing();

        std::string enableFixedId = "使用固定数据包##fixed_" + ctx->instanceId;
        if (ImGuiGBK::Checkbox(enableFixedId.c_str(), &ctx->enableFixedPacket)) {
            SaveSingleHeartbeatConfig(ctx);
            needsRedraw = true;
        }

        ImGui::Spacing();

        if (!ctx->fixedPacketData.empty()) {
            ImGuiGBK::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "已加载固定包 (%d 字节)", (int)ctx->fixedPacketData.size());
        } else {
            ImGuiGBK::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "未加载固定包");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.15f, 0.15f, 0.2f, 1.0f));
        std::string helpId = "FixedPacketHelp_" + ctx->instanceId;
        ImGui::BeginChild(helpId.c_str(), ImVec2(0, 60), true);
        ImGuiGBK::TextColored(ImVec4(1.0f, 1.0f, 0.5f, 1.0f), "说明:");
        ImGui::Spacing();
        ImGuiGBK::TextWrapped("固定包加载请在主界面进行。");
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    break;

    case HEARTBEAT_SUBMENU_SECONDARY_PROXY:
    {
        ImGuiGBK::TextColored(ImVec4(0.5f, 1.0f, 1.0f, 1.0f), "二级代理配置");
        ImGui::Separator();
        ImGui::Spacing();

        std::string enableProxyId = "启用二级代理##hb_proxy_" + ctx->instanceId;
        if (ImGuiGBK::Checkbox(enableProxyId.c_str(), &ctx->enableSecondaryProxy)) {
            if (running && forwarder) {
                if (ctx->enableSecondaryProxy) {
                    int port = atoi(ctx->secondaryProxyPort);
                    forwarder->SetSecondaryProxy(true, ctx->secondaryProxyHost, port,
                        ctx->secondaryProxyUsername, ctx->secondaryProxyPassword);
                } else {
                    forwarder->SetSecondaryProxy(false, "", 0, "", "");
                }
            }
            SaveSingleHeartbeatConfig(ctx);
            needsRedraw = true;
        }

        ImGui::Spacing();

        ImGuiGBK::Text("地址:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150);
        std::string hostId = "##hb_proxy_host_" + ctx->instanceId;
        ImGui::InputText(hostId.c_str(), ctx->secondaryProxyHost, sizeof(ctx->secondaryProxyHost));

        ImGui::SameLine();
        ImGuiGBK::Text("端口:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        std::string portId = "##hb_proxy_port_" + ctx->instanceId;
        ImGui::InputText(portId.c_str(), ctx->secondaryProxyPort, sizeof(ctx->secondaryProxyPort));

        ImGui::Spacing();

        ImGuiGBK::Text("用户:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150);
        std::string userId = "##hb_proxy_user_" + ctx->instanceId;
        ImGui::InputText(userId.c_str(), ctx->secondaryProxyUsername, sizeof(ctx->secondaryProxyUsername));

        ImGui::SameLine();
        ImGuiGBK::Text("密码:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150);
        std::string passId = "##hb_proxy_pass_" + ctx->instanceId;
        if (ctx->secondaryProxyShowPassword) {
            ImGui::InputText(passId.c_str(), ctx->secondaryProxyPassword, sizeof(ctx->secondaryProxyPassword));
        } else {
            ImGui::InputText(passId.c_str(), ctx->secondaryProxyPassword, sizeof(ctx->secondaryProxyPassword), ImGuiInputTextFlags_Password);
        }
        ImGui::SameLine();
        std::string showPassId = "显示##hb_pass_" + ctx->instanceId;
        if (ImGuiGBK::Checkbox(showPassId.c_str(), &ctx->secondaryProxyShowPassword)) {
            needsRedraw = true;
        }
    }
    break;

    case HEARTBEAT_SUBMENU_FORWARD_CONTROL:
    {
        ImGuiGBK::TextColored(ImVec4(0.5f, 1.0f, 1.0f, 1.0f), "转发控制");
        ImGui::Separator();
        ImGui::Spacing();

        std::string enableFwdId = "启用真实转发##fwd_" + ctx->instanceId;
        if (ImGuiGBK::Checkbox(enableFwdId.c_str(), &ctx->enableHeartbeatForward)) {
            SaveSingleHeartbeatConfig(ctx);
            needsRedraw = true;
        }

        ImGui::Spacing();

        if (ctx->enableHeartbeatForward) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.0f, 0.0f, 1.0f));
            ImGuiGBK::TextWrapped("【真实转发模式】修改后的数据会真正发送到服务器！");
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.0f, 1.0f));
            ImGuiGBK::TextWrapped("【测试模式】只在GUI显示，实际发送原始数据。");
            ImGui::PopStyleColor();
        }

        ImGui::Spacing();

        std::string enableRecId = "启用数据包记录##rec_" + ctx->instanceId;
        if (ImGuiGBK::Checkbox(enableRecId.c_str(), &ctx->enableHeartbeatRecording)) {
            SaveSingleHeartbeatConfig(ctx);
            needsRedraw = true;
        }
    }
    break;

    case HEARTBEAT_SUBMENU_PACKET_TYPE:
    {
        ImGuiGBK::TextColored(ImVec4(0.5f, 1.0f, 1.0f, 1.0f), "包头类型管理");
        ImGui::Separator();
        ImGui::Spacing();

        // 全伪模式开关
        std::string fullFakeId = "全伪模式(忽略包头类型限制)##hb_full_fake_" + ctx->instanceId;
        if (ImGuiGBK::Checkbox(fullFakeId.c_str(), &ctx->disableHeartbeatHeaderFilter)) {
            SaveSingleHeartbeatConfig(ctx);
            needsRedraw = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGuiGBK::Text("启用后将忽略包头类型限制，对所有数据包进行伪心跳替换");
            ImGui::EndTooltip();
        }
        if (ctx->disableHeartbeatHeaderFilter) {
            ImGui::SameLine();
            ImGuiGBK::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "[全伪模式已启用]");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGuiGBK::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "当前包头类型:");
        ImGui::Spacing();

        int displayedCount = 0;
        for (int type : ctx->packetTypes) {
            if (displayedCount > 0 && displayedCount % 8 == 0) {
            } else if (displayedCount > 0) {
                ImGui::SameLine();
            }

            char hexBuffer[16];
            sprintf_s(hexBuffer, "0x%02X", type);
            std::string labelId = std::string(hexBuffer) + "##hbpt_" + ctx->instanceId + "_" + std::to_string(type);

            bool enabled = ctx->packetTypeEnabled[type];
            if (ImGui::Checkbox(ImGuiText::U(labelId.c_str()), &enabled)) {
                ctx->packetTypeEnabled[type] = enabled;
                needsRedraw = true;
            }
            displayedCount++;
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGuiGBK::Text("添加:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        std::string newTypeId = "##hbNewType_" + ctx->instanceId;
        ImGui::InputText(newTypeId.c_str(), ctx->customTypeBuffer, sizeof(ctx->customTypeBuffer),
            ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase);

        ImGui::SameLine();
        std::string addBtnId = "添加##hbpt_" + ctx->instanceId;
        if (ImGuiGBK::Button(addBtnId.c_str(), ImVec2(60, 0))) {
            if (strlen(ctx->customTypeBuffer) > 0) {
                char* endptr;
                int newType = (int)strtol(ctx->customTypeBuffer, &endptr, 16);
                if (newType >= 0 && newType <= 255) {
                    auto it = std::find(ctx->packetTypes.begin(), ctx->packetTypes.end(), newType);
                    if (it == ctx->packetTypes.end()) {
                        ctx->packetTypes.push_back(newType);
                        std::sort(ctx->packetTypes.begin(), ctx->packetTypes.end());
                        ctx->packetTypeEnabled[newType] = true;
                        ctx->customTypeBuffer[0] = '\0';
                    }
                }
            }
            needsRedraw = true;
        }
    }
    break;

    case HEARTBEAT_SUBMENU_DATA_POOL:
    {
        ImGuiGBK::TextColored(ImVec4(0.5f, 1.0f, 1.0f, 1.0f), "数据池管理");
        ImGui::Separator();
        ImGui::Spacing();

        // 数据源选择 - 三种模式
        ImGuiGBK::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "数据源:");
        ImGui::Spacing();

        std::string srcMemoryId = "内存池(绑定采集实例)##src_" + ctx->instanceId;
        std::string srcDbId = "数据库读取##src_" + ctx->instanceId;
        std::string srcRemoteId = "远程同步##src_" + ctx->instanceId;

        if (ImGui::RadioButton(ImGuiText::U(srcMemoryId.c_str()), &ctx->dataSourceType, 0)) {
            SaveSingleHeartbeatConfig(ctx);
            needsRedraw = true;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton(ImGuiText::U(srcDbId.c_str()), &ctx->dataSourceType, 1)) {
            SaveSingleHeartbeatConfig(ctx);
            needsRedraw = true;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton(ImGuiText::U(srcRemoteId.c_str()), &ctx->dataSourceType, 2)) {
            SaveSingleHeartbeatConfig(ctx);
            needsRedraw = true;
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // 根据数据源类型显示不同配置
        if (ctx->dataSourceType == 0) {
            // 内存池模式 - 绑定采集实例
            ImGuiGBK::TextColored(ImVec4(0.5f, 1.0f, 1.0f, 1.0f), "内存池配置:");
            ImGui::Spacing();

            // 获取所有可用的单伪采集实例
            ImGuiGBK::Text("绑定采集实例:");
            ImGui::SameLine();

            // 构建采集实例列表
            std::vector<std::string> collectorIds;
            std::vector<const char*> collectorIdsCStr;
            collectorIds.push_back("(未绑定)");

            {
                std::lock_guard<std::mutex> lock(g_singleCtxMutex);
                for (const auto& pair : g_singleCollectorCtxMap) {
                    collectorIds.push_back(pair.first);
                }
            }

            for (const auto& id : collectorIds) {
                collectorIdsCStr.push_back(id.c_str());
            }

            int currentIndex = 0;
            for (size_t i = 1; i < collectorIds.size(); i++) {
                if (collectorIds[i] == ctx->boundCollectorId) {
                    currentIndex = (int)i;
                    break;
                }
            }

            ImGui::SetNextItemWidth(200);
            std::string comboId = "##bound_collector_" + ctx->instanceId;
            if (ImGui::Combo(comboId.c_str(), &currentIndex, collectorIdsCStr.data(), (int)collectorIdsCStr.size())) {
                if (currentIndex == 0) {
                    ctx->boundCollectorId = "";
                } else {
                    ctx->boundCollectorId = collectorIds[currentIndex];
                }
                SaveSingleHeartbeatConfig(ctx);
                needsRedraw = true;
                AB_LOG_INFO("[单伪心跳][" + ctx->instanceId + "] 绑定采集实例: " +
                    (ctx->boundCollectorId.empty() ? "(未绑定)" : ctx->boundCollectorId));
            }

            ImGui::Spacing();
            if (ctx->boundCollectorId.empty()) {
                ImGuiGBK::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "警告: 未绑定采集实例，内存池模式将无法获取数据");
            } else {
                std::lock_guard<std::mutex> lock(g_singleCtxMutex);
                auto poolIt = g_singleCollectorPoolMap.find(ctx->boundCollectorId);
                auto ctxIt = g_singleCollectorCtxMap.find(ctx->boundCollectorId);

                if (ctxIt != g_singleCollectorCtxMap.end()) {
                    ImGuiGBK::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "已绑定: %s", ctx->boundCollectorId.c_str());
                    if (poolIt != g_singleCollectorPoolMap.end() && poolIt->second) {
                        auto stats = poolIt->second->GetStats();
                        ImGuiGBK::Text("数据池状态: %d 条数据, %d 个GameID", stats.totalPackets, stats.gameIDCount);
                    } else {
                        ImGuiGBK::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "数据池状态: 暂无数据");
                    }
                } else {
                    ImGuiGBK::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "错误: 绑定的采集实例不存在");
                }
            }

            ImGui::Spacing();
            std::string useMemPoolId = "使用内存池数据##mempool_" + ctx->instanceId;
            if (ImGuiGBK::Checkbox(useMemPoolId.c_str(), &ctx->useMemoryPool)) {
                SaveSingleHeartbeatConfig(ctx);
                needsRedraw = true;
            }

            ImGui::Spacing();
            std::string refreshPoolId = "从采集实例刷新数据##refresh_from_collector_" + ctx->instanceId;
            if (ImGuiGBK::Button(refreshPoolId.c_str(), ImVec2(180, 0))) {
                if (!ctx->boundCollectorId.empty()) {
                    UpdateHeartbeatPoolStatsFromBoundCollector(ctx);
                    AB_LOG_INFO("[单伪心跳][" + ctx->instanceId + "] 从采集实例刷新数据: " +
                        std::to_string(ctx->loadedPacketCount) + " 条");
                }
                needsRedraw = true;
            }

            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
            ImGuiGBK::TextWrapped(
                "说明: 内存池模式下，伪心跳将直接使用绑定的采集实例的内存数据。"
                "这种模式下数据实时性最好，但需要采集实例处于运行状态。"
            );
            ImGui::PopStyleColor();

        } else if (ctx->dataSourceType == 1) {
            // 数据库读取模式
            ImGuiGBK::TextColored(ImVec4(0.5f, 1.0f, 1.0f, 1.0f), "数据库读取配置:");
            ImGui::Spacing();

            // 目录选择
            ImGuiGBK::Text("数据库目录:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(300);
            std::string dirInputId = "##hb_db_dir_" + ctx->instanceId;
            ImGui::InputText(dirInputId.c_str(), ctx->dbDirectory, sizeof(ctx->dbDirectory), ImGuiInputTextFlags_ReadOnly);

            ImGui::SameLine();
            std::string browseDirId = "选择目录##hb_browse_dir_" + ctx->instanceId;
            if (ImGuiGBK::Button(browseDirId.c_str(), ImVec2(80, 0))) {
                if (SelectDirectoryDialog(ctx->dbDirectory, sizeof(ctx->dbDirectory))) {
                    ctx->dbFileList = ScanDbFilesInDirectory(ctx->dbDirectory);
                    ctx->selectedDbIndex = -1;
                    ctx->selectedDbPath = "";
                    SaveSingleHeartbeatConfig(ctx);
                    needsRedraw = true;
                    AB_LOG_INFO("[单伪心跳][" + ctx->instanceId + "] 选择数据库目录: " + std::string(ctx->dbDirectory));
                }
            }

            ImGui::Spacing();

            // 刷新和新建按钮
            std::string refreshListId = "刷新列表##hb_refresh_db_" + ctx->instanceId;
            if (ImGuiGBK::Button(refreshListId.c_str(), ImVec2(80, 0))) {
                ctx->dbFileList = ScanDbFilesInDirectory(ctx->dbDirectory);
                needsRedraw = true;
            }

            ImGui::SameLine();
            std::string createDbId = "新建数据库##hb_create_db_" + ctx->instanceId;
            if (ImGuiGBK::Button(createDbId.c_str(), ImVec2(100, 0))) {
                if (strlen(ctx->dbDirectory) > 0) {
                    ctx->showCreateDbDialog = true;
                    memset(ctx->dbNameBuffer, 0, sizeof(ctx->dbNameBuffer));
                } else {
                    AB_LOG_WARNING("[单伪心跳][" + ctx->instanceId + "] 请先选择数据库目录");
                }
            }

            ImGui::Spacing();

            // 数据库文件列表
            if (strlen(ctx->dbDirectory) > 0) {
                ImGuiGBK::Text("目录下的数据库文件 (%d个):", (int)ctx->dbFileList.size());

                std::string listBoxId = "##hb_db_list_" + ctx->instanceId;
                if (ImGui::BeginListBox(listBoxId.c_str(), ImVec2(-1, 120))) {
                    for (size_t i = 0; i < ctx->dbFileList.size(); i++) {
                        bool isSelected = (ctx->selectedDbIndex == (int)i);
                        if (ImGui::Selectable(ctx->dbFileList[i].c_str(), isSelected)) {
                            ctx->selectedDbIndex = (int)i;
                            ctx->selectedDbPath = (std::filesystem::path(ctx->dbDirectory) / ctx->dbFileList[i]).string();
                            SaveSingleHeartbeatConfig(ctx);
                            needsRedraw = true;
                            AB_LOG_INFO("[单伪心跳][" + ctx->instanceId + "] 选中数据库: " + ctx->selectedDbPath);
                        }
                        if (isSelected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndListBox();
                }

                if (!ctx->selectedDbPath.empty()) {
                    ImGui::Spacing();
                    ImGuiGBK::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "当前选中: %s", ctx->selectedDbPath.c_str());
                }
            } else {
                ImGuiGBK::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "请先选择数据库目录");
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // 自动加载配置
            ImGuiGBK::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "自动增量加载:");
            ImGui::Spacing();

            std::string loadModeHourly = "按小时增量##load_mode_" + ctx->instanceId;
            std::string loadModeDaily = "按天增量##load_mode_" + ctx->instanceId;

            if (ImGui::RadioButton(ImGuiText::U(loadModeHourly.c_str()), &ctx->autoLoadMode, 0)) {
                SaveSingleHeartbeatConfig(ctx);
                needsRedraw = true;
            }
            ImGui::SameLine();
            if (ImGui::RadioButton(ImGuiText::U(loadModeDaily.c_str()), &ctx->autoLoadMode, 1)) {
                SaveSingleHeartbeatConfig(ctx);
                needsRedraw = true;
            }

            ImGui::Spacing();

            // 加载控制按钮
            std::string loadDbId = "加载数据##hb_load_db_" + ctx->instanceId;
            if (ImGuiGBK::Button(loadDbId.c_str(), ImVec2(100, 0))) {
                if (!ctx->selectedDbPath.empty()) {
                    ctx->dbLoadStatus = "正在加载...";
                    ctx->isLoadingDb = true;
                    needsRedraw = true;
                } else {
                    ctx->dbLoadStatus = "请先选择数据库";
                }
            }

            ImGui::SameLine();
            std::string startAutoLoadId = ctx->autoLoadRunning ? "停止自动加载##hb_auto_" : "启动自动加载##hb_auto_";
            startAutoLoadId += ctx->instanceId;
            if (ImGuiGBK::Button(startAutoLoadId.c_str(), ImVec2(120, 0))) {
                ctx->autoLoadRunning = !ctx->autoLoadRunning;
                if (ctx->autoLoadRunning) {
                    AB_LOG_INFO("[单伪心跳][" + ctx->instanceId + "] 启动自动增量加载");
                } else {
                    AB_LOG_INFO("[单伪心跳][" + ctx->instanceId + "] 停止自动增量加载");
                }
                needsRedraw = true;
            }

            if (ctx->autoLoadRunning) {
                ImGui::SameLine();
                ImGuiGBK::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "[运行中]");
            }

            ImGui::Spacing();
            if (!ctx->dbLoadStatus.empty()) {
                if (ctx->isLoadingDb) {
                    ImGuiGBK::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "%s", ctx->dbLoadStatus.c_str());
                } else if (ctx->dbLoadStatus.find("成功") != std::string::npos) {
                    ImGuiGBK::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "%s", ctx->dbLoadStatus.c_str());
                } else {
                    ImGuiGBK::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "%s", ctx->dbLoadStatus.c_str());
                }
            }

            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
            ImGuiGBK::TextWrapped(
                "说明: 数据库读取模式下，从指定目录的数据库文件加载心跳数据。"
                "支持按小时或按天增量加载，自动跟踪已加载的记录ID。"
            );
            ImGui::PopStyleColor();

            // 新建数据库对话框
            if (ctx->showCreateDbDialog) {
                std::string modalId = "新建数据库##hb_create_modal_" + ctx->instanceId;
                ImGui::OpenPopup(ImGuiText::U(modalId.c_str()));

                if (ImGui::BeginPopupModal(ImGuiText::U(modalId.c_str()), &ctx->showCreateDbDialog, ImGuiWindowFlags_AlwaysAutoResize)) {
                    ImGuiGBK::Text("数据库名称:");
                    ImGui::SameLine();
                    std::string nameInputId = "##hb_new_db_name_" + ctx->instanceId;
                    ImGui::SetNextItemWidth(200);
                    ImGui::InputText(nameInputId.c_str(), ctx->dbNameBuffer, sizeof(ctx->dbNameBuffer));

                    ImGui::Spacing();
                    ImGuiGBK::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "提示: 不需要输入.db扩展名");

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    std::string confirmId = "创建##hb_confirm_create_" + ctx->instanceId;
                    if (ImGuiGBK::Button(confirmId.c_str(), ImVec2(100, 0))) {
                        if (strlen(ctx->dbNameBuffer) > 0) {
                            std::string newDbPath;
                            if (CreateDatabaseFile(ctx->dbDirectory, ctx->dbNameBuffer, newDbPath)) {
                                ctx->dbFileList = ScanDbFilesInDirectory(ctx->dbDirectory);
                                ctx->selectedDbPath = newDbPath;
                                std::string newFileName = std::filesystem::path(newDbPath).filename().string();
                                for (size_t i = 0; i < ctx->dbFileList.size(); i++) {
                                    if (ctx->dbFileList[i] == newFileName) {
                                        ctx->selectedDbIndex = (int)i;
                                        break;
                                    }
                                }
                                SaveSingleHeartbeatConfig(ctx);
                                ctx->showCreateDbDialog = false;
                                needsRedraw = true;
                            }
                        }
                    }

                    ImGui::SameLine();
                    std::string cancelId = "取消##hb_cancel_create_" + ctx->instanceId;
                    if (ImGuiGBK::Button(cancelId.c_str(), ImVec2(100, 0))) {
                        ctx->showCreateDbDialog = false;
                    }

                    ImGui::EndPopup();
                }
            }
        } else if (ctx->dataSourceType == 2) {
            // 远程同步模式
            ImGuiGBK::TextColored(ImVec4(0.5f, 1.0f, 1.0f, 1.0f), "远程同步配置:");
            ImGui::Spacing();

            // 远程服务器地址
            ImGuiGBK::Text("服务器地址:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(200);
            std::string remoteHostId = "##hb_remote_host_" + ctx->instanceId;
            if (ImGui::InputText(remoteHostId.c_str(), ctx->remoteHost, sizeof(ctx->remoteHost))) {
                SaveSingleHeartbeatConfig(ctx);
                needsRedraw = true;
            }

            ImGui::SameLine();
            ImGuiGBK::Text("端口:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80);
            std::string remotePortId = "##hb_remote_port_" + ctx->instanceId;
            if (ImGui::InputText(remotePortId.c_str(), ctx->remotePort, sizeof(ctx->remotePort), ImGuiInputTextFlags_CharsDecimal)) {
                SaveSingleHeartbeatConfig(ctx);
                needsRedraw = true;
            }

            ImGui::Spacing();

            // 自动同步开关
            std::string autoSyncId = "启用自动同步##hb_auto_sync_" + ctx->instanceId;
            if (ImGuiGBK::Checkbox(autoSyncId.c_str(), &ctx->enableAutoSync)) {
                SaveSingleHeartbeatConfig(ctx);
                needsRedraw = true;
            }

            if (ctx->enableAutoSync) {
                ImGui::SameLine();
                ImGuiGBK::Text("间隔(秒):");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80);
                std::string syncIntervalId = "##hb_sync_interval_" + ctx->instanceId;
                if (ImGui::InputText(syncIntervalId.c_str(), ctx->syncIntervalBuffer, sizeof(ctx->syncIntervalBuffer), ImGuiInputTextFlags_CharsDecimal)) {
                    int interval = atoi(ctx->syncIntervalBuffer);
                    if (interval >= 10 && interval <= 3600) {
                        ctx->syncIntervalSeconds = interval;
                        SaveSingleHeartbeatConfig(ctx);
                    }
                    needsRedraw = true;
                }
            }

            ImGui::Spacing();

            // 同步控制按钮
            std::string syncNowId = "立即同步##hb_sync_now_" + ctx->instanceId;
            if (ImGuiGBK::Button(syncNowId.c_str(), ImVec2(100, 0))) {
                if (!ctx->isSyncing) {
                    ctx->isSyncing = true;
                    ctx->syncStatus = "正在连接...";
                    ctx->syncProgress = 0.0f;
                    ctx->syncDownloadedBytes = 0;
                    ctx->syncTotalBytes = 0;
                    // TODO: 启动同步线程
                    AB_LOG_INFO("[单伪心跳][" + ctx->instanceId + "] 开始远程同步: " +
                        std::string(ctx->remoteHost) + ":" + std::string(ctx->remotePort));
                }
                needsRedraw = true;
            }

            ImGui::SameLine();
            std::string stopSyncId = "停止同步##hb_stop_sync_" + ctx->instanceId;
            if (ImGuiGBK::Button(stopSyncId.c_str(), ImVec2(100, 0))) {
                if (ctx->isSyncing) {
                    ctx->isSyncing = false;
                    ctx->syncRunning = false;
                    ctx->syncStatus = "已停止";
                    AB_LOG_INFO("[单伪心跳][" + ctx->instanceId + "] 停止远程同步");
                }
                needsRedraw = true;
            }

            ImGui::Spacing();

            // 同步状态显示
            if (ctx->isSyncing) {
                ImGuiGBK::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "正在同步...");
                if (ctx->syncTotalBytes > 0) {
                    float progress = (float)ctx->syncDownloadedBytes / (float)ctx->syncTotalBytes;
                    char progressText[64];
                    sprintf_s(progressText, "%.1f / %.1f MB",
                        ctx->syncDownloadedBytes / 1024.0 / 1024.0,
                        ctx->syncTotalBytes / 1024.0 / 1024.0);
                    ImGui::ProgressBar(progress, ImVec2(-1, 0), progressText);
                }
            } else if (!ctx->syncStatus.empty()) {
                if (ctx->syncStatus.find("成功") != std::string::npos) {
                    ImGuiGBK::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "%s", ctx->syncStatus.c_str());
                } else if (ctx->syncStatus.find("失败") != std::string::npos || ctx->syncStatus.find("错误") != std::string::npos) {
                    ImGuiGBK::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "%s", ctx->syncStatus.c_str());
                } else {
                    ImGuiGBK::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "%s", ctx->syncStatus.c_str());
                }
            }

            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
            ImGuiGBK::TextWrapped(
                "说明: 远程同步模式下，从远程TCP文件服务器下载数据库文件。"
                "需要远程采集端启动TCP文件共享服务。"
                "支持增量同步，只下载新增的数据。"
            );
            ImGui::PopStyleColor();
        }
    }
    break;

    case HEARTBEAT_SUBMENU_TARGET_FILTER:
    {
        ImGuiGBK::TextColored(ImVec4(0.5f, 1.0f, 1.0f, 1.0f), "目标地址过滤");
        ImGui::Separator();
        ImGui::Spacing();

        std::string noFilterId = "不过滤##hb_" + ctx->instanceId;
        std::string byIpId = "按IP##hb_" + ctx->instanceId;
        std::string byDomainId = "按域名##hb_" + ctx->instanceId;
        std::string byPortId = "按端口##hb_" + ctx->instanceId;

        if (ImGui::RadioButton(ImGuiText::U(noFilterId.c_str()), &ctx->filterType, 0)) needsRedraw = true;
        ImGui::SameLine();
        if (ImGui::RadioButton(ImGuiText::U(byIpId.c_str()), &ctx->filterType, 1)) needsRedraw = true;
        ImGui::SameLine();
        if (ImGui::RadioButton(ImGuiText::U(byDomainId.c_str()), &ctx->filterType, 2)) needsRedraw = true;
        ImGui::SameLine();
        if (ImGui::RadioButton(ImGuiText::U(byPortId.c_str()), &ctx->filterType, 3)) needsRedraw = true;

        ImGui::Spacing();

        ImGuiGBK::Text("值:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200);
        std::string filterValId = "##hbFilterValue_" + ctx->instanceId;
        ImGui::InputText(filterValId.c_str(), ctx->filterValue, sizeof(ctx->filterValue));

        ImGui::SameLine();
        std::string applyId = "应用##hb_filter_" + ctx->instanceId;
        if (ImGuiGBK::Button(applyId.c_str(), ImVec2(80, 0))) {
            if (forwarder) {
                if (ctx->filterType == 0) {
                    forwarder->ClearFilter();
                } else {
                    FilterType ft = static_cast<FilterType>(ctx->filterType);
                    forwarder->SetFilter(ft, ctx->filterValue);
                }
            }
            needsRedraw = true;
        }

        ImGui::SameLine();
        std::string clearId = "清除##hb_filter_" + ctx->instanceId;
        if (ImGuiGBK::Button(clearId.c_str(), ImVec2(80, 0))) {
            if (forwarder) {
                forwarder->ClearFilter();
                ctx->filterType = 0;
                ctx->filterValue[0] = '\0';
            }
            needsRedraw = true;
        }
    }
    break;

    case HEARTBEAT_SUBMENU_SOCKS5_ACCOUNTS:
    {
        ImGuiGBK::TextColored(ImVec4(0.5f, 1.0f, 1.0f, 1.0f), "SOCKS5账号库绑定");
        ImGui::Separator();
        ImGui::Spacing();

        // 启用SOCKS5认证
        std::string authCheckId = "启用SOCKS5认证##hb_socks_auth_" + ctx->instanceId;
        if (ImGuiGBK::Checkbox(authCheckId.c_str(), &ctx->enableSocksAuth)) {
            if (forwarder) {
                forwarder->SetSocks5Auth(ctx->enableSocksAuth);
            }
            SaveSingleHeartbeatConfig(ctx);
            needsRedraw = true;
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // 账号库绑定选择
        ImGuiGBK::Text("绑定账号库:");
        ImGui::SameLine();

        auto& instMgr = InstanceManager::GetInstance();
        auto pools = instMgr.GetSocks5PoolInstances();

        std::vector<std::string> poolNames;
        std::vector<const char*> poolNamesCStr;
        poolNames.push_back("(未绑定)");

        int currentIndex = 0;
        for (size_t i = 0; i < pools.size(); i++) {
            std::string name = pools[i]->GetName() + " (" + std::to_string(pools[i]->GetAccountCount()) + "个账号)";
            poolNames.push_back(name);
            if (pools[i]->GetId() == ctx->boundSocks5PoolId) {
                currentIndex = static_cast<int>(i + 1);
            }
        }

        for (const auto& name : poolNames) {
            poolNamesCStr.push_back(name.c_str());
        }

        ImGui::SetNextItemWidth(250);
        std::string comboId = "##hb_socks5_pool_" + ctx->instanceId;
        if (ImGui::Combo(comboId.c_str(), &currentIndex, poolNamesCStr.data(), static_cast<int>(poolNamesCStr.size()))) {
            if (currentIndex == 0) {
                ctx->boundSocks5PoolId = "";
            } else {
                ctx->boundSocks5PoolId = pools[currentIndex - 1]->GetId();
            }
            SaveSingleHeartbeatConfig(ctx);

            // 同步账号到forwarder
            if (forwarder) {
                SyncHeartbeatSocks5Accounts(ctx, forwarder);
            }

            needsRedraw = true;
        }

        ImGui::Spacing();

        // 显示当前绑定状态
        if (ctx->boundSocks5PoolId.empty()) {
            ImGuiGBK::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "未绑定账号库，SOCKS5认证将无账号可用");
        } else {
            auto* pool = instMgr.GetSocks5PoolInstance(ctx->boundSocks5PoolId);
            if (pool) {
                ImGuiGBK::TextColored(ImVec4(0.0f, 1.0f, 0.5f, 1.0f), "已绑定: %s", pool->GetName().c_str());
                ImGuiGBK::Text("账号数量: %d", pool->GetAccountCount());
            } else {
                ImGuiGBK::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "绑定的账号库已不存在");
                ctx->boundSocks5PoolId = "";
                SaveSingleHeartbeatConfig(ctx);
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
        ImGuiGBK::TextWrapped(
            "说明: 在主菜单的 [SOCKS5账号] 页面创建和管理账号库实例。"
            "每个采集/伪心跳实例可以绑定一个账号库，启用认证后将使用该库中的账号进行验证。"
        );
        ImGui::PopStyleColor();
    }
    break;

    default:
        ImGuiGBK::Text("子菜单: %s", GetHeartbeatSubMenuName(menuType));
        break;
    }
}

// ==================== 悬浮窗口渲染 ====================
static void RenderSingleCollectorFloatingWindows(SingleCollectorCtx* ctx, PacketCollector* collector,
    CollectorInstance* instance, bool& needsRedraw) {
    if (!ctx) return;

    for (auto it = ctx->floatingWindows.begin(); it != ctx->floatingWindows.end(); ) {
        if (!it->isOpen) {
            it = ctx->floatingWindows.erase(it);
            continue;
        }

        std::string windowTitle = std::string(GetCollectorSubMenuName(it->menuType)) +
            "##coll_float_" + ctx->instanceId + "_" + std::to_string(it->windowId);

        ImGui::SetNextWindowSize(ImVec2(400, 300), ImGuiCond_FirstUseEver);
        if (ImGui::Begin(windowTitle.c_str(), &it->isOpen)) {
            RenderSingleCollectorSubMenuContent(ctx, collector, instance, it->menuType, needsRedraw);
        }
        ImGui::End();
        ++it;
    }
}

static void RenderSingleHeartbeatFloatingWindows(SingleHeartbeatCtx* ctx, PacketCollector* forwarder,
    HeartbeatInstance* instance, bool& needsRedraw) {
    if (!ctx) return;

    for (auto it = ctx->floatingWindows.begin(); it != ctx->floatingWindows.end(); ) {
        if (!it->isOpen) {
            it = ctx->floatingWindows.erase(it);
            continue;
        }

        std::string windowTitle = std::string(GetHeartbeatSubMenuName(it->menuType)) +
            "##hb_float_" + ctx->instanceId + "_" + std::to_string(it->windowId);

        ImGui::SetNextWindowSize(ImVec2(400, 300), ImGuiCond_FirstUseEver);
        if (ImGui::Begin(windowTitle.c_str(), &it->isOpen)) {
            RenderSingleHeartbeatSubMenuContent(ctx, forwarder, instance, it->menuType, needsRedraw);
        }
        ImGui::End();
        ++it;
    }
}

// ==================== 详情窗口渲染 ====================
static void RenderSingleCollectorLeftDetailWindow(SingleCollectorCtx* ctx, bool& needsRedraw) {
    if (!ctx || !ctx->guiState.showLeftDetail) return;

    std::string windowTitle = "采集详情##coll_detail_" + ctx->instanceId;
    ImGui::SetNextWindowSize(ImVec2(500, 400), ImGuiCond_FirstUseEver);

    if (ImGui::Begin(windowTitle.c_str(), &ctx->guiState.showLeftDetail)) {
        int selectedId = ctx->guiState.leftSelectedId;
        DisplayPacket* pkt = nullptr;

        {
            std::lock_guard<std::mutex> lock(ctx->packetsMutex);
            for (auto& p : ctx->packets) {
                if (p.id == selectedId) {
                    pkt = &p;
                    break;
                }
            }
        }

        if (pkt) {
            ImGuiGBK::Text("ID: %d", pkt->id);
            ImGuiGBK::Text("GameID: %s", pkt->gameID.c_str());
            ImGuiGBK::Text("时间: %s", pkt->timestamp.c_str());
            ImGuiGBK::Text("大小: %d 字节", pkt->size);
        } else {
            ImGuiGBK::Text("未选中数据包");
        }
    }
    ImGui::End();
}

static void RenderSingleHeartbeatLeftDetailWindow(SingleHeartbeatCtx* ctx, bool& needsRedraw) {
    if (!ctx || !ctx->guiState.showLeftDetail) return;

    std::string windowTitle = "伪心跳详情##hb_detail_" + ctx->instanceId;
    ImGui::SetNextWindowSize(ImVec2(500, 400), ImGuiCond_FirstUseEver);

    if (ImGui::Begin(windowTitle.c_str(), &ctx->guiState.showLeftDetail)) {
        int selectedId = ctx->guiState.leftSelectedId;
        DisplayPacket* pkt = nullptr;

        {
            std::lock_guard<std::mutex> lock(ctx->heartbeatPacketsMutex);
            for (auto& p : ctx->heartbeatPackets) {
                if (p.id == selectedId) {
                    pkt = &p;
                    break;
                }
            }
        }

        if (pkt) {
            ImGuiGBK::Text("ID: %d", pkt->id);
            ImGuiGBK::Text("GameID: %s", pkt->gameID.c_str());
            ImGuiGBK::Text("时间: %s", pkt->timestamp.c_str());
            ImGuiGBK::Text("大小: %d 字节", pkt->size);
        } else {
            ImGuiGBK::Text("未选中数据包");
        }
    }
    ImGui::End();
}

static void RenderSingleHeartbeatRightDetailWindow(SingleHeartbeatCtx* ctx, bool& needsRedraw) {
    if (!ctx || !ctx->guiState.showRightDetail) return;

    std::string windowTitle = "数据内容##hb_data_" + ctx->instanceId;
    ImGui::SetNextWindowSize(ImVec2(600, 400), ImGuiCond_FirstUseEver);

    if (ImGui::Begin(windowTitle.c_str(), &ctx->guiState.showRightDetail)) {
        ImGuiGBK::Text("数据内容查看窗口");
    }
    ImGui::End();
}

// ==================== 采集主页渲染 ====================
static void RenderCollectorMainPage(std::shared_ptr<SingleCollectorCtx> ctx, PacketCollector* collector,
    CollectorInstance* instance, bool& needsRedraw) {

    if (!ctx) return;
    bool running = collector && collector->IsRunning();

    // 控制面板
    ImGuiGBK::Text("监听端口: ");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    std::string portId = "##port_" + ctx->instanceId;
    if (ImGui::InputText(portId.c_str(), ctx->portBuffer, sizeof(ctx->portBuffer))) {
        needsRedraw = true;
    }

    ImGui::SameLine();
    std::string btnId = (running ? "停止代理##" : "启动代理##") + ctx->instanceId;
    if (ImGuiGBK::Button(btnId.c_str(), ImVec2(120, 0))) {
        if (running) {
            if (instance) instance->Stop();
        } else {
            int newPort = atoi(ctx->portBuffer);
            if (instance) {
                instance->SetPort(newPort);
                instance->Start();
            }
        }
        needsRedraw = true;
    }

    ImGui::SameLine();
    if (running) {
        ImGuiGBK::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "● 运行中");
    } else {
        ImGuiGBK::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "● 已停止");
    }

    ImGui::Separator();

    // 记录控制
    std::string recId = "启用数据包记录##rec_" + ctx->instanceId;
    if (ImGuiGBK::Checkbox(recId.c_str(), &ctx->enableRecording)) {
        SaveSingleCollectorConfig(ctx.get());
        needsRedraw = true;
    }

    ImGui::SameLine();
    std::string clearId = "清空列表##clear_" + ctx->instanceId;
    if (ImGuiGBK::Button(clearId.c_str(), ImVec2(100, 0))) {
        std::lock_guard<std::mutex> lock(ctx->packetsMutex);
        ctx->packets.clear();
        ctx->nextPacketId = 1;
        needsRedraw = true;
    }

    ImGui::Separator();

    // 数据包列表
    ImGuiGBK::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "数据包列表");
    ImGui::Spacing();

    std::string listId = "PacketList_" + ctx->instanceId;
    ImGui::BeginChild(listId.c_str(), ImVec2(0, 0), true);

    std::string tableId = "PacketsTable_" + ctx->instanceId;
    if (ImGui::BeginTable(tableId.c_str(), 5,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {

        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 50);
        ImGui::TableSetupColumn(ImGuiText::U("时间"), ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableSetupColumn(ImGuiText::U("游戏ID"), ImGuiTableColumnFlags_WidthFixed, 120);
        ImGui::TableSetupColumn(ImGuiText::U("大小"), ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn(ImGuiText::U("预览"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        std::lock_guard<std::mutex> lock(ctx->packetsMutex);
        for (const auto& pkt : ctx->packets) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("#%d", pkt.id);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%s", pkt.timestamp.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%s", pkt.gameID.c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%d B", pkt.size);
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%s", pkt.GetPreview().c_str());
        }

        ImGui::EndTable();
    }

    ImGui::EndChild();
}

// ==================== 伪心跳主页渲染 ====================
static void RenderHeartbeatMainPage(std::shared_ptr<SingleHeartbeatCtx> ctx, PacketCollector* forwarder,
    HeartbeatInstance* instance, bool& needsRedraw) {

    if (!ctx) return;
    bool running = forwarder && forwarder->IsRunning();

    // 控制面板
    ImGuiGBK::Text("监听端口: ");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    std::string portId = "##hbPort_" + ctx->instanceId;
    if (ImGui::InputText(portId.c_str(), ctx->heartbeatPortBuffer, sizeof(ctx->heartbeatPortBuffer))) {
        needsRedraw = true;
    }

    ImGui::SameLine();
    std::string btnId = (running ? "停止转发##" : "启动转发##") + ctx->instanceId;
    if (ImGuiGBK::Button(btnId.c_str(), ImVec2(120, 0))) {
        if (running) {
            if (instance) instance->Stop();
        } else {
            int newPort = atoi(ctx->heartbeatPortBuffer);
            if (instance) {
                instance->SetPort(newPort);
                instance->Start();
            }
        }
        needsRedraw = true;
    }

    ImGui::SameLine();
    if (running) {
        ImGuiGBK::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "● 运行中");
    } else {
        ImGuiGBK::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "● 已停止");
    }

    ImGui::Separator();

    // 转发控制
    std::string fwdId = "启用伪心跳转发##fwd_" + ctx->instanceId;
    if (ImGuiGBK::Checkbox(fwdId.c_str(), &ctx->enableHeartbeatForward)) {
        SaveSingleHeartbeatConfig(ctx.get());
        needsRedraw = true;
    }

    ImGui::SameLine();
    std::string recId = "启用记录##hbRec_" + ctx->instanceId;
    if (ImGuiGBK::Checkbox(recId.c_str(), &ctx->enableHeartbeatRecording)) {
        SaveSingleHeartbeatConfig(ctx.get());
        needsRedraw = true;
    }

    ImGui::Separator();

    // 数据包列表
    ImGuiGBK::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "伪心跳数据包列表");
    ImGui::Spacing();

    std::string listId = "HbPacketList_" + ctx->instanceId;
    ImGui::BeginChild(listId.c_str(), ImVec2(0, 0), true);

    std::string tableId = "HbPacketsTable_" + ctx->instanceId;
    if (ImGui::BeginTable(tableId.c_str(), 5,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {

        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 50);
        ImGui::TableSetupColumn(ImGuiText::U("时间"), ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableSetupColumn(ImGuiText::U("游戏ID"), ImGuiTableColumnFlags_WidthFixed, 120);
        ImGui::TableSetupColumn(ImGuiText::U("大小"), ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn(ImGuiText::U("预览"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        std::lock_guard<std::mutex> lock(ctx->heartbeatPacketsMutex);
        for (const auto& pkt : ctx->heartbeatPackets) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("#%d", pkt.id);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%s", pkt.timestamp.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%s", pkt.gameID.c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%d B", pkt.size);
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%s", pkt.GetPreview().c_str());
        }

        ImGui::EndTable();
    }

    ImGui::EndChild();
}

} // anonymous namespace

// ==================== 公开接口实现 ====================

void SingleInstanceInterop::RenderCollectorInstanceUi(CollectorInstance* instance, bool& needsRedraw) {
    if (!instance) {
        ImGuiGBK::Text("实例不存在");
        return;
    }

    auto info = instance->GetInfo();
    auto* collector = instance->GetCollector();
    auto ctx = GetOrCreateSingleCollectorCtx(info.id);

    // 初始化端口
    if (!ctx->portInitializedFromInstance) {
        ctx->listenPort = info.port;
        snprintf(ctx->portBuffer, sizeof(ctx->portBuffer), "%d", info.port);
        ctx->portInitializedFromInstance = true;
    }

    // 获取可用区域大小
    ImVec2 contentSize = ImGui::GetContentRegionAvail();
    float sidebarWidth = 140.0f;

    // ========== 左侧子菜单栏 ==========
    std::string sidebarId = "CollectorSidebar_" + info.id;
    ImGui::BeginChild(sidebarId.c_str(), ImVec2(sidebarWidth, contentSize.y), true);

    ImVec2 btnSize(sidebarWidth - 16, 28);

    auto DrawMenuButton = [&](const char* label, int menuIndex) {
        bool isSelected = (ctx->collectorSubMenu == menuIndex);
        if (isSelected) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.8f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.6f, 0.9f, 1.0f));
        }

        std::string btnId = std::string(label) + "##coll_" + info.id;
        if (ImGuiGBK::Button(btnId.c_str(), btnSize)) {
            ctx->collectorSubMenu = menuIndex;
            needsRedraw = true;
        }

        if (isSelected) {
            ImGui::PopStyleColor(2);
        }
    };

    DrawMenuButton("主页", COLLECTOR_SUBMENU_MAIN);
    DrawMenuButton("存储模式", COLLECTOR_SUBMENU_STORAGE);
    DrawMenuButton("二级代理", COLLECTOR_SUBMENU_SECONDARY_PROXY);
    DrawMenuButton("包头类型", COLLECTOR_SUBMENU_PACKET_TYPE);
    DrawMenuButton("特殊ID", COLLECTOR_SUBMENU_SPECIAL_ID);
    DrawMenuButton("显示过滤", COLLECTOR_SUBMENU_DISPLAY_FILTER);
    DrawMenuButton("目标过滤", COLLECTOR_SUBMENU_TARGET_FILTER);
    DrawMenuButton("白名单", COLLECTOR_SUBMENU_WHITELIST);
    DrawMenuButton("黑名单", COLLECTOR_SUBMENU_BLACKLIST);
    DrawMenuButton("TCP共享", COLLECTOR_SUBMENU_TCP_SHARE);
    DrawMenuButton("SOCKS5账号", COLLECTOR_SUBMENU_SOCKS5_ACCOUNTS);

    ImGui::EndChild();
    ImGui::SameLine();

    // ========== 右侧内容区 ==========
    std::string contentId = "CollectorContent_" + info.id;
    ImGui::BeginChild(contentId.c_str(), ImVec2(0, contentSize.y), true);

    bool running = collector && collector->IsRunning();

    // 根据子菜单显示不同内容
    switch (ctx->collectorSubMenu) {
    case COLLECTOR_SUBMENU_MAIN:
        RenderCollectorMainPage(ctx, collector, instance, needsRedraw);
        break;
    default:
        RenderSingleCollectorSubMenuContent(ctx.get(), collector, instance, ctx->collectorSubMenu, needsRedraw);
        break;
    }

    ImGui::EndChild();

    // 渲染悬浮窗口
    RenderSingleCollectorFloatingWindows(ctx.get(), collector, instance, needsRedraw);
    RenderSingleCollectorLeftDetailWindow(ctx.get(), needsRedraw);
}

void SingleInstanceInterop::RenderHeartbeatInstanceUi(HeartbeatInstance* instance, bool& needsRedraw) {
    if (!instance) {
        ImGuiGBK::Text("实例不存在");
        return;
    }

    auto info = instance->GetInfo();
    auto* forwarder = instance->GetCollector();
    auto ctx = GetOrCreateSingleHeartbeatCtx(info.id);

    // 初始化端口
    if (!ctx->portInitializedFromInstance) {
        ctx->heartbeatPort = info.port;
        snprintf(ctx->heartbeatPortBuffer, sizeof(ctx->heartbeatPortBuffer), "%d", info.port);
        ctx->portInitializedFromInstance = true;
    }

    // 获取可用区域大小
    ImVec2 contentSize = ImGui::GetContentRegionAvail();
    float sidebarWidth = 140.0f;

    // ========== 左侧子菜单栏 ==========
    std::string sidebarId = "HeartbeatSidebar_" + info.id;
    ImGui::BeginChild(sidebarId.c_str(), ImVec2(sidebarWidth, contentSize.y), true);

    ImVec2 btnSize(sidebarWidth - 16, 28);

    auto DrawMenuButton = [&](const char* label, int menuIndex) {
        bool isSelected = (ctx->heartbeatSubMenu == menuIndex);
        if (isSelected) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.8f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.6f, 0.9f, 1.0f));
        }

        std::string btnId = std::string(label) + "##hb_" + info.id;
        if (ImGuiGBK::Button(btnId.c_str(), btnSize)) {
            ctx->heartbeatSubMenu = menuIndex;
            needsRedraw = true;
        }

        if (isSelected) {
            ImGui::PopStyleColor(2);
        }
    };

    DrawMenuButton("主页", HEARTBEAT_SUBMENU_MAIN);
    DrawMenuButton("替换模式", HEARTBEAT_SUBMENU_REPLACE_MODE);
    DrawMenuButton("特殊ID", HEARTBEAT_SUBMENU_SPECIAL_ID);
    DrawMenuButton("VTD滤镜", HEARTBEAT_SUBMENU_VTD_FILTER);
    DrawMenuButton("23偏移", HEARTBEAT_SUBMENU_PATTERN23);
    DrawMenuButton("09偏移", HEARTBEAT_SUBMENU_PATTERN09);
    DrawMenuButton("白名单", HEARTBEAT_SUBMENU_WHITELIST);
    DrawMenuButton("黑名单", HEARTBEAT_SUBMENU_BLACKLIST);
    DrawMenuButton("固定包", HEARTBEAT_SUBMENU_FIXED_PACKET);
    DrawMenuButton("二级代理", HEARTBEAT_SUBMENU_SECONDARY_PROXY);
    DrawMenuButton("转发控制", HEARTBEAT_SUBMENU_FORWARD_CONTROL);
    DrawMenuButton("包头类型", HEARTBEAT_SUBMENU_PACKET_TYPE);
    DrawMenuButton("数据池", HEARTBEAT_SUBMENU_DATA_POOL);
    DrawMenuButton("目标过滤", HEARTBEAT_SUBMENU_TARGET_FILTER);
    DrawMenuButton("SOCKS5账号", HEARTBEAT_SUBMENU_SOCKS5_ACCOUNTS);

    ImGui::EndChild();
    ImGui::SameLine();

    // ========== 右侧内容区 ==========
    std::string contentId = "HeartbeatContent_" + info.id;
    ImGui::BeginChild(contentId.c_str(), ImVec2(0, contentSize.y), true);

    // 根据子菜单显示不同内容
    switch (ctx->heartbeatSubMenu) {
    case HEARTBEAT_SUBMENU_MAIN:
        RenderHeartbeatMainPage(ctx, forwarder, instance, needsRedraw);
        break;
    default:
        RenderSingleHeartbeatSubMenuContent(ctx.get(), forwarder, instance, ctx->heartbeatSubMenu, needsRedraw);
        break;
    }

    ImGui::EndChild();

    // 渲染悬浮窗口
    RenderSingleHeartbeatFloatingWindows(ctx.get(), forwarder, instance, needsRedraw);
    RenderSingleHeartbeatLeftDetailWindow(ctx.get(), needsRedraw);
    RenderSingleHeartbeatRightDetailWindow(ctx.get(), needsRedraw);
}

// ==================== 采集器启动/停止 ====================
bool SingleInstanceInterop::StartCollector(std::unique_ptr<PacketCollector>& collector,
    CollectorInstance* instance, const std::string& instanceId, int port,
    DatabaseManager* db, ::StorageMode desiredMode) {

    auto ctx = GetOrCreateSingleCollectorCtx(instanceId);

    if (collector && collector->IsRunning()) {
        AB_LOG_WARNING("[单伪采集] 采集器已在运行: " + instanceId);
        return true;
    }

    collector = std::make_unique<PacketCollector>(port);

    // 设置数据包回调
    collector->SetPacketCallback([ctx, db](const PacketInfo& info,
        const std::vector<uint8_t>& forwardedData, const std::vector<uint8_t>& callbackData) {

        if (!ctx->enableRecording) return;

        DisplayPacket pkt;
        pkt.id = ctx->nextPacketId++;
        pkt.gameID = info.gameID;
        pkt.timestamp = info.timestamp;
        pkt.size = static_cast<int>(callbackData.size());
        pkt.rawData = callbackData;
        pkt.packetType = info.packetType;
        pkt.isComplete = info.isComplete;

        {
            std::lock_guard<std::mutex> lock(ctx->packetsMutex);
            ctx->packets.push_back(pkt);
            if (ctx->packets.size() > 1000) {
                ctx->packets.erase(ctx->packets.begin());
            }
        }
    });

    // ===== 同步该实例的SOCKS5账号 =====
    SyncCollectorSocks5Accounts(ctx.get(), collector.get());
    collector->SetSocks5Auth(ctx->enableSocksAuth);

    if (collector->Start()) {
        AB_LOG_INFO("[单伪采集] 启动成功: " + instanceId + " 端口: " + std::to_string(port));
        return true;
    }

    AB_LOG_ERROR("[单伪采集] 启动失败: " + instanceId);
    collector.reset();
    return false;
}

void SingleInstanceInterop::StopCollector(std::unique_ptr<PacketCollector>& collector) {
    if (collector) {
        // 从所有账号库取消注册，避免悬空指针
        auto allPools = InstanceManager::GetInstance().GetSocks5PoolInstances();
        for (auto* pool : allPools) {
            pool->UnregisterBoundCollector(collector.get());
        }
        collector->Stop();
        collector.reset();
    }
}

// ==================== 伪心跳转发器启动/停止 ====================
bool SingleInstanceInterop::StartHeartbeatForwarder(
    std::unique_ptr<PacketCollector>& forwarder,
    HeartbeatInstance* instance,
    const std::string& instanceId,
    int port,
    std::atomic<DatabaseManager*>* boundDbPtr,
    bool useBoundCollectorDb,
    HeartbeatCore* core) {

    auto ctx = GetOrCreateSingleHeartbeatCtx(instanceId);

    if (forwarder && forwarder->IsRunning()) {
        AB_LOG_WARNING("[单伪伪心跳] 转发器已在运行: " + instanceId);
        return true;
    }

    forwarder = std::make_unique<PacketCollector>(port);

    // 设置数据包回调
    forwarder->SetPacketCallback([ctx](const PacketInfo& info,
        const std::vector<uint8_t>& forwardedData, const std::vector<uint8_t>& callbackData) {

        if (!ctx->enableHeartbeatRecording) return;

        DisplayPacket pkt;
        pkt.id = ctx->nextHeartbeatPacketId++;
        pkt.gameID = info.gameID;
        pkt.timestamp = info.timestamp;
        pkt.size = static_cast<int>(callbackData.size());
        pkt.rawData = callbackData;
        pkt.packetType = info.packetType;
        pkt.isComplete = info.isComplete;

        {
            std::lock_guard<std::mutex> lock(ctx->heartbeatPacketsMutex);
            ctx->heartbeatPackets.push_back(pkt);
            if (ctx->heartbeatPackets.size() > 1000) {
                ctx->heartbeatPackets.erase(ctx->heartbeatPackets.begin());
            }
        }
    });

    // ===== 同步该实例的SOCKS5账号 =====
    SyncHeartbeatSocks5Accounts(ctx.get(), forwarder.get());
    forwarder->SetSocks5Auth(ctx->enableSocksAuth);

    if (forwarder->Start()) {
        AB_LOG_INFO("[单伪伪心跳] 启动成功: " + instanceId + " 端口: " + std::to_string(port));
        return true;
    }

    AB_LOG_ERROR("[单伪伪心跳] 启动失败: " + instanceId);
    forwarder.reset();
    return false;
}

void SingleInstanceInterop::StopHeartbeatForwarder(std::unique_ptr<PacketCollector>& forwarder) {
    if (forwarder) {
        // 从所有账号库取消注册，避免悬空指针
        auto allPools = InstanceManager::GetInstance().GetSocks5PoolInstances();
        for (auto* pool : allPools) {
            pool->UnregisterBoundCollector(forwarder.get());
        }
        forwarder->Stop();
        forwarder.reset();
    }
}

// ==================== 上下文销毁 ====================
void SingleInstanceInterop::DestroyCollectorContext(const std::string& instanceId) {
    std::lock_guard<std::mutex> lock(g_singleCtxMutex);
    g_singleCollectorCtxMap.erase(instanceId);
    g_singleCollectorPoolMap.erase(instanceId);
    AB_LOG_INFO("[单伪采集] 销毁上下文: " + instanceId);
}

void SingleInstanceInterop::DestroyHeartbeatContext(const std::string& instanceId) {
    std::lock_guard<std::mutex> lock(g_singleCtxMutex);
    g_singleHeartbeatCtxMap.erase(instanceId);
    AB_LOG_INFO("[单伪伪心跳] 销毁上下文: " + instanceId);
}

} // namespace SingleInstanceInterop
