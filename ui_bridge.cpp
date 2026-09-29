#define WIN32_LEAN_AND_MEAN
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commdlg.h>
#include <bcrypt.h>
#include <fstream>
#include "ui_bridge.h"
#include "webview2_host.h"
#include "InstanceManager.h"
#include "GlobalAntiCCCoordinator.h"
#include "Logger.h"
#include "WPEFilter.h"
#include "AntiCC.h"
#include "DatabaseManager.h"
#include "UserFilterManager.h"
#include "TinyAES.h"
#include "RemoteBrowserServer.h"
#include "DisconnectRuleTypes.h"
#include "res/json.hpp"
#include <thread>
#include <atomic>
#include <regex>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <unordered_map>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#include <chrono>
#include <iomanip>
#include <sstream>

using json = nlohmann::json;

// Message queue (thread-safe)
static std::mutex g_messageMutex;
static std::queue<std::string> g_messageQueue;
static std::atomic<bool> g_hasMessages(false);

// Background push thread
static std::atomic<bool> g_pushThreadRunning(false);
static std::thread g_pushThread;
static std::atomic<bool> g_backendAutoLoginTriggered(false);
static std::atomic<bool> g_logsRealtimeSubscribed(false);
static std::mutex g_statusCacheMutex;
static std::string g_lastStatusPayload;
static std::mutex g_logsRealtimeMutex;
static size_t g_logsRealtimeLastCount = 0;
static std::string g_logsRealtimeLastAnchor;
static bool g_logsRealtimeInitialized = false;
static std::mutex g_proxydataSubscriptionMutex;
static std::string g_proxydataSubscribedInstanceId;
static bool g_proxydataRealtimeEnabled = false;
static std::mutex g_proxydataSearchMutex;
static std::string g_proxydataSearchInstanceId;
static std::string g_proxydataSearchHex;
static std::vector<uint8_t> g_proxydataSearchPattern;

static std::filesystem::path UIBridge_GetModuleDirectoryLocal() {
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return std::filesystem::current_path();
    }
    return std::filesystem::path(path).parent_path();
}

// External references (will be set by main application)
extern HWND g_mainHwnd;
extern WPEFilter::FilterManager* g_wpeFilterManager;
extern AntiCC* g_antiCC;
extern DatabaseManager* g_database;

// WPE filter config persistence (defined in 鏂颁吉蹇冭烦.cpp)
extern bool SaveWPEFilterConfig(std::string* outError);

// WPE filter import pending data
static std::string g_wpeImportPendingPath;
static std::vector<uint8_t> g_wpeImportPendingBytes;
static bool g_wpeImportIsEncrypted = false;
static bool g_wpeImportNeedPassword = false;
static std::atomic<bool> g_wpeImmediateSyncRunning(false);
static std::atomic<bool> g_wpeImmediateSyncPending(false);

// Remote browser mode config
static std::mutex g_remoteBrowserConfigMutex;
static bool g_remoteBrowserConfigLoaded = false;
static RemoteBrowserServer::Config g_remoteBrowserConfig;
static std::string g_remoteBrowserLastError;

static SocksForwardInstance* UIBridge_GetSocksForwardInstanceById(const std::string& instanceId) {
    if (instanceId.empty()) {
        return nullptr;
    }
    return InstanceManager::GetInstance().GetSocksForwardInstance(instanceId);
}

static PacketCollector* UIBridge_GetSocksCollectorById(const std::string& instanceId) {
    auto* instance = UIBridge_GetSocksForwardInstanceById(instanceId);
    return instance ? instance->GetCollector() : nullptr;
}

static std::string UIBridge_TrimCopy(const std::string& text) {
    size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    if (begin >= text.size()) {
        return "";
    }
    size_t end = text.size() - 1;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end]))) {
        --end;
    }
    return text.substr(begin, end - begin + 1);
}

static std::vector<int> UIBridge_ReadGroupIds(const json& msg) {
    std::vector<int> groupIds;
    if (!msg.contains("groupIds") || !msg["groupIds"].is_array()) {
        return groupIds;
    }

    for (const auto& idVal : msg["groupIds"]) {
        try {
            if (idVal.is_number_integer()) {
                groupIds.push_back(idVal.get<int>());
            } else if (idVal.is_string()) {
                groupIds.push_back(std::stoi(idVal.get<std::string>()));
            }
        } catch (...) {}
    }

    return groupIds;
}

static bool UIBridge_ParseBoolValue(const std::string& text) {
    return text == "1" || text == "true" || text == "TRUE" || text == "True";
}

static bool UIBridge_IsSocksForwardInstanceJson(const json& obj) {
    if (!obj.is_object()) {
        return false;
    }
    if (!obj.contains("port")) {
        return false;
    }
    // 兼容旧/简化结构：只要带端口 + 下面任意一个关键字段，就认为是 SocksForward 实例
    if (obj.contains("name")) return true;
    if (obj.contains("threadPoolMode")) return true;
    if (obj.contains("enableSecondaryProxy")) return true;
    if (obj.contains("enableSocks5Auth")) return true;
    if (obj.contains("accountSourceInstanceId")) return true;
    if (obj.contains("enablePacketSplit")) return true;
    if (obj.contains("enableTrafficFilter")) return true;
    if (obj.contains("enableUserFilterMode")) return true;
    return false;
}

static bool UIBridge_TryParseJsonText(const std::string& text, json& outParsed) {
    try {
        outParsed = json::parse(text);
        return true;
    } catch (...) {
        return false;
    }
}

static bool UIBridge_FindSocksForwardInstancesArrayRecursive(
    const json& node,
    json& outInstances,
    int depth,
    const char* parentKey = nullptr) {
    if (depth <= 0) {
        return false;
    }

    if (node.is_array()) {
        // 如果字段名就是 instances，则即使为空数组也接受（后续会提示为空）
        if (parentKey && std::string(parentKey) == "instances") {
            outInstances = node;
            return true;
        }

        // 其它数组：用启发式判断是否像实例数组
        bool looksLike = false;
        for (const auto& item : node) {
            if (UIBridge_IsSocksForwardInstanceJson(item)) {
                looksLike = true;
                break;
            }
        }
        if (looksLike) {
            outInstances = node;
            return true;
        }

        for (const auto& item : node) {
            if (UIBridge_FindSocksForwardInstancesArrayRecursive(item, outInstances, depth - 1, nullptr)) {
                return true;
            }
        }
        return false;
    }

    if (node.is_object()) {
        // 优先找名为 instances 的字段
        auto it = node.find("instances");
        if (it != node.end()) {
            const json& v = it.value();
            if (v.is_array()) {
                outInstances = v;
                return true;
            }
            if (v.is_string()) {
                const std::string raw = UIBridge_TrimCopy(v.get<std::string>());
                if (!raw.empty() && raw.find("\"instances\"") != std::string::npos &&
                    (raw.front() == '{' || raw.front() == '[')) {
                    json parsed;
                    if (UIBridge_TryParseJsonText(raw, parsed) &&
                        UIBridge_FindSocksForwardInstancesArrayRecursive(parsed, outInstances, depth - 1, nullptr)) {
                        return true;
                    }
                }
            }
        }

        for (auto it2 = node.begin(); it2 != node.end(); ++it2) {
            const std::string& key = it2.key();
            const json& child = it2.value();
            if (UIBridge_FindSocksForwardInstancesArrayRecursive(child, outInstances, depth - 1, key.c_str())) {
                return true;
            }
        }
        return false;
    }

    if (node.is_string()) {
        // 兼容 “jsonObject/requestJson/payload”等字段把 JSON 当字符串保存的情况
        const std::string raw = UIBridge_TrimCopy(node.get<std::string>());
        if (!raw.empty() &&
            raw.find("\"instances\"") != std::string::npos &&
            (raw.front() == '{' || raw.front() == '[')) {
            json parsed;
            if (UIBridge_TryParseJsonText(raw, parsed) &&
                UIBridge_FindSocksForwardInstancesArrayRecursive(parsed, outInstances, depth - 1, nullptr)) {
                return true;
            }
        }
    }

    return false;
}

static std::string UIBridge_GenerateRemoteToken() {
    uint8_t bytes[16] = { 0 };
    if (BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        const uint64_t seed = GetTickCount64();
        for (size_t i = 0; i < sizeof(bytes); ++i) {
            bytes[i] = static_cast<uint8_t>((seed >> ((i % 8) * 8)) ^ (i * 31 + 17));
        }
    }

    static const char* hex = "0123456789abcdef";
    std::string token;
    token.reserve(sizeof(bytes) * 2);
    for (const uint8_t byte : bytes) {
        token.push_back(hex[(byte >> 4) & 0x0F]);
        token.push_back(hex[byte & 0x0F]);
    }
    return token;
}

static void UIBridge_SaveRemoteBrowserConfig(const RemoteBrowserServer::Config& config) {
    if (!g_database) {
        return;
    }
    g_database->SetConfigValue("remote_browser_enabled", config.enabled ? "1" : "0");
    g_database->SetConfigValue("remote_browser_bind_host", config.bindHost);
    g_database->SetConfigValue("remote_browser_port", std::to_string(config.port));
    g_database->SetConfigValue("remote_browser_access_token", config.accessToken);
}

static void UIBridge_EnsureRemoteBrowserConfigLoaded() {
    RemoteBrowserServer::Config loadedConfig;
    bool needLoad = false;
    const bool hasDatabase = (g_database != nullptr);
    {
        std::lock_guard<std::mutex> lock(g_remoteBrowserConfigMutex);
        needLoad = !g_remoteBrowserConfigLoaded;
    }
    if (!needLoad) {
        return;
    }

    loadedConfig.enabled = false;
    loadedConfig.bindHost = "0.0.0.0";
    loadedConfig.port = 18080;

    if (hasDatabase) {
        loadedConfig.enabled = UIBridge_ParseBoolValue(g_database->GetConfigValue("remote_browser_enabled", "0"));
        loadedConfig.bindHost = UIBridge_TrimCopy(g_database->GetConfigValue("remote_browser_bind_host", "0.0.0.0"));
        loadedConfig.port = (std::max)(1, (std::min)(65535, atoi(g_database->GetConfigValue("remote_browser_port", "18080").c_str())));
        loadedConfig.accessToken = UIBridge_TrimCopy(g_database->GetConfigValue("remote_browser_access_token", ""));
    }

    if (loadedConfig.bindHost.empty()) {
        loadedConfig.bindHost = "0.0.0.0";
    }
    if (loadedConfig.accessToken.empty()) {
        loadedConfig.accessToken = UIBridge_GenerateRemoteToken();
        if (hasDatabase) {
            g_database->SetConfigValue("remote_browser_access_token", loadedConfig.accessToken);
        }
    }

    std::string applyError;
    if (loadedConfig.enabled) {
        if (!RemoteBrowserServer::ApplyConfig(loadedConfig, applyError)) {
            loadedConfig.enabled = false;
            AB_LOG_ERROR("[RemoteBrowser] 自动启动失败: " + applyError);
            if (hasDatabase) {
                g_database->SetConfigValue("remote_browser_enabled", "0");
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_remoteBrowserConfigMutex);
        g_remoteBrowserConfigLoaded = hasDatabase;
        g_remoteBrowserConfig = loadedConfig;
        g_remoteBrowserLastError = applyError;
    }
}

static void UIBridge_PushRemoteBrowserConfig() {
    UIBridge_EnsureRemoteBrowserConfigLoaded();

    RemoteBrowserServer::Config config;
    std::string lastError;
    {
        std::lock_guard<std::mutex> lock(g_remoteBrowserConfigMutex);
        config = g_remoteBrowserConfig;
        lastError = g_remoteBrowserLastError;
    }

    json response;
    response["type"] = "remote_browser_config";
    response["enabled"] = config.enabled;
    response["running"] = RemoteBrowserServer::IsRunning();
    response["bindHost"] = config.bindHost;
    response["port"] = config.port;
    response["accessToken"] = config.accessToken;
    response["accessUrl"] = RemoteBrowserServer::BuildAccessUrl(config);
    if (!lastError.empty()) {
        response["error"] = lastError;
    }

    UIBridge_PushMessage(response.dump());
}

// CCProxy HTML parsing helper
static std::vector<std::string> UIBridge_ExtractInputValues(const std::string& html) {
    std::vector<std::string> values;
    std::regex inputRegex(R"(<input[^>]*\svalue=["']([^"']*)["'][^>]*>)", std::regex::icase);
    std::smatch matches;
    std::string::const_iterator searchStart(html.cbegin());
    while (std::regex_search(searchStart, html.cend(), matches, inputRegex)) {
        if (matches.size() >= 2) values.push_back(matches[1].str());
        searchStart = matches.suffix().first;
    }
    return values;
}

// Simple HTTP GET with Basic Auth (for CCProxy import)
static std::string UIBridge_HttpGet(const std::string& host, int port, const std::string& path,
    const std::string& user, const std::string& pwd) {
    std::string result;
    int whl = MultiByteToWideChar(CP_UTF8, 0, host.c_str(), -1, NULL, 0);
    std::vector<wchar_t> wh(whl);
    MultiByteToWideChar(CP_UTF8, 0, host.c_str(), -1, wh.data(), whl);
    int wpl = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, NULL, 0);
    std::vector<wchar_t> wp(wpl);
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wp.data(), wpl);

    HINTERNET hSession = WinHttpOpen(L"UIBridge/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) return result;
    HINTERNET hConnect = WinHttpConnect(hSession, wh.data(), port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return result; }
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", wp.data(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return result; }

    if (!user.empty()) {
        std::string cred = user + ":" + pwd;
        // Base64 encode for Basic Auth
        static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string b64str;
        for (size_t i = 0; i < cred.size(); i += 3) {
            uint32_t n = ((uint8_t)cred[i]) << 16;
            if (i + 1 < cred.size()) n |= ((uint8_t)cred[i + 1]) << 8;
            if (i + 2 < cred.size()) n |= (uint8_t)cred[i + 2];
            b64str += b64[(n >> 18) & 0x3F]; b64str += b64[(n >> 12) & 0x3F];
            b64str += (i + 1 < cred.size()) ? b64[(n >> 6) & 0x3F] : '=';
            b64str += (i + 2 < cred.size()) ? b64[n & 0x3F] : '=';
        }
        std::string authHeader = "Authorization: Basic " + b64str + "\r\n";
        int wahl = MultiByteToWideChar(CP_UTF8, 0, authHeader.c_str(), -1, NULL, 0);
        std::vector<wchar_t> wah(wahl);
        MultiByteToWideChar(CP_UTF8, 0, authHeader.c_str(), -1, wah.data(), wahl);
        WinHttpAddRequestHeaders(hRequest, wah.data(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
    }

    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, NULL, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, NULL)) {
        DWORD bytesAvail = 0;
        while (WinHttpQueryDataAvailable(hRequest, &bytesAvail) && bytesAvail > 0) {
            std::vector<char> buf(bytesAvail + 1, 0);
            DWORD bytesRead = 0;
            WinHttpReadData(hRequest, buf.data(), bytesAvail, &bytesRead);
            result.append(buf.data(), bytesRead);
        }
    }
    WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
    return result;
}

// SHA256 helper for WPE filter encryption
static bool WPE_SHA256(const uint8_t* data, size_t len, uint8_t out[32]) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    DWORD cbHashObject = 0, cbHash = 0, cbData = 0;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return false;
    if (BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&cbHashObject, sizeof(cbHashObject), &cbData, 0) != 0 || cbHashObject == 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0); return false;
    }
    if (BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH, (PUCHAR)&cbHash, sizeof(cbHash), &cbData, 0) != 0 || cbHash != 32) {
        BCryptCloseAlgorithmProvider(hAlg, 0); return false;
    }
    std::vector<uint8_t> hashObject(cbHashObject);
    if (BCryptCreateHash(hAlg, &hHash, hashObject.data(), cbHashObject, nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0); return false;
    }
    if (BCryptHashData(hHash, (PUCHAR)data, (ULONG)len, 0) != 0) {
        BCryptDestroyHash(hHash); BCryptCloseAlgorithmProvider(hAlg, 0); return false;
    }
    const NTSTATUS st = BCryptFinishHash(hHash, (PUCHAR)out, 32, 0);
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return st == 0;
}

// AES file header structure
#pragma pack(push, 1)
struct WpeAesFileHeader {
    uint8_t magic[8];
    uint32_t version;
    uint8_t flags;
    uint8_t reserved[3];
    uint8_t salt[16];
    uint8_t iv[16];
    uint32_t plainLen;
    uint32_t cipherLen;
};
#pragma pack(pop)
extern bool g_isLoggedIn;
extern std::string g_cloudUsername;
extern std::string g_expireTime;
extern int g_remainingDays;

void UIBridge_Init() {
    // Setup message handler
    webview2_setup_message_handler(UIBridge_HandleMessage);
    RemoteBrowserServer::SetActionHandler(UIBridge_HandleMessage);
}

static bool UIBridge_ShouldPushCachedPayload(std::string& cache, const std::string& payload, bool force) {
    std::lock_guard<std::mutex> lock(g_statusCacheMutex);
    if (!force && cache == payload) {
        return false;
    }
    cache = payload;
    return true;
}

static std::string UIBridge_BuildFullHexString(const std::vector<uint8_t>& data) {
    std::ostringstream fullHex;
    for (size_t i = 0; i < data.size(); ++i) {
        fullHex << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]);
        if (i + 1 < data.size()) {
            fullHex << " ";
        }
    }
    return fullHex.str();
}

static std::string UIBridge_BuildProxyPacketSummaryKey(const ProxyPacketRecordSummary& record) {
    return record.timestamp + "|" + record.username + "|" +
           (record.isRequest ? "1" : "0") + "|" +
           std::to_string(record.dataLength) + "|" + record.dataPreview;
}

static const char* UIBridge_LogCategoryToUiName(LogCategory cat) {
    switch (cat) {
    case LOG_CAT_ALL: return "全部日志";
    case LOG_CAT_HEARTBEAT_LOAD: return "心跳加载";
    case LOG_CAT_HEARTBEAT_CLEANUP: return "心跳清理";
    case LOG_CAT_HEARTBEAT_REPLACE: return "伪心跳替换";
    case LOG_CAT_LOGIN_AUTH: return "登录验证";
    case LOG_CAT_ANTICC: return "防CC";
    case LOG_CAT_COLLECTOR: return "采集日志";
    case LOG_CAT_API: return "API日志";
    case LOG_CAT_SOCKS_ACCOUNT: return "SOCKS账号日志";
    default: return "系统";
    }
}

static json UIBridge_BuildLogJsonLocal(const LogEntry& log) {
    json logJson;
    logJson["timestamp"] = log.timestamp;
    logJson["level"] = (log.level == LOG_INFO ? "INFO" :
                       log.level == LOG_WARNING ? "WARNING" : "ERROR");
    logJson["category"] = UIBridge_LogCategoryToUiName(log.category);
    logJson["message"] = Logger::NormalizeTextForDisplay(log.message);
    return logJson;
}

static std::string UIBridge_BuildLogAnchorLocal(const LogEntry& log) {
    return log.timestamp + "|" + std::to_string(static_cast<int>(log.level)) + "|" +
           std::to_string(static_cast<int>(log.category)) + "|" + log.message;
}

static void UIBridge_ResetRealtimeLogsStateLocal() {
    std::lock_guard<std::mutex> lock(g_logsRealtimeMutex);
    g_logsRealtimeLastCount = 0;
    g_logsRealtimeLastAnchor.clear();
    g_logsRealtimeInitialized = false;
}

static void UIBridge_PushLogsSnapshotLocal(const std::vector<LogEntry>& logs) {
    json response;
    response["type"] = "logs";
    response["logs"] = json::array();
    for (const auto& log : logs) {
        response["logs"].push_back(UIBridge_BuildLogJsonLocal(log));
    }
    UIBridge_PushMessage(response.dump());
}

static void UIBridge_PushLogsAppendLocal(const std::vector<LogEntry>& logs, size_t startIndex) {
    if (startIndex >= logs.size()) {
        return;
    }

    json response;
    response["type"] = "logs_append";
    response["logs"] = json::array();
    for (size_t i = startIndex; i < logs.size(); ++i) {
        response["logs"].push_back(UIBridge_BuildLogJsonLocal(logs[i]));
    }
    UIBridge_PushMessage(response.dump());
}

static void UIBridge_UpdateRealtimeLogsStateLocked(const std::vector<LogEntry>& logs) {
    g_logsRealtimeLastCount = logs.size();
    g_logsRealtimeLastAnchor = logs.empty() ? std::string() : UIBridge_BuildLogAnchorLocal(logs.back());
    g_logsRealtimeInitialized = true;
}

static void UIBridge_PushRealtimeLogsDeltaIfNeededLocal() {
    if (!g_logsRealtimeSubscribed.load(std::memory_order_acquire)) {
        return;
    }

    const auto logs = Logger::GetLogs();

    std::lock_guard<std::mutex> lock(g_logsRealtimeMutex);
    if (!g_logsRealtimeSubscribed.load(std::memory_order_acquire)) {
        return;
    }

    if (!g_logsRealtimeInitialized) {
        UIBridge_UpdateRealtimeLogsStateLocked(logs);
        return;
    }

    if (logs.empty()) {
        if (g_logsRealtimeLastCount != 0 || !g_logsRealtimeLastAnchor.empty()) {
            UIBridge_PushLogsSnapshotLocal(logs);
            UIBridge_UpdateRealtimeLogsStateLocked(logs);
        }
        return;
    }

    if (g_logsRealtimeLastCount == 0) {
        UIBridge_PushLogsAppendLocal(logs, 0);
        UIBridge_UpdateRealtimeLogsStateLocked(logs);
        return;
    }

    if (logs.size() > g_logsRealtimeLastCount &&
        g_logsRealtimeLastCount <= logs.size() &&
        UIBridge_BuildLogAnchorLocal(logs[g_logsRealtimeLastCount - 1]) == g_logsRealtimeLastAnchor) {
        UIBridge_PushLogsAppendLocal(logs, g_logsRealtimeLastCount);
        UIBridge_UpdateRealtimeLogsStateLocked(logs);
        return;
    }

    if (logs.size() == g_logsRealtimeLastCount &&
        UIBridge_BuildLogAnchorLocal(logs.back()) == g_logsRealtimeLastAnchor) {
        return;
    }

    UIBridge_PushLogsSnapshotLocal(logs);
    UIBridge_UpdateRealtimeLogsStateLocked(logs);
}

void UIBridge_PushMessage(const std::string& jsonMessage) {
    {
        std::lock_guard<std::mutex> lock(g_messageMutex);
        g_messageQueue.push(jsonMessage);
    }
    g_hasMessages.store(true, std::memory_order_release);
    RemoteBrowserServer::PushMessage(jsonMessage);
}

void UIBridge_FlushMessages() {
    if (!g_hasMessages.load(std::memory_order_acquire)) {
        return;
    }

    if (!webview2_is_ready()) {
        return;
    }

    std::queue<std::string> localQueue;
    {
        std::lock_guard<std::mutex> lock(g_messageMutex);
        std::swap(localQueue, g_messageQueue);
    }

    g_hasMessages.store(!localQueue.empty(), std::memory_order_release);

    if (localQueue.empty()) {
        return;
    }

    std::ostringstream batch;
    batch << "[";

    bool first = true;
    while (!localQueue.empty()) {
        if (!first) {
            batch << ",";
        }
        batch << localQueue.front();
        localQueue.pop();
        first = false;
    }

    batch << "]";
    webview2_post_message(batch.str().c_str());
}

void UIBridge_Toast(const std::string& type, const std::string& title, const std::string& message) {
    json msg;
    msg["type"] = "message";
    msg["messageType"] = type;
    msg["title"] = Logger::NormalizeTextForDisplay(title);
    msg["message"] = Logger::NormalizeTextForDisplay(message);
    UIBridge_PushMessage(msg.dump());
}

bool UIBridge_ShouldPushProxyPacket(const std::string& instanceId) {
    std::lock_guard<std::mutex> lock(g_proxydataSubscriptionMutex);
    return g_proxydataRealtimeEnabled && !instanceId.empty() && instanceId == g_proxydataSubscribedInstanceId;
}

static bool UIBridge_ParseHexSearchQuery(const std::string& text, std::vector<uint8_t>& outBytes, std::string& outError) {
    outBytes.clear();
    outError.clear();

    std::string normalized;
    normalized.reserve(text.size());
    for (char ch : text) {
        if (std::isxdigit(static_cast<unsigned char>(ch))) {
            normalized.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
        }
    }

    if (normalized.empty()) {
        return true;
    }

    if ((normalized.size() % 2) != 0) {
        outError = "Hex 搜索必须是完整字节，长度需为偶数";
        return false;
    }

    for (size_t i = 0; i < normalized.size(); i += 2) {
        const std::string byteText = normalized.substr(i, 2);
        char* end = nullptr;
        unsigned long value = std::strtoul(byteText.c_str(), &end, 16);
        if (end != byteText.c_str() + 2 || value > 0xFF) {
            outError = "Hex 搜索包含无效字节: " + byteText;
            return false;
        }
        outBytes.push_back(static_cast<uint8_t>(value));
    }

    return true;
}

static bool UIBridge_ProxyPacketMatchesSearch(const std::vector<uint8_t>& data) {
    std::lock_guard<std::mutex> lock(g_proxydataSearchMutex);
    if (g_proxydataSearchPattern.empty()) {
        return true;
    }
    if (data.size() < g_proxydataSearchPattern.size()) {
        return false;
    }
    return std::search(data.begin(), data.end(), g_proxydataSearchPattern.begin(), g_proxydataSearchPattern.end()) != data.end();
}

bool UIBridge_ShouldPushProxyPacketData(const std::string& instanceId, const std::vector<uint8_t>& data) {
    if (!UIBridge_ShouldPushProxyPacket(instanceId)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_proxydataSearchMutex);
    if (g_proxydataSearchPattern.empty() || g_proxydataSearchInstanceId != instanceId) {
        return true;
    }
    if (data.size() < g_proxydataSearchPattern.size()) {
        return false;
    }
    return std::search(data.begin(), data.end(), g_proxydataSearchPattern.begin(), g_proxydataSearchPattern.end()) != data.end();
}

void UIBridge_SetProxydataSubscription(const std::string& instanceId, bool enabled) {
    std::lock_guard<std::mutex> lock(g_proxydataSubscriptionMutex);
    g_proxydataRealtimeEnabled = enabled;
    g_proxydataSubscribedInstanceId = enabled ? instanceId : std::string();
    AB_LOG_INFO_CAT(
        LOG_CAT_COLLECTOR,
        "[代理数据] 实时订阅已" + std::string(enabled ? "开启" : "关闭") +
        ": instance=" + (instanceId.empty() ? std::string("<empty>") : instanceId));
    if (!enabled) {
        std::lock_guard<std::mutex> searchLock(g_proxydataSearchMutex);
        g_proxydataSearchInstanceId.clear();
        g_proxydataSearchHex.clear();
        g_proxydataSearchPattern.clear();
    }
}

static std::string UIBridge_AnsiToUtf8Path(const std::string& text) {
    if (text.empty()) {
        return std::string();
    }

    int wideLen = MultiByteToWideChar(CP_ACP, 0, text.c_str(), -1, nullptr, 0);
    if (wideLen <= 0) {
        return Logger::NormalizeTextForDisplay(text);
    }

    std::wstring wide(static_cast<size_t>(wideLen), L'\0');
    if (MultiByteToWideChar(CP_ACP, 0, text.c_str(), -1, wide.data(), wideLen) <= 0) {
        return Logger::NormalizeTextForDisplay(text);
    }

    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (utf8Len <= 0) {
        return Logger::NormalizeTextForDisplay(text);
    }

    std::string utf8(static_cast<size_t>(utf8Len), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, utf8.data(), utf8Len, nullptr, nullptr) <= 0) {
        return Logger::NormalizeTextForDisplay(text);
    }

    if (!utf8.empty() && utf8.back() == '\0') {
        utf8.pop_back();
    }
    return utf8;
}

struct UIBridge_AccountPrefsLocal {
    bool rememberPassword = false;
    bool autoLogin = false;
    std::string loginMode = "account";
    std::string username;
    std::string password;
};

static std::wstring UIBridge_Utf8ToWide(const std::string& text) {
    if (text.empty()) {
        return std::wstring();
    }

    const int wideLen = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (wideLen <= 0) {
        return std::wstring();
    }

    std::wstring wide(static_cast<size_t>(wideLen), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), wideLen) <= 0) {
        return std::wstring();
    }
    if (!wide.empty() && wide.back() == L'\0') {
        wide.pop_back();
    }
    return wide;
}

static std::string UIBridge_WideToUtf8(const std::wstring& text) {
    if (text.empty()) {
        return std::string();
    }

    const int utf8Len = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (utf8Len <= 0) {
        return std::string();
    }

    std::string utf8(static_cast<size_t>(utf8Len), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, utf8.data(), utf8Len, nullptr, nullptr) <= 0) {
        return std::string();
    }
    if (!utf8.empty() && utf8.back() == '\0') {
        utf8.pop_back();
    }
    return utf8;
}

static std::string UIBridge_AnsiToUtf8TextLocal(const std::string& text) {
    if (text.empty()) {
        return std::string();
    }

    const int wideLen = MultiByteToWideChar(CP_ACP, 0, text.c_str(), -1, nullptr, 0);
    if (wideLen <= 0) {
        return text;
    }

    std::wstring wide(static_cast<size_t>(wideLen), L'\0');
    if (MultiByteToWideChar(CP_ACP, 0, text.c_str(), -1, wide.data(), wideLen) <= 0) {
        return text;
    }

    return UIBridge_WideToUtf8(wide.c_str());
}

static bool UIBridge_IsValidUtf8Local(const std::string& text) {
    if (text.empty()) {
        return true;
    }
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0) > 0;
}

static std::string UIBridge_TrimAsciiCopy(const std::string& text) {
    size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return text.substr(begin, end - begin);
}

static std::string UIBridge_ToLowerAsciiCopy(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}

static std::string UIBridge_UnescapeIniValueLocal(const std::string& text) {
    std::string result;
    result.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (ch == '\\' && i + 1 < text.size()) {
            const char next = text[i + 1];
            if (next == 'n') {
                result.push_back('\n');
                ++i;
                continue;
            }
            if (next == 'r') {
                result.push_back('\r');
                ++i;
                continue;
            }
            if (next == '\\') {
                result.push_back('\\');
                ++i;
                continue;
            }
        }
        result.push_back(ch);
    }
    return result;
}

static std::string UIBridge_EscapeIniValueLocal(const std::string& text) {
    std::string result;
    result.reserve(text.size());
    for (const char ch : text) {
        if (ch == '\\') {
            result += "\\\\";
        } else if (ch == '\n') {
            result += "\\n";
        } else if (ch == '\r') {
            result += "\\r";
        } else {
            result.push_back(ch);
        }
    }
    return result;
}

static std::filesystem::path UIBridge_GetAccountIniPathLocal() {
    wchar_t modulePath[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return std::filesystem::current_path() / "account.ini";
    }
    return std::filesystem::path(modulePath).parent_path() / "account.ini";
}

static bool UIBridge_ReadTextFileUtf8Local(const std::filesystem::path& path, std::string& outText) {
    outText.clear();
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return false;
    }

    std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (bytes.size() >= 2 &&
        static_cast<unsigned char>(bytes[0]) == 0xFF &&
        static_cast<unsigned char>(bytes[1]) == 0xFE) {
        const wchar_t* widePtr = reinterpret_cast<const wchar_t*>(bytes.data() + 2);
        const size_t wideLen = (bytes.size() - 2) / sizeof(wchar_t);
        outText = UIBridge_WideToUtf8(std::wstring(widePtr, widePtr + wideLen));
        return true;
    }

    if (bytes.size() >= 3 &&
        static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF) {
        bytes.erase(0, 3);
    }

    outText = UIBridge_IsValidUtf8Local(bytes)
        ? bytes
        : UIBridge_AnsiToUtf8TextLocal(bytes);
    return true;
}

static std::unordered_map<std::string, std::string> UIBridge_ParseIniMapLocal(const std::string& text) {
    std::unordered_map<std::string, std::string> values;
    std::istringstream input(text);
    std::string currentSection;
    std::string line;

    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        const std::string trimmed = UIBridge_TrimAsciiCopy(line);
        if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#') {
            continue;
        }

        if (trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']') {
            currentSection = UIBridge_ToLowerAsciiCopy(UIBridge_TrimAsciiCopy(trimmed.substr(1, trimmed.size() - 2)));
            continue;
        }

        const size_t equalsPos = trimmed.find('=');
        if (equalsPos == std::string::npos) {
            continue;
        }

        const std::string key = UIBridge_ToLowerAsciiCopy(UIBridge_TrimAsciiCopy(trimmed.substr(0, equalsPos)));
        const std::string rawValue = trimmed.substr(equalsPos + 1);
        values[currentSection + ":" + key] = UIBridge_UnescapeIniValueLocal(rawValue);
    }

    return values;
}

static std::string UIBridge_GetIniValueLocal(
    const std::unordered_map<std::string, std::string>& values,
    std::initializer_list<const char*> sections,
    std::initializer_list<const char*> keys) {
    for (const char* section : sections) {
        const std::string sectionKey = section ? UIBridge_ToLowerAsciiCopy(section) : std::string();
        for (const char* key : keys) {
            const std::string compositeKey = sectionKey + ":" + UIBridge_ToLowerAsciiCopy(key ? key : "");
            const auto it = values.find(compositeKey);
            if (it != values.end()) {
                return it->second;
            }
        }
    }
    return std::string();
}

static UIBridge_AccountPrefsLocal UIBridge_LoadAccountPrefsLocal() {
    UIBridge_AccountPrefsLocal prefs;
    const auto iniPath = UIBridge_GetAccountIniPathLocal();
    std::string text;
    if (!UIBridge_ReadTextFileUtf8Local(iniPath, text)) {
        AB_LOG_WARNING_CAT(
            LOG_CAT_LOGIN_AUTH,
            "[登录] 未找到 account.ini 或读取失败: " + UIBridge_AnsiToUtf8Path(iniPath.string()));
        return prefs;
    }

    const auto values = UIBridge_ParseIniMapLocal(text);
    const std::string rememberValue = UIBridge_GetIniValueLocal(
        values,
        { "account", "login", "" },
        { "remember_password", "rememberpassword", "remember" });
    const std::string autoLoginValue = UIBridge_GetIniValueLocal(
        values,
        { "account", "login", "" },
        { "auto_login", "autologin" });
    prefs.loginMode = UIBridge_GetIniValueLocal(
        values,
        { "account", "login", "" },
        { "login_mode", "loginmode", "mode" });
    if (prefs.loginMode != "card") {
        prefs.loginMode = "account";
    }
    prefs.username = UIBridge_GetIniValueLocal(
        values,
        { "account", "login", "" },
        { "username", "saved_username", "savedusername", "account", "user" });
    prefs.password = UIBridge_GetIniValueLocal(
        values,
        { "account", "login", "" },
        { "password", "saved_password", "savedpassword", "pass" });
    prefs.rememberPassword = (rememberValue == "1" || rememberValue == "true" || rememberValue == "TRUE");
    prefs.autoLogin = (autoLoginValue == "1" || autoLoginValue == "true" || autoLoginValue == "TRUE");
    AB_LOG_INFO_CAT(
        LOG_CAT_LOGIN_AUTH,
        "[登录] 已读取 account.ini: path=" + UIBridge_AnsiToUtf8Path(iniPath.string()) +
        ", mode=" + prefs.loginMode +
        ", remember=" + std::string(prefs.rememberPassword ? "1" : "0") +
        ", autoLogin=" + std::string(prefs.autoLogin ? "1" : "0") +
        ", usernameLen=" + std::to_string(prefs.username.size()) +
        ", passwordLen=" + std::to_string(prefs.password.size()));
    return prefs;
}

static bool UIBridge_SaveAccountPrefsLocal(
    const std::string& loginMode,
    const std::string& username,
    const std::string& password,
    bool rememberPassword,
    bool autoLogin) {
    const bool remember = rememberPassword || autoLogin;
    const auto iniPath = UIBridge_GetAccountIniPathLocal();
    std::error_code ec;
    const auto parent = iniPath.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }

    std::ofstream output(iniPath, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        AB_LOG_WARNING_CAT(
            LOG_CAT_LOGIN_AUTH,
            "[登录] 无法写入 account.ini: " + UIBridge_AnsiToUtf8Path(iniPath.string()));
        return false;
    }

    const std::string fileContent =
        "\xEF\xBB\xBF[account]\r\n"
        "login_mode=" + UIBridge_EscapeIniValueLocal(loginMode == "card" ? "card" : "account") + "\r\n"
        "remember_password=" + std::string(remember ? "1" : "0") + "\r\n"
        "auto_login=" + std::string(autoLogin ? "1" : "0") + "\r\n"
        "username=" + UIBridge_EscapeIniValueLocal(remember ? username : std::string()) + "\r\n"
        "password=" + UIBridge_EscapeIniValueLocal(remember ? password : std::string()) + "\r\n";
    output.write(fileContent.data(), static_cast<std::streamsize>(fileContent.size()));
    output.flush();
    const bool ok = output.good();
    AB_LOG_INFO_CAT(
        LOG_CAT_LOGIN_AUTH,
        "[登录] account.ini 写入" + std::string(ok ? "成功" : "失败") +
        ": path=" + UIBridge_AnsiToUtf8Path(iniPath.string()) +
        ", mode=" + (loginMode == "card" ? "card" : "account") +
        ", remember=" + std::string(remember ? "1" : "0") +
        ", autoLogin=" + std::string(autoLogin ? "1" : "0") +
        ", usernameLen=" + std::to_string((remember ? username : std::string()).size()) +
        ", passwordLen=" + std::to_string((remember ? password : std::string()).size()));
    return ok;
}

static void UIBridge_TriggerBackendAutoLoginOnceLocal(const UIBridge_AccountPrefsLocal& prefs) {
    return;
}

static bool UIBridge_ValidateWpeImportJson(const std::string& jsonText, std::string& outError) {
    outError.clear();

    try {
        json parsed = json::parse(jsonText);
        if (!parsed.is_object()) {
            outError = "JSON 根节点必须是对象";
            return false;
        }

        if (!parsed.contains("filters") || !parsed["filters"].is_array()) {
            outError = "缺少 filters 数组";
            return false;
        }

        return true;
    } catch (const json::exception& e) {
        outError = e.what();
        return false;
    } catch (const std::exception& e) {
        outError = e.what();
        return false;
    } catch (...) {
        outError = "未知 JSON 异常";
        return false;
    }
}

static bool UIBridge_SaveWPEConfigWithFeedback(bool showSuccessToast,
                                               const std::string& successTitle,
                                               const std::string& successMessage) {
    std::string persistError;
    if (SaveWPEFilterConfig(&persistError)) {
        if (showSuccessToast) {
            UIBridge_Toast("success", successTitle, successMessage);
        }
        return true;
    }

    std::string warningMessage = "当前修改已应用到本次运行，但保存到本地失败，重启后会丢失";
    if (!persistError.empty()) {
        warningMessage += "：" + persistError;
    }
    UIBridge_Toast("warning", "保存未落库", warningMessage);
    return false;
}

static void UIBridge_RequestImmediateWpeCloudSync(const char* reason) {
    // Cloud sync disabled
}

void UIBridge_PushStatus(bool force) {
    json msg;
    msg["type"] = "status";
    msg["loggedIn"] = true;
    msg["username"] = g_cloudUsername;
    msg["loginType"] = "";
    msg["expireTime"] = g_expireTime;
    msg["remainingDays"] = g_remainingDays;

    auto& instMgr = InstanceManager::GetInstance();
    auto instances = instMgr.GetAllInstances();
    int runningCount = 0;
    for (const auto& inst : instances) {
        if (inst.state == InstanceState::Running) runningCount++;
    }
    msg["runningInstances"] = runningCount;
    msg["totalInstances"] = static_cast<int>(instances.size());

    const std::string payload = msg.dump();
    if (!UIBridge_ShouldPushCachedPayload(g_lastStatusPayload, payload, force)) {
        return;
    }

    UIBridge_PushMessage(payload);
}

void UIBridge_PushInstances(bool force) {
    (void)force;
    auto& instMgr = InstanceManager::GetInstance();
    auto instances = instMgr.GetAllInstances();

    json msg;
    msg["type"] = "instances";
    msg["instances"] = json::array();

    for (const auto& inst : instances) {
        json instJson;
        instJson["id"] = inst.id;
        instJson["name"] = inst.name;

        switch (inst.type) {
            case InstanceType::Collector: instJson["type"] = "Collector"; break;
            case InstanceType::Heartbeat: instJson["type"] = "Heartbeat"; break;
            case InstanceType::AbCollector: instJson["type"] = "AbCollector"; break;
            case InstanceType::AbHeartbeat: instJson["type"] = "AbHeartbeat"; break;
            case InstanceType::Socks5Pool: instJson["type"] = "Socks5Pool"; break;
            case InstanceType::SocksForward: instJson["type"] = "SocksForward"; break;
        }

        switch (inst.state) {
            case InstanceState::Stopped: instJson["state"] = "Stopped"; break;
            case InstanceState::Starting: instJson["state"] = "Starting"; break;
            case InstanceState::Running: instJson["state"] = "Running"; break;
            case InstanceState::Stopping: instJson["state"] = "Stopping"; break;
            case InstanceState::Error: instJson["state"] = "Error"; break;
        }

        instJson["port"] = inst.port;
        instJson["currentConnections"] = inst.currentConnections;
        instJson["totalPackets"] = inst.totalPackets;
        instJson["totalBytes"] = inst.totalBytes;

        msg["instances"].push_back(instJson);
    }

    UIBridge_PushMessage(msg.dump());
}

// Handle incoming messages from frontend
void UIBridge_HandleMessage(const char* jsonMessage) {
    try {
        json msg = json::parse(jsonMessage);
        std::string action = msg.value("action", "");

        if (action.empty()) return;

        // ========== Window Close ==========
        if (action == "window_close") {
            AB_LOG_INFO("[UI Bridge] 收到关闭窗口请求");
            if (g_mainHwnd && IsWindow(g_mainHwnd)) {
                PostMessage(g_mainHwnd, WM_CLOSE, 0, 0);
            }
            return;
        }

        // ========== Cloud Login/Logout ==========
        if (action == "cloud_login" || action == "trial_login") {
            // 云验证与卡密授权已移除：本地模式直接视为已登录
            const bool isTrialLogin = (action == "trial_login");
            const std::string loginType = msg.value("loginType", "account");
            const bool isCardLogin = (!isTrialLogin && loginType == "card");
            std::string username = isCardLogin ? msg.value("card", "") : msg.value("username", "");
            if (username.empty()) {
                username = "local";
            }

            g_isLoggedIn = true;
            g_cloudUsername = username;
            g_expireTime.clear();
            g_remainingDays = 0;

            json response;
            response["type"] = "cloud_login_result";
            response["success"] = true;
            response["loginType"] = isCardLogin ? "card" : "account";
            response["isTrial"] = isTrialLogin;
            response["username"] = username;
            response["expireTime"] = "";
            response["remainingDays"] = 0;
            UIBridge_PushMessage(response.dump());
            UIBridge_PushStatus(true);

        }
        else if (action == "ui_sync_state") {
            json response;
            response["type"] = "ui_sync_state";

            if (msg.contains("mainPage") && msg["mainPage"].is_string()) {
                response["mainPage"] = msg.value("mainPage", "");
            }
            if (msg.contains("loginTab") && msg["loginTab"].is_string()) {
                response["loginTab"] = msg.value("loginTab", "");
            }
            if (msg.contains("configMenu") && msg["configMenu"].is_string()) {
                response["configMenu"] = msg.value("configMenu", "");
            }

            UIBridge_PushMessage(response.dump());
        }

        // ========== App Config ==========
        else if (action == "get_app_config") {
            json response;
            response["type"] = "app_config";
            response["fastMode"] = !Logger::IsEnabled();
            response["enableLogging"] = Logger::IsEnabled();
            const auto accountPrefs = UIBridge_LoadAccountPrefsLocal();
            response["savedLoginMode"] = accountPrefs.loginMode;
            response["rememberPassword"] = accountPrefs.rememberPassword || accountPrefs.autoLogin;
            response["autoLogin"] = accountPrefs.autoLogin;
            response["savedUsername"] = (accountPrefs.rememberPassword || accountPrefs.autoLogin)
                ? accountPrefs.username
                : std::string("");
            response["savedPassword"] = (accountPrefs.rememberPassword || accountPrefs.autoLogin)
                ? accountPrefs.password
                : std::string("");

            if (g_database) {
                AppConfig cfg = g_database->LoadAppConfig();
                response["autoScrollLog"] = cfg.autoScrollLog;
            } else {
                response["autoScrollLog"] = true;
            }

            UIBridge_PushMessage(response.dump());
            UIBridge_TriggerBackendAutoLoginOnceLocal(accountPrefs);
        }
        else if (action == "set_fast_mode") {
            const bool fastMode = msg.value("enabled", false);
            const bool enableLogging = !fastMode;
            const bool previousLogging = Logger::IsEnabled();

            if (previousLogging && !enableLogging) {
                AB_LOG_WARNING("[系统] 已开启极速模式，新的运行日志将停止记录");
            }

            Logger::SetEnabled(enableLogging);

            if (!previousLogging && enableLogging) {
                AB_LOG_INFO("[系统] 已关闭极速模式，日志输出已恢复");
            }

            // Best-effort persistence: LoadAppConfig() reads this key; do not require cloud permit.
            if (g_database) {
                g_database->SetConfigValue("enable_logging", enableLogging ? "1" : "0");
            }

            json response;
            response["type"] = "app_config";
            response["fastMode"] = fastMode;
            response["enableLogging"] = enableLogging;
            response["autoScrollLog"] = true;
            UIBridge_PushMessage(response.dump());

            UIBridge_PushStatus();
            UIBridge_HandleMessage("{\"action\":\"get_logs\"}");
        }

        // ========== Get Notice ==========
        else if (action == "get_notice") {
            json response;
            response["type"] = "notice";
            response["notice"] = "";
            UIBridge_PushMessage(response.dump());
        }
        else if (action == "remote_browser_get_config") {
            UIBridge_PushRemoteBrowserConfig();
        }
        else if (action == "remote_browser_set_mode") {
            UIBridge_EnsureRemoteBrowserConfigLoaded();

            const bool enabled = msg.value("enabled", false);
            std::string bindHost = UIBridge_TrimCopy(msg.value("bindHost", "0.0.0.0"));
            int port = msg.value("port", 18080);
            std::string accessToken = UIBridge_TrimCopy(msg.value("accessToken", ""));

            if (bindHost.empty()) {
                bindHost = "0.0.0.0";
            }
            if (port <= 0 || port > 65535) {
                UIBridge_Toast("error", "Remote mode config failed", "Port must be in range 1-65535");
                UIBridge_PushRemoteBrowserConfig();
                return;
            }
            if (accessToken.empty()) {
                accessToken = UIBridge_GenerateRemoteToken();
            }

            RemoteBrowserServer::Config newConfig;
            newConfig.enabled = enabled;
            newConfig.bindHost = bindHost;
            newConfig.port = port;
            newConfig.accessToken = accessToken;

            std::string applyError;
            bool applyOk = true;
            if (enabled) {
                applyOk = RemoteBrowserServer::ApplyConfig(newConfig, applyError);
                if (!applyOk) {
                    newConfig.enabled = false;
                }
            } else {
                RemoteBrowserServer::Stop();
            }

            {
                std::lock_guard<std::mutex> lock(g_remoteBrowserConfigMutex);
                g_remoteBrowserConfigLoaded = true;
                g_remoteBrowserConfig = newConfig;
                g_remoteBrowserLastError = applyOk ? "" : applyError;
            }
            UIBridge_SaveRemoteBrowserConfig(newConfig);

            if (!applyOk) {
                UIBridge_Toast("error", "Remote mode start failed",
                    applyError.empty() ? "Unable to start remote browser server" : applyError);
            } else if (newConfig.enabled) {
                UIBridge_Toast("success", "Remote mode enabled",
                    "Access URL: " + RemoteBrowserServer::BuildAccessUrl(newConfig));
            } else {
                UIBridge_Toast("info", "Remote mode disabled", "Remote browser control service stopped");
            }

            UIBridge_PushRemoteBrowserConfig();
        }

        // ========== Instance Management ==========
        else if (action == "get_instances") {
            UIBridge_PushInstances();
        }
        else if (action == "instance_create") {
            std::string name = msg.value("name", "");
            std::string typeStr = msg.value("type", "");
            int port = msg.value("port", 0);

            InstanceConfig config;
            config.name = name;
            config.port = port;

            // Parse type
            if (typeStr == "Collector") config.type = InstanceType::Collector;
            else if (typeStr == "Heartbeat") config.type = InstanceType::Heartbeat;
            else if (typeStr == "AbCollector") config.type = InstanceType::AbCollector;
            else if (typeStr == "AbHeartbeat") config.type = InstanceType::AbHeartbeat;
            else if (typeStr == "Socks5Pool") config.type = InstanceType::Socks5Pool;
            else if (typeStr == "SocksForward") config.type = InstanceType::SocksForward;

            auto& instMgr = InstanceManager::GetInstance();
            std::string instanceId;

            switch (config.type) {
                case InstanceType::Collector:
                    instanceId = instMgr.CreateCollectorInstance(config);
                    break;
                case InstanceType::Heartbeat:
                    instanceId = instMgr.CreateHeartbeatInstance(config);
                    break;
                case InstanceType::AbCollector:
                    instanceId = instMgr.CreateAbCollectorInstance(config);
                    break;
                case InstanceType::AbHeartbeat:
                    instanceId = instMgr.CreateAbHeartbeatInstance(config);
                    break;
                case InstanceType::Socks5Pool:
                    instanceId = instMgr.CreateSocks5PoolInstance(config);
                    break;
                case InstanceType::SocksForward:
                    instanceId = instMgr.CreateSocksForwardInstance(config);
                    break;
            }

            if (!instanceId.empty()) {
                UIBridge_Toast("success", "Create succeeded", "Instance " + name + " created");
                UIBridge_PushInstances();
                UIBridge_PushStatus();
            } else {
                UIBridge_Toast("error", "创建失败", "无法创建实例");
            }
        }
        else if (action == "socks_instances_export_file") {
            auto& instMgr = InstanceManager::GetInstance();
            const auto socksInstances = instMgr.GetSocksForwardInstances();

            if (socksInstances.empty()) {
                UIBridge_Toast("warning", "导出失败", "没有 SOCKS 转发实例可导出");
                return;
            }

            OPENFILENAMEA ofn;
            char szFile[260] = "socks_instances.json";
            ZeroMemory(&ofn, sizeof(ofn));
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = GetActiveWindow();
            ofn.lpstrFile = szFile;
            ofn.nMaxFile = sizeof(szFile);
            ofn.lpstrFilter = "JSON Files\0*.json\0All Files\0*.*\0";
            ofn.nFilterIndex = 1;
            ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
            ofn.lpstrDefExt = "json";

            if (!GetSaveFileNameA(&ofn)) {
                return;
            }

            json root;
            root["username"] = "";
            root["timestamp"] = static_cast<long long>(
                std::chrono::system_clock::now().time_since_epoch().count());
            root["instances"] = json::array();

            for (auto* instance : socksInstances) {
                if (!instance) continue;

                const std::string instanceId = instance->GetId();
                const InstanceInfo info = instance->GetInfo();

                json instConfig;
                instConfig["id"] = instanceId;
                instConfig["name"] = info.name;
                instConfig["port"] = info.port;
                instConfig["createTime"] = info.createTime;
                instConfig["currentConnections"] = info.currentConnections;
                instConfig["totalPackets"] = info.totalPackets;
                instConfig["totalBytes"] = info.totalBytes;

                auto getDb = [&](const char* key, const char* defaultValue) -> std::string {
                    if (!g_database) return defaultValue ? std::string(defaultValue) : std::string();
                    return g_database->GetConfigValue(InstanceManager::MakeInstanceConfigKey(instanceId, key),
                        defaultValue ? std::string(defaultValue) : std::string());
                };

                // 配置字段多数为字符串
                instConfig["threadPoolMode"] = getDb("threadPoolMode", "0");
                instConfig["whitelistPoolSize"] = getDb("whitelistPoolSize", "10");
                instConfig["normalPoolSize"] = getDb("normalPoolSize", "50");
                instConfig["iocpMaxWhitelist"] = getDb("iocpMaxWhitelist", "2000");
                instConfig["iocpMaxNormal"] = getDb("iocpMaxNormal", "500");

                instConfig["enableSocks5Auth"] = getDb("enableSocks5Auth", "0");
                instConfig["accountSourceInstanceId"] = getDb("accountSourceInstanceId", "");

                instConfig["enableSecondaryProxy"] = getDb("enableSecondaryProxy", "0");
                instConfig["secondaryProxyHost"] = getDb("secondaryProxyHost", "127.0.0.1");
                instConfig["secondaryProxyPort"] = getDb("secondaryProxyPort", "1080");
                instConfig["secondaryProxyUsername"] = getDb("secondaryProxyUsername", "");
                instConfig["secondaryProxyPassword"] = getDb("secondaryProxyPassword", "");

                instConfig["enablePacketSplit"] = getDb("enablePacketSplit", "1");
                instConfig["packetSplitPorts"] = getDb("packetSplitPorts", "");
                instConfig["applyWpeOnNonSplitTraffic"] = getDb("applyWpeOnNonSplitTraffic", "0");

                instConfig["enableTrafficFilter"] = getDb("enableTrafficFilter", "0");
                instConfig["enableSniSniffing"] = getDb("enableSniSniffing", "0");
                instConfig["trafficFilterRules"] = getDb("trafficFilterRules", "");
                instConfig["enableSSLMitm"] = getDb("enableSSLMitm", "0");
                instConfig["sslMitmRules"] = getDb("sslMitmRules", "[]");
                instConfig["enableHttpLocalMap"] = getDb("enableHttpLocalMap", "0");
                instConfig["httpLocalMapRules"] = getDb("httpLocalMapRules", "[]");

                instConfig["autoStart"] = getDb("autoStart", "0");

                instConfig["enableUserFilterMode"] = getDb("enableUserFilterMode", "0");
                instConfig["userFilterHttpPort"] = getDb("userFilterHttpPort", "8080");

                if (g_userFilterManager) {
                    const std::vector<int> defaultFilters = g_userFilterManager->LoadDefaultFilters(instanceId);
                    json defaultFiltersArray = json::array();
                    for (int filterId : defaultFilters) {
                        defaultFiltersArray.push_back(filterId);
                    }
                    instConfig["defaultFilters"] = defaultFiltersArray;

                    const std::vector<std::string> users = g_userFilterManager->GetInstanceUsers(instanceId);
                    json userFiltersObj = json::object();
                    for (const std::string& username : users) {
                        const std::set<int> userFilters = g_userFilterManager->GetUserEnabledFilters(instanceId, username);
                        json userFiltersArray = json::array();
                        for (int filterId : userFilters) {
                            userFiltersArray.push_back(filterId);
                        }
                        userFiltersObj[username] = userFiltersArray;
                    }
                    instConfig["userFilters"] = userFiltersObj;
                }

                root["instances"].push_back(instConfig);
            }

            try {
                const std::string exportJson = root.dump();
                std::ofstream outFile(szFile, std::ios::binary);
                if (!outFile.is_open()) {
                    UIBridge_Toast("error", "导出失败", "无法写入文件");
                    return;
                }
                outFile.write(exportJson.data(), static_cast<std::streamsize>(exportJson.size()));
                outFile.close();
                UIBridge_Toast("success", "导出成功", "已导出 " + std::to_string(socksInstances.size()) + " 个 SOCKS 转发实例");
            } catch (const std::exception& e) {
                UIBridge_Toast("error", "导出失败", std::string("写入文件异常: ") + e.what());
            } catch (...) {
                UIBridge_Toast("error", "导出失败", "写入文件异常(未知错误)");
            }
        }
        else if (action == "socks_instances_import_file") {
            OPENFILENAMEA ofn;
            char szFile[260] = "";
            ZeroMemory(&ofn, sizeof(ofn));
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = GetActiveWindow();
            ofn.lpstrFile = szFile;
            ofn.nMaxFile = sizeof(szFile);
            ofn.lpstrFilter = "JSON Files\0*.json\0All Files\0*.*\0";
            ofn.nFilterIndex = 1;
            ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

            if (!GetOpenFileNameA(&ofn)) {
                return;
            }

            std::string fileText;
            try {
                std::ifstream f(szFile, std::ios::binary);
                if (!f.is_open()) {
                    UIBridge_Toast("error", "导入失败", "无法打开文件");
                    return;
                }
                f.seekg(0, std::ios::end);
                const std::streamoff size = f.tellg();
                f.seekg(0, std::ios::beg);
                if (size <= 0) {
                    UIBridge_Toast("error", "导入失败", "文件为空");
                    return;
                }
                fileText.resize(static_cast<size_t>(size));
                f.read(&fileText[0], size);
                f.close();
            } catch (const std::exception& e) {
                UIBridge_Toast("error", "导入失败", std::string("读取文件异常: ") + e.what());
                return;
            } catch (...) {
                UIBridge_Toast("error", "导入失败", "读取文件异常(未知错误)");
                return;
            }

            json root;
            try {
                root = json::parse(fileText);
            } catch (const std::exception& e) {
                UIBridge_Toast("error", "导入失败", std::string("JSON 解析失败: ") + e.what());
                return;
            }

            json instances = json::array();
            if (root.is_array()) {
                instances = root;
            } else if (root.is_object() && root.contains("instances") && root["instances"].is_array()) {
                instances = root["instances"];
            } else {
                // 兼容多种封装：递归寻找 instances 数组
                json found;
                if (UIBridge_FindSocksForwardInstancesArrayRecursive(root, found, 8, nullptr) && found.is_array()) {
                    instances = found;
                } else {
                    std::string keysHint;
                    if (root.is_object()) {
                        int shown = 0;
                        for (auto it = root.begin(); it != root.end() && shown < 8; ++it) {
                            if (!keysHint.empty()) keysHint += ", ";
                            keysHint += it.key();
                            shown++;
                        }
                        if (!keysHint.empty()) {
                            keysHint = " (顶层字段: " + keysHint + ")";
                        }
                    }
                    UIBridge_Toast("error", "导入失败", "文件 JSON 结构不符合：未找到 instances 数组" + keysHint);
                    return;
                }
            }

            if (instances.empty()) {
                UIBridge_Toast("warning", "导入失败", "instances 为空，没有可导入的实例");
                return;
            }

            auto& instMgr = InstanceManager::GetInstance();

            int importedCount = 0;
            int failedCount = 0;

            for (const auto& instJson : instances) {
                if (!instJson.is_object()) {
                    failedCount++;
                    continue;
                }

                auto getStr = [&](const char* key, const std::string& def) -> std::string {
                    if (!instJson.contains(key) || instJson[key].is_null()) return def;
                    const auto& v = instJson[key];
                    if (v.is_string()) return v.get<std::string>();
                    if (v.is_boolean()) return v.get<bool>() ? "1" : "0";
                    if (v.is_number_integer()) return std::to_string(v.get<long long>());
                    if (v.is_number_unsigned()) return std::to_string(v.get<unsigned long long>());
                    if (v.is_number_float()) return std::to_string(v.get<double>());
                    return def;
                };

                auto getInt = [&](const char* key, int def) -> int {
                    try {
                        if (!instJson.contains(key) || instJson[key].is_null()) return def;
                        const auto& v = instJson[key];
                        if (v.is_number_integer()) return v.get<int>();
                        if (v.is_number_unsigned()) return static_cast<int>(v.get<unsigned long long>());
                        if (v.is_string()) {
                            const std::string s = UIBridge_TrimCopy(v.get<std::string>());
                            if (s.empty()) return def;
                            return std::stoi(s);
                        }
                    } catch (...) {}
                    return def;
                };

                auto getBool = [&](const char* key, bool def) -> bool {
                    if (!instJson.contains(key) || instJson[key].is_null()) return def;
                    const auto& v = instJson[key];
                    if (v.is_boolean()) return v.get<bool>();
                    if (v.is_number_integer()) return v.get<long long>() != 0;
                    if (v.is_number_unsigned()) return v.get<unsigned long long>() != 0;
                    if (v.is_string()) return UIBridge_ParseBoolValue(UIBridge_TrimCopy(v.get<std::string>()));
                    return def;
                };

                const std::string name = UIBridge_TrimCopy(getStr("name", "Socks转发实例"));
                const int port = getInt("port", 1080);
                if (port <= 0 || port > 65535) {
                    failedCount++;
                    continue;
                }

                InstanceConfig config;
                config.type = InstanceType::SocksForward;
                config.name = name.empty() ? std::string("Socks转发实例") : name;
                config.port = port;
                config.accountSourceInstanceId = getStr("accountSourceInstanceId", "");

                const std::string newId = instMgr.CreateSocksForwardInstance(config);
                if (newId.empty()) {
                    failedCount++;
                    continue;
                }

                auto* instance = instMgr.GetSocksForwardInstance(newId);
                if (!instance) {
                    failedCount++;
                    continue;
                }

                auto cached = instance->GetCachedConfig();

                cached.threadPoolMode = getInt("threadPoolMode", cached.threadPoolMode);
                cached.whitelistPoolSize = (std::max)(1, getInt("whitelistPoolSize", cached.whitelistPoolSize));
                cached.normalPoolSize = (std::max)(1, getInt("normalPoolSize", cached.normalPoolSize));
                cached.iocpMaxWhitelist = (std::max)(1, getInt("iocpMaxWhitelist", cached.iocpMaxWhitelist));
                cached.iocpMaxNormal = (std::max)(1, getInt("iocpMaxNormal", cached.iocpMaxNormal));

                cached.enableSocks5Auth = getBool("enableSocks5Auth", cached.enableSocks5Auth);
                cached.accountSourceInstanceId = getStr("accountSourceInstanceId", cached.accountSourceInstanceId);

                cached.enableSecondaryProxy = getBool("enableSecondaryProxy", cached.enableSecondaryProxy);
                cached.secondaryProxyHost = getStr("secondaryProxyHost", cached.secondaryProxyHost);
                cached.secondaryProxyPort = getInt("secondaryProxyPort", cached.secondaryProxyPort);
                cached.secondaryProxyUsername = getStr("secondaryProxyUsername", cached.secondaryProxyUsername);
                cached.secondaryProxyPassword = getStr("secondaryProxyPassword", cached.secondaryProxyPassword);

                cached.enablePacketSplit = getBool("enablePacketSplit", cached.enablePacketSplit);
                cached.packetSplitPorts = getStr("packetSplitPorts", cached.packetSplitPorts);
                cached.applyWpeOnNonSplitTraffic = getBool("applyWpeOnNonSplitTraffic", cached.applyWpeOnNonSplitTraffic);

                cached.enableTrafficFilter = getBool("enableTrafficFilter", cached.enableTrafficFilter);
                cached.enableSniSniffing = getBool("enableSniSniffing", cached.enableSniSniffing);
                cached.trafficFilterRules = getStr("trafficFilterRules", cached.trafficFilterRules);
                cached.enableSSLMitm = getBool("enableSSLMitm", cached.enableSSLMitm);
                cached.sslMitmRules = getStr("sslMitmRules", cached.sslMitmRules);
                cached.enableHttpLocalMap = getBool("enableHttpLocalMap", cached.enableHttpLocalMap);
                cached.httpLocalMapRules = getStr("httpLocalMapRules", cached.httpLocalMapRules);

                cached.enableUserFilterMode = getBool("enableUserFilterMode", cached.enableUserFilterMode);
                cached.userFilterHttpPort = (std::max)(1, getInt("userFilterHttpPort", cached.userFilterHttpPort));

                instance->UpdateConfig(cached);

                if (g_database) {
                    const bool autoStart = getBool("autoStart", false);
                    g_database->SetConfigValue(InstanceManager::MakeInstanceConfigKey(newId, "autoStart"), autoStart ? "1" : "0");
                }

                if (g_userFilterManager) {
                    // defaultFilters
                    if (instJson.contains("defaultFilters") && instJson["defaultFilters"].is_array()) {
                        std::vector<int> filterIds;
                        for (const auto& idVal : instJson["defaultFilters"]) {
                            if (idVal.is_number_integer()) filterIds.push_back(idVal.get<int>());
                            else if (idVal.is_number_unsigned()) filterIds.push_back(static_cast<int>(idVal.get<unsigned long long>()));
                            else if (idVal.is_string()) {
                                try { filterIds.push_back(std::stoi(UIBridge_TrimCopy(idVal.get<std::string>()))); } catch (...) {}
                            }
                        }
                        (void)g_userFilterManager->SaveDefaultFilters(newId, filterIds);
                    }

                    // userFilters
                    if (instJson.contains("userFilters") && instJson["userFilters"].is_object()) {
                        for (auto it = instJson["userFilters"].begin(); it != instJson["userFilters"].end(); ++it) {
                            const std::string username = it.key();
                            if (!it.value().is_array()) continue;
                            std::vector<int> filterIds;
                            for (const auto& idVal : it.value()) {
                                if (idVal.is_number_integer()) filterIds.push_back(idVal.get<int>());
                                else if (idVal.is_number_unsigned()) filterIds.push_back(static_cast<int>(idVal.get<unsigned long long>()));
                                else if (idVal.is_string()) {
                                    try { filterIds.push_back(std::stoi(UIBridge_TrimCopy(idVal.get<std::string>()))); } catch (...) {}
                                }
                            }
                            (void)g_userFilterManager->UpdateUserFilters(newId, username, filterIds);
                        }
                    }
                }

                importedCount++;
            }

            if (importedCount > 0) {
                UIBridge_Toast("success", "导入完成",
                    "成功导入 " + std::to_string(importedCount) + " 个实例" +
                    (failedCount > 0 ? (", 失败 " + std::to_string(failedCount) + " 个") : std::string()));
                UIBridge_PushInstances();
                UIBridge_PushStatus();
            } else {
                UIBridge_Toast("error", "导入失败", "没有任何实例被导入" +
                    (failedCount > 0 ? (", 失败 " + std::to_string(failedCount) + " 个") : std::string()));
            }
        }
        else if (action == "instance_start") {
            std::string id = msg.value("id", "");
            auto& instMgr = InstanceManager::GetInstance();
            if (instMgr.StartInstance(id)) {
                UIBridge_Toast("success", "Start succeeded", "Instance started");
                UIBridge_PushInstances();
                UIBridge_PushStatus();
            } else {
                UIBridge_Toast("error", "启动失败", "无法启动实例");
            }
        }
        else if (action == "instance_stop") {
            std::string id = msg.value("id", "");
            auto& instMgr = InstanceManager::GetInstance();
            if (instMgr.StopInstance(id)) {
                UIBridge_Toast("success", "Stop succeeded", "Instance stopped");
                UIBridge_PushInstances();
                UIBridge_PushStatus();
            } else {
                UIBridge_Toast("error", "停止失败", "无法停止实例");
            }
        }
        else if (action == "instance_delete") {
            std::string id = msg.value("id", "");
            auto& instMgr = InstanceManager::GetInstance();
            if (instMgr.DeleteInstance(id)) {
                UIBridge_Toast("success", "Delete succeeded", "Instance deleted");
                UIBridge_PushInstances();
                UIBridge_PushStatus();
            } else {
                UIBridge_Toast("error", "删除失败", "无法删除实例");
            }
        }
        else if (action == "instance_get_config") {
            std::string id = msg.value("id", "");
            auto& instMgr = InstanceManager::GetInstance();
            auto instInfo = instMgr.GetInstanceInfo(id);

            json response;
            response["type"] = "instance_config";

            json inst;
            inst["id"] = instInfo.id;
            inst["name"] = instInfo.name;
            inst["port"] = instInfo.port;
            inst["currentConnections"] = instInfo.currentConnections;
            inst["totalPackets"] = instInfo.totalPackets;
            inst["totalBytes"] = instInfo.totalBytes;

            // Type
            switch (instInfo.type) {
                case InstanceType::Collector: inst["type"] = "Collector"; break;
                case InstanceType::Heartbeat: inst["type"] = "Heartbeat"; break;
                case InstanceType::AbCollector: inst["type"] = "AbCollector"; break;
                case InstanceType::AbHeartbeat: inst["type"] = "AbHeartbeat"; break;
                case InstanceType::Socks5Pool: inst["type"] = "Socks5Pool"; break;
                case InstanceType::SocksForward: inst["type"] = "SocksForward"; break;
            }

            // State
            switch (instInfo.state) {
                case InstanceState::Stopped: inst["state"] = "Stopped"; break;
                case InstanceState::Running: inst["state"] = "Running"; break;
                case InstanceState::Error: inst["state"] = "Error"; break;
            }

            // Basic config (only what's available in InstanceInfo)
            json config;
            config["storageMode"] = static_cast<int>(instInfo.storageMode);
            config["memoryPoolCount"] = instInfo.memoryPoolCount;
            config["poolSource"] = static_cast<int>(instInfo.poolSource);
            config["bindToInstanceId"] = instInfo.bindToInstanceId;

            // Placeholder arrays for rules (to be populated by actual instance data)
            config["pattern23Rules"] = json::array();
            config["pattern09Rules"] = json::array();
            config["pattern62Rules"] = json::array();
            config["whitelistRules"] = json::array();
            config["blacklistRules"] = json::array();
            config["replaceCountRules"] = json::array();

            inst["config"] = config;
            response["instance"] = inst;

            UIBridge_PushMessage(response.dump());
        }

        // ========== SOCKS5 Pool Management ==========
        else if (action == "get_socks5_pools") {
            auto& instMgr = InstanceManager::GetInstance();
            auto pools = instMgr.GetSocks5PoolInstances();

            json response;
            response["type"] = "socks5_pools";
            response["pools"] = json::array();

            for (auto* pool : pools) {
                if (pool) {
                    json poolJson;
                    poolJson["id"] = pool->GetId();
                    poolJson["name"] = pool->GetName();
                    poolJson["accountCount"] = pool->GetAccountCount();
                    poolJson["onlineDevicePolicy"] =
                        pool->GetOnlineDevicePolicy() == Socks5PoolInstance::OnlineDevicePolicy::SingleDeviceSingleInstance
                        ? "single_device_single_instance"
                        : "unlimited";
                    response["pools"].push_back(poolJson);
                }
            }

            UIBridge_PushMessage(response.dump());
        }
        else if (action == "socks5_pool_create") {
            std::string name = msg.value("name", "");
            InstanceConfig config;
            config.name = name;
            config.type = InstanceType::Socks5Pool;

            auto& instMgr = InstanceManager::GetInstance();
            std::string poolId = instMgr.CreateSocks5PoolInstance(config);

            if (!poolId.empty()) {
                UIBridge_Toast("success", "Create succeeded", "Pool " + name + " created");
                // Trigger refresh
                UIBridge_HandleMessage("{\"action\":\"get_socks5_pools\"}");
            } else {
                UIBridge_Toast("error", "Create failed", "Unable to create pool");
            }
        }
        else if (action == "socks5_pool_delete") {
            std::string poolId = msg.value("poolId", "");
            auto& instMgr = InstanceManager::GetInstance();
            if (instMgr.DeleteInstance(poolId)) {
                UIBridge_Toast("success", "删除成功", "账号库已删除");
                UIBridge_HandleMessage("{\"action\":\"get_socks5_pools\"}");
            } else {
                UIBridge_Toast("error", "Delete failed", "Unable to delete pool");
            }
        }
        else if (action == "socks5_pool_get_accounts") {
            std::string poolId = msg.value("poolId", "");
            auto& instMgr = InstanceManager::GetInstance();
            auto* pool = instMgr.GetSocks5PoolInstance(poolId);

            if (pool) {
                json response;
                response["type"] = "socks5_pool_accounts";
                response["poolId"] = poolId;
                response["poolName"] = pool->GetName();
                response["onlineDevicePolicy"] =
                    pool->GetOnlineDevicePolicy() == Socks5PoolInstance::OnlineDevicePolicy::SingleDeviceSingleInstance
                    ? "single_device_single_instance"
                    : "unlimited";
                response["accounts"] = json::array();

                auto accounts = pool->GetAllAccounts();
                for (const auto& acc : accounts) {
                    json accJson;
                    accJson["id"] = acc.id;
                    accJson["username"] = acc.username;
                    accJson["password"] = acc.password;
                    accJson["expireTime"] = acc.expireTime;
                    accJson["maxConnections"] = acc.maxConnections;
                    accJson["isEnabled"] = acc.isEnabled;
                    accJson["createdAt"] = acc.createdAt;
                    response["accounts"].push_back(accJson);
                }

                UIBridge_PushMessage(response.dump());
            } else {
                UIBridge_Toast("error", "获取失败", "无法获取账号列表");
            }
        }
        else if (action == "socks5_pool_set_online_policy") {
            std::string poolId = msg.value("poolId", "");
            std::string policy = msg.value("policy", "single_device_single_instance");

            auto& instMgr = InstanceManager::GetInstance();
            auto* pool = instMgr.GetSocks5PoolInstance(poolId);
            if (pool) {
                pool->SetOnlineDevicePolicy(
                    policy == "unlimited"
                        ? Socks5PoolInstance::OnlineDevicePolicy::Unlimited
                        : Socks5PoolInstance::OnlineDevicePolicy::SingleDeviceSingleInstance);
                UIBridge_Toast("success", "更新成功", "账号库在线设备策略已更新");

                json refreshMsg;
                refreshMsg["action"] = "socks5_pool_get_accounts";
                refreshMsg["poolId"] = poolId;
                UIBridge_HandleMessage(refreshMsg.dump().c_str());
                UIBridge_HandleMessage("{\"action\":\"get_socks5_pools\"}");
            } else {
                UIBridge_Toast("error", "更新失败", "账号库不存在");
            }
        }
        else if (action == "socks5_account_add") {
            std::string poolId = msg.value("poolId", "");
            json accountJson = msg.value("account", json::object());

            auto& instMgr = InstanceManager::GetInstance();
            auto* pool = instMgr.GetSocks5PoolInstance(poolId);

            if (pool) {
                std::string username = accountJson.value("username", "");
                std::string password = accountJson.value("password", "");
                std::string expireTime = accountJson.value("expireTime", "");
                int maxConnections = accountJson.value("maxConnections", 0);

                if (pool->AddAccount(username, password, expireTime, maxConnections)) {
                    UIBridge_Toast("success", "Add succeeded", "Account added");
                    // Refresh accounts list
                    json refreshMsg;
                    refreshMsg["action"] = "socks5_pool_get_accounts";
                    refreshMsg["poolId"] = poolId;
                    UIBridge_HandleMessage(refreshMsg.dump().c_str());
                } else {
                    UIBridge_Toast("error", "添加失败", "无法添加账号");
                }
            }
        }
        else if (action == "socks5_account_delete") {
            std::string poolId = msg.value("poolId", "");
            std::string username = msg.value("username", "");

            auto& instMgr = InstanceManager::GetInstance();
            auto* pool = instMgr.GetSocks5PoolInstance(poolId);

            if (pool && pool->RemoveAccount(username)) {
                UIBridge_Toast("success", "Delete succeeded", "Account deleted");
                // Refresh accounts list
                json refreshMsg;
                refreshMsg["action"] = "socks5_pool_get_accounts";
                refreshMsg["poolId"] = poolId;
                UIBridge_HandleMessage(refreshMsg.dump().c_str());
            } else {
                UIBridge_Toast("error", "删除失败", "无法删除账号");
            }
        }
        else if (action == "socks5_account_update") {
            std::string poolId = msg.value("poolId", "");
            std::string username = msg.value("username", "");
            json accountJson = msg.value("account", json::object());

            auto& instMgr = InstanceManager::GetInstance();
            auto* pool = instMgr.GetSocks5PoolInstance(poolId);

            if (pool) {
                std::string password = accountJson.value("password", "");
                std::string expireTime = accountJson.value("expireTime", "");
                int maxConnections = accountJson.value("maxConnections", 0);
                bool isEnabled = accountJson.value("isEnabled", true);

                if (pool->UpdateAccount(username, password, expireTime, maxConnections, isEnabled)) {
                    UIBridge_Toast("success", "Update succeeded", "Account updated");
                    json refreshMsg;
                    refreshMsg["action"] = "socks5_pool_get_accounts";
                    refreshMsg["poolId"] = poolId;
                    UIBridge_HandleMessage(refreshMsg.dump().c_str());
                } else {
                    UIBridge_Toast("error", "更新失败", "无法更新账号");
                }
            }
        }
        else if (action == "socks5_ccproxy_import") {
            std::string poolId = msg.value("poolId", "");
            std::string host = msg.value("host", "127.0.0.1");
            int port = std::stoi(msg.value("port", "90"));
            std::string username = msg.value("username", "admin");
            std::string password = msg.value("password", "admin");

            auto& instMgr = InstanceManager::GetInstance();
            auto* pool = instMgr.GetSocks5PoolInstance(poolId);

            if (pool) {
                std::string path = "/account";
                AB_LOG_INFO_CAT(LOG_CAT_SOCKS_ACCOUNT,
                    "[远程导入] 开始请求: http://" + host + ":" + std::to_string(port) + path);

                std::string response = UIBridge_HttpGet(host, port, path, username, password);

                if (response.empty()) {
                    UIBridge_Toast("error", "导入失败", "远程服务器无响应");
                } else {
                    std::string cleanResponse = std::regex_replace(response, std::regex("[\\r\\n]+"), " ");
                    std::regex rowRegex(R"(<tr[^>]*>(.*?)</tr>)", std::regex::icase);
                    std::smatch rowMatches;
                    std::string::const_iterator rowSearchStart(cleanResponse.cbegin());
                    int successCount = 0, skipCount = 0;

                    while (std::regex_search(rowSearchStart, cleanResponse.cend(), rowMatches, rowRegex)) {
                        if (rowMatches.size() < 2) { rowSearchStart = rowMatches.suffix().first; continue; }
                        std::string rowContent = rowMatches[1].str();
                        std::regex tdRegex(R"(<td[^>]*>(.*?)</td>)", std::regex::icase);
                        std::smatch tdMatches;
                        std::string::const_iterator tdSearchStart(rowContent.cbegin());
                        std::vector<std::string> tdContents;
                        while (std::regex_search(tdSearchStart, rowContent.cend(), tdMatches, tdRegex)) {
                            if (tdMatches.size() >= 2) tdContents.push_back(tdMatches[1].str());
                            tdSearchStart = tdMatches.suffix().first;
                        }
                        if (tdContents.size() < 12) { rowSearchStart = rowMatches.suffix().first; continue; }

                        auto usernameInputs = UIBridge_ExtractInputValues(tdContents[2]);
                        std::string remoteUser = usernameInputs.empty() ? "" : usernameInputs[0];
                        auto passwordInputs = UIBridge_ExtractInputValues(tdContents[4]);
                        std::string remotePwd = (passwordInputs.size() >= 2) ? passwordInputs[1] : "";
                        int maxConn = 1;
                        if (tdContents.size() > 7) {
                            auto connInputs = UIBridge_ExtractInputValues(tdContents[7]);
                            if (!connInputs.empty()) { try { maxConn = std::stoi(connInputs[0]); } catch (...) { maxConn = 1; } }
                        }
                        if (maxConn == -1) maxConn = 0;
                        auto expireInputs = UIBridge_ExtractInputValues(tdContents[11]);
                        std::string expDate = (expireInputs.size() >= 2) ? expireInputs[1] : "";
                        std::string expTime = (expireInputs.size() >= 3) ? expireInputs[2] : "";
                        std::string fullExpire = expDate + " " + expTime;

                        auto trim = [](std::string& s) {
                            s.erase(0, s.find_first_not_of(" \t\n\r"));
                            s.erase(s.find_last_not_of(" \t\n\r") + 1);
                        };
                        trim(remoteUser); trim(remotePwd); trim(fullExpire);

                        if (remoteUser.empty() || remotePwd.empty()) { skipCount++; rowSearchStart = rowMatches.suffix().first; continue; }
                        if (pool->AccountExists(remoteUser)) { skipCount++; rowSearchStart = rowMatches.suffix().first; continue; }
                        if (pool->AddAccount(remoteUser, remotePwd, fullExpire, maxConn)) { successCount++; } else { skipCount++; }
                        rowSearchStart = rowMatches.suffix().first;
                    }

                    if (successCount > 0) {
                        UIBridge_Toast("success", "Import succeeded", "Success: " + std::to_string(successCount) + ", Skipped: " + std::to_string(skipCount));
                        json refreshMsg;
                        refreshMsg["action"] = "socks5_pool_get_accounts";
                        refreshMsg["poolId"] = poolId;
                        UIBridge_HandleMessage(refreshMsg.dump().c_str());
                    } else {
                        UIBridge_Toast("info", "导入完成", "没有新账号需要导入（跳过: " + std::to_string(skipCount) + " 个）");
                    }
                }
            } else {
                UIBridge_Toast("error", "Import failed", "Target pool not found");
            }
        }
        else if (action == "socks5_accounts_export") {
            std::string poolId = msg.value("poolId", "");
            auto& instMgr = InstanceManager::GetInstance();
            auto* pool = instMgr.GetSocks5PoolInstance(poolId);

            if (pool) {
                auto accounts = pool->GetAllAccounts();
                std::string exportText;
                for (const auto& acc : accounts) {
                    exportText += acc.username + ":" + acc.password + "\n";
                }

                json response;
                response["type"] = "socks5_accounts_export";
                response["success"] = true;
                response["text"] = exportText;
                UIBridge_PushMessage(response.dump());
            } else {
                json response;
                response["type"] = "socks5_accounts_export";
                response["success"] = false;
                response["error"] = "无法导出账号";
                UIBridge_PushMessage(response.dump());
            }
        }
        // ========== SOCKS5 Pool API Service ==========
        else if (action == "socks5_api_start") {
            std::string poolId = msg.value("poolId", "");
            int port = msg.value("port", 90);
            std::string username = msg.value("username", "admin");
            std::string password = msg.value("password", "admin");

            auto& instMgr = InstanceManager::GetInstance();
            auto* pool = instMgr.GetSocks5PoolInstance(poolId);

            if (pool) {
                if (port <= 0 || port > 65535) {
                    UIBridge_Toast("error", "错误", "端口号无效！");
                } else {
                    if (pool->StartApiServer(port, username, password)) {
                        UIBridge_Toast("success", "启动成功", "API服务已启动，端口: " + std::to_string(port));
                        json response;
                        response["type"] = "socks5_api_status";
                        response["running"] = true;
                        response["port"] = port;
                        UIBridge_PushMessage(response.dump());
                    } else {
                        UIBridge_Toast("error", "Start failed", "API server failed to start (port may be in use)");
                    }
                }
            }
        }
        else if (action == "socks5_api_stop") {
            std::string poolId = msg.value("poolId", "");

            auto& instMgr = InstanceManager::GetInstance();
            auto* pool = instMgr.GetSocks5PoolInstance(poolId);

            if (pool) {
                pool->StopApiServer();
                UIBridge_Toast("success", "Stopped", "API service stopped");
                json response;
                response["type"] = "socks5_api_status";
                response["running"] = false;
                response["port"] = 0;
                UIBridge_PushMessage(response.dump());
            }
        }
        else if (action == "socks5_api_status") {
            std::string poolId = msg.value("poolId", "");

            auto& instMgr = InstanceManager::GetInstance();
            auto* pool = instMgr.GetSocks5PoolInstance(poolId);

            if (pool) {
                json response;
                response["type"] = "socks5_api_status";
                response["running"] = pool->IsApiServerRunning();
                response["port"] = pool->GetApiServerPort();
                UIBridge_PushMessage(response.dump());
            }
        }

        // ========== Online Stats ==========
        else if (action == "get_online_stats") {
            std::string instanceId = msg.value("instanceId", "");

            json response;
            response["type"] = "online_stats";
            response["accounts"] = json::array();

            auto& instMgr = InstanceManager::GetInstance();
            auto socksInstances = instMgr.GetSocksForwardInstances();


            // 鏋勫缓瀹炰緥鍒楄〃
            response["instances"] = json::array();
            for (auto* socksInst : socksInstances) {
                if (!socksInst) continue;
                auto instInfo = instMgr.GetInstanceInfo(socksInst->GetId());
                json inst;
                inst["id"] = socksInst->GetId();
                inst["name"] = instInfo.name;
                response["instances"].push_back(inst);
            }

            // 濡傛灉娌℃湁閫夋嫨瀹炰緥锛屽彧杩斿洖瀹炰緥鍒楄〃
            if (instanceId.empty()) {
                response["stats"]["totalAccounts"] = 0;
                response["stats"]["onlineCount"] = 0;
                response["stats"]["offlineCount"] = 0;
                response["stats"]["totalConnections"] = 0;
                UIBridge_PushMessage(response.dump());
                return;
            }

            // 鑾峰彇閫変腑鐨勫疄渚?
            auto* socksInst = instMgr.GetSocksForwardInstance(instanceId);
            if (!socksInst) {
                response["stats"]["totalAccounts"] = 0;
                response["stats"]["onlineCount"] = 0;
                response["stats"]["offlineCount"] = 0;
                response["stats"]["totalConnections"] = 0;
                UIBridge_PushMessage(response.dump());
                return;
            }

            auto collector = socksInst->GetCollector();
            if (!collector) {
                response["stats"]["totalAccounts"] = 0;
                response["stats"]["onlineCount"] = 0;
                response["stats"]["offlineCount"] = 0;
                response["stats"]["totalConnections"] = 0;
                UIBridge_PushMessage(response.dump());
                return;
            }

            // 获取该实例实际使用的账号状态源
            auto* accountSource = collector->GetResolvedAccountStateOwner();
            if (!accountSource) {
                accountSource = collector;
            }

            auto accounts = accountSource->GetAllAccounts();

            int totalAccounts = 0;
            int onlineCount = 0;
            int offlineCount = 0;
            int totalConnections = 0;

            for (const auto& acc : accounts) {
                totalAccounts++;

                SharedAccountLeaseInfo leaseInfo;
                const bool hasSharedLease =
                    (accountSource != collector) &&
                    accountSource->TryGetSharedAccountLeaseInfo(acc.username, leaseInfo);
                int visibleConnections = acc.currentConnections;
                if (hasSharedLease && leaseInfo.ownerInstanceId != instanceId) {
                    visibleConnections = 0;
                }

                json account;
                account["username"] = acc.username;
                account["currentConnections"] = visibleConnections;
                account["globalCurrentConnections"] = acc.currentConnections;
                account["maxConnections"] = acc.maxConnections;
                account["expireTime"] = acc.expireTime;
                account["lastLoginTime"] = acc.lastLoginTime.empty() ? "-" : acc.lastLoginTime;
                account["lastLoginIP"] = acc.lastLoginIP.empty() ? "-" : acc.lastLoginIP;
                account["instanceId"] = instanceId;
                account["leaseOwnerInstanceId"] = hasSharedLease ? leaseInfo.ownerInstanceId : "";
                account["leaseOwnerDeviceKey"] = hasSharedLease ? leaseInfo.ownerDeviceKey : "";
                account["leaseRefCount"] = hasSharedLease ? leaseInfo.refCount : 0;

                // 鏍煎紡鍖栫疮璁″湪绾挎椂闀?
                int onlineSeconds = static_cast<int>(acc.totalOnlineSeconds);
                int hours = onlineSeconds / 3600;
                int minutes = (onlineSeconds % 3600) / 60;
                int seconds = onlineSeconds % 60;

                char durationBuf[64];
                if (hours > 0) {
                    snprintf(durationBuf, sizeof(durationBuf), "%dh %dm %ds", hours, minutes, seconds);
                } else if (minutes > 0) {
                    snprintf(durationBuf, sizeof(durationBuf), "%dm %ds", minutes, seconds);
                } else {
                    snprintf(durationBuf, sizeof(durationBuf), "%ds", seconds);
                }
                account["onlineDuration"] = std::string(durationBuf);

                if (visibleConnections > 0) {
                    onlineCount++;
                    totalConnections += visibleConnections;
                } else {
                    offlineCount++;
                }

                response["accounts"].push_back(account);
            }

            // 统计信息
            response["stats"]["totalAccounts"] = totalAccounts;
            response["stats"]["onlineCount"] = onlineCount;
            response["stats"]["offlineCount"] = offlineCount;
            response["stats"]["totalConnections"] = totalConnections;

            UIBridge_PushMessage(response.dump());
        }
        else if (action == "online_kick") {
            std::string username = msg.value("username", "");
            std::string instanceId = msg.value("instanceId", "");

            if (username.empty() || instanceId.empty()) {
                UIBridge_Toast("error", "参数错误", "用户名或实例ID为空");
                return;
            }

            auto& instMgr = InstanceManager::GetInstance();
            auto* socksInst = instMgr.GetSocksForwardInstance(instanceId);

            if (!socksInst) {
                UIBridge_Toast("error", "Kick failed", "Target instance not found");
                return;
            }

            auto collector = socksInst->GetCollector();
            if (!collector) {
                UIBridge_Toast("error", "Kick failed", "Instance is not running");
                return;
            }

            // 🔥 断开该用户的所有连�?
            bool success = collector->DisconnectUser(username);
            if (success) {
                UIBridge_Toast("success", "踢出成功", "用户 " + username + " 已被踢出");
                // 刷新在线统计
                UIBridge_HandleMessage("{\"action\":\"get_online_stats\"}");
            } else {
                UIBridge_Toast("error", "踢出失败", "无法断开用户连接");
            }
        }

        // ========== AntiCC Config ==========
        else if (action == "anticc_get_config") {
            const std::string instanceId = msg.value("instanceId", "");
            auto* instance = UIBridge_GetSocksForwardInstanceById(instanceId);
            if (!instance) {
                UIBridge_Toast("error", "Load failed", "未找到Socks转发实例");
            }
            else {
                auto cached = instance->GetCachedConfig();
                auto* collector = instance->GetCollector();

                json response;
                response["type"] = "anticc_config";
                response["instanceId"] = instanceId;

                json configJson;
                configJson["enabled"] = cached.antiCC.enabled;
                configJson["timeWindow"] = cached.antiCC.timeWindowSeconds;
                configJson["maxRequests"] = cached.antiCC.maxRequestsInWindow;
                configJson["banTime"] = cached.antiCC.banTimeSeconds;
                configJson["maxConnections"] = cached.antiCC.maxConnections;
                configJson["useBlacklist"] = cached.antiCC.useBlacklist;
                configJson["useWhitelist"] = cached.antiCC.useWhitelist;
                configJson["whitelistDuration"] = cached.antiCC.whitelistDuration;
                configJson["authFailBanTime"] = cached.antiCC.authFailBanTime;
                configJson["noAuthBanTime"] = cached.antiCC.noAuthBanTime;
                configJson["useFirewall"] = cached.antiCC.useFirewall;
                configJson["rateLimitEnabled"] = cached.antiCC.rateLimitEnabled;
                configJson["rateLimit"] = cached.antiCC.rateLimit;
                configJson["rateTimeWindow"] = cached.antiCC.rateTimeWindow;
                configJson["listenPort"] = instance->GetPort();
                configJson["blockNonSocks"] = cached.antiCC.blockNonSocks;
                configJson["enableAuthPriorityAdmission"] = cached.antiCC.enableAuthPriorityAdmission;
                configJson["authPriorityQueueLimit"] = cached.antiCC.authPriorityQueueLimit;
                configJson["enableLowPriorityEviction"] = cached.antiCC.enableLowPriorityEviction;
                configJson["lowPriorityEvictionThreshold"] = cached.antiCC.lowPriorityEvictionThreshold;
                configJson["enableCoordinator"] = cached.antiCC.enableCoordinator;

                configJson["blacklist"] = json::array();
                for (const auto& ip : GlobalAntiCCCoordinator::GetInstance().GetGlobalBlacklist()) {
                    configJson["blacklist"].push_back(ip);
                }

                configJson["whitelist"] = json::array();
                json whitelistEntries = json::array();
                for (const auto& entry : GlobalAntiCCCoordinator::GetInstance().GetScopedWhitelistEntriesForInstance(instanceId)) {
                    json item;
                    item["id"] = entry.id;
                    item["ip"] = entry.ip;
                    item["scopeType"] = static_cast<int>(entry.scopeType);
                    item["targetInstanceIds"] = entry.targetInstanceIds;
                    item["source"] = entry.source;
                    item["expireUnixSeconds"] = entry.expireUnixSeconds;
                    whitelistEntries.push_back(item);
                    configJson["whitelist"].push_back(entry.ip);
                }
                configJson["whitelistEntries"] = whitelistEntries;

                if (collector) {
                    configJson["blockedCount"] = collector->GetAntiCCBlockedCount();
                    configJson["totalConnections"] = collector->GetAntiCCTotalConnections();
                    configJson["underAttack"] = collector->IsUnderAttack();
                }

                configJson["globalConnectionCount"] = GlobalAntiCCCoordinator::GetInstance().GetGlobalConnectionCount();
                configJson["instanceConnectionCount"] = GlobalAntiCCCoordinator::GetInstance().GetInstanceConnectionCount(instanceId);

                response["config"] = configJson;
                UIBridge_PushMessage(response.dump());
            }
        }
        else if (action == "anticc_get_global_state") {
            json response;
            response["type"] = "anticc_global_state";

            json state;
            state["globalConnectionCount"] = GlobalAntiCCCoordinator::GetInstance().GetGlobalConnectionCount();
            state["globalPressure"] = static_cast<int>(GlobalAntiCCCoordinator::GetInstance().GetGlobalPressure());

            state["blacklist"] = json::array();
            for (const auto& ip : GlobalAntiCCCoordinator::GetInstance().GetGlobalBlacklist()) {
                state["blacklist"].push_back(ip);
            }

            state["whitelistEntries"] = json::array();
            for (const auto& entry : GlobalAntiCCCoordinator::GetInstance().GetScopedWhitelistEntries()) {
                json item;
                item["id"] = entry.id;
                item["ip"] = entry.ip;
                item["scopeType"] = static_cast<int>(entry.scopeType);
                item["targetInstanceIds"] = entry.targetInstanceIds;
                item["source"] = entry.source;
                item["expireUnixSeconds"] = entry.expireUnixSeconds;
                state["whitelistEntries"].push_back(item);
            }

            state["topIpConnections"] = json::array();
            for (const auto& pair : GlobalAntiCCCoordinator::GetInstance().GetTopIpConnectionCounts()) {
                json item;
                item["ip"] = pair.first;
                item["connections"] = pair.second;
                state["topIpConnections"].push_back(item);
            }

            response["state"] = state;
            UIBridge_PushMessage(response.dump());
        }
        else if (action == "anticc_save_config") {
            const std::string instanceId = msg.value("instanceId", "");
            auto* instance = UIBridge_GetSocksForwardInstanceById(instanceId);
            if (!instance) {
                UIBridge_Toast("error", "Save failed", "未找到Socks转发实例");
            }
            else {
                auto cached = instance->GetCachedConfig();
                cached.antiCC.enabled = msg.value("enabled", false);
                cached.antiCC.timeWindowSeconds = msg.value("timeWindow", 10);
                cached.antiCC.maxRequestsInWindow = msg.value("maxRequests", 20);
                cached.antiCC.banTimeSeconds = msg.value("banTime", 300);
                cached.antiCC.maxConnections = msg.value("maxConnections", 100);
                cached.antiCC.useBlacklist = msg.value("useBlacklist", true);
                cached.antiCC.useWhitelist = msg.value("useWhitelist", true);
                cached.antiCC.whitelistDuration = msg.value("whitelistDuration", 3600);
                cached.antiCC.authFailBanTime = msg.value("authFailBanTime", 60);
                cached.antiCC.noAuthBanTime = msg.value("noAuthBanTime", 30);
                cached.antiCC.useFirewall = msg.value("useFirewall", false);
                cached.antiCC.rateLimitEnabled = msg.value("rateLimitEnabled", false);
                cached.antiCC.rateLimit = msg.value("rateLimit", 100);
                cached.antiCC.rateTimeWindow = msg.value("rateTimeWindow", 1);
                cached.antiCC.blockNonSocks = msg.value("blockNonSocks", false);
                cached.antiCC.enableAuthPriorityAdmission = msg.value("enableAuthPriorityAdmission", true);
                cached.antiCC.authPriorityQueueLimit = msg.value("authPriorityQueueLimit", 128);
                cached.antiCC.enableLowPriorityEviction = msg.value("enableLowPriorityEviction", true);
                cached.antiCC.lowPriorityEvictionThreshold = msg.value("lowPriorityEvictionThreshold", 80);
                cached.antiCC.enableCoordinator = msg.value("enableCoordinator", true);

                instance->UpdateConfig(cached);

                if (msg.contains("blacklist") && msg["blacklist"].is_array()) {
                    std::vector<std::string> blacklist;
                    for (const auto& ip : msg["blacklist"]) {
                        if (ip.is_string()) {
                            blacklist.push_back(ip.get<std::string>());
                        }
                    }
                    GlobalAntiCCCoordinator::GetInstance().ReplaceGlobalBlacklist(blacklist);
                }

                if (msg.contains("whitelistEntries") && msg["whitelistEntries"].is_array()) {
                    std::vector<ScopedWhitelistEntry> entries;
                    for (const auto& item : msg["whitelistEntries"]) {
                        if (!item.is_object()) {
                            continue;
                        }
                        ScopedWhitelistEntry entry;
                        entry.id = item.value("id", "");
                        entry.ip = item.value("ip", "");
                        entry.scopeType = static_cast<AntiCCScopeType>(item.value("scopeType", static_cast<int>(AntiCCScopeType::SelectedInstances)));
                        entry.source = item.value("source", "manual");
                        entry.sourceInstanceId = item.value("sourceInstanceId", instanceId);
                        entry.expireUnixSeconds = item.value("expireUnixSeconds", static_cast<int64_t>(0));
                        if (item.contains("targetInstanceIds") && item["targetInstanceIds"].is_array()) {
                            for (const auto& target : item["targetInstanceIds"]) {
                                if (target.is_string()) {
                                    entry.targetInstanceIds.push_back(target.get<std::string>());
                                }
                            }
                        }
                        if (!entry.ip.empty()) {
                            entries.push_back(entry);
                        }
                    }
                    GlobalAntiCCCoordinator::GetInstance().ReplaceManualWhitelistEntriesForSource(instanceId, entries);
                }
                else if (msg.contains("whitelist") && msg["whitelist"].is_array()) {
                    std::vector<std::string> whitelist;
                    for (const auto& ip : msg["whitelist"]) {
                        if (ip.is_string()) {
                            whitelist.push_back(ip.get<std::string>());
                        }
                    }
                    GlobalAntiCCCoordinator::GetInstance().ReplaceInstanceManualWhitelist(instanceId, whitelist);
                }

                UIBridge_Toast("success", "Save succeeded", "AntiCC config saved");
            }
        }
        else if (action == "anticc_reset_config") {
            const std::string instanceId = msg.value("instanceId", "");
            auto* instance = UIBridge_GetSocksForwardInstanceById(instanceId);
            if (!instance) {
                UIBridge_Toast("error", "Reset failed", "未找到Socks转发实例");
            }
            else {
                auto cached = instance->GetCachedConfig();
                cached.antiCC = SocksForwardInstance::CachedConfig::CachedAntiCCConfig{};
                instance->UpdateConfig(cached);
                GlobalAntiCCCoordinator::GetInstance().ReplaceInstanceManualWhitelist(instanceId, {});
                UIBridge_Toast("success", "Reset succeeded", "AntiCC config reset to defaults");

                json reloadMsg;
                reloadMsg["action"] = "anticc_get_config";
                reloadMsg["instanceId"] = instanceId;
                const std::string reloadText = reloadMsg.dump();
                UIBridge_HandleMessage(reloadText.c_str());
            }
        }

        // ========== WPE Filters ==========
        else if (action == "get_wpe_filters") {
            if (g_wpeFilterManager) {
                auto filters = g_wpeFilterManager->GetAllFilters();

                json response;
                response["type"] = "wpe_filters";
                response["filters"] = json::array();

                for (const auto& filter : filters) {
                    json fj;
                    fj["id"] = filter.id;
                    fj["name"] = filter.name;
                    fj["webDisplayName"] = filter.webDisplayName;
                    fj["enabled"] = filter.isEnabled;
                    fj["executionCount"] = filter.executionCount;
                    fj["mode"] = (filter.mode == WPEFilter::FilterMode::Normal) ? "normal" : "advanced";

                    std::string actionStr = "replace";
                    switch (filter.action) {
                        case WPEFilter::FilterAction::Replace: actionStr = "replace"; break;
                        case WPEFilter::FilterAction::Intercept: actionStr = "intercept"; break;
                        case WPEFilter::FilterAction::NoModify_Display: actionStr = "display"; break;
                        case WPEFilter::FilterAction::NoModify_NoDisplay: actionStr = "nodisplay"; break;
                        case WPEFilter::FilterAction::Change: actionStr = "change"; break;
                        default: actionStr = "replace"; break;
                    }
                    fj["action"] = actionStr;

                    json dir;
                    dir["request"] = filter.direction.applyToRequest;
                    dir["response"] = filter.direction.applyToResponse;
                    fj["direction"] = dir;

                    fj["global"] = filter.target.applyToAllInstances;
                    fj["targetInstances"] = filter.target.targetInstanceIds;
                    fj["priority"] = (filter.priority == WPEFilter::FilterPriority::BeforeHeartbeat) ? "before" : "after";

                    response["filters"].push_back(fj);
                }

                UIBridge_PushMessage(response.dump());
            }
        }
        else if (action == "get_wpe_filter_groups") {
            json response;
            response["type"] = "wpe_filter_groups";
            response["groups"] = json::array();

            if (g_database) {
                auto groups = g_database->LoadWPEFilterGroups(true);
                for (const auto& group : groups) {
                    json gj;
                    gj["id"] = group.id;
                    gj["name"] = group.name;
                    gj["description"] = group.description;
                    gj["enabled"] = group.enabled;
                    gj["items"] = json::array();
                    for (const auto& item : group.items) {
                        gj["items"].push_back({
                            {"filterId", item.filterId},
                            {"defaultEnabled", item.defaultEnabled}
                        });
                    }
                    response["groups"].push_back(gj);
                }
            }

            UIBridge_PushMessage(response.dump());
        }
        else if (action == "save_wpe_filter_group") {
            if (!g_database) {
                UIBridge_Toast("error", "保存失败", "数据库未初始化");
            } else {
                WPEFilterGroupRecord group;
                group.id = msg.value("groupId", msg.value("id", 0));
                group.name = UIBridge_TrimCopy(msg.value("name", ""));
                group.description = msg.value("description", "");
                group.enabled = msg.value("enabled", true);

                if (group.name.empty()) {
                    UIBridge_Toast("warning", "保存失败", "滤镜组名称不能为空");
                } else {
                    if (msg.contains("items") && msg["items"].is_array()) {
                        for (const auto& itemJson : msg["items"]) {
                            WPEFilterGroupItemRecord item;
                            item.filterId = itemJson.value("filterId", 0);
                            item.defaultEnabled = itemJson.value("defaultEnabled", false);
                            if (item.filterId > 0) {
                                group.items.push_back(item);
                            }
                        }
                    }

                    int savedId = g_database->SaveWPEFilterGroup(group);
                    if (savedId > 0) {
                        if (g_userFilterManager) {
                            g_userFilterManager->LoadInstanceConfig("");
                        }
                        UIBridge_Toast("success", "保存成功", "WPE滤镜组已保存");
                        UIBridge_HandleMessage("{\"action\":\"get_wpe_filter_groups\"}");
                    } else {
                        UIBridge_Toast("error", "保存失败", "无法保存WPE滤镜组");
                    }
                }
            }
        }
        else if (action == "delete_wpe_filter_group") {
            int groupId = msg.value("groupId", msg.value("id", 0));
            if (g_database && groupId > 0 && g_database->DeleteWPEFilterGroup(groupId)) {
                if (g_userFilterManager) {
                    g_userFilterManager->LoadInstanceConfig("");
                }
                UIBridge_Toast("success", "删除成功", "WPE滤镜组已删除");
                UIBridge_HandleMessage("{\"action\":\"get_wpe_filter_groups\"}");
            } else {
                UIBridge_Toast("error", "删除失败", "无法删除WPE滤镜组");
            }
        }
        else if (action == "save_card_wpe_filter_groups") {
            const std::string cardKey = UIBridge_TrimCopy(msg.value("cardKey", ""));
            const std::vector<int> groupIds = UIBridge_ReadGroupIds(msg);

            if (cardKey.empty()) {
                UIBridge_Toast("warning", "保存失败", "卡密不能为空");
            } else if (g_database && g_database->SaveCardWPEFilterGroups(cardKey, groupIds)) {
                UIBridge_Toast("success", "保存成功", "卡密滤镜组权限已保存");
            } else {
                UIBridge_Toast("error", "保存失败", "无法保存卡密滤镜组权限");
            }
        }
        else if (action == "save_user_wpe_filter_groups") {
            const std::string instanceId = msg.value("instanceId", "");
            const std::string username = UIBridge_TrimCopy(msg.value("username", ""));
            const std::vector<int> groupIds = UIBridge_ReadGroupIds(msg);

            if (instanceId.empty() || username.empty()) {
                UIBridge_Toast("warning", "保存失败", "实例和账号不能为空");
            } else if (g_userFilterManager && g_userFilterManager->UpdateUserFilterGroups(instanceId, username, groupIds)) {
                UIBridge_Toast("success", "保存成功", "账号滤镜组权限已保存");
            } else if (g_database && g_database->SaveUserWPEFilterGroups(instanceId, username, groupIds)) {
                UIBridge_Toast("success", "保存成功", "账号滤镜组权限已保存");
            } else {
                UIBridge_Toast("error", "保存失败", "无法保存账号滤镜组权限");
            }
        }
        else if (action == "wpe_filter_create") {
            if (g_wpeFilterManager) {
                WPEFilter::FilterInfo filter;
                filter.name = msg.value("name", "");
                filter.webDisplayName = msg.value("webDisplayName", "");
                filter.isEnabled = msg.value("enabled", true);
                filter.mode = WPEFilter::FilterMode::Normal;
                filter.action = WPEFilter::FilterAction::Replace;

                int filterId = g_wpeFilterManager->AddFilter(filter);
                if (filterId > 0) {
                    UIBridge_SaveWPEConfigWithFeedback(true, "Create succeeded", "Filter created");
                    UIBridge_HandleMessage("{\"action\":\"get_wpe_filters\"}");
                    UIBridge_RequestImmediateWpeCloudSync("wpe_filter_create");
                } else {
                    UIBridge_Toast("error", "创建失败", "无法创建滤镜");
                }
            }
        }
        else if (action == "wpe_filter_create_full") {
            AB_LOG_INFO("[UI] 收到创建WPE滤镜请求");
            AB_LOG_INFO("[UI] g_wpeFilterManager pointer: " + std::string(g_wpeFilterManager ? "valid" : "null"));
            if (g_wpeFilterManager) {
                AB_LOG_INFO("[UI] 开始解析filter字段");
                json filterJson = msg.value("filter", json::object());
                AB_LOG_INFO("[UI] filter瀛楁瑙ｆ瀽瀹屾垚");
                AB_LOG_INFO("[UI] filterJson empty: " + std::string(filterJson.empty() ? "yes" : "no"));

                try {
                    std::string filterDump = filterJson.dump();
                    AB_LOG_INFO("[UI] 滤镜数据长度: " + std::to_string(filterDump.length()));
                } catch (const std::exception& e) {
                    AB_LOG_ERROR("[UI] 滤镜数据dump失败: " + std::string(e.what()));
                }

                WPEFilter::FilterInfo filter;
                filter.name = filterJson.value("name", "");
                filter.webDisplayName = filterJson.value("webDisplayName", "");
                filter.isEnabled = filterJson.value("enabled", true);

                std::string modeStr = filterJson.value("mode", "normal");
                filter.mode = (modeStr == "advanced") ? WPEFilter::FilterMode::Advanced : WPEFilter::FilterMode::Normal;

                std::string actionStr = filterJson.value("action", "replace");
                if (actionStr == "intercept") filter.action = WPEFilter::FilterAction::Intercept;
                else if (actionStr == "display") filter.action = WPEFilter::FilterAction::NoModify_Display;
                else if (actionStr == "nodisplay") filter.action = WPEFilter::FilterAction::NoModify_NoDisplay;
                else if (actionStr == "change") filter.action = WPEFilter::FilterAction::Change;
                else filter.action = WPEFilter::FilterAction::Replace;

                std::string startFromStr = filterJson.value("startFrom", "head");
                filter.startFrom = (startFromStr == "position") ? WPEFilter::FilterStartFrom::Position : WPEFilter::FilterStartFrom::Head;

                if (filterJson.contains("direction")) {
                    auto dirJson = filterJson["direction"];
                    filter.direction.applyToRequest = dirJson.value("request", true);
                    filter.direction.applyToResponse = dirJson.value("response", true);
                }

                if (filterJson.contains("target")) {
                    auto targetJson = filterJson["target"];
                    filter.target.applyToAllInstances = targetJson.value("allInstances", false);
                    if (targetJson.contains("targetInstanceIds") && targetJson["targetInstanceIds"].is_array()) {
                        filter.target.targetInstanceIds.clear();
                        for (const auto& id : targetJson["targetInstanceIds"]) {
                            if (id.is_string()) filter.target.targetInstanceIds.push_back(id.get<std::string>());
                        }
                    }
                }

                std::string priorityStr = filterJson.value("priority", "before");
                filter.priority = (priorityStr == "after") ? WPEFilter::FilterPriority::AfterHeartbeat : WPEFilter::FilterPriority::BeforeHeartbeat;

                std::string collPriorityStr = filterJson.value("collectorPriority", "after");
                filter.collectorPriority = (collPriorityStr == "after") ? WPEFilter::FilterPriority::AfterHeartbeat : WPEFilter::FilterPriority::BeforeHeartbeat;

                filter.searchPattern = filterJson.value("searchPattern", "");
                filter.modifyPattern = filterJson.value("modifyPattern", "");
                {
                    const std::string modifyRangeMode = filterJson.value("modifyRangeMode", "standard");
                    filter.useCustomModifyRange = (modifyRangeMode == "custom");
                    filter.modifyRangeMin = filterJson.value("modifyRangeMin", WPEFilter::kDefaultModifyRangeMin);
                    filter.modifyRangeMax = filterJson.value("modifyRangeMax", WPEFilter::kDefaultModifyRangeMax);
                    if (filter.modifyRangeMin >= 0) filter.modifyRangeMin = WPEFilter::kDefaultModifyRangeMin;
                    if (filter.modifyRangeMax <= 0) filter.modifyRangeMax = WPEFilter::kDefaultModifyRangeMax;
                    if (filter.modifyRangeMin >= filter.modifyRangeMax) {
                        filter.modifyRangeMin = WPEFilter::kDefaultModifyRangeMin;
                        filter.modifyRangeMax = WPEFilter::kDefaultModifyRangeMax;
                        filter.useCustomModifyRange = false;
                    }
                }

                filter.appointHeader = filterJson.value("appointHeader", false);
                filter.headerContent = filterJson.value("headerContent", "");
                filter.appointLength = filterJson.value("appointLength", false);
                filter.minLength = filterJson.value("minLength", 0);
                filter.maxLength = filterJson.value("maxLength", 65535);
                filter.appointPort = filterJson.value("appointPort", false);
                filter.portContent = filterJson.value("portContent", 0);

                if (filterJson.contains("progression")) {
                    auto progJson = filterJson["progression"];
                    filter.progression.isEnabled = progJson.value("isEnabled", false);
                    filter.progression.isContinuous = progJson.value("isContinuous", false);
                    filter.progression.step = progJson.value("step", 1);
                    filter.progression.enableCarry = progJson.value("enableCarry", false);
                    filter.progression.carryDigits = progJson.value("carryDigits", 1);
                    filter.progression.positions = progJson.value("positions", "");
                }

                if (filterJson.contains("advancedToggle")) {
                    auto advJson = filterJson["advancedToggle"];
                    filter.advancedToggle.isEnabled = advJson.value("isEnabled", false);
                    filter.advancedToggle.defaultState = advJson.value("defaultState", true);
                    filter.advancedToggle.enableTriggerEnabled = advJson.value("enableTriggerEnabled", false);
                    filter.advancedToggle.enablePattern = advJson.value("enablePattern", "");
                    filter.advancedToggle.disableTriggerEnabled = advJson.value("disableTriggerEnabled", false);
                    filter.advancedToggle.disablePattern = advJson.value("disablePattern", "");
                    filter.advancedToggle.applyOnEnableTrigger = advJson.value("applyOnEnableTrigger", false);
                    filter.advancedToggle.applyOnDisableTrigger = advJson.value("applyOnDisableTrigger", true);
                }

                int filterId = g_wpeFilterManager->AddFilter(filter);
                if (filterId > 0) {
                    UIBridge_SaveWPEConfigWithFeedback(true, "创建成功", "滤镜已创建 (ID: " + std::to_string(filterId) + ")");
                    UIBridge_HandleMessage("{\"action\":\"get_wpe_filters\"}");
                    UIBridge_RequestImmediateWpeCloudSync("wpe_filter_create_full");
                } else {
                    // 🔥 显示详细的错误信�?
                    std::string errorMsg = "无法创建滤镜";
                    if (filterId == -1) {
                        errorMsg = "Permission check failed, please verify cloud account permission";
                    }
                    UIBridge_Toast("error", "创建失败", errorMsg);
                    AB_LOG_ERROR("[UI] WPE滤镜创建失败，返回ID: " + std::to_string(filterId));
                }
            }
        }
        else if (action == "wpe_filter_get") {
            if (g_wpeFilterManager) {
                int filterId = msg.value("filterId", 0);
                auto filterPtr = g_wpeFilterManager->GetFilter(filterId);

                if (filterPtr) {
                    json response;
                    response["type"] = "wpe_filter_detail";

                    json fj;
                    fj["id"] = filterPtr->id;
                    fj["name"] = filterPtr->name;
                    fj["webDisplayName"] = filterPtr->webDisplayName;
                    fj["enabled"] = filterPtr->isEnabled;
                    fj["executionCount"] = filterPtr->executionCount;

                    fj["mode"] = (filterPtr->mode == WPEFilter::FilterMode::Normal) ? "normal" : "advanced";

                    std::string actionStr = "replace";
                    switch (filterPtr->action) {
                        case WPEFilter::FilterAction::Replace: actionStr = "replace"; break;
                        case WPEFilter::FilterAction::Intercept: actionStr = "intercept"; break;
                        case WPEFilter::FilterAction::NoModify_Display: actionStr = "display"; break;
                        case WPEFilter::FilterAction::NoModify_NoDisplay: actionStr = "nodisplay"; break;
                        case WPEFilter::FilterAction::Change: actionStr = "change"; break;
                        default: actionStr = "replace"; break;
                    }
                    fj["action"] = actionStr;

                    fj["startFrom"] = (filterPtr->startFrom == WPEFilter::FilterStartFrom::Head) ? "head" : "position";

                    json dir;
                    dir["request"] = filterPtr->direction.applyToRequest;
                    dir["response"] = filterPtr->direction.applyToResponse;
                    fj["direction"] = dir;

                    json target;
                    target["allInstances"] = filterPtr->target.applyToAllInstances;
                    target["targetInstanceIds"] = filterPtr->target.targetInstanceIds;
                    fj["target"] = target;

                    fj["global"] = filterPtr->target.applyToAllInstances;
                    fj["priority"] = (filterPtr->priority == WPEFilter::FilterPriority::BeforeHeartbeat) ? "before" : "after";
                    fj["collectorPriority"] = (filterPtr->collectorPriority == WPEFilter::FilterPriority::BeforeHeartbeat) ? "before" : "after";

                    fj["searchPattern"] = filterPtr->searchPattern;
                    fj["modifyPattern"] = filterPtr->modifyPattern;
                    fj["modifyRangeMode"] = filterPtr->useCustomModifyRange ? "custom" : "standard";
                    fj["modifyRangeMin"] = filterPtr->modifyRangeMin;
                    fj["modifyRangeMax"] = filterPtr->modifyRangeMax;

                    fj["appointHeader"] = filterPtr->appointHeader;
                    fj["headerContent"] = filterPtr->headerContent;
                    fj["appointLength"] = filterPtr->appointLength;
                    fj["minLength"] = filterPtr->minLength;
                    fj["maxLength"] = filterPtr->maxLength;
                    fj["appointPort"] = filterPtr->appointPort;
                    fj["portContent"] = filterPtr->portContent;

                    json prog;
                    prog["isEnabled"] = filterPtr->progression.isEnabled;
                    prog["isContinuous"] = filterPtr->progression.isContinuous;
                    prog["step"] = filterPtr->progression.step;
                    prog["enableCarry"] = filterPtr->progression.enableCarry;
                    prog["carryDigits"] = filterPtr->progression.carryDigits;
                    prog["positions"] = filterPtr->progression.positions;
                    fj["progression"] = prog;

                    json adv;
                    adv["isEnabled"] = filterPtr->advancedToggle.isEnabled;
                    adv["defaultState"] = filterPtr->advancedToggle.defaultState;
                    adv["enableTriggerEnabled"] = filterPtr->advancedToggle.enableTriggerEnabled;
                    adv["enablePattern"] = filterPtr->advancedToggle.enablePattern;
                    adv["disableTriggerEnabled"] = filterPtr->advancedToggle.disableTriggerEnabled;
                    adv["disablePattern"] = filterPtr->advancedToggle.disablePattern;
                    adv["applyOnEnableTrigger"] = filterPtr->advancedToggle.applyOnEnableTrigger;
                    adv["applyOnDisableTrigger"] = filterPtr->advancedToggle.applyOnDisableTrigger;
                    fj["advancedToggle"] = adv;

                    fj["createTime"] = filterPtr->createTime;
                    fj["lastModifyTime"] = filterPtr->lastModifyTime;

                    response["filter"] = fj;
                    UIBridge_PushMessage(response.dump());
                }
            }
        }
        else if (action == "wpe_filter_update") {
            AB_LOG_INFO("[UI] 收到更新WPE滤镜请求");
            if (g_wpeFilterManager) {
                int filterId = msg.value("filterId", 0);
                json filterJson = msg.value("filter", json::object());
                AB_LOG_INFO("[UI] 更新滤镜ID: " + std::to_string(filterId) + ", 数据: " + filterJson.dump());

                auto existingPtr = g_wpeFilterManager->GetFilter(filterId);
                if (!existingPtr) {
                    UIBridge_Toast("error", "Save failed", "Filter not found");
                } else {
                    WPEFilter::FilterInfo filter = *existingPtr;
                    filter.id = filterId;
                    filter.name = filterJson.value("name", filter.name);
                    filter.webDisplayName = filterJson.value("webDisplayName", filter.webDisplayName);
                    filter.isEnabled = filterJson.value("enabled", filter.isEnabled);

                    std::string modeStr = filterJson.value("mode", "");
                    if (!modeStr.empty()) {
                        filter.mode = (modeStr == "advanced") ? WPEFilter::FilterMode::Advanced : WPEFilter::FilterMode::Normal;
                    }

                    std::string actionStr = filterJson.value("action", "");
                    if (!actionStr.empty()) {
                        if (actionStr == "intercept") filter.action = WPEFilter::FilterAction::Intercept;
                        else if (actionStr == "display") filter.action = WPEFilter::FilterAction::NoModify_Display;
                        else if (actionStr == "nodisplay") filter.action = WPEFilter::FilterAction::NoModify_NoDisplay;
                        else if (actionStr == "change") filter.action = WPEFilter::FilterAction::Change;
                        else filter.action = WPEFilter::FilterAction::Replace;
                    }

                    std::string startFromStr = filterJson.value("startFrom", "");
                    if (!startFromStr.empty()) {
                        filter.startFrom = (startFromStr == "position") ? WPEFilter::FilterStartFrom::Position : WPEFilter::FilterStartFrom::Head;
                    }

                    if (filterJson.contains("direction")) {
                        auto dirJson = filterJson["direction"];
                        filter.direction.applyToRequest = dirJson.value("request", filter.direction.applyToRequest);
                        filter.direction.applyToResponse = dirJson.value("response", filter.direction.applyToResponse);
                    }

                    if (filterJson.contains("target")) {
                        auto targetJson = filterJson["target"];
                        filter.target.applyToAllInstances = targetJson.value("allInstances", filter.target.applyToAllInstances);
                        if (targetJson.contains("targetInstanceIds") && targetJson["targetInstanceIds"].is_array()) {
                            filter.target.targetInstanceIds.clear();
                            for (const auto& id : targetJson["targetInstanceIds"]) {
                                if (id.is_string()) filter.target.targetInstanceIds.push_back(id.get<std::string>());
                            }
                        }
                    }

                    std::string priorityStr = filterJson.value("priority", "");
                    if (!priorityStr.empty()) {
                        filter.priority = (priorityStr == "after") ? WPEFilter::FilterPriority::AfterHeartbeat : WPEFilter::FilterPriority::BeforeHeartbeat;
                    }

                    std::string collPriorityStr = filterJson.value("collectorPriority", "");
                    if (!collPriorityStr.empty()) {
                        filter.collectorPriority = (collPriorityStr == "after") ? WPEFilter::FilterPriority::AfterHeartbeat : WPEFilter::FilterPriority::BeforeHeartbeat;
                    }

                    if (filterJson.contains("searchPattern")) filter.searchPattern = filterJson.value("searchPattern", "");
                    if (filterJson.contains("modifyPattern")) filter.modifyPattern = filterJson.value("modifyPattern", "");
                    if (filterJson.contains("modifyRangeMode")) {
                        const std::string modifyRangeMode = filterJson.value("modifyRangeMode", "standard");
                        filter.useCustomModifyRange = (modifyRangeMode == "custom");
                    }
                    if (filterJson.contains("modifyRangeMin")) {
                        filter.modifyRangeMin = filterJson.value("modifyRangeMin", WPEFilter::kDefaultModifyRangeMin);
                    }
                    if (filterJson.contains("modifyRangeMax")) {
                        filter.modifyRangeMax = filterJson.value("modifyRangeMax", WPEFilter::kDefaultModifyRangeMax);
                    }
                    if (filter.modifyRangeMin >= 0) filter.modifyRangeMin = WPEFilter::kDefaultModifyRangeMin;
                    if (filter.modifyRangeMax <= 0) filter.modifyRangeMax = WPEFilter::kDefaultModifyRangeMax;
                    if (filter.modifyRangeMin >= filter.modifyRangeMax) {
                        filter.modifyRangeMin = WPEFilter::kDefaultModifyRangeMin;
                        filter.modifyRangeMax = WPEFilter::kDefaultModifyRangeMax;
                        filter.useCustomModifyRange = false;
                    }

                    if (filterJson.contains("appointHeader")) filter.appointHeader = filterJson.value("appointHeader", false);
                    if (filterJson.contains("headerContent")) filter.headerContent = filterJson.value("headerContent", "");
                    if (filterJson.contains("appointLength")) filter.appointLength = filterJson.value("appointLength", false);
                    if (filterJson.contains("minLength")) filter.minLength = filterJson.value("minLength", 0);
                    if (filterJson.contains("maxLength")) filter.maxLength = filterJson.value("maxLength", 65535);
                    if (filterJson.contains("appointPort")) filter.appointPort = filterJson.value("appointPort", false);
                    if (filterJson.contains("portContent")) filter.portContent = filterJson.value("portContent", 0);

                    if (filterJson.contains("progression")) {
                        auto progJson = filterJson["progression"];
                        filter.progression.isEnabled = progJson.value("isEnabled", filter.progression.isEnabled);
                        filter.progression.isContinuous = progJson.value("isContinuous", filter.progression.isContinuous);
                        filter.progression.step = progJson.value("step", filter.progression.step);
                        filter.progression.enableCarry = progJson.value("enableCarry", filter.progression.enableCarry);
                        filter.progression.carryDigits = progJson.value("carryDigits", filter.progression.carryDigits);
                        filter.progression.positions = progJson.value("positions", filter.progression.positions);
                    }

                    if (filterJson.contains("advancedToggle")) {
                        auto advJson = filterJson["advancedToggle"];
                        filter.advancedToggle.isEnabled = advJson.value("isEnabled", filter.advancedToggle.isEnabled);
                        filter.advancedToggle.defaultState = advJson.value("defaultState", filter.advancedToggle.defaultState);
                        filter.advancedToggle.enableTriggerEnabled = advJson.value("enableTriggerEnabled", filter.advancedToggle.enableTriggerEnabled);
                        filter.advancedToggle.enablePattern = advJson.value("enablePattern", filter.advancedToggle.enablePattern);
                        filter.advancedToggle.disableTriggerEnabled = advJson.value("disableTriggerEnabled", filter.advancedToggle.disableTriggerEnabled);
                        filter.advancedToggle.disablePattern = advJson.value("disablePattern", filter.advancedToggle.disablePattern);
                        filter.advancedToggle.applyOnEnableTrigger = advJson.value("applyOnEnableTrigger", filter.advancedToggle.applyOnEnableTrigger);
                        filter.advancedToggle.applyOnDisableTrigger = advJson.value("applyOnDisableTrigger", filter.advancedToggle.applyOnDisableTrigger);
                    }

                    if (g_wpeFilterManager->UpdateFilter(filterId, filter)) {
                        UIBridge_SaveWPEConfigWithFeedback(true, "Save succeeded", "Filter updated");
                        UIBridge_HandleMessage("{\"action\":\"get_wpe_filters\"}");
                        UIBridge_RequestImmediateWpeCloudSync("wpe_filter_update");
                    } else {
                        // 🔥 显示详细的错误信�?
                        UIBridge_Toast("error", "Save failed", "Permission check failed or filter not found");
                        AB_LOG_ERROR("[UI] WPE滤镜更新失败，ID: " + std::to_string(filterId));
                    }
                }
            }
        }
        else if (action == "wpe_filter_toggle") {
            if (g_wpeFilterManager) {
                int filterId = msg.value("filterId", 0);
                bool enabled = msg.value("enabled", true);

                g_wpeFilterManager->EnableFilter(filterId, enabled);
                UIBridge_SaveWPEConfigWithFeedback(true,
                              enabled ? "Enabled" : "Disabled",
                              "Filter has been " + std::string(enabled ? "enabled" : "disabled"));
                UIBridge_HandleMessage("{\"action\":\"get_wpe_filters\"}");
                UIBridge_RequestImmediateWpeCloudSync("wpe_filter_toggle");
            }
        }
        else if (action == "wpe_filter_delete") {
            if (g_wpeFilterManager) {
                int filterId = msg.value("filterId", 0);
                AB_LOG_INFO("[UI] 尝试删除WPE滤镜，ID: " + std::to_string(filterId));
                if (g_wpeFilterManager->RemoveFilter(filterId)) {
                    UIBridge_SaveWPEConfigWithFeedback(true, "Delete succeeded", "Filter deleted");
                    UIBridge_HandleMessage("{\"action\":\"get_wpe_filters\"}");
                    UIBridge_RequestImmediateWpeCloudSync("wpe_filter_delete");
                } else {
                    AB_LOG_ERROR("[UI] WPE滤镜删除失败，ID: " + std::to_string(filterId));
                    UIBridge_Toast("error", "Delete failed", "Permission check failed or filter not found");
                }
            }
        }
        else if (action == "wpe_filter_move") {
            if (g_wpeFilterManager) {
                int filterId = msg.value("filterId", 0);
                std::string direction = msg.value("direction", "");
                bool result = false;
                std::string moveError;
                if (direction == "up") result = g_wpeFilterManager->MoveFilterUp(filterId, &moveError);
                else if (direction == "down") result = g_wpeFilterManager->MoveFilterDown(filterId, &moveError);
                else moveError = "未知的移动方向";
                if (result) {
                    UIBridge_SaveWPEConfigWithFeedback(false, "", "");
                    UIBridge_HandleMessage("{\"action\":\"get_wpe_filters\"}");
                    UIBridge_RequestImmediateWpeCloudSync("wpe_filter_move");
                } else {
                    const std::string normalizedError = moveError.empty() ? std::string("WPE滤镜移动失败") : Logger::NormalizeTextForDisplay(moveError);
                    AB_LOG_WARNING_CAT(LOG_CAT_LOGIN_AUTH,
                        "[WPE滤镜] 移动失败(ID=" + std::to_string(filterId) + ", direction=" + direction + "): " + normalizedError);
                    UIBridge_Toast("warning", "移动失败", normalizedError);
                }
            }
        }
        else if (action == "save_wpe_web_display_names") {
            if (!g_wpeFilterManager) {
                UIBridge_Toast("error", "保存失败", "WPE滤镜管理器未初始化");
            } else if (!msg.contains("items") || !msg["items"].is_array()) {
                UIBridge_Toast("warning", "保存失败", "缺少显示名称列表");
            } else {
                int updated = 0;
                for (const auto& item : msg["items"]) {
                    int filterId = item.value("filterId", 0);
                    std::string displayName = UIBridge_TrimCopy(item.value("webDisplayName", ""));
                    auto* filterPtr = g_wpeFilterManager->GetFilter(filterId);
                    if (!filterPtr) continue;

                    WPEFilter::FilterInfo copy = *filterPtr;
                    copy.webDisplayName = displayName;
                    if (g_wpeFilterManager->UpdateFilter(filterId, copy)) {
                        ++updated;
                    }
                }

                UIBridge_SaveWPEConfigWithFeedback(false, "", "");
                UIBridge_Toast("success", "保存成功",
                    "已更新 " + std::to_string(updated) + " 个用户Web显示名称");
                UIBridge_HandleMessage("{\"action\":\"get_wpe_filters\"}");
            }
        }
        else if (action == "wpe_filter_clear_stats") {
            if (g_wpeFilterManager) {
                g_wpeFilterManager->ResetStatistics();
                UIBridge_SaveWPEConfigWithFeedback(true, "Cleared", "All filter statistics have been reset");
                UIBridge_HandleMessage("{\"action\":\"get_wpe_filters\"}");
                UIBridge_RequestImmediateWpeCloudSync("wpe_filter_clear_stats");
            }
        }
        else if (action == "wpe_filters_import") {
            if (g_wpeFilterManager) {
                std::string jsonStr = msg.value("json", "");
                if (!jsonStr.empty()) {
                    if (g_wpeFilterManager->ImportFromJsonMerge(jsonStr)) {
                        UIBridge_SaveWPEConfigWithFeedback(true, "Import succeeded", "Filters imported");
                        UIBridge_HandleMessage("{\"action\":\"get_wpe_filters\"}");
                        UIBridge_RequestImmediateWpeCloudSync("wpe_filters_import");
                    } else {
                        UIBridge_Toast("error", "导入失败", "无法导入滤镜");
                    }
                }
            }
        }
        else if (action == "wpe_filters_export") {
            if (g_wpeFilterManager) {
                std::string exportJson = g_wpeFilterManager->ExportToJson();

                json response;
                response["type"] = "socks5_accounts_export";
                response["success"] = true;
                response["text"] = exportJson;
                UIBridge_PushMessage(response.dump());
            }
        }
        else if (action == "wpe_filter_export_file") {
            if (!g_wpeFilterManager) {
                UIBridge_Toast("error", "Export failed", "Filter manager not initialized");
            } else {
                auto filterIds = msg.value("filterIds", json::array());
                bool usePassword = msg.value("usePassword", false);
                std::string password = msg.value("password", "");

                if (filterIds.empty()) {
                    UIBridge_Toast("error", "导出失败", "未选择任何滤镜");
                } else if (usePassword && password.empty()) {
                    UIBridge_Toast("error", "导出失败", "已启用密码保护但密码为空");
                } else {
                    OPENFILENAMEA ofn;
                    char szFile[260] = "wpe_filters.abwpe";
                    ZeroMemory(&ofn, sizeof(ofn));
                    ofn.lStructSize = sizeof(ofn);
                    ofn.hwndOwner = GetActiveWindow();
                    ofn.lpstrFile = szFile;
                    ofn.nMaxFile = sizeof(szFile);
                    ofn.lpstrFilter = "AB2 WPE Filters\0*.abwpe\0All Files\0*.*\0";
                    ofn.nFilterIndex = 1;
                    ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;

                    if (GetSaveFileNameA(&ofn)) {
                        auto allFilters = g_wpeFilterManager->GetAllFilters();
                        json root;
                        root["version"] = 2;
                        json arr = json::array();

                        for (const auto& f : allFilters) {
                            bool selected = false;
                            for (const auto& idVal : filterIds) {
                                if (idVal.is_number() && idVal.get<int>() == f.id) {
                                    selected = true;
                                    break;
                                }
                            }
                            if (!selected) continue;

                            json item;
                            item["id"] = f.id;
                            item["name"] = f.name;
                            item["webDisplayName"] = f.webDisplayName;
                            item["isEnabled"] = f.isEnabled;
                            item["executionCount"] = f.executionCount;
                            item["mode"] = static_cast<int>(f.mode);
                            item["action"] = static_cast<int>(f.action);
                            item["startFrom"] = static_cast<int>(f.startFrom);
                            item["priority"] = static_cast<int>(f.priority);
                            item["collectorPriority"] = static_cast<int>(f.collectorPriority);
                            item["appointHeader"] = f.appointHeader;
                            item["headerContent"] = f.headerContent;
                            item["appointLength"] = f.appointLength;
                            item["minLength"] = f.minLength;
                            item["maxLength"] = f.maxLength;
                            item["appointPort"] = f.appointPort;
                            item["portContent"] = f.portContent;
                            item["searchPattern"] = f.searchPattern;
                            item["modifyPattern"] = f.modifyPattern;
                            item["modifyRangeMode"] = f.useCustomModifyRange ? "custom" : "standard";
                            item["modifyRangeMin"] = f.modifyRangeMin;
                            item["modifyRangeMax"] = f.modifyRangeMax;
                            item["applyToRequest"] = f.direction.applyToRequest;
                            item["applyToResponse"] = f.direction.applyToResponse;
                            item["applyToAllInstances"] = f.target.applyToAllInstances;
                            item["progressionEnabled"] = f.progression.isEnabled;
                            item["progressionContinuous"] = f.progression.isContinuous;
                            item["progressionStep"] = f.progression.step;
                            item["progressionEnableCarry"] = f.progression.enableCarry;
                            item["progressionCarryDigits"] = f.progression.carryDigits;
                            item["progressionPositions"] = f.progression.positions;
                            item["createTime"] = f.createTime;
                            item["lastModifyTime"] = f.lastModifyTime;
                            arr.push_back(item);
                        }
                        root["filters"] = arr;

                        const std::string plainJson = root.dump();

                        WpeAesFileHeader hdr{};
                        const uint8_t magic[8] = { 'A','B','2','W','P','E','F','1' };
                        memcpy(hdr.magic, magic, 8);
                        hdr.version = 1;
                        hdr.flags = usePassword ? 0x1 : 0x0;

                        if (BCryptGenRandom(nullptr, hdr.salt, sizeof(hdr.salt), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0 ||
                            BCryptGenRandom(nullptr, hdr.iv, sizeof(hdr.iv), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
                            UIBridge_Toast("error", "Export failed", "Random generator failed");
                        } else {
                            const std::string kctx = "AB2.WPE.Export";
                            std::vector<uint8_t> ksrc;
                            ksrc.reserve(sizeof(hdr.salt) + password.size() + kctx.size());
                            ksrc.insert(ksrc.end(), hdr.salt, hdr.salt + sizeof(hdr.salt));
                            ksrc.insert(ksrc.end(), password.begin(), password.end());
                            ksrc.insert(ksrc.end(), kctx.begin(), kctx.end());
                            uint8_t key[32];
                            if (!WPE_SHA256(ksrc.data(), ksrc.size(), key)) {
                                UIBridge_Toast("error", "导出失败", "SHA256失败");
                            } else {
                                hdr.plainLen = (uint32_t)plainJson.size();
                                const size_t maxLen = plainJson.size() + TinyAES::AES_BLOCKLEN;
                                std::vector<uint8_t> buf(maxLen);
                                memcpy(buf.data(), plainJson.data(), plainJson.size());
                                const size_t paddedLen = TinyAES::PKCS7_Pad(buf.data(), plainJson.size(), buf.size());
                                if (paddedLen == 0 || (paddedLen % TinyAES::AES_BLOCKLEN) != 0) {
                                    UIBridge_Toast("error", "导出失败", "PKCS7填充失败");
                                } else {
                                    buf.resize(paddedLen);
                                    TinyAES::AES_ctx ctx;
                                    TinyAES::AES_init_ctx_iv(&ctx, key, hdr.iv);
                                    TinyAES::AES_CBC_encrypt_buffer(&ctx, buf.data(), buf.size());
                                    hdr.cipherLen = (uint32_t)buf.size();

                                    try {
                                        std::ofstream outFile(szFile, std::ios::binary);
                                        if (!outFile.is_open()) {
                                            UIBridge_Toast("error", "导出失败", "无法写入文件");
                                        } else {
                                            outFile.write((const char*)&hdr, sizeof(hdr));
                                            outFile.write((const char*)buf.data(), buf.size());
                                            outFile.close();
                                            UIBridge_Toast("success", "导出成功", std::to_string(filterIds.size()) + " 个滤镜已导出");
                                        }
                                    } catch (...) {
                                        UIBridge_Toast("error", "Export failed", "File write exception");
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        else if (action == "wpe_filter_import_file") {
            OPENFILENAMEA ofn;
            char szFile[260] = "";
            ZeroMemory(&ofn, sizeof(ofn));
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = GetActiveWindow();
            ofn.lpstrFile = szFile;
            ofn.nMaxFile = sizeof(szFile);
            ofn.lpstrFilter = "AB2 WPE Filters\0*.abwpe\0JSON Files\0*.json\0All Files\0*.*\0";
            ofn.nFilterIndex = 1;
            ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

            if (GetOpenFileNameA(&ofn)) {
                g_wpeImportPendingPath = szFile;
                g_wpeImportPendingBytes.clear();
                g_wpeImportIsEncrypted = false;
                g_wpeImportNeedPassword = false;

                try {
                    std::ifstream f(szFile, std::ios::binary);
                    if (!f.is_open()) {
                        UIBridge_Toast("error", "导入失败", "无法打开文件");
                    } else {
                        f.seekg(0, std::ios::end);
                        const std::streamoff size = f.tellg();
                        f.seekg(0, std::ios::beg);
                        if (size <= 0) {
                            UIBridge_Toast("error", "导入失败", "文件为空");
                        } else {
                            g_wpeImportPendingBytes.resize((size_t)size);
                            f.read((char*)g_wpeImportPendingBytes.data(), size);
                            f.close();

                            // 检测加�?
                            if (g_wpeImportPendingBytes.size() >= 8) {
                                const uint8_t magic[8] = { 'A','B','2','W','P','E','F','1' };
                                g_wpeImportIsEncrypted = (memcmp(g_wpeImportPendingBytes.data(), magic, 8) == 0);
                            }

                            if (g_wpeImportIsEncrypted && g_wpeImportPendingBytes.size() >= sizeof(WpeAesFileHeader)) {
                                const size_t flagsOffset = 8 + 4;
                                g_wpeImportNeedPassword = (g_wpeImportPendingBytes[flagsOffset] & 0x1) != 0;
                            }

                            json response;
                            response["type"] = "wpe_import_options";
                            response["encrypted"] = g_wpeImportIsEncrypted;
                            response["needPassword"] = g_wpeImportNeedPassword;
                            response["filePath"] = UIBridge_AnsiToUtf8Path(g_wpeImportPendingPath);
                            UIBridge_PushMessage(response.dump());
                        }
                    }
                } catch (const std::exception& e) {
                    std::string errMsg = std::string("读取文件异常: ") + e.what();
                    UIBridge_Toast("error", "导入失败", errMsg);
                } catch (...) {
                    UIBridge_Toast("error", "导入失败", "读取文件异常(未知错误)");
                }
            }
        }
        else if (action == "wpe_filter_import_confirm") {
            if (!g_wpeFilterManager) {
                UIBridge_Toast("error", "Import failed", "Filter manager not initialized");
            } else if (g_wpeImportPendingBytes.empty()) {
                UIBridge_Toast("error", "导入失败", "无待导入数据");
            } else {
                std::string mode = msg.value("mode", "merge");
                std::string password = msg.value("password", "");

                bool ok = false;
                std::string err = "未知错误";

                if (!g_wpeImportIsEncrypted) {
                    std::string jsonStr((const char*)g_wpeImportPendingBytes.data(), g_wpeImportPendingBytes.size());
                    std::string validateError;
                    if (!UIBridge_ValidateWpeImportJson(jsonStr, validateError)) {
                        err = "文件内容不是有效的 JSON 滤镜配置：" + validateError;
                    } else {
                        if (mode == "overwrite") {
                            ok = g_wpeFilterManager->ImportFromJsonOverwrite(jsonStr);
                        } else {
                            ok = g_wpeFilterManager->ImportFromJsonMerge(jsonStr);
                        }
                        err = ok ? "" : "JSON 结构有效，但导入失败（可能结构不兼容或被门禁拒绝）";
                    }
                } else {
                    if (g_wpeImportPendingBytes.size() < sizeof(WpeAesFileHeader)) {
                        err = "文件过短";
                    } else {
                        WpeAesFileHeader hdr{};
                        memcpy(&hdr, g_wpeImportPendingBytes.data(), sizeof(hdr));
                        const uint8_t magic[8] = { 'A','B','2','W','P','E','F','1' };
                        if (memcmp(hdr.magic, magic, 8) != 0 || hdr.version != 1) {
                            err = "加密头不匹配";
                        } else {
                            const size_t remain = g_wpeImportPendingBytes.size() - sizeof(hdr);
                            if (hdr.cipherLen == 0 || hdr.cipherLen > remain || (hdr.cipherLen % TinyAES::AES_BLOCKLEN) != 0) {
                                err = "Invalid cipher length";
                            } else if ((hdr.flags & 0x1) != 0 && password.empty()) {
                                err = "This file requires a password";
                            } else {
                                const std::string kctx = "AB2.WPE.Export";
                                std::vector<uint8_t> ksrc;
                                ksrc.reserve(sizeof(hdr.salt) + password.size() + kctx.size());
                                ksrc.insert(ksrc.end(), hdr.salt, hdr.salt + sizeof(hdr.salt));
                                ksrc.insert(ksrc.end(), password.begin(), password.end());
                                ksrc.insert(ksrc.end(), kctx.begin(), kctx.end());
                                uint8_t key[32];
                                if (!WPE_SHA256(ksrc.data(), ksrc.size(), key)) {
                                    err = "SHA256失败";
                                } else {
                                    std::vector<uint8_t> cipher(hdr.cipherLen);
                                    memcpy(cipher.data(), g_wpeImportPendingBytes.data() + sizeof(hdr), hdr.cipherLen);
                                    TinyAES::AES_ctx ctx;
                                    TinyAES::AES_init_ctx_iv(&ctx, key, hdr.iv);
                                    TinyAES::AES_CBC_decrypt_buffer(&ctx, cipher.data(), cipher.size());
                                    const size_t plainLen = TinyAES::PKCS7_Unpad(cipher.data(), cipher.size());
                                    if (plainLen == 0 || plainLen > cipher.size()) {
                                        err = "Decrypt failed or password incorrect";
                                    } else {
                                        std::string jsonStr((const char*)cipher.data(), plainLen);
                                        std::string validateError;
                                        if (!UIBridge_ValidateWpeImportJson(jsonStr, validateError)) {
                                            err = "解密成功，但内容不是有效的 JSON 滤镜配置（可能密码错误或文件已损坏）：" + validateError;
                                        } else {
                                            if (mode == "overwrite") {
                                                ok = g_wpeFilterManager->ImportFromJsonOverwrite(jsonStr);
                                            } else {
                                                ok = g_wpeFilterManager->ImportFromJsonMerge(jsonStr);
                                            }
                                            err = ok ? "" : "解密成功，JSON 结构有效，但导入失败（可能结构不兼容或被门禁拒绝）";
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                if (ok) {
                    UIBridge_SaveWPEConfigWithFeedback(true, "Import succeeded", "Filters imported");
                    UIBridge_HandleMessage("{\"action\":\"get_wpe_filters\"}");
                    UIBridge_RequestImmediateWpeCloudSync("wpe_filter_import_confirm");
                } else {
                    UIBridge_Toast("error", "导入失败", err);
                }

                g_wpeImportPendingBytes.clear();
                g_wpeImportPendingPath.clear();
            }
        }

        // ========== SocksForward Instance Config ==========
        else if (action == "config_get_proxy") {
            std::string instanceId = msg.value("instanceId", "");
            auto& instMgr = InstanceManager::GetInstance();
            auto* instance = instMgr.GetSocksForwardInstance(instanceId);

            json response;
            response["type"] = "config_proxy";

            if (instance) {
                // 🔥 使用 GetCachedConfig 读取配置
                auto config = instance->GetCachedConfig();
                response["enabled"] = config.enableSecondaryProxy;
                response["host"] = config.secondaryProxyHost;
                response["port"] = config.secondaryProxyPort;
                response["username"] = config.secondaryProxyUsername;
                response["password"] = config.secondaryProxyPassword;
            } else if (g_database) {
                // Fallback: read from database
                std::string enableStr = g_database->GetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(instanceId, "enableSecondaryProxy"), "0");
                response["enabled"] = (enableStr == "1");
                response["host"] = g_database->GetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(instanceId, "secondaryProxyHost"), "127.0.0.1");
                std::string portStr = g_database->GetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(instanceId, "secondaryProxyPort"), "1080");
                response["port"] = std::stoi(portStr);
                response["username"] = g_database->GetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(instanceId, "secondaryProxyUsername"), "");
                response["password"] = g_database->GetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(instanceId, "secondaryProxyPassword"), "");
            }

            UIBridge_PushMessage(response.dump());
        }
        else if (action == "config_set_proxy") {
            std::string instanceId = msg.value("instanceId", "");
            bool enabled = msg.value("enabled", false);
            std::string host = msg.value("host", "127.0.0.1");
            int port = msg.value("port", 1080);
            std::string username = msg.value("username", "");
            std::string password = msg.value("password", "");

            auto& instMgr = InstanceManager::GetInstance();
            auto* instance = instMgr.GetSocksForwardInstance(instanceId);

            if (instance) {
                // 🔥 使用 UpdateConfig 方法更新配置（会自动保存到数据库并应用）
                auto config = instance->GetCachedConfig();
                config.enableSecondaryProxy = enabled;
                config.secondaryProxyHost = host;
                config.secondaryProxyPort = port;
                config.secondaryProxyUsername = username;
                config.secondaryProxyPassword = password;
                instance->UpdateConfig(config);

                UIBridge_Toast("success", "Save succeeded", "Secondary proxy config saved");
            } else {
                UIBridge_Toast("error", "Save failed", "Instance not found");
            }
        }
        else if (action == "config_get_thread") {
            std::string instanceId = msg.value("instanceId", "");
            auto& instMgr = InstanceManager::GetInstance();
            auto* instance = instMgr.GetSocksForwardInstance(instanceId);

            json response;
            response["type"] = "config_thread";
            response["mode"] = 0;
            response["whitelistPoolSize"] = 10;
            response["normalPoolSize"] = 50;
            response["iocpMaxWhitelist"] = 2000;
            response["iocpMaxNormal"] = 500;

            if (instance && instance->GetCollector()) {
                auto currentMode = instance->GetCollector()->GetThreadPoolMode();
                if (currentMode == PacketCollector::ThreadPoolMode::TRADITIONAL) response["mode"] = 0;
                else if (currentMode == PacketCollector::ThreadPoolMode::BLOCKING) response["mode"] = 1;
                else if (currentMode == PacketCollector::ThreadPoolMode::IOCP) response["mode"] = 2;
            }

            // Load from database if available
            if (g_database) {
                std::string modeStr = g_database->GetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(instanceId, "threadPoolMode"), "0");
                response["mode"] = std::stoi(modeStr);

                AB_LOG_INFO("[UI Bridge] 瀹炰緥 " + instanceId + " 绾跨▼妯″紡: " + modeStr + " (mode=" + std::to_string(response["mode"].get<int>()) + ")");

                std::string wlPool = g_database->GetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(instanceId, "whitelistPoolSize"), "10");
                response["whitelistPoolSize"] = std::stoi(wlPool);

                std::string nlPool = g_database->GetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(instanceId, "normalPoolSize"), "50");
                response["normalPoolSize"] = std::stoi(nlPool);

                std::string iocpWl = g_database->GetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(instanceId, "iocpMaxWhitelist"), "2000");
                response["iocpMaxWhitelist"] = std::stoi(iocpWl);

                std::string iocpNl = g_database->GetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(instanceId, "iocpMaxNormal"), "500");
                response["iocpMaxNormal"] = std::stoi(iocpNl);
            }

            UIBridge_PushMessage(response.dump());
        }
        else if (action == "config_set_thread_mode") {
            std::string instanceId = msg.value("instanceId", "");
            int mode = msg.value("mode", 0);

            auto& instMgr = InstanceManager::GetInstance();
            auto* instance = instMgr.GetSocksForwardInstance(instanceId);

            if (instance) {
                // 馃敟 浣跨敤 UpdateConfig 鏂规硶
                auto config = instance->GetCachedConfig();
                config.threadPoolMode = mode;
                instance->UpdateConfig(config);
                UIBridge_Toast("success", "Applied", "Thread mode applied");
            } else {
                UIBridge_Toast("error", "Apply failed", "Instance not found");
            }
        }
        else if (action == "config_set_pool_size") {
            std::string instanceId = msg.value("instanceId", "");
            int wlSize = msg.value("whitelistPoolSize", 10);
            int nlSize = msg.value("normalPoolSize", 50);

            auto& instMgr = InstanceManager::GetInstance();
            auto* instance = instMgr.GetSocksForwardInstance(instanceId);

            if (instance) {
                // 馃敟 浣跨敤 UpdateConfig 鏂规硶
                auto config = instance->GetCachedConfig();
                config.whitelistPoolSize = (std::max)(1, wlSize);
                config.normalPoolSize = (std::max)(1, nlSize);
                instance->UpdateConfig(config);
                UIBridge_Toast("success", "应用成功", "线程池大小已应用");
            } else {
                UIBridge_Toast("error", "Apply failed", "Instance not found");
            }
        }
        else if (action == "config_set_iocp_max") {
            std::string instanceId = msg.value("instanceId", "");
            int wlMax = msg.value("iocpMaxWhitelist", 2000);
            int nlMax = msg.value("iocpMaxNormal", 500);

            auto& instMgr = InstanceManager::GetInstance();
            auto* instance = instMgr.GetSocksForwardInstance(instanceId);

            if (instance) {
                // 馃敟 浣跨敤 UpdateConfig 鏂规硶
                auto config = instance->GetCachedConfig();
                config.iocpMaxWhitelist = (std::max)(1, wlMax);
                config.iocpMaxNormal = (std::max)(1, nlMax);
                instance->UpdateConfig(config);
                UIBridge_Toast("success", "Applied", "IOCP connection limit applied");
            } else {
                UIBridge_Toast("error", "Apply failed", "Instance not found");
            }
        }
        else if (action == "config_get_auth") {
            std::string instanceId = msg.value("instanceId", "");
            auto& instMgr = InstanceManager::GetInstance();
            auto* instance = instMgr.GetSocksForwardInstance(instanceId);

            json response;
            response["type"] = "config_auth";
            response["enabled"] = false;
            response["selectedPoolId"] = "";
            response["accountCount"] = 0;

            if (instance) {
                // 🔥 使用 GetCachedConfig 读取配置
                auto config = instance->GetCachedConfig();
                response["enabled"] = config.enableSocks5Auth;
                response["selectedPoolId"] = config.accountSourceInstanceId;
            } else if (g_database) {
                std::string authEnabledStr = g_database->GetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(instanceId, "enableSocks5Auth"), "0");
                response["enabled"] = (authEnabledStr == "1");
                response["selectedPoolId"] = g_database->GetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(instanceId, "accountSourceInstanceId"), "");
            }

            // Include available pools
            auto pools = instMgr.GetSocks5PoolInstances();
            response["pools"] = json::array();
            for (auto* pool : pools) {
                if (pool) {
                    json poolJson;
                    poolJson["id"] = pool->GetId();
                    poolJson["name"] = pool->GetName();
                    response["pools"].push_back(poolJson);
                }
            }

            // Get account count from selected pool
            std::string selectedPoolId = response["selectedPoolId"].get<std::string>();
            if (!selectedPoolId.empty()) {
                auto* pool = instMgr.GetSocks5PoolInstance(selectedPoolId);
                if (pool) {
                    response["accountCount"] = (int)pool->GetAccountCount();
                }
            }

            UIBridge_PushMessage(response.dump());
        }
        else if (action == "config_set_auth_enabled") {
            std::string instanceId = msg.value("instanceId", "");
            bool enabled = msg.value("enabled", false);

            auto& instMgr = InstanceManager::GetInstance();
            auto* instance = instMgr.GetSocksForwardInstance(instanceId);

            if (instance) {
                // 馃敟 浣跨敤 UpdateConfig 鏂规硶
                auto config = instance->GetCachedConfig();
                config.enableSocks5Auth = enabled;
                instance->UpdateConfig(config);
                UIBridge_Toast("success", "Updated", std::string("SOCKS5 auth ") + (enabled ? "enabled" : "disabled"));
            } else {
                UIBridge_Toast("error", "Update failed", "Instance not found");
            }
        }
        else if (action == "config_set_auth_pool") {
            std::string instanceId = msg.value("instanceId", "");
            std::string poolId = msg.value("poolId", "");

            auto& instMgr = InstanceManager::GetInstance();
            auto* instance = instMgr.GetSocksForwardInstance(instanceId);

            if (instance) {
                // 馃敟 浣跨敤 UpdateConfig 鏂规硶
                auto config = instance->GetCachedConfig();
                config.accountSourceInstanceId = poolId;
                instance->UpdateConfig(config);
                UIBridge_Toast("success", "Bound", "Bound pool: " + poolId);
            } else {
                UIBridge_Toast("error", "Bind failed", "Instance not found");
            }
        }
        else if (action == "config_set_auth_device_limit") {
            std::string instanceId = msg.value("instanceId", "");

            auto& instMgr = InstanceManager::GetInstance();
            auto* instance = instMgr.GetSocksForwardInstance(instanceId);

            if (instance) {
                auto config = instance->GetCachedConfig();
                config.instanceDeviceLimit = 0;
                instance->UpdateConfig(config);
                UIBridge_Toast("info", "已移除", "实例级设备限制已移除，请在账号库页面配置在线设备策略");
            } else {
                UIBridge_Toast("error", "Update failed", "Instance not found");
            }
        }
        else if (action == "config_get_packet") {
            std::string instanceId = msg.value("instanceId", "");

            json response;
            response["type"] = "config_packet";
            response["enabled"] = true;
            response["applyWpeOnNonSplit"] = false;
            response["ports"] = json::array();

            if (g_database) {
                std::string enabledStr = g_database->GetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(instanceId, "enablePacketSplit"), "1");
                response["enabled"] = (enabledStr == "1");

                std::string wpeStr = g_database->GetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(instanceId, "applyWpeOnNonSplitTraffic"), "0");
                response["applyWpeOnNonSplit"] = (wpeStr == "1");

                std::string portsStr = g_database->GetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(instanceId, "packetSplitPorts"), "");
                if (!portsStr.empty()) {
                    size_t pos = 0;
                    while (pos < portsStr.size()) {
                        size_t comma = portsStr.find(',', pos);
                        if (comma == std::string::npos) comma = portsStr.size();
                        std::string portStr = portsStr.substr(pos, comma - pos);
                        if (!portStr.empty()) {
                            try { response["ports"].push_back(std::stoi(portStr)); } catch (...) {}
                        }
                        pos = comma + 1;
                    }
                }
            }

            UIBridge_PushMessage(response.dump());
        }
        else if (action == "config_set_packet_split") {
            std::string instanceId = msg.value("instanceId", "");
            bool enabled = msg.value("enabled", true);

            auto& instMgr = InstanceManager::GetInstance();
            auto* instance = instMgr.GetSocksForwardInstance(instanceId);

            if (instance) {
                // 馃敟 浣跨敤 UpdateConfig 鏂规硶
                auto config = instance->GetCachedConfig();
                config.enablePacketSplit = enabled;
                instance->UpdateConfig(config);
                UIBridge_Toast("success", "Updated", std::string("Packet split ") + (enabled ? "enabled" : "disabled"));
            } else {
                UIBridge_Toast("error", "Update failed", "Instance not found");
            }
        }
        else if (action == "config_set_wpe_non_split") {
            std::string instanceId = msg.value("instanceId", "");
            bool enabled = msg.value("enabled", false);

            auto& instMgr = InstanceManager::GetInstance();
            auto* instance = instMgr.GetSocksForwardInstance(instanceId);

            if (instance) {
                // 馃敟 浣跨敤 UpdateConfig 鏂规硶
                auto config = instance->GetCachedConfig();
                config.applyWpeOnNonSplitTraffic = enabled;
                instance->UpdateConfig(config);
            } else {
                UIBridge_Toast("error", "Update failed", "Instance not found");
            }
        }
        else if (action == "config_add_port") {
            std::string instanceId = msg.value("instanceId", "");
            int port = msg.value("port", 0);

            if (port > 0 && port <= 65535) {
                auto& instMgr = InstanceManager::GetInstance();
                auto* instance = instMgr.GetSocksForwardInstance(instanceId);

                if (instance) {
                    // 馃敟 浣跨敤 UpdateConfig 鏂规硶
                    auto config = instance->GetCachedConfig();

                    // Parse existing ports
                    std::vector<int> ports;
                    if (!config.packetSplitPorts.empty()) {
                        size_t pos = 0;
                        while (pos < config.packetSplitPorts.size()) {
                            size_t comma = config.packetSplitPorts.find(',', pos);
                            if (comma == std::string::npos) comma = config.packetSplitPorts.size();
                            std::string ps = config.packetSplitPorts.substr(pos, comma - pos);
                            if (!ps.empty()) try { ports.push_back(std::stoi(ps)); } catch (...) {}
                            pos = comma + 1;
                        }
                    }

                    // Check duplicates
                    bool exists = false;
                    for (int p : ports) {
                        if (p == port) { exists = true; break; }
                    }

                    if (!exists) {
                        ports.push_back(port);

                        // Build new ports string
                        std::string newPortsStr;
                        for (size_t i = 0; i < ports.size(); i++) {
                            if (i > 0) newPortsStr += ",";
                            newPortsStr += std::to_string(ports[i]);
                        }

                        config.packetSplitPorts = newPortsStr;
                        instance->UpdateConfig(config);

                        // Refresh
                        json refreshMsg;
                        refreshMsg["action"] = "config_get_packet";
                        refreshMsg["instanceId"] = instanceId;
                        UIBridge_HandleMessage(refreshMsg.dump().c_str());
                    } else {
                        UIBridge_Toast("warning", "Port exists", "Port already exists in list");
                    }
                }
            }
        }
        else if (action == "config_remove_port") {
            std::string instanceId = msg.value("instanceId", "");
            int port = msg.value("port", 0);

            auto& instMgr = InstanceManager::GetInstance();
            auto* instance = instMgr.GetSocksForwardInstance(instanceId);

            if (instance) {
                // 馃敟 浣跨敤 UpdateConfig 鏂规硶
                auto config = instance->GetCachedConfig();

                // Parse existing ports
                std::vector<int> ports;
                if (!config.packetSplitPorts.empty()) {
                    size_t pos = 0;
                    while (pos < config.packetSplitPorts.size()) {
                        size_t comma = config.packetSplitPorts.find(',', pos);
                        if (comma == std::string::npos) comma = config.packetSplitPorts.size();
                        std::string ps = config.packetSplitPorts.substr(pos, comma - pos);
                        if (!ps.empty()) try { ports.push_back(std::stoi(ps)); } catch (...) {}
                        pos = comma + 1;
                    }
                }

                // Remove port
                ports.erase(std::remove(ports.begin(), ports.end(), port), ports.end());

                // Build new ports string
                std::string newPortsStr;
                for (size_t i = 0; i < ports.size(); i++) {
                    if (i > 0) newPortsStr += ",";
                    newPortsStr += std::to_string(ports[i]);
                }

                config.packetSplitPorts = newPortsStr;
                instance->UpdateConfig(config);

                // Refresh
                json refreshMsg;
                refreshMsg["action"] = "config_get_packet";
                refreshMsg["instanceId"] = instanceId;
                UIBridge_HandleMessage(refreshMsg.dump().c_str());
            }
        }
        else if (action == "config_get_traffic") {
            std::string instanceId = msg.value("instanceId", "");
            auto& instMgr = InstanceManager::GetInstance();
            auto* instance = instMgr.GetSocksForwardInstance(instanceId);

            json response;
            response["type"] = "config_traffic";
            response["enableTrafficFilter"] = false;
            response["enableSniSniffing"] = false;
            response["rules"] = json::array();

            if (instance && instance->GetCollector()) {
                auto* collector = instance->GetCollector();
                response["enableTrafficFilter"] = collector->IsTrafficFilterEnabled();
                response["enableSniSniffing"] = collector->IsSNISniffingEnabled();

                auto rules = collector->GetAllTrafficRules();
                for (const auto& rule : rules) {
                    json ruleJson;
                    ruleJson["id"] = rule.id;
                    ruleJson["type"] = static_cast<int>(rule.type);
                    ruleJson["value1"] = rule.value1;
                    ruleJson["value2"] = rule.value2;
                    ruleJson["enabled"] = rule.enabled;
                    response["rules"].push_back(ruleJson);
                }
            } else if (g_database) {
                std::string enableFilterStr = g_database->GetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(instanceId, "enableTrafficFilter"), "0");
                response["enableTrafficFilter"] = (enableFilterStr == "1");

                std::string enableSniStr = g_database->GetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(instanceId, "enableSniSniffing"), "0");
                response["enableSniSniffing"] = (enableSniStr == "1");

                std::string rulesJsonStr = g_database->GetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(instanceId, "trafficFilterRules"), "[]");
                try {
                    json rulesJson = json::parse(rulesJsonStr);
                    if (rulesJson.is_array()) {
                        response["rules"] = rulesJson;
                    }
                } catch (...) {
                    response["rules"] = json::array();
                }
            }

            UIBridge_PushMessage(response.dump());
        }
        else if (action == "config_get_sslproxy") {
            std::string instanceId = msg.value("instanceId", "");
            auto& instMgr = InstanceManager::GetInstance();
            auto* instance = instMgr.GetSocksForwardInstance(instanceId);

            json response;
            response["type"] = "config_sslproxy";
            response["enableSSLMitm"] = false;
            response["sslMitmRules"] = json::array();

            if (instance) {
                auto cached = instance->GetCachedConfig();
                response["enableSSLMitm"] = cached.enableSSLMitm;
                try {
                    json sslRulesJson = json::parse(cached.sslMitmRules.empty() ? "[]" : cached.sslMitmRules);
                    if (sslRulesJson.is_array()) {
                        response["sslMitmRules"] = sslRulesJson;
                    }
                } catch (...) {
                    response["sslMitmRules"] = json::array();
                }
            } else if (g_database) {
                std::string enableMitmStr = g_database->GetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(instanceId, "enableSSLMitm"), "0");
                response["enableSSLMitm"] = (enableMitmStr == "1");

                std::string sslRulesJsonStr = g_database->GetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(instanceId, "sslMitmRules"), "[]");
                try {
                    json sslRulesJson = json::parse(sslRulesJsonStr);
                    if (sslRulesJson.is_array()) {
                        response["sslMitmRules"] = sslRulesJson;
                    }
                } catch (...) {
                    response["sslMitmRules"] = json::array();
                }
            }

            UIBridge_PushMessage(response.dump());
        }
        else if (action == "config_get_localmap") {
            std::string instanceId = msg.value("instanceId", "");
            auto& instMgr = InstanceManager::GetInstance();
            auto* instance = instMgr.GetSocksForwardInstance(instanceId);

            json response;
            response["type"] = "config_localmap";
            response["enabled"] = false;
            response["rules"] = json::array();

            if (instance) {
                auto cached = instance->GetCachedConfig();
                response["enabled"] = cached.enableHttpLocalMap;
                try {
                    json rulesJson = json::parse(cached.httpLocalMapRules.empty() ? "[]" : cached.httpLocalMapRules);
                    if (rulesJson.is_array()) {
                        response["rules"] = rulesJson;
                    }
                } catch (...) {
                    response["rules"] = json::array();
                }
            } else if (g_database) {
                std::string enabledStr = g_database->GetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(instanceId, "enableHttpLocalMap"), "0");
                response["enabled"] = (enabledStr == "1");

                std::string rulesJsonStr = g_database->GetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(instanceId, "httpLocalMapRules"), "[]");
                try {
                    json rulesJson = json::parse(rulesJsonStr);
                    if (rulesJson.is_array()) {
                        response["rules"] = rulesJson;
                    }
                } catch (...) {
                    response["rules"] = json::array();
                }
            }

            UIBridge_PushMessage(response.dump());
        }
        else if (action == "config_set_traffic_filter") {
            std::string instanceId = msg.value("instanceId", "");
            bool enabled = msg.value("enabled", false);

            auto& instMgr = InstanceManager::GetInstance();
            auto* instance = instMgr.GetSocksForwardInstance(instanceId);

            if (instance) {
                // 馃敟 浣跨敤 UpdateConfig 鏂规硶
                auto config = instance->GetCachedConfig();
                config.enableTrafficFilter = enabled;
                instance->UpdateConfig(config);
            }
        }
        else if (action == "config_set_sni_sniffing") {
            std::string instanceId = msg.value("instanceId", "");
            bool enabled = msg.value("enabled", false);

            auto& instMgr = InstanceManager::GetInstance();
            auto* instance = instMgr.GetSocksForwardInstance(instanceId);

            if (instance) {
                // 馃敟 浣跨敤 UpdateConfig 鏂规硶
                auto config = instance->GetCachedConfig();
                config.enableSniSniffing = enabled;
                instance->UpdateConfig(config);
            }
        }
        else if (action == "config_set_ssl_mitm") {
            std::string instanceId = msg.value("instanceId", "");
            bool enabled = msg.value("enabled", false);

            auto& instMgr = InstanceManager::GetInstance();
            auto* instance = instMgr.GetSocksForwardInstance(instanceId);

            if (instance) {
                auto config = instance->GetCachedConfig();
                config.enableSSLMitm = enabled;
                instance->UpdateConfig(config);

                if (enabled) {
                    UIBridge_Toast("info", "SSL MITM 已启用", "请先导出并安装 CA 证书后再抓取 HTTPS/TLS 流量");
                }
            }
        }
        else if (action == "config_set_localmap") {
            std::string instanceId = msg.value("instanceId", "");
            bool enabled = msg.value("enabled", false);

            auto& instMgr = InstanceManager::GetInstance();
            auto* instance = instMgr.GetSocksForwardInstance(instanceId);

            if (instance) {
                auto config = instance->GetCachedConfig();
                config.enableHttpLocalMap = enabled;
                instance->UpdateConfig(config);
            }
        }
        else if (action == "config_set_localmap_rules") {
            std::string instanceId = msg.value("instanceId", "");
            json rulesJson = msg.value("rules", json::array());

            auto& instMgr = InstanceManager::GetInstance();
            auto* instance = instMgr.GetSocksForwardInstance(instanceId);

            if (!instance) {
                UIBridge_Toast("error", "保存失败", "实例不存在");
            } else if (!rulesJson.is_array()) {
                UIBridge_Toast("error", "保存失败", "本地映射规则格式无效");
            } else {
                json normalized = json::array();
                for (const auto& item : rulesJson) {
                    const std::string localFilePath = item.value("localFilePath", "");
                    if (localFilePath.empty()) {
                        continue;
                    }

                    json rule;
                    rule["id"] = item.value("id", 0);
                    rule["enabled"] = item.value("enabled", true);
                    rule["scheme"] = item.value("scheme", "*");
                    rule["hostPattern"] = item.value("hostPattern", "*");
                    rule["pathPattern"] = item.value("pathPattern", "/");
                    rule["method"] = item.value("method", "*");
                    rule["localFilePath"] = localFilePath;
                    rule["contentType"] = item.value("contentType", "");
                    normalized.push_back(rule);
                }

                auto config = instance->GetCachedConfig();
                config.httpLocalMapRules = normalized.dump();
                instance->UpdateConfig(config);
            }

            json refreshMsg;
            refreshMsg["action"] = "config_get_localmap";
            refreshMsg["instanceId"] = instanceId;
            UIBridge_HandleMessage(refreshMsg.dump().c_str());
        }
        else if (action == "config_set_ssl_mitm_rules") {
            std::string instanceId = msg.value("instanceId", "");
            json rulesJson = msg.value("rules", json::array());

            auto& instMgr = InstanceManager::GetInstance();
            auto* instance = instMgr.GetSocksForwardInstance(instanceId);

            if (!instance) {
                UIBridge_Toast("error", "保存失败", "实例不存在");
            } else if (!rulesJson.is_array()) {
                UIBridge_Toast("error", "保存失败", "SSL MITM 规则格式无效");
            } else {
                json normalized = json::array();
                for (const auto& item : rulesJson) {
                    bool matchByPort = item.value("matchByPort", true);
                    if (matchByPort) {
                        int port = item.value("port", 0);
                        if (port > 0 && port <= 65535) {
                            json rule;
                            rule["matchByPort"] = true;
                            rule["port"] = port;
                            rule["domain"] = "";
                            normalized.push_back(rule);
                        }
                    } else {
                        std::string domain = item.value("domain", "");
                        if (!domain.empty()) {
                            json rule;
                            rule["matchByPort"] = false;
                            rule["port"] = 0;
                            rule["domain"] = domain;
                            normalized.push_back(rule);
                        }
                    }
                }

                auto config = instance->GetCachedConfig();
                config.sslMitmRules = normalized.dump();
                instance->UpdateConfig(config);
            }

            json refreshMsg;
            refreshMsg["action"] = "config_get_sslproxy";
            refreshMsg["instanceId"] = instanceId;
            UIBridge_HandleMessage(refreshMsg.dump().c_str());
        }
        else if (action == "config_export_ssl_mitm_ca") {
            if (!PacketCollector::InitSSLMitmCA()) {
                const std::string detail = PacketCollector::GetSSLMitmLastErrorDetail();
                AB_LOG_WARNING("[SSL-MITM] CA 初始化失败（用户导出证书时触发）" +
                    (detail.empty() ? std::string() : (": " + detail)));
                UIBridge_Toast("error", "导出失败",
                    detail.empty() ? "SSL MITM CA 初始化失败"
                                   : ("SSL MITM CA 初始化失败: " + detail));
            } else {
                const std::filesystem::path exportPath = UIBridge_GetModuleDirectoryLocal() / "ssl-mitm-ca.cer";
                const std::string exportPathUtf8 = exportPath.u8string();
                AB_LOG_INFO("[SSL-MITM] 正在导出 CA 证书到程序目录: " + exportPathUtf8);

                if (PacketCollector::ExportSSLMitmCACert(exportPathUtf8)) {
                    AB_LOG_INFO("[SSL-MITM] CA 证书导出成功: " + exportPathUtf8);
                    UIBridge_Toast("success", "导出成功", "CA 证书已导出到程序目录: " + exportPathUtf8);
                } else {
                    const std::string detail = PacketCollector::GetSSLMitmLastErrorDetail();
                    AB_LOG_WARNING("[SSL-MITM] CA 证书导出失败: " + exportPathUtf8 +
                        (detail.empty() ? std::string() : (", " + detail)));
                    UIBridge_Toast("error", "导出失败",
                        detail.empty() ? ("写入 CA 证书文件失败: " + exportPathUtf8)
                                       : ("写入 CA 证书文件失败: " + detail));
                }
            }
        }
        else if (action == "config_get_disconnect") {
            std::string instanceId = msg.value("instanceId", "");
            auto& instMgr = InstanceManager::GetInstance();
            auto* instance = instMgr.GetSocksForwardInstance(instanceId);

            json response;
            response["type"] = "config_disconnect";
            response["rules"] = json::array();

            std::vector<DisconnectRule> rules;
            if (instance) {
                auto config = instance->GetCachedConfig();
                std::string error;
                DisconnectRuleCodec::DeserializeRulesFromJson(config.disconnectRulesJson, rules, &error);

                if (instance->GetCollector()) {
                    rules = instance->GetCollector()->GetDisconnectRules();
                }
            }
            else if (g_database) {
                std::string rulesJsonStr = g_database->GetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(instanceId, "disconnectRulesJson"), "[]");
                std::string error;
                DisconnectRuleCodec::DeserializeRulesFromJson(rulesJsonStr, rules, &error);
            }

            for (const auto& rule : rules) {
                json item;
                item["targetPort"] = rule.targetPort;
                item["mode"] = static_cast<int>(rule.mode);
                item["disconnectIntervalSec"] = rule.disconnectIntervalSec;
                item["enabled"] = rule.enabled;
                item["hexPattern"] = rule.hexPattern;
                item["hexDelayEnabled"] = rule.hexDelayEnabled;
                item["hexDelaySeconds"] = rule.hexDelaySeconds;
                response["rules"].push_back(std::move(item));
            }

            UIBridge_PushMessage(response.dump());
        }
        else if (action == "config_set_disconnect") {
            std::string instanceId = msg.value("instanceId", "");
            json rulesJson = msg.value("rules", json::array());
            bool silent = msg.value("silent", 0) != 0;

            auto& instMgr = InstanceManager::GetInstance();
            auto* instance = instMgr.GetSocksForwardInstance(instanceId);
            if (!instance) {
                UIBridge_Toast("error", "保存失败", "实例不存在");
            } else {
                std::vector<DisconnectRule> rules;
                std::string error;
                if (!DisconnectRuleCodec::DeserializeRulesFromJson(rulesJson.dump(), rules, &error)) {
                    UIBridge_Toast("error", "保存失败", error.empty() ? "断网规则无效" : error);
                } else {
                    auto config = instance->GetCachedConfig();
                    config.disconnectRulesJson = DisconnectRuleCodec::SerializeRulesToJson(rules);
                    instance->UpdateConfig(config);
                    if (!silent) {
                        UIBridge_Toast("success", "保存成功", "断网规则已更新");
                    }
                }
            }

            json refreshMsg;
            refreshMsg["action"] = "config_get_disconnect";
            refreshMsg["instanceId"] = instanceId;
            UIBridge_HandleMessage(refreshMsg.dump().c_str());
        }
        else if (action == "config_add_traffic_rule") {
            std::string instanceId = msg.value("instanceId", "");
            int type = msg.value("type", 0);
            std::string value = msg.value("value", "");
            std::string sniDomain = msg.value("sniDomain", "");

            auto& instMgr = InstanceManager::GetInstance();
            auto* instance = instMgr.GetSocksForwardInstance(instanceId);

            if (instance && instance->GetCollector()) {
                TrafficFilterRule rule;
                rule.type = static_cast<TrafficRuleType>(type);
                rule.value1 = value;
                rule.value2 = (type == 3) ? sniDomain : "";
                rule.enabled = true;

                instance->GetCollector()->AddTrafficRule(rule);

                // Save to database
                if (g_database) {
                    auto rules = instance->GetCollector()->GetAllTrafficRules();
                    json rulesJson = json::array();
                    for (const auto& r : rules) {
                        json rj;
                        rj["id"] = r.id;
                        rj["type"] = static_cast<int>(r.type);
                        rj["value1"] = r.value1;
                        rj["value2"] = r.value2;
                        rj["enabled"] = r.enabled;
                        rj["description"] = r.description;
                        rulesJson.push_back(rj);
                    }
                    g_database->SetConfigValue(
                        InstanceManager::MakeInstanceConfigKey(instanceId, "trafficFilterRules"),
                        rulesJson.dump());
                }

                UIBridge_Toast("success", "Add succeeded", "Traffic filter rule added");

                // Refresh
                json refreshMsg;
                refreshMsg["action"] = "config_get_traffic";
                refreshMsg["instanceId"] = instanceId;
                UIBridge_HandleMessage(refreshMsg.dump().c_str());
            }
        }
        else if (action == "config_toggle_traffic_rule") {
            std::string instanceId = msg.value("instanceId", "");
            int ruleId = msg.value("ruleId", 0);
            bool enabled = msg.value("enabled", true);

            auto& instMgr = InstanceManager::GetInstance();
            auto* instance = instMgr.GetSocksForwardInstance(instanceId);

            if (instance && instance->GetCollector()) {
                auto rules = instance->GetCollector()->GetAllTrafficRules();
                for (auto& rule : rules) {
                    if (rule.id == ruleId) {
                        rule.enabled = enabled;
                        instance->GetCollector()->UpdateTrafficRule(rule);
                        break;
                    }
                }

                // Save to database
                if (g_database) {
                    auto updatedRules = instance->GetCollector()->GetAllTrafficRules();
                    json rulesJson = json::array();
                    for (const auto& r : updatedRules) {
                        json rj;
                        rj["id"] = r.id;
                        rj["type"] = static_cast<int>(r.type);
                        rj["value1"] = r.value1;
                        rj["value2"] = r.value2;
                        rj["enabled"] = r.enabled;
                        rj["description"] = r.description;
                        rulesJson.push_back(rj);
                    }
                    g_database->SetConfigValue(
                        InstanceManager::MakeInstanceConfigKey(instanceId, "trafficFilterRules"),
                        rulesJson.dump());
                }
            }
        }
        else if (action == "config_delete_traffic_rule") {
            std::string instanceId = msg.value("instanceId", "");
            int ruleId = msg.value("ruleId", 0);

            auto& instMgr = InstanceManager::GetInstance();
            auto* instance = instMgr.GetSocksForwardInstance(instanceId);

            if (instance && instance->GetCollector()) {
                instance->GetCollector()->RemoveTrafficRule(ruleId);

                // Save to database
                if (g_database) {
                    auto rules = instance->GetCollector()->GetAllTrafficRules();
                    json rulesJson = json::array();
                    for (const auto& r : rules) {
                        json rj;
                        rj["id"] = r.id;
                        rj["type"] = static_cast<int>(r.type);
                        rj["value1"] = r.value1;
                        rj["value2"] = r.value2;
                        rj["enabled"] = r.enabled;
                        rj["description"] = r.description;
                        rulesJson.push_back(rj);
                    }
                    g_database->SetConfigValue(
                        InstanceManager::MakeInstanceConfigKey(instanceId, "trafficFilterRules"),
                        rulesJson.dump());
                }

                UIBridge_Toast("success", "Delete succeeded", "Rule deleted");

                // Refresh
                json refreshMsg;
                refreshMsg["action"] = "config_get_traffic";
                refreshMsg["instanceId"] = instanceId;
                UIBridge_HandleMessage(refreshMsg.dump().c_str());
            }
        }
        else if (action == "config_clear_traffic_rules") {
            std::string instanceId = msg.value("instanceId", "");

            auto& instMgr = InstanceManager::GetInstance();
            auto* instance = instMgr.GetSocksForwardInstance(instanceId);

            if (instance && instance->GetCollector()) {
                instance->GetCollector()->ClearAllTrafficRules();

                // Save to database
                if (g_database) {
                    g_database->SetConfigValue(
                        InstanceManager::MakeInstanceConfigKey(instanceId, "trafficFilterRules"),
                        "[]");
                }

                UIBridge_Toast("success", "清空成功", "所有规则已清空");

                // Refresh
                json refreshMsg;
                refreshMsg["action"] = "config_get_traffic";
                refreshMsg["instanceId"] = instanceId;
                UIBridge_HandleMessage(refreshMsg.dump().c_str());
            }
        }
        else if (action == "config_get_userfilter") {
            std::string instanceId = msg.value("instanceId", "");
            auto& instMgr = InstanceManager::GetInstance();
            auto* instance = instMgr.GetSocksForwardInstance(instanceId);

            json response;
            response["type"] = "config_userfilter";
            response["enabled"] = false;
            response["httpPort"] = 8080;
            response["httpServerRunning"] = false;
            response["remoteUrl"] = "";
            response["showCounts"] = false;
            response["availableFilters"] = json::array();
            response["defaultFilters"] = json::array();
            response["wpeFilterGroups"] = json::array();

            if (g_database) {
                std::string enableStr = g_database->GetConfigValue(
                    "instance_" + instanceId + "_enableUserFilterMode", "0");
                response["enabled"] = (enableStr == "1");

                std::string portStr = g_database->GetConfigValue(
                    "instance_" + instanceId + "_userFilterHttpPort", "8080");
                response["httpPort"] = std::stoi(portStr);

                std::string showCountsStr = g_database->GetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(instanceId, "userFilterShowCounts"), "0");
                response["showCounts"] = (showCountsStr == "1");
            }

            if (instance && instance->GetCollector()) {
                response["httpServerRunning"] = instance->GetCollector()->IsUserFilterHttpServerRunning();
            }

            // Reuse the same IPv4 access host logic as remote browser mode.
            // Reuse the same IPv4 access host logic as remote browser mode.
            const int port = response["httpPort"].get<int>();
            const std::string detectedHost = RemoteBrowserServer::DetectPreferredIPv4Host();
            response["remoteUrl"] = "";
            response["remoteHost"] = detectedHost;
            response["remoteHostScope"] = "unknown";
            response["remoteAccessHint"] = "未检测到可用的IPv4地址，请检查网卡状态。";
            if (!detectedHost.empty()) {
                response["remoteUrl"] = "http://" + detectedHost + ":" + std::to_string(port);
                const bool isPrivate = RemoteBrowserServer::IsPrivateIPv4Host(detectedHost);
                response["remoteHostScope"] = isPrivate ? "lan" : "public";
                response["remoteAccessHint"] = isPrivate
                    ? "当前显示的是局域网IPv4地址，仅同局域网可直接访问；若需跨公网访问，请放行系统防火墙并配置路由器/安全组端口映射。"
                    : "当前显示的是可直连IPv4地址；如仍无法访问，请检查系统防火墙、安全组和端口占用情况。";
            }

            // Get available WPE filters for this instance
            if (g_wpeFilterManager) {
                auto allFilters = g_wpeFilterManager->GetAllFilters();
                for (const auto& filter : allFilters) {
                    if (filter.target.applyToAllInstances) {
                        json fJson;
                        fJson["id"] = filter.id;
                        fJson["name"] = filter.name;
                        fJson["webDisplayName"] = filter.webDisplayName;
                        response["availableFilters"].push_back(fJson);
                    } else {
                        auto& targetIds = filter.target.targetInstanceIds;
                        if (std::find(targetIds.begin(), targetIds.end(), instanceId) != targetIds.end()) {
                            json fJson;
                            fJson["id"] = filter.id;
                            fJson["name"] = filter.name;
                            fJson["webDisplayName"] = filter.webDisplayName;
                            response["availableFilters"].push_back(fJson);
                        }
                    }
                }
            }

            // Get default filters from database
            if (g_database) {
                auto defaultFilterIds = g_database->LoadDefaultFilterConfig(instanceId);
                for (int filterId : defaultFilterIds) {
                    response["defaultFilters"].push_back(filterId);
                }

                auto groups = g_database->LoadWPEFilterGroups(true);
                for (const auto& group : groups) {
                    json gj;
                    gj["id"] = group.id;
                    gj["name"] = group.name;
                    gj["description"] = group.description;
                    gj["enabled"] = group.enabled;
                    gj["items"] = json::array();
                    for (const auto& item : group.items) {
                        gj["items"].push_back({
                            {"filterId", item.filterId},
                            {"defaultEnabled", item.defaultEnabled}
                        });
                    }
                    response["wpeFilterGroups"].push_back(gj);
                }
            }

            UIBridge_PushMessage(response.dump());
        }
        else if (action == "config_set_defaultfilter") {
            std::string instanceId = msg.value("instanceId", "");
            auto filterIdsJson = msg.value("filterIds", json::array());
            bool applyToAllUsers = msg.value("applyToAllUsers", false);

            std::vector<int> filterIds;
            for (const auto& idVal : filterIdsJson) {
                if (idVal.is_string()) {
                    try {
                        filterIds.push_back(std::stoi(idVal.get<std::string>()));
                    } catch (...) {}
                } else if (idVal.is_number()) {
                    filterIds.push_back(idVal.get<int>());
                }
            }

            if (g_userFilterManager) {
                if (applyToAllUsers) {
                    // 应用到所有现有用户并保存默认配置
                    if (g_userFilterManager->ApplyDefaultFiltersToAllUsers(instanceId, filterIds)) {
                        UIBridge_Toast("success", "Save succeeded", "Default filters saved and applied to all users");
                    } else {
                        UIBridge_Toast("error", "保存失败", "应用到所有用户时出错");
                    }
                } else {
                    // 浠呬繚瀛橀粯璁ら厤缃紙閫氳繃 UserFilterManager 浠ュ悓姝ュ唴瀛樼紦瀛橈級
                    if (g_userFilterManager->SaveDefaultFilters(instanceId, filterIds)) {
                        UIBridge_Toast("success", "Save succeeded", "Default filters saved");
                    } else {
                        UIBridge_Toast("error", "Save failed", "Failed to save default filter config");
                    }
                }
            } else if (g_database) {
                g_database->SaveDefaultFilterConfig(instanceId, filterIds);
                UIBridge_Toast("success", "Save succeeded", "Default filters saved");
            } else {
                UIBridge_Toast("error", "保存失败", "系统未初始化");
            }
        }
        else if (action == "config_set_userfilter_mode") {
            std::string instanceId = msg.value("instanceId", "");
            bool enabled = msg.value("enabled", false);

            auto& instMgr = InstanceManager::GetInstance();
            auto* instance = instMgr.GetSocksForwardInstance(instanceId);

            if (instance) {
                // 🔥 使用 UpdateConfig 方法（会自动处理HTTP服务器的启动/停止�?
                auto config = instance->GetCachedConfig();
                config.enableUserFilterMode = enabled;
                instance->UpdateConfig(config);

                if (enabled) {
                    UIBridge_Toast("success", "Enabled", "User filter mode enabled");
                } else {
                    UIBridge_Toast("info", "Disabled", "User filter mode disabled");
                }
            } else {
                UIBridge_Toast("error", "Operation failed", "Instance not found");
            }

            // Refresh
            json refreshMsg;
            refreshMsg["action"] = "config_get_userfilter";
            refreshMsg["instanceId"] = instanceId;
            UIBridge_HandleMessage(refreshMsg.dump().c_str());
        }
        else if (action == "config_set_userfilter_showcounts") {
            std::string instanceId = msg.value("instanceId", "");
            bool enabled = msg.value("enabled", false);

            if (g_database) {
                g_database->SetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(instanceId, "userFilterShowCounts"),
                    enabled ? "1" : "0");
                UIBridge_Toast("success", "保存成功",
                    enabled ? "远程控制台已显示执行次数" : "远程控制台已隐藏执行次数");
            } else {
                UIBridge_Toast("error", "保存失败", "系统未初始化");
            }

            // Refresh
            json refreshMsg;
            refreshMsg["action"] = "config_get_userfilter";
            refreshMsg["instanceId"] = instanceId;
            UIBridge_HandleMessage(refreshMsg.dump().c_str());
        }
        else if (action == "config_set_http_port") {
            std::string instanceId = msg.value("instanceId", "");
            int port = msg.value("port", 8080);

            auto& instMgr = InstanceManager::GetInstance();
            auto* instance = instMgr.GetSocksForwardInstance(instanceId);

            if (instance) {
                // 🔥 使用 UpdateConfig 方法（会自动处理HTTP服务器的重启�?
                auto config = instance->GetCachedConfig();
                config.userFilterHttpPort = port;
                instance->UpdateConfig(config);

                UIBridge_Toast("success", "保存成功", "HTTP端口已更新: " + std::to_string(port));
            }

            if (g_database) {
                g_database->SetConfigValue("instance_" + instanceId + "_userFilterHttpPort",
                    std::to_string(port));
            }

            // Refresh
            json refreshMsg;
            refreshMsg["action"] = "config_get_userfilter";
            refreshMsg["instanceId"] = instanceId;
            UIBridge_HandleMessage(refreshMsg.dump().c_str());
        }
        else if (action == "config_start_http_server") {
            std::string instanceId = msg.value("instanceId", "");
            auto& instMgr = InstanceManager::GetInstance();
            auto* instance = instMgr.GetSocksForwardInstance(instanceId);

            if (instance && instance->GetCollector()) {
                if (instance->GetCollector()->StartUserFilterHttpServer()) {
                    UIBridge_Toast("success", "启动成功", "HTTP服务器已启动");
                } else {
                    UIBridge_Toast("error", "启动失败", "HTTP服务器启动失败，请检查端口是否被占用");
                }
            }

            // Refresh
            json refreshMsg;
            refreshMsg["action"] = "config_get_userfilter";
            refreshMsg["instanceId"] = instanceId;
            UIBridge_HandleMessage(refreshMsg.dump().c_str());
        }
        else if (action == "config_stop_http_server") {
            std::string instanceId = msg.value("instanceId", "");
            auto& instMgr = InstanceManager::GetInstance();
            auto* instance = instMgr.GetSocksForwardInstance(instanceId);

            if (instance && instance->GetCollector()) {
                instance->GetCollector()->StopUserFilterHttpServer();
                UIBridge_Toast("success", "Stopped", "HTTP server stopped");
            }

            // Refresh
            json refreshMsg;
            refreshMsg["action"] = "config_get_userfilter";
            refreshMsg["instanceId"] = instanceId;
            UIBridge_HandleMessage(refreshMsg.dump().c_str());
        }
        else if (action == "config_get_accountfilter") {
            std::string instanceId = msg.value("instanceId", "");
            auto& instMgr = InstanceManager::GetInstance();
            auto* instance = instMgr.GetSocksForwardInstance(instanceId);

            json response;
            response["type"] = "config_accountfilter";
            response["accounts"] = json::array();
            response["wpeFilterGroups"] = json::array();

            if (g_database) {
                auto groups = g_database->LoadWPEFilterGroups(true);
                for (const auto& group : groups) {
                    json gj;
                    gj["id"] = group.id;
                    gj["name"] = group.name;
                    gj["description"] = group.description;
                    gj["enabled"] = group.enabled;
                    response["wpeFilterGroups"].push_back(gj);
                }
            }

            if (instance && instance->GetCollector()) {
                auto* collector = instance->GetCollector();

                // Check if user filter mode is enabled
                if (!collector->IsUserFilterModeEnabled()) {
                    response["error"] = "Current instance does not enable user-filter mode";
                } else {
                    auto* externalSource = collector->GetExternalAccountSource();
                    if (!externalSource) {
                        response["error"] = "当前实例未绑定账号库";
                    } else {
                        auto allAccounts = externalSource->GetAllAccounts();
                        for (const auto& account : allAccounts) {
                            json accJson;
                            accJson["username"] = account.username;
                            accJson["filters"] = json::array();
                            accJson["groupIds"] = json::array();

                            // 鏌ヨ鐢ㄦ埛鐨勬湁鏁堟护闀滈厤缃?
                            if (g_userFilterManager) {
                                accJson["hasCustomConfig"] = g_userFilterManager->HasUserConfig(instanceId, account.username);
                                for (int groupId : g_userFilterManager->GetUserFilterGroups(instanceId, account.username)) {
                                    accJson["groupIds"].push_back(groupId);
                                }

                                auto effectiveFilters = g_userFilterManager->GetEffectiveUserFilters(instanceId, account.username);

                                // 将滤镜ID映射�?{id, name} 对象
                                if (g_wpeFilterManager) {
                                    auto allFilters = g_wpeFilterManager->GetAllFilters();
                                    for (int filterId : effectiveFilters) {
                                        for (const auto& filter : allFilters) {
                                            if (filter.id == filterId) {
                                                json fJson;
                                                fJson["id"] = filter.id;
                                                fJson["name"] = filter.name;
                                                accJson["filters"].push_back(fJson);
                                                break;
                                            }
                                        }
                                    }
                                }
                            } else {
                                accJson["hasCustomConfig"] = false;
                            }

                            response["accounts"].push_back(accJson);
                        }
                    }
                }
            } else {
                response["error"] = "请先启动实例才能查看账号滤镜配置";
            }

            UIBridge_PushMessage(response.dump());
        }
        else if (action == "config_get_proxydata") {
            std::string instanceId = msg.value("instanceId", "");
            size_t knownCount = msg.value("knownCount", static_cast<size_t>(0));
            std::string firstKey = msg.value("firstKey", "");
            std::string lastKey = msg.value("lastKey", "");
            auto& instMgr = InstanceManager::GetInstance();
            auto* instance = instMgr.GetSocksForwardInstance(instanceId);

            AB_LOG_INFO_CAT(
                LOG_CAT_COLLECTOR,
                "[代理数据] 收到列表请求: instance=" + instanceId +
                ", knownCount=" + std::to_string(knownCount) +
                ", firstKey=" + (firstKey.empty() ? std::string("<empty>") : firstKey) +
                ", lastKey=" + (lastKey.empty() ? std::string("<empty>") : lastKey) +
                ", instanceFound=" + std::string((instance && instance->GetCollector()) ? "true" : "false"));

            json response;
            response["type"] = "proxydata_list";
            response["instanceId"] = instanceId;
            response["packets"] = json::array();
            response["enabled"] = false;
            response["bufferSize"] = 200;
            response["reset"] = true;
            response["totalCount"] = 0;
            response["firstKey"] = "";
            response["lastKey"] = "";

            if (instance && instance->GetCollector()) {
                auto* collector = instance->GetCollector();
                response["enabled"] = collector->IsProxyPacketRecordEnabled();
                response["bufferSize"] = collector->GetProxyPacketBufferSize();
                std::vector<uint8_t> activeSearchPattern;
                {
                    std::lock_guard<std::mutex> lock(g_proxydataSearchMutex);
                    if (g_proxydataSearchInstanceId == instanceId) {
                        activeSearchPattern = g_proxydataSearchPattern;
                        response["searchHex"] = g_proxydataSearchHex;
                    } else {
                        response["searchHex"] = "";
                    }
                }

                if (!activeSearchPattern.empty()) {
                    auto records = collector->GetProxyPacketRecords();
                    response["reset"] = true;
                    response["totalCount"] = 0;
                    response["firstKey"] = "";
                    response["lastKey"] = "";

                    bool first = true;
                    for (const auto& record : records) {
                        if (record.fullData.size() < activeSearchPattern.size()) {
                            continue;
                        }
                        if (std::search(record.fullData.begin(), record.fullData.end(), activeSearchPattern.begin(), activeSearchPattern.end()) == record.fullData.end()) {
                            continue;
                        }

                        json pkt;
                        pkt["sequence"] = record.sequence;
                        pkt["connectionId"] = record.connectionId;
                        pkt["timestamp"] = record.timestamp;
                        pkt["username"] = record.username;
                        pkt["gameID"] = record.gameID;
                        pkt["clientIP"] = record.clientIP;
                        pkt["targetHost"] = record.targetHost;
                        pkt["sniHost"] = record.sniHost;
                        pkt["targetPort"] = record.targetPort;
                        pkt["sslMitmEnabled"] = record.sslMitmEnabled;
                        pkt["isRequest"] = record.isRequest;
                        pkt["dataLength"] = record.dataLength;
                        pkt["dataPreview"] = record.dataPreview;
                        response["packets"].push_back(pkt);

                        const std::string key = std::to_string(record.sequence);
                        if (first) {
                            response["firstKey"] = key;
                            first = false;
                        }
                        response["lastKey"] = key;
                        response["totalCount"] = response["totalCount"].get<size_t>() + 1;
                    }
                } else {
                    bool reset = true;
                    size_t totalCount = 0;
                    std::string currentFirstKey;
                    std::string currentLastKey;
                    std::vector<ProxyPacketRecordSummary> records;
                    collector->GetProxyPacketRecordSummariesDelta(
                        knownCount,
                        firstKey,
                        lastKey,
                        reset,
                        totalCount,
                        currentFirstKey,
                        currentLastKey,
                        records);

                    response["reset"] = reset;
                    response["totalCount"] = totalCount;
                    response["firstKey"] = currentFirstKey;
                    response["lastKey"] = currentLastKey;

                    for (const auto& record : records) {
                        json pkt;
                        pkt["sequence"] = record.sequence;
                        pkt["connectionId"] = record.connectionId;
                        pkt["timestamp"] = record.timestamp;
                        pkt["username"] = record.username;
                        pkt["gameID"] = record.gameID;
                        pkt["clientIP"] = record.clientIP;
                        pkt["targetHost"] = record.targetHost;
                        pkt["sniHost"] = record.sniHost;
                        pkt["targetPort"] = record.targetPort;
                        pkt["sslMitmEnabled"] = record.sslMitmEnabled;
                        pkt["isRequest"] = record.isRequest;
                        pkt["dataLength"] = record.dataLength;
                        pkt["dataPreview"] = record.dataPreview;
                        response["packets"].push_back(pkt);
                    }
                }
            }

            AB_LOG_INFO_CAT(
                LOG_CAT_COLLECTOR,
                "[代理数据] 列表响应: instance=" + instanceId +
                ", enabled=" + std::string(response["enabled"].get<bool>() ? "true" : "false") +
                ", reset=" + std::string(response["reset"].get<bool>() ? "true" : "false") +
                ", totalCount=" + std::to_string(response["totalCount"].get<size_t>()) +
                ", returned=" + std::to_string(response["packets"].size()) +
                ", firstKey=" + response["firstKey"].get<std::string>() +
                ", lastKey=" + response["lastKey"].get<std::string>());

            UIBridge_PushMessage(response.dump());
        }
        else if (action == "config_search_proxydata") {
            std::string instanceId = msg.value("instanceId", "");
            std::string hexQuery = msg.value("hexQuery", "");

            std::vector<uint8_t> pattern;
            std::string parseError;
            if (!UIBridge_ParseHexSearchQuery(hexQuery, pattern, parseError)) {
                UIBridge_Toast("error", "搜索失败", parseError);
            } else {
                {
                    std::lock_guard<std::mutex> lock(g_proxydataSearchMutex);
                    g_proxydataSearchInstanceId = instanceId;
                    g_proxydataSearchHex = hexQuery;
                    g_proxydataSearchPattern = pattern;
                }

                json refreshMsg;
                refreshMsg["action"] = "config_get_proxydata";
                refreshMsg["instanceId"] = instanceId;
                refreshMsg["knownCount"] = 0;
                refreshMsg["firstKey"] = "";
                refreshMsg["lastKey"] = "";
                UIBridge_HandleMessage(refreshMsg.dump().c_str());
            }
        }
        else if (action == "config_get_proxydata_detail") {
            std::string instanceId = msg.value("instanceId", "");
            size_t index = msg.value("index", static_cast<size_t>(0));
            uint64_t sequence = msg.value("sequence", static_cast<uint64_t>(0));
            auto& instMgr = InstanceManager::GetInstance();
            auto* instance = instMgr.GetSocksForwardInstance(instanceId);

            json response;
            response["type"] = "proxydata_detail";
            response["instanceId"] = instanceId;
            response["index"] = index;
            response["sequence"] = sequence;
            response["success"] = false;
            response["fullDataHex"] = "";

            if (instance && instance->GetCollector()) {
                ProxyPacketRecord record;
                bool found = false;
                if (sequence > 0) {
                    found = instance->GetCollector()->GetProxyPacketRecordBySequence(sequence, record);
                } else {
                    found = instance->GetCollector()->GetProxyPacketRecordByOrderedIndex(index, record);
                }
                if (found) {
                    response["success"] = true;
                    response["fullDataHex"] = UIBridge_BuildFullHexString(record.fullData);
                    response["connectionId"] = record.connectionId;
                    response["timestamp"] = record.timestamp;
                    response["username"] = record.username;
                    response["gameID"] = record.gameID;
                    response["clientIP"] = record.clientIP;
                    response["targetHost"] = record.targetHost;
                    response["sniHost"] = record.sniHost;
                    response["targetPort"] = record.targetPort;
                    response["sslMitmEnabled"] = record.sslMitmEnabled;
                    response["dataLength"] = record.dataLength;
                    response["isRequest"] = record.isRequest;
                }
            }

            UIBridge_PushMessage(response.dump());
        }
        else if (action == "config_clear_proxydata") {
            std::string instanceId = msg.value("instanceId", "");
            auto& instMgr = InstanceManager::GetInstance();
            auto* instance = instMgr.GetSocksForwardInstance(instanceId);

            if (instance && instance->GetCollector()) {
                instance->GetCollector()->ClearProxyPacketRecords();
                UIBridge_Toast("success", "Cleared", "Proxy data cleared");
            } else {
                UIBridge_Toast("error", "Clear failed", "Instance not found");
            }
        }
        else if (action == "config_set_proxydata_settings") {
            std::string instanceId = msg.value("instanceId", "");
            bool enabled = msg.value("enabled", false);
            int bufferSize = msg.value("bufferSize", 200);

            auto& instMgr = InstanceManager::GetInstance();
            auto* instance = instMgr.GetSocksForwardInstance(instanceId);

            if (instance && instance->GetCollector()) {
                auto* collector = instance->GetCollector();
                collector->SetProxyPacketRecordEnabled(enabled);
                collector->SetProxyPacketBufferSize(static_cast<size_t>(bufferSize));

                // 鎸佷箙鍖栧埌鏁版嵁搴?
                if (g_database) {
                    std::string key = "proxydata_enabled_" + instanceId;
                    g_database->SetConfigValue(key, enabled ? "1" : "0");
                    key = "proxydata_buffersize_" + instanceId;
                    g_database->SetConfigValue(key, std::to_string(bufferSize));
                }

                AB_LOG_INFO_CAT(
                    LOG_CAT_COLLECTOR,
                    "[代理数据] 已保存页面设置: instance=" + instanceId +
                    ", enabled=" + std::string(enabled ? "true" : "false") +
                    ", bufferSize=" + std::to_string(bufferSize));

                UIBridge_Toast("success", "Saved", enabled ? "Packet log enabled" : "Packet log disabled");
            } else {
                AB_LOG_WARNING_CAT(
                    LOG_CAT_COLLECTOR,
                    "[代理数据] 保存页面设置失败，未找到 SocksForward 实例: instance=" + instanceId);
                UIBridge_Toast("error", "Set failed", "Instance not found");
            }
        }
        else if (action == "config_set_proxydata_realtime") {
            std::string instanceId = msg.value("instanceId", "");
            bool enabled = msg.value("enabled", false);
            UIBridge_SetProxydataSubscription(instanceId, enabled);
        }
        // ========== Logs ==========
        else if (action == "get_logs") {
            auto logs = Logger::GetLogs();
            UIBridge_PushLogsSnapshotLocal(logs);
        }
        else if (action == "set_logs_realtime") {
            const bool enabled = msg.value("enabled", false);
            g_logsRealtimeSubscribed.store(enabled, std::memory_order_release);
            UIBridge_ResetRealtimeLogsStateLocal();
            if (enabled) {
                const auto logs = Logger::GetLogs();
                {
                    std::lock_guard<std::mutex> lock(g_logsRealtimeMutex);
                    UIBridge_UpdateRealtimeLogsStateLocked(logs);
                }
                UIBridge_PushLogsSnapshotLocal(logs);
            }
        }
        else if (action == "log_clear") {
            Logger::Clear();
            UIBridge_Toast("success", "Cleared", "Logs cleared");
            UIBridge_HandleMessage("{\"action\":\"get_logs\"}");
        }
    } catch (const std::exception& e) {
        AB_LOG_ERROR("[UI Bridge] 澶勭悊娑堟伅澶辫触: " + std::string(e.what()));
    }
}

// Background push thread
void UIBridge_StartPushThread() {
    g_pushThreadRunning = true;
    g_pushThread = std::thread([]() {
        int statusTick = 0;
        while (g_pushThreadRunning) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            UIBridge_PushRealtimeLogsDeltaIfNeededLocal();
            statusTick++;
            if (statusTick >= 3) {
                statusTick = 0;
                // Push status update in background only when payload changed
                UIBridge_PushStatus(false);
            }
        }
    });
}

void UIBridge_StopPushThread() {
    g_pushThreadRunning = false;
    g_logsRealtimeSubscribed.store(false, std::memory_order_release);
    UIBridge_ResetRealtimeLogsStateLocal();
    if (g_pushThread.joinable()) {
        g_pushThread.join();
    }
    RemoteBrowserServer::Stop();
}










