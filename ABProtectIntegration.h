#pragma once

#include <string>
#include <cstdint>
#include <functional>

// ABProtect Integration Layer — Maximum Protection
// Replaces SProtect/CloudIntegration with ABProtect authorization + cloud computing
// Features: encrypted auth token, periodic verification thread, forced termination

namespace ABProtectLayer {

// ============================================================
// Initialization & Lifecycle
// ============================================================

bool Init();
void Shutdown();

// ============================================================
// Authorization (replaces SProtect login)
// ============================================================

struct LoginResult {
    bool success = false;
    std::string errorMsg;
    std::string username;
    std::string expireTime;
    int remainingDays = 0;
    int maxConnections = 0;
    int currentConnections = 0;
};

LoginResult Login(const std::string& username, const std::string& password);
LoginResult CardLogin(const std::string& cardKey);
bool Heartbeat();
void Logout();
bool IsLoggedIn();

struct UserInfo {
    std::string username;
    std::string expireTime;
    int remainingDays = 0;
};
UserInfo GetUserInfo();

// Callbacks
void SetOnForceOffline(std::function<void(const std::string&)> cb);
void SetOnExpired(std::function<void()> cb);

// ============================================================
// Cloud Computing Functions (extracted to server by ABProtectCLI)
// ============================================================

#pragma optimize("", off)
int64_t Cloud_AES_KeySchedule(int64_t keyPtr, int64_t outPtr, int64_t keyLen, int64_t reserved);
int64_t Cloud_TrustScore(int64_t successAuths, int64_t authFailures, int64_t failedAttempts, int64_t packedFlags);
int64_t Cloud_DynamicRateLimit(int64_t baseMax, int64_t trustScore, int64_t successAuths, int64_t consecutiveFails);
int64_t Cloud_VerifyToken(int64_t tokenPart1, int64_t tokenPart2, int64_t hwid, int64_t timestamp);

int64_t Cloud_HeartbeatDecrypt(int64_t encDataHash, int64_t timestamp, int64_t hwid, int64_t sessionSeq);
int64_t Cloud_ValidatePattern(int64_t patternType, int64_t dataHash, int64_t ruleId, int64_t reserved);
int64_t Cloud_ComputeChecksum(int64_t dataPart1, int64_t dataPart2, int64_t len, int64_t salt);
int64_t Cloud_GetLicenseFlags(int64_t hwid, int64_t tokenPart, int64_t featureGroup, int64_t reserved);
#pragma optimize("", on)

// License feature flags (returned by Cloud_GetLicenseFlags)
static constexpr int64_t LICENSE_FLAG_HEARTBEAT    = 0x01;
static constexpr int64_t LICENSE_FLAG_WPE_FILTER   = 0x02;
static constexpr int64_t LICENSE_FLAG_PACKET_PROXY = 0x04;
static constexpr int64_t LICENSE_FLAG_ADVANCED_CFF = 0x08;

// ============================================================
// Integrity Check Points (scatter throughout code)
// ============================================================

void CheckPoint_Main();
void CheckPoint_Login();
void CheckPoint_Network();
void CheckPoint_Critical();

// ============================================================
// Force Termination (called on tamper / auth failure)
// ============================================================

void ForceTerminate(const char* reason);

// ============================================================
// HWID
// ============================================================

std::string GetHWID();

} // namespace ABProtectLayer
