#include "HttpNotify.h"

#include <Windows.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "crypt32.lib")

// 简易JSON解析和构建
namespace {

std::string EscapeJsonString(const std::string& value) {
    std::string escaped;
    for (char c : value) {
        switch (c) {
        case '"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default: escaped += c; break;
        }
    }
    return escaped;
}

std::string BuildJsonObject(const std::vector<std::pair<std::string, std::string>>& fields) {
    std::ostringstream oss;
    oss << "{";
    bool first = true;
    for (const auto& field : fields) {
        if (!first) oss << ",";
        first = false;
        oss << "\"" << field.first << "\":\"" << EscapeJsonString(field.second) << "\"";
    }
    oss << "}";
    return oss.str();
}

std::string ToHex(const unsigned char* data, size_t len) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; i++) {
        oss << std::setw(2) << static_cast<int>(data[i]);
    }
    return oss.str();
}

std::string GenerateHmacSha256(const std::string& key, const std::string& data) {
    // 使用简化的HMAC计算
    // HMAC(K, m) = H((K ^ opad) || H((K ^ ipad) || m))
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;

    if (!CryptAcquireContextW(&hProv, NULL, MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        return "";
    }

    unsigned char ipad[64] = {};
    unsigned char opad[64] = {};
    memset(ipad, 0x36, 64);
    memset(opad, 0x5c, 64);

    for (size_t i = 0; i < key.length() && i < 64; i++) {
        ipad[i] ^= key[i];
        opad[i] ^= key[i];
    }

    // 内层哈希: H(ipad || data)
    if (!CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) {
        CryptReleaseContext(hProv, 0);
        return "";
    }

    CryptHashData(hHash, ipad, 64, 0);
    CryptHashData(hHash, (BYTE*)data.c_str(), static_cast<DWORD>(data.length()), 0);

    DWORD hashLen = 32;
    BYTE innerHash[32] = {};
    CryptGetHashParam(hHash, HP_HASHVAL, innerHash, &hashLen, 0);
    CryptDestroyHash(hHash);

    // 外层哈希: H(opad || innerHash)
    if (!CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) {
        CryptReleaseContext(hProv, 0);
        return "";
    }

    CryptHashData(hHash, opad, 64, 0);
    CryptHashData(hHash, innerHash, 32, 0);

    BYTE outerHash[32] = {};
    CryptGetHashParam(hHash, HP_HASHVAL, outerHash, &hashLen, 0);

    CryptDestroyHash(hHash);
    CryptReleaseContext(hProv, 0);

    return ToHex(outerHash, 32);
}

std::string SendAuthenticatedPost(const std::string& url, const std::string& body, const std::string& apiKey, int timeoutMs) {
    std::string result;

    // 生成时间戳和签名
    std::string timestamp = std::to_string(time(nullptr));
    std::string signature = GenerateHmacSha256(apiKey, timestamp + body);

    // 解析URL
    std::string host;
    std::string path;
    int port = 80;
    bool https = false;

    size_t pos = 0;
    if (url.find("https://") == 0) {
        https = true;
        port = 443;
        pos = 8;
    } else if (url.find("http://") == 0) {
        pos = 7;
    }

    size_t pathPos = url.find('/', pos);
    if (pathPos != std::string::npos) {
        host = url.substr(pos, pathPos - pos);
        path = url.substr(pathPos);
    } else {
        host = url.substr(pos);
        path = "/";
    }

    // 检查端口
    size_t portPos = host.find(':');
    if (portPos != std::string::npos) {
        port = std::stoi(host.substr(portPos + 1));
        host = host.substr(0, portPos);
    }

    // 转换为宽字符
    std::wstring wHost(host.begin(), host.end());
    std::wstring wPath(path.begin(), path.end());

    // 使用WinHTTP
    HINTERNET hSession = WinHttpOpen(
        L"HttpNotify/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0
    );

    if (!hSession) return result;

    WinHttpSetTimeouts(hSession, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    HINTERNET hConnect = WinHttpConnect(hSession, wHost.c_str(), port, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return result;
    }

    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect,
        L"POST",
        wPath.c_str(),
        NULL,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        https ? WINHTTP_FLAG_SECURE : 0
    );

    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return result;
    }

    // 构建请求头
    std::wostringstream headerStream;
    headerStream << L"Content-Type: application/json\r\n";
    headerStream << L"X-Api-Key: " << std::wstring(apiKey.begin(), apiKey.end()) << L"\r\n";
    headerStream << L"X-Timestamp: " << std::wstring(timestamp.begin(), timestamp.end()) << L"\r\n";
    headerStream << L"X-Signature: " << std::wstring(signature.begin(), signature.end()) << L"\r\n";

    std::wstring headers = headerStream.str();

    BOOL bResults = WinHttpSendRequest(
        hRequest,
        headers.c_str(),
        static_cast<DWORD>(headers.length()),
        (LPVOID)body.c_str(),
        static_cast<DWORD>(body.length()),
        static_cast<DWORD>(body.length()),
        0
    );

    if (bResults) {
        bResults = WinHttpReceiveResponse(hRequest, NULL);
    }

    if (bResults) {
        DWORD dwSize = 0;
        do {
            dwSize = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
            if (dwSize == 0) break;

            char* buffer = new char[dwSize + 1];
            DWORD dwDownloaded = 0;

            if (WinHttpReadData(hRequest, buffer, dwSize, &dwDownloaded)) {
                buffer[dwDownloaded] = '\0';
                result += buffer;
            }

            delete[] buffer;
        } while (dwSize > 0);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return result;
}

// 简易JSON解析（提取字符串值）
std::string GetJsonString(const std::string& json, const std::string& key) {
    std::string searchKey = "\"" + key + "\"";
    size_t pos = json.find(searchKey);
    if (pos == std::string::npos) return "";

    size_t colonPos = json.find(':', pos);
    if (colonPos == std::string::npos) return "";

    size_t quoteStart = json.find('"', colonPos);
    if (quoteStart == std::string::npos) return "";

    size_t quoteEnd = json.find('"', quoteStart + 1);
    if (quoteEnd == std::string::npos) return "";

    return json.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
}

bool GetJsonBool(const std::string& json, const std::string& key, bool defaultValue) {
    std::string searchKey = "\"" + key + "\"";
    size_t pos = json.find(searchKey);
    if (pos == std::string::npos) return defaultValue;

    size_t colonPos = json.find(':', pos);
    if (colonPos == std::string::npos) return defaultValue;

    size_t valueStart = json.find_first_not_of(" \t\r\n", colonPos + 1);
    if (valueStart == std::string::npos) return defaultValue;

    if (json.substr(valueStart, 4) == "true") return true;
    if (json.substr(valueStart, 5) == "false") return false;

    return defaultValue;
}

int GetJsonInt(const std::string& json, const std::string& key, int defaultValue) {
    std::string searchKey = "\"" + key + "\"";
    size_t pos = json.find(searchKey);
    if (pos == std::string::npos) return defaultValue;

    size_t colonPos = json.find(':', pos);
    if (colonPos == std::string::npos) return defaultValue;

    size_t valueStart = json.find_first_of("0123456789-", colonPos);
    if (valueStart == std::string::npos) return defaultValue;

    size_t valueEnd = json.find_first_not_of("0123456789", valueStart + 1);
    if (valueEnd == std::string::npos) valueEnd = json.length();

    try {
        return std::stoi(json.substr(valueStart, valueEnd - valueStart));
    } catch (...) {
        return defaultValue;
    }
}

} // anonymous namespace

HttpNotifyConfig LoadHttpNotifyConfig(const std::string& configPath) {
    HttpNotifyConfig config;

    std::ifstream file(configPath);
    if (!file.is_open()) return config;

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string json = buffer.str();

    // 解析thinkphp_notify节点
    size_t notifyPos = json.find("\"thinkphp_notify\"");
    if (notifyPos != std::string::npos) {
        size_t braceStart = json.find('{', notifyPos);
        if (braceStart != std::string::npos) {
            int braceCount = 1;
            size_t braceEnd = braceStart + 1;
            while (braceEnd < json.length() && braceCount > 0) {
                if (json[braceEnd] == '{') braceCount++;
                else if (json[braceEnd] == '}') braceCount--;
                braceEnd++;
            }

            std::string notifyJson = json.substr(braceStart, braceEnd - braceStart);
            config.enabled = GetJsonBool(notifyJson, "enabled", false);
            config.thinkphpUrl = GetJsonString(notifyJson, "url");
            config.apiKey = GetJsonString(notifyJson, "api_key");
            config.timeoutMs = GetJsonInt(notifyJson, "timeout_ms", 5000);
        }
    }

    return config;
}

bool SaveHttpNotifyConfig(const HttpNotifyConfig& config, const std::string& configPath) {
    std::ofstream file(configPath);
    if (!file.is_open()) return false;

    file << "{\n";
    file << "    \"thinkphp_notify\": {\n";
    file << "        \"enabled\": " << (config.enabled ? "true" : "false") << ",\n";
    file << "        \"url\": \"" << EscapeJsonString(config.thinkphpUrl) << "\",\n";
    file << "        \"api_key\": \"" << EscapeJsonString(config.apiKey) << "\",\n";
    file << "        \"timeout_ms\": " << config.timeoutMs << "\n";
    file << "    }\n";
    file << "}\n";

    return true;
}

bool NotifyLoginSuccess(
    const HttpNotifyConfig& config,
    const std::string& account,
    const std::string& pid,
    const std::string& publicIp,
    int apiPort,
    const std::string& rc4Key,
    const std::string& expiredAt
) {
    if (!config.enabled || config.thinkphpUrl.empty() || config.apiKey.empty()) {
        return false;
    }

    // 构建JSON请求体
    std::vector<std::pair<std::string, std::string>> fields = {
        {"account", account},
        {"pid", pid},
        {"public_ip", publicIp},
        {"api_port", std::to_string(apiPort)},
    };

    if (!rc4Key.empty()) {
        fields.push_back({"rc4_key", rc4Key});
    }

    if (!expiredAt.empty()) {
        fields.push_back({"expired_at", expiredAt});
    }

    std::string body = BuildJsonObject(fields);
    std::string url = config.thinkphpUrl + config.registerPath;

    // 发送请求
    std::string response = SendAuthenticatedPost(url, body, config.apiKey, config.timeoutMs);

    if (response.empty()) {
        return false;
    }

    // 检查响应
    int code = GetJsonInt(response, "code", 0);
    return code == 200;
}

bool NotifyLogout(const HttpNotifyConfig& config, const std::string& pid) {
    if (!config.enabled || config.thinkphpUrl.empty() || config.apiKey.empty()) {
        return false;
    }

    std::vector<std::pair<std::string, std::string>> fields = {
        {"pid", pid},
    };

    std::string body = BuildJsonObject(fields);
    std::string url = config.thinkphpUrl + "/api/user/unregister";

    std::string response = SendAuthenticatedPost(url, body, config.apiKey, config.timeoutMs);

    if (response.empty()) {
        return false;
    }

    int code = GetJsonInt(response, "code", 0);
    return code == 200;
}
