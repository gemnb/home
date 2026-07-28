// CloudIntegration.cpp — ABProtect-backed replacement for SProtect CloudIntegration
// This file replaces the old CloudIntegration.obj and routes all auth through ABProtect Server.

#include "CloudIntegration.h"
#include "ABProtectIntegration.h"
#include "ABProtectSDK.h"

#include <windows.h>
#include <mutex>
#include <thread>
#include <chrono>

namespace CloudIntegration {

static CloudConfig s_config;
static std::mutex s_mutex;
static std::string s_cachedUsername;
static std::string s_cachedPassword;

void SetConfig(const CloudConfig& cfg) {
    std::lock_guard<std::mutex> lk(s_mutex);
    s_config = cfg;
}

CloudConfig GetConfig() {
    std::lock_guard<std::mutex> lk(s_mutex);
    return s_config;
}

bool Login(const std::string& username, const std::string& password, std::string& outError, int* outErrorCode) {
    ABProtectLayer::CheckPoint_Login();

    ABProtectLayer::LoginResult result = ABProtectLayer::Login(username, password);
    if (result.success) {
        std::lock_guard<std::mutex> lk(s_mutex);
        s_cachedUsername = username;
        s_cachedPassword = password;
        outError.clear();
        return true;
    }

    outError = result.errorMsg;
    if (outErrorCode) *outErrorCode = -1;
    return false;
}

bool TrialLogin(std::string& outError, int* outErrorCode) {
    outError = "试用登录已禁用，请使用正式账号或卡密登录";
    if (outErrorCode) *outErrorCode = -99;
    return false;
}

void Logout() {
    ABProtectLayer::Logout();
}

bool IsLoggedIn() {
    return ABProtectLayer::IsLoggedIn();
}

bool QueryOnlineClients(std::vector<CloudOnlineClient>& outClients, std::string& outError) {
    outClients.clear();
    outError = "ABProtect模式暂不支持查询在线客户端";
    return false;
}

bool KickOnlineClientByCID(unsigned int cid, std::string& outError) {
    (void)cid;
    outError = "ABProtect模式暂不支持踢线";
    return false;
}

bool GetUpdateInfo(CloudUpdateInfo& outInfo, std::string& outError) {
    outInfo.valid = true;
    outInfo.localVer = s_config.localVer;
    outInfo.serverVer = s_config.localVer;
    outInfo.forceUpdate = false;
    outInfo.minVer = 0;
    outInfo.minVerEnabled = false;
    outError.clear();
    return true;
}

bool RegisterUser(const std::string& username, const std::string& password, const std::string& superPassword,
    const std::string& rechargeCards, std::string& outError) {
    (void)username; (void)password; (void)superPassword; (void)rechargeCards;
    outError = "ABProtect模式下请在服务端管理面板注册账号";
    return false;
}

bool RechargeUser(const std::string& username, const std::string& rechargeCards, CloudRechargeInfo& outInfo, std::string& outError) {
    (void)username; (void)rechargeCards;
    outInfo = {};
    outError = "ABProtect模式下请在服务端管理面板续费";
    return false;
}

bool BeatOnce(std::string& outError) {
    ABProtectLayer::CheckPoint_Network();
    if (ABProtectLayer::Heartbeat()) {
        outError.clear();
        return true;
    }
    outError = "心跳失败";
    return false;
}

bool BeatOnce(std::string& outError, int* outErrorCode) {
    bool ok = BeatOnce(outError);
    if (outErrorCode) *outErrorCode = ok ? 0 : -1;
    return ok;
}

bool UpdateTimeoutMs(int timeoutMs, std::string& outError) {
    (void)timeoutMs;
    outError.clear();
    return true;
}

CloudUserInfo QueryUserInfo(std::string* outError) {
    auto info = ABProtectLayer::GetUserInfo();
    CloudUserInfo result;
    result.valid = ABProtectLayer::IsLoggedIn();
    result.username = info.username;
    result.expireTimeText = info.expireTime;
    result.daysLeft = info.remainingDays;
    result.onlineCount = 1;
    if (outError) outError->clear();
    return result;
}

bool GetNotices(std::string& outNotices, std::string& outError) {
    outNotices = "";
    outError.clear();
    return true;
}

bool CloudRequestJson(int cloudId, const std::string& requestJson, std::string& outResponseJson, std::string& outError) {
    (void)cloudId; (void)requestJson;
    outResponseJson = "{\"ok\":true}";
    outError.clear();
    return true;
}

void CloudRequestJsonAsync(int cloudId, const std::string& requestJson, CloudRequestCallback callback) {
    std::thread([cloudId, requestJson, callback]() {
        std::string resp, err;
        bool ok = CloudRequestJson(cloudId, requestJson, resp, err);
        if (callback) callback(ok, resp, err);
    }).detach();
}

void RequirePermitAsync(int cloudId, const std::string& contextJson, PermitCallback callback) {
    std::thread([callback]() {
        if (callback) callback(true, "");
    }).detach();
}

void ReportEventAsync(const std::string& eventName, std::string detailJsonObject) {
    (void)eventName; (void)detailJsonObject;
}

void ReportMetricAsync(const std::string& metricName, double value, std::string unit, std::string tagsJsonObject) {
    (void)metricName; (void)value; (void)unit; (void)tagsJsonObject;
}

void ShutdownReporter() {}

bool RequirePermit(int cloudId, const std::string& contextJson, std::string& outError) {
    (void)cloudId; (void)contextJson;
    ABProtectLayer::CheckPoint_Critical();
    if (!ABProtectLayer::IsLoggedIn()) {
        outError = "未登录";
        return false;
    }
    outError.clear();
    return true;
}

bool Checkpoint(int checkpointCloudId, const std::string& contextJson, std::string& outError) {
    (void)checkpointCloudId; (void)contextJson;
    ABProtectLayer::CheckPoint_Network();
    if (!ABProtectLayer::IsLoggedIn()) {
        outError = "授权已失效";
        return false;
    }
    outError.clear();
    return true;
}

bool CheckpointSoft(int checkpointCloudId, const std::string& contextJson, std::string& outError) {
    (void)checkpointCloudId; (void)contextJson;
    if (!ABProtectLayer::IsLoggedIn()) {
        outError.clear();
        return true;
    }
    return Checkpoint(checkpointCloudId, contextJson, outError);
}

bool CheckpointCached(int checkpointCloudId, std::string& outError) {
    (void)checkpointCloudId;
    if (!ABProtectLayer::IsLoggedIn()) {
        outError = "未登录";
        return false;
    }
    outError.clear();
    return true;
}

bool CheckpointCachedSoft(int checkpointCloudId, std::string& outError) {
    (void)checkpointCloudId;
    outError.clear();
    return true;
}

bool ReconnectWithCachedCredentials(std::string& outError, int* outErrorCode) {
    std::string user, pass;
    {
        std::lock_guard<std::mutex> lk(s_mutex);
        user = s_cachedUsername;
        pass = s_cachedPassword;
    }
    if (user.empty()) {
        outError = "无缓存凭据";
        if (outErrorCode) *outErrorCode = -1;
        return false;
    }
    return Login(user, pass, outError, outErrorCode);
}

} // namespace CloudIntegration
