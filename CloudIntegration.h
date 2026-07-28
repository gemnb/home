#pragma once

#pragma warning(disable: 4819)

#include <string>
#include <vector>
#include <functional>

namespace CloudIntegration {

enum CloudID : int {
    CloudID_GetPolicySummary = 1000,
    CloudID_GetPreLoginUpdate = 1005,
    CloudID_GetAppConfig = 1001,
    CloudID_GetAntiCCConfig = 1002,
    CloudID_GetWPEFilterRules = 1003,
    CloudID_GetSocksAccounts = 1004,
    CloudID_UploadWPEFilters = 1010,
    CloudID_UploadSocksInstances = 1011,
    CloudID_DownloadSocksInstances = 1012,
    CloudID_PermitStartCollector = 2001,
    CloudID_PermitStopCollector = 2002,
    CloudID_PermitStartHeartbeatForwarder = 2011,
    CloudID_PermitStopHeartbeatForwarder = 2012,
    CloudID_PermitApplyRuntimeConfig = 2021,
    CloudID_PermitExportOrSnapshot = 2031,
    CloudID_CheckpointBase = 3000,
    CloudID_ReportEvent = 4001,
    CloudID_ReportMetrics = 4002,
};

struct CloudConfig {
    std::string softwareName = "AB工具";
    std::string serverIp = "127.0.0.1";
    int serverPort = 8896;
    int timeoutMs = 30 * 1000;
    int localVer = 1;
    bool popMsg = false;
};

struct CloudUserInfo {
    bool valid = false;
    std::string username;
    std::string expireTimeText;
    int daysLeft = -1;
    int onlineCount = -1;
};

struct CloudRechargeInfo {
    bool valid = false;
    std::string oldExpireTimeText;
    std::string newExpireTimeText;
    unsigned long long oldFYI = 0;
    unsigned long long newFYI = 0;
    unsigned int rechargeCount = 0;
};

struct CloudOnlineClient {
    unsigned int cid = 0;
    std::string computerName;
    std::string winVer;
    unsigned long long cloudInitTs = 0;
    std::string pcSignMasked;
};

struct CloudUpdateInfo {
    bool valid = false;
    int localVer = 0;
    bool forceUpdate = false;
    int serverVer = 0;
    bool directUrl = false;
    std::string url;
    std::string runExe;
    std::string runCmd;
    int minVer = 0;
    bool minVerEnabled = true;
    bool mustUpdateByMinVer = false;
    bool kickOnMinVerFail = false;
};

void SetConfig(const CloudConfig& cfg);
CloudConfig GetConfig();

bool Login(const std::string& username, const std::string& password, std::string& outError, int* outErrorCode = nullptr);
bool TrialLogin(std::string& outError, int* outErrorCode = nullptr);
void Logout();
bool IsLoggedIn();

bool QueryOnlineClients(std::vector<CloudOnlineClient>& outClients, std::string& outError);
bool KickOnlineClientByCID(unsigned int cid, std::string& outError);
bool GetUpdateInfo(CloudUpdateInfo& outInfo, std::string& outError);

bool RegisterUser(const std::string& username, const std::string& password, const std::string& superPassword,
    const std::string& rechargeCards, std::string& outError);
bool RechargeUser(const std::string& username, const std::string& rechargeCards, CloudRechargeInfo& outInfo, std::string& outError);

bool BeatOnce(std::string& outError);
bool BeatOnce(std::string& outError, int* outErrorCode);
bool UpdateTimeoutMs(int timeoutMs, std::string& outError);
CloudUserInfo QueryUserInfo(std::string* outError = nullptr);
bool GetNotices(std::string& outNotices, std::string& outError);
bool CloudRequestJson(int cloudId, const std::string& requestJson, std::string& outResponseJson, std::string& outError);

using CloudRequestCallback = std::function<void(bool success, const std::string& responseJson, const std::string& errorMsg)>;
using PermitCallback = std::function<void(bool permitted, const std::string& errorMsg)>;
void CloudRequestJsonAsync(int cloudId, const std::string& requestJson, CloudRequestCallback callback);
void RequirePermitAsync(int cloudId, const std::string& contextJson, PermitCallback callback);

void ReportEventAsync(const std::string& eventName, std::string detailJsonObject = {});
void ReportMetricAsync(const std::string& metricName, double value, std::string unit = {}, std::string tagsJsonObject = {});
void ShutdownReporter();

bool RequirePermit(int cloudId, const std::string& contextJson, std::string& outError);
bool Checkpoint(int checkpointCloudId, const std::string& contextJson, std::string& outError);
bool CheckpointSoft(int checkpointCloudId, const std::string& contextJson, std::string& outError);
bool CheckpointCached(int checkpointCloudId, std::string& outError);
bool CheckpointCachedSoft(int checkpointCloudId, std::string& outError);

bool ReconnectWithCachedCredentials(std::string& outError, int* outErrorCode = nullptr);

} // namespace CloudIntegration
