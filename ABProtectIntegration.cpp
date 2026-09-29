#define _CRT_SECURE_NO_WARNINGS
#include "ABProtectIntegration.h"
#include "ABProtectSDK.h"
#include "TinyAES.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>
#include <winhttp.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <chrono>
#include <cstring>
#include <vector>
#include <ctime>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "winhttp.lib")

namespace ABProtectLayer {

// ============================================================
// Internal State — Encrypted Auth Token (replaces simple bool)
// ============================================================

static std::atomic<uint64_t> s_authToken{0};
static uint8_t s_tokenXorKey[8] = {};
static std::atomic<uint32_t> s_heartbeatCounter{0};
static std::atomic<int64_t> s_lastHeartbeatTime{0};

static uint8_t s_sessionTokenRaw[32] = {};
static uint8_t s_aesKey[32] = {};
static uint8_t s_sessionNonce[8] = {};
static std::atomic<uint32_t> s_seqNum{1};
static std::mutex s_mutex;
static std::string s_username;
static std::string s_expireTime;
static int s_remainingDays = 0;
static std::string s_hwid;
static std::string s_sessionTokenStr;

static std::function<void(const std::string&)> s_onForceOffline;
static std::function<void()> s_onExpired;

static std::atomic<bool> s_verifyThreadRunning{false};

// Server config — overridden by compile-time macros or packer
#ifndef ABPROTECT_SERVER_HOST
#define ABPROTECT_SERVER_HOST "127.0.0.1"
#endif
#ifndef ABPROTECT_SERVER_PORT
#define ABPROTECT_SERVER_PORT 9527
#endif

static char g_serverHost[64] = ABPROTECT_SERVER_HOST;
static uint16_t g_serverPort = ABPROTECT_SERVER_PORT;

// ABProtect protocol constants
static constexpr uint32_t ABPC_MAGIC = 0x41424350;
static constexpr uint16_t ABPC_VERSION = 0x0002;
static constexpr uint16_t CMD_LOGIN = 0x01;
static constexpr uint16_t CMD_LOGOUT = 0x02;
static constexpr uint16_t CMD_CALL_FUNC = 0x03;
static constexpr uint16_t CMD_HEARTBEAT = 0x04;

#pragma pack(push, 1)
struct PacketHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t cmd;
    uint32_t bodyLen;
    uint8_t  sessionToken[32];
};

struct LoginRequest {
    char cardKey[64];
    char hwid[64];
};

struct LoginResponse {
    uint32_t resultCode;
    uint8_t  sessionToken[32];
    uint8_t  aesKey[32];
    uint8_t  sessionNonce[8];
};

struct CallFuncRequest {
    uint32_t funcID;
    int64_t  param0;
    int64_t  param1;
    int64_t  param2;
    int64_t  param3;
};

struct CallFuncResponse {
    uint32_t resultCode;
    int64_t  returnValue;
};

struct EncryptedEnvelope {
    uint32_t seqNum;
    uint8_t  hmac[16];
};
#pragma pack(pop)

static constexpr uint32_t ENCRYPTED_ENVELOPE_SIZE = 20;

// ============================================================
// AES-256-CTR + CBC-MAC (inline implementation for cloud protocol)
// ============================================================

static const uint8_t s_sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};
static const uint8_t s_rcon[11] = {0,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36};

static void AES256_KeyExpand(const uint8_t key[32], uint8_t rk[240]) {
    memcpy(rk, key, 32);
    uint8_t temp[4];
    for (int i = 8; i < 60; i++) {
        memcpy(temp, rk + (i-1)*4, 4);
        if (i % 8 == 0) {
            uint8_t t = temp[0];
            temp[0] = s_sbox[temp[1]] ^ s_rcon[i/8];
            temp[1] = s_sbox[temp[2]];
            temp[2] = s_sbox[temp[3]];
            temp[3] = s_sbox[t];
        } else if (i % 8 == 4) {
            for (int j = 0; j < 4; j++) temp[j] = s_sbox[temp[j]];
        }
        for (int j = 0; j < 4; j++) rk[i*4+j] = rk[(i-8)*4+j] ^ temp[j];
    }
}

static uint8_t xtime(uint8_t x) { return (x << 1) ^ ((x >> 7) * 0x1b); }

static void AES256_EncryptBlock_Inline(const uint8_t in[16], uint8_t out[16], const uint8_t key[32]) {
    uint8_t rk[240];
    AES256_KeyExpand(key, rk);
    uint8_t s[16];
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ rk[i];
    for (int round = 1; round <= 14; round++) {
        for (int i = 0; i < 16; i++) s[i] = s_sbox[s[i]];
        uint8_t t;
        t=s[1]; s[1]=s[5]; s[5]=s[9]; s[9]=s[13]; s[13]=t;
        t=s[2]; s[2]=s[10]; s[10]=t; t=s[6]; s[6]=s[14]; s[14]=t;
        t=s[15]; s[15]=s[11]; s[11]=s[7]; s[7]=s[3]; s[3]=t;
        if (round < 14) {
            for (int c = 0; c < 4; c++) {
                int i = c*4;
                uint8_t a0=s[i],a1=s[i+1],a2=s[i+2],a3=s[i+3];
                uint8_t h = a0^a1^a2^a3;
                s[i]   = a0 ^ xtime(a0^a1) ^ h;
                s[i+1] = a1 ^ xtime(a1^a2) ^ h;
                s[i+2] = a2 ^ xtime(a2^a3) ^ h;
                s[i+3] = a3 ^ xtime(a3^a0) ^ h;
            }
        }
        for (int i = 0; i < 16; i++) s[i] ^= rk[round*16+i];
    }
    memcpy(out, s, 16);
}

static void CloudCTRCrypt(uint8_t* data, size_t len, const uint8_t key[32], uint32_t seqNum) {
    uint8_t iv[16] = {};
    iv[0] = (uint8_t)(seqNum); iv[1] = (uint8_t)(seqNum>>8);
    iv[2] = (uint8_t)(seqNum>>16); iv[3] = (uint8_t)(seqNum>>24);
    uint8_t ctr[16], keystream[16];
    memcpy(ctr, iv, 16);
    size_t off = 0;
    while (off < len) {
        AES256_EncryptBlock_Inline(ctr, keystream, key);
        size_t chunk = (len - off < 16) ? (len - off) : 16;
        for (size_t i = 0; i < chunk; i++) data[off+i] ^= keystream[i];
        off += chunk;
        for (int i = 15; i >= 0; i--) { if (++ctr[i]) break; }
    }
}

static void ComputeCBCMAC(const uint8_t key[32], uint32_t seqNum, const uint8_t* data, size_t dataLen, uint8_t mac[16]) {
    uint8_t state[16] = {};
    size_t totalLen = 4 + dataLen;
    size_t logOff = 0;
    while (logOff < totalLen) {
        uint8_t block[16] = {};
        size_t fill = 0;
        while (fill < 16 && logOff < totalLen) {
            if (logOff < 4) block[fill] = (uint8_t)(seqNum >> (logOff*8));
            else block[fill] = data[logOff - 4];
            fill++; logOff++;
        }
        for (int i = 0; i < 16; i++) state[i] ^= block[i];
        uint8_t enc[16];
        AES256_EncryptBlock_Inline(state, enc, key);
        memcpy(state, enc, 16);
    }
    memcpy(mac, state, 16);
}

// ============================================================
// Encrypted Auth Token helpers
// ============================================================

static uint64_t ComputeExpectedToken(const uint8_t sessionToken[32], const uint8_t xorKey[8]) {
    uint64_t hash = 0x5A3C6F91E2D47B08ULL;
    for (int i = 0; i < 32; i++) {
        hash ^= (uint64_t)sessionToken[i] << ((i % 8) * 8);
        hash *= 0x100000001B3ULL;
    }
    uint64_t xk = 0;
    memcpy(&xk, xorKey, 8);
    return hash ^ xk;
}

static void GenerateRandomKey(uint8_t key[8]) {
    HCRYPTPROV hProv = 0;
    if (CryptAcquireContextW(&hProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        CryptGenRandom(hProv, 8, key);
        CryptReleaseContext(hProv, 0);
    } else {
        LARGE_INTEGER pc;
        QueryPerformanceCounter(&pc);
        uint64_t seed = pc.QuadPart ^ GetTickCount64() ^ (uint64_t)GetCurrentProcessId();
        memcpy(key, &seed, 8);
    }
}

// ============================================================
// HWID Generation
// ============================================================

std::string GetHWID() {
    if (!s_hwid.empty()) return s_hwid;

    char volumeName[MAX_PATH] = {};
    DWORD serialNumber = 0;
    GetVolumeInformationA("C:\\", volumeName, MAX_PATH, &serialNumber, nullptr, nullptr, nullptr, 0);

    char computerName[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD nameLen = sizeof(computerName);
    GetComputerNameA(computerName, &nameLen);

    char hwid[128] = {};
    snprintf(hwid, sizeof(hwid), "%08X-%s", serialNumber, computerName);
    s_hwid = hwid;
    return s_hwid;
}

// ============================================================
// TCP Communication
// ============================================================

static SOCKET ConnectToServer() {
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return INVALID_SOCKET;

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(g_serverPort);
    inet_pton(AF_INET, g_serverHost, &addr.sin_addr);

    DWORD timeout = 5000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        closesocket(sock);
        return INVALID_SOCKET;
    }
    return sock;
}

static bool SendPacket(SOCKET sock, uint16_t cmd, const void* body, uint32_t bodyLen) {
    PacketHeader hdr = {};
    hdr.magic = ABPC_MAGIC;
    hdr.version = ABPC_VERSION;
    hdr.cmd = cmd;
    hdr.bodyLen = bodyLen;
    if (s_sessionTokenStr.size() > 0) {
        memcpy(hdr.sessionToken, s_sessionTokenStr.c_str(),
               min(s_sessionTokenStr.size(), (size_t)32));
    }

    if (send(sock, (const char*)&hdr, sizeof(hdr), 0) != sizeof(hdr)) return false;
    if (bodyLen > 0 && body) {
        if (send(sock, (const char*)body, bodyLen, 0) != (int)bodyLen) return false;
    }
    return true;
}

static bool RecvFull(SOCKET sock, void* buf, int len) {
    char* p = (char*)buf;
    int remaining = len;
    while (remaining > 0) {
        int n = recv(sock, p, remaining, 0);
        if (n <= 0) return false;
        p += n;
        remaining -= n;
    }
    return true;
}

// ============================================================
// Force Termination — secure shutdown on tamper detection
// ============================================================

void ForceTerminate(const char* reason) {
    ABPROTECT_CHECK_DEBUGGER;
    SecureZeroMemory(s_aesKey, sizeof(s_aesKey));
    SecureZeroMemory(s_sessionTokenRaw, sizeof(s_sessionTokenRaw));
    SecureZeroMemory(s_sessionNonce, sizeof(s_sessionNonce));
    SecureZeroMemory(s_tokenXorKey, sizeof(s_tokenXorKey));
    s_authToken.store(0);
    s_lastHeartbeatTime.store(0);
    s_heartbeatCounter.store(0);
    s_verifyThreadRunning.store(false);
    if (s_onForceOffline) s_onForceOffline(reason ? reason : "protection_violation");
    TerminateProcess(GetCurrentProcess(), 1);
}

// ============================================================
// Periodic Verification Thread
// ============================================================

static void VerificationThread() {
    s_verifyThreadRunning.store(true);
    while (s_authToken.load() != 0) {
        Sleep(30000 + (GetTickCount() % 10000));
        ABPROTECT_CHECK_DEBUGGER;
        ABPROTECT_CHECK_INTEGRITY;

        if (!Heartbeat()) {
            ForceTerminate("heartbeat_failed");
            return;
        }

        int64_t hwidVal = 0;
        std::string hw = GetHWID();
        if (hw.size() >= 8) memcpy(&hwidVal, hw.c_str(), 8);

        int64_t tokPart = 0;
        memcpy(&tokPart, s_sessionTokenRaw, 8);

        int64_t result = Cloud_VerifyToken(tokPart, (int64_t)s_heartbeatCounter.load(), hwidVal, (int64_t)time(nullptr));
        if (result <= 0) {
            ForceTerminate("cloud_verify_failed");
            return;
        }

        if (IsDebuggerPresent()) {
            ForceTerminate("debugger_detected_runtime");
            return;
        }
    }
    s_verifyThreadRunning.store(false);
}

// ============================================================
// Authorization Implementation
// ============================================================

bool Init() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    GetHWID();
    return true;
}

void Shutdown() {
    if (s_authToken.load() != 0) Logout();
    WSACleanup();
}

LoginResult CardLogin(const std::string& cardKey) {
    ABPROTECT_VM_BEGIN;
    LoginResult result;
    ABPROTECT_CHECK_DEBUGGER;

    SOCKET sock = ConnectToServer();
    if (sock == INVALID_SOCKET) {
        result.errorMsg = "\xe6\x97\xa0\xe6\xb3\x95\xe8\xbf\x9e\xe6\x8e\xa5\xe6\x8e\x88\xe6\x9d\x83\xe6\x9c\x8d\xe5\x8a\xa1\xe5\x99\xa8";
        return result;
    }

    LoginRequest req = {};
    strncpy(req.cardKey, cardKey.c_str(), sizeof(req.cardKey) - 1);
    strncpy(req.hwid, GetHWID().c_str(), sizeof(req.hwid) - 1);

    if (!SendPacket(sock, CMD_LOGIN, &req, sizeof(req))) {
        closesocket(sock);
        result.errorMsg = "\xe5\x8f\x91\xe9\x80\x81\xe7\x99\xbb\xe5\xbd\x95\xe8\xaf\xb7\xe6\xb1\x82\xe5\xa4\xb1\xe8\xb4\xa5";
        return result;
    }

    PacketHeader respHdr = {};
    if (!RecvFull(sock, &respHdr, sizeof(respHdr))) {
        closesocket(sock);
        result.errorMsg = "\xe6\x8e\xa5\xe6\x94\xb6\xe5\x93\x8d\xe5\xba\x94\xe8\xb6\x85\xe6\x97\xb6";
        return result;
    }

    LoginResponse resp = {};
    if (respHdr.bodyLen >= sizeof(resp)) {
        RecvFull(sock, &resp, sizeof(resp));
    }
    closesocket(sock);

    if (resp.resultCode == 0) {
        std::lock_guard<std::mutex> lk(s_mutex);
        memcpy(s_sessionTokenRaw, resp.sessionToken, 32);
        s_sessionTokenStr.assign((char*)resp.sessionToken, strnlen((char*)resp.sessionToken, 32));
        memcpy(s_aesKey, resp.aesKey, 32);
        memcpy(s_sessionNonce, resp.sessionNonce, 8);
        s_seqNum.store(1);

        GenerateRandomKey(s_tokenXorKey);
        s_authToken.store(ComputeExpectedToken(s_sessionTokenRaw, s_tokenXorKey));
        s_heartbeatCounter.store(0);
        s_lastHeartbeatTime.store((int64_t)time(nullptr));

        s_username = cardKey;
        result.success = true;
        result.username = cardKey;

        if (!s_verifyThreadRunning.load()) {
            std::thread(VerificationThread).detach();
        }
    } else {
        const char* errors[] = {"", "\xe5\x8d\xa1\xe5\xaf\x86\xe6\x97\xa0\xe6\x95\x88\xe6\x88\x96\xe5\xb7\xb2\xe8\xbf\x87\xe6\x9c\x9f", "\xe5\x8d\xa1\xe5\xaf\x86\xe5\xb7\xb2\xe8\xbf\x87\xe6\x9c\x9f", "\xe8\xae\xbe\xe5\xa4\x87\xe4\xb8\x8d\xe5\x8c\xb9\xe9\x85\x8d", "\xe4\xbc\x9a\xe8\xaf\x9d\xe9\x94\x99\xe8\xaf\xaf"};
        int idx = (resp.resultCode < 5) ? resp.resultCode : 0;
        result.errorMsg = errors[idx];
    }
    ABPROTECT_VM_END;
    return result;
}

LoginResult Login(const std::string& username, const std::string& password) {
    ABPROTECT_VM_BEGIN;
    std::string combined = username + ":" + password;
    ABPROTECT_VM_END;
    return CardLogin(combined);
}

// ============================================================
// Heartbeat & Logout
// ============================================================

bool Heartbeat() {
    ABPROTECT_CHECK_INTEGRITY;
    if (s_authToken.load() == 0) return false;

    SOCKET sock = ConnectToServer();
    if (sock == INVALID_SOCKET) return false;

    if (!SendPacket(sock, CMD_HEARTBEAT, nullptr, 0)) {
        closesocket(sock);
        return false;
    }

    PacketHeader respHdr = {};
    bool ok = RecvFull(sock, &respHdr, sizeof(respHdr));
    closesocket(sock);

    if (!ok || respHdr.cmd == 0xFF) {
        s_authToken.store(0);
        if (s_onForceOffline) s_onForceOffline("\xe5\xbf\x83\xe8\xb7\xb3\xe9\xaa\x8c\xe8\xaf\x81\xe5\xa4\xb1\xe8\xb4\xa5");
        return false;
    }

    s_lastHeartbeatTime.store((int64_t)time(nullptr));
    s_heartbeatCounter.fetch_add(1);
    return true;
}

void Logout() {
    if (s_authToken.load() == 0) return;

    SOCKET sock = ConnectToServer();
    if (sock != INVALID_SOCKET) {
        SendPacket(sock, CMD_LOGOUT, nullptr, 0);
        closesocket(sock);
    }

    std::lock_guard<std::mutex> lk(s_mutex);
    s_authToken.store(0);
    s_lastHeartbeatTime.store(0);
    s_heartbeatCounter.store(0);
    s_sessionTokenStr.clear();
    SecureZeroMemory(s_sessionTokenRaw, 32);
    SecureZeroMemory(s_aesKey, 32);
    SecureZeroMemory(s_sessionNonce, 8);
    SecureZeroMemory(s_tokenXorKey, 8);
    s_username.clear();
}

bool IsLoggedIn() {
    ABPROTECT_CHECK_INTEGRITY;
    uint64_t tok = s_authToken.load();
    if (tok == 0) return false;

    uint64_t expected = ComputeExpectedToken(s_sessionTokenRaw, s_tokenXorKey);
    if (tok != expected) {
        ForceTerminate("auth_token_tampered");
        return false;
    }

    int64_t now = (int64_t)time(nullptr);
    if (now - s_lastHeartbeatTime.load() > 90) {
        Logout();
        if (s_onForceOffline) s_onForceOffline("heartbeat_timeout");
        return false;
    }
    return true;
}

UserInfo GetUserInfo() {
    std::lock_guard<std::mutex> lk(s_mutex);
    return {s_username, s_expireTime, s_remainingDays};
}

void SetOnForceOffline(std::function<void(const std::string&)> cb) { s_onForceOffline = cb; }
void SetOnExpired(std::function<void()> cb) { s_onExpired = cb; }

// ============================================================
// Cloud Computing — Runtime Network Call
// ============================================================

static int64_t CallCloudFunction(uint32_t funcID, int64_t p0, int64_t p1, int64_t p2, int64_t p3) {
    ABPROTECT_VM_BEGIN;
    ABPROTECT_CHECK_DEBUGGER;
    if (s_authToken.load() == 0) return 0;

    SOCKET sock = ConnectToServer();
    if (sock == INVALID_SOCKET) return 0;

    CallFuncRequest req = {};
    req.funcID = funcID;
    req.param0 = p0;
    req.param1 = p1;
    req.param2 = p2;
    req.param3 = p3;

    uint32_t seq = s_seqNum.fetch_add(1);

    uint8_t payload[sizeof(CallFuncRequest)];
    memcpy(payload, &req, sizeof(req));

    uint8_t mac[16];
    ComputeCBCMAC(s_aesKey, seq, payload, sizeof(payload), mac);
    CloudCTRCrypt(payload, sizeof(payload), s_aesKey, seq);

    EncryptedEnvelope env = {};
    env.seqNum = seq;
    memcpy(env.hmac, mac, 16);

    PacketHeader hdr = {};
    hdr.magic = ABPC_MAGIC;
    hdr.version = ABPC_VERSION;
    hdr.cmd = CMD_CALL_FUNC;
    hdr.bodyLen = ENCRYPTED_ENVELOPE_SIZE + sizeof(CallFuncRequest);
    if (s_sessionTokenStr.size() > 0)
        memcpy(hdr.sessionToken, s_sessionTokenStr.c_str(), min(s_sessionTokenStr.size(), (size_t)32));

    bool ok = true;
    ok = ok && (send(sock, (const char*)&hdr, sizeof(hdr), 0) == sizeof(hdr));
    ok = ok && (send(sock, (const char*)&env, sizeof(env), 0) == sizeof(env));
    ok = ok && (send(sock, (const char*)payload, sizeof(payload), 0) == sizeof(payload));

    if (!ok) { closesocket(sock); return 0; }

    PacketHeader respHdr = {};
    if (!RecvFull(sock, &respHdr, sizeof(respHdr))) { closesocket(sock); return 0; }
    if (respHdr.bodyLen < ENCRYPTED_ENVELOPE_SIZE + sizeof(CallFuncResponse)) { closesocket(sock); return 0; }

    EncryptedEnvelope respEnv = {};
    if (!RecvFull(sock, &respEnv, sizeof(respEnv))) { closesocket(sock); return 0; }

    uint8_t respPayload[sizeof(CallFuncResponse)];
    if (!RecvFull(sock, respPayload, sizeof(respPayload))) { closesocket(sock); return 0; }
    closesocket(sock);

    uint8_t respMac[16];
    uint8_t decrypted[sizeof(CallFuncResponse)];
    memcpy(decrypted, respPayload, sizeof(decrypted));
    CloudCTRCrypt(decrypted, sizeof(decrypted), s_aesKey, respEnv.seqNum);
    ComputeCBCMAC(s_aesKey, respEnv.seqNum, decrypted, sizeof(decrypted), respMac);

    if (memcmp(respMac, respEnv.hmac, 16) != 0) return 0;

    CallFuncResponse resp;
    memcpy(&resp, decrypted, sizeof(resp));
    ABPROTECT_VM_END;
    return (resp.resultCode == 0) ? resp.returnValue : 0;
}

// ============================================================
// Cloud Computing Functions
// ============================================================

#pragma optimize("", off)

int64_t Cloud_AES_KeySchedule(int64_t p0, int64_t p1, int64_t p2, int64_t p3) {
    ABPROTECT_CLOUD_BEGIN;
    int64_t r = CallCloudFunction(1, p0, p1, p2, p3);
    ABPROTECT_CLOUD_END;
    return r;
}

int64_t Cloud_TrustScore(int64_t successAuths, int64_t authFailures, int64_t failedAttempts, int64_t packedFlags) {
    ABPROTECT_CLOUD_BEGIN;
    int64_t r = CallCloudFunction(2, successAuths, authFailures, failedAttempts, packedFlags);
    if (r == 0 && !IsLoggedIn()) return 0;
    ABPROTECT_CLOUD_END;
    return r;
}

int64_t Cloud_DynamicRateLimit(int64_t baseMax, int64_t trustScore, int64_t successAuths, int64_t consecutiveFails) {
    ABPROTECT_CLOUD_BEGIN;
    int64_t r = CallCloudFunction(3, baseMax, trustScore, successAuths, consecutiveFails);
    if (r == 0) r = baseMax;
    ABPROTECT_CLOUD_END;
    return r;
}

int64_t Cloud_VerifyToken(int64_t tokenPart1, int64_t tokenPart2, int64_t hwid, int64_t timestamp) {
    ABPROTECT_CLOUD_BEGIN;
    int64_t r = CallCloudFunction(4, tokenPart1, tokenPart2, hwid, timestamp);
    ABPROTECT_CLOUD_END;
    return r;
}

int64_t Cloud_HeartbeatDecrypt(int64_t encDataHash, int64_t timestamp, int64_t hwid, int64_t sessionSeq) {
    ABPROTECT_CLOUD_BEGIN;
    int64_t r = CallCloudFunction(5, encDataHash, timestamp, hwid, sessionSeq);
    ABPROTECT_CLOUD_END;
    return r;
}

int64_t Cloud_ValidatePattern(int64_t patternType, int64_t dataHash, int64_t ruleId, int64_t reserved) {
    ABPROTECT_CLOUD_BEGIN;
    int64_t r = CallCloudFunction(6, patternType, dataHash, ruleId, reserved);
    ABPROTECT_CLOUD_END;
    return r;
}

int64_t Cloud_ComputeChecksum(int64_t dataPart1, int64_t dataPart2, int64_t len, int64_t salt) {
    ABPROTECT_CLOUD_BEGIN;
    int64_t r = CallCloudFunction(7, dataPart1, dataPart2, len, salt);
    ABPROTECT_CLOUD_END;
    return r;
}

int64_t Cloud_GetLicenseFlags(int64_t hwid, int64_t tokenPart, int64_t featureGroup, int64_t reserved) {
    ABPROTECT_CLOUD_BEGIN;
    int64_t r = CallCloudFunction(8, hwid, tokenPart, featureGroup, reserved);
    ABPROTECT_CLOUD_END;
    return r;
}

#pragma optimize("", on)

// ============================================================
// Integrity Check Points
// ============================================================

void CheckPoint_Main() {
    ABPROTECT_CHECK_DEBUGGER;
    ABPROTECT_CHECK_INTEGRITY;
    if (IsDebuggerPresent()) ForceTerminate("debugger_at_main");
}

void CheckPoint_Login() {
    ABPROTECT_CHECK_DEBUGGER;
    ABPROTECT_CHECK_DUMP;
}

void CheckPoint_Network() {
    ABPROTECT_CHECK_INTEGRITY;
    if (!IsLoggedIn()) ForceTerminate("network_op_without_auth");
}

void CheckPoint_Critical() {
    ABPROTECT_CHECK_DEBUGGER;
    ABPROTECT_CHECK_INTEGRITY;
    ABPROTECT_CHECK_DUMP;
    if (!IsLoggedIn()) ForceTerminate("critical_op_without_auth");
}

} // namespace ABProtectLayer
