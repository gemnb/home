#define WIN32_LEAN_AND_MEAN
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include "RemoteBrowserServer.h"

#include "Logger.h"
#include "web_resource.h"
#include "res/httplib.h"
#include "res/json.hpp"

#include <atomic>
#include <condition_variable>
#include <cctype>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#pragma comment(lib, "iphlpapi.lib")

using json = nlohmann::json;

namespace {

std::mutex g_stateMutex;
std::mutex g_actionExecMutex;
RemoteBrowserServer::Config g_config;
std::string g_lastError;
std::unique_ptr<httplib::Server> g_server;
std::thread g_serverThread;
std::atomic<bool> g_running(false);
void (*g_actionHandler)(const char* jsonMessage) = nullptr;

std::mutex g_sessionMutex;
std::condition_variable g_sessionCv;

struct SessionState {
    std::deque<std::string> messages;
    uint64_t lastSeenMs = 0;
};

std::unordered_map<std::string, SessionState> g_sessions;

constexpr size_t kMaxQueuedMessagesPerSession = 512;
constexpr uint64_t kSessionTtlMs = 10ULL * 60ULL * 1000ULL;
constexpr int kPollWaitMs = 15000;

uint64_t NowMs() {
    return GetTickCount64();
}

std::string LoadResourceText(int resourceId) {
    HRSRC hRes = FindResource(nullptr, MAKEINTRESOURCE(resourceId), RT_RCDATA);
    if (!hRes) return "";
    HGLOBAL hData = LoadResource(nullptr, hRes);
    if (!hData) return "";
    DWORD size = SizeofResource(nullptr, hRes);
    if (size == 0) return "";
    const char* data = static_cast<const char*>(LockResource(hData));
    if (!data) return "";
    return std::string(data, size);
}

std::string BuildInlineHtml() {
    std::string html = LoadResourceText(IDR_HTML_INDEX);
    std::string css = LoadResourceText(IDR_CSS_STYLE);
    std::string js = LoadResourceText(IDR_JS_APP);

    if (html.empty()) {
        return "<!doctype html><html><body><h1>Remote UI unavailable</h1></body></html>";
    }

    const std::string linkTag = "<link rel=\"stylesheet\" href=\"style.css\">";
    const std::string styleBlock = "<style>\n" + css + "\n</style>";
    const size_t linkPos = html.find(linkTag);
    if (linkPos != std::string::npos) {
        html.replace(linkPos, linkTag.size(), styleBlock);
    } else {
        const size_t headEnd = html.find("</head>");
        if (headEnd != std::string::npos) {
            html.insert(headEnd, styleBlock);
        }
    }

    const std::string scriptTag = "<script src=\"app.js\"></script>";
    const std::string scriptBlock = "<script>\n" + js + "\n</script>";
    const size_t scriptPos = html.find(scriptTag);
    if (scriptPos != std::string::npos) {
        html.replace(scriptPos, scriptTag.size(), scriptBlock);
    } else {
        const size_t bodyEnd = html.find("</body>");
        if (bodyEnd != std::string::npos) {
            html.insert(bodyEnd, scriptBlock);
        }
    }

    return html;
}

void CleanupExpiredSessionsLocked(uint64_t nowMs) {
    for (auto it = g_sessions.begin(); it != g_sessions.end();) {
        if (nowMs - it->second.lastSeenMs > kSessionTtlMs) {
            it = g_sessions.erase(it);
        } else {
            ++it;
        }
    }
}

std::string NormalizeSessionId(const std::string& sid) {
    if (sid.empty() || sid.size() > 64) return "";
    for (unsigned char ch : sid) {
        if (!std::isalnum(ch) && ch != '-' && ch != '_') {
            return "";
        }
    }
    return sid;
}

std::string GetRequestToken(const httplib::Request& req) {
    if (req.has_header("X-Remote-Token")) {
        return req.get_header_value("X-Remote-Token");
    }
    if (req.has_param("token")) {
        return req.get_param_value("token");
    }
    return "";
}

bool IsAuthorized(const httplib::Request& req, const RemoteBrowserServer::Config& cfg) {
    if (cfg.accessToken.empty()) {
        return false;
    }
    return GetRequestToken(req) == cfg.accessToken;
}

void SetJsonResponse(httplib::Response& res, const json& payload, int status = 200) {
    res.status = status;
    res.set_header("Cache-Control", "no-store");
    res.set_header("Pragma", "no-cache");
    res.set_content(payload.dump(), "application/json; charset=utf-8");
}

void SetForbidden(httplib::Response& res, const std::string& message) {
    json out;
    out["ok"] = false;
    out["error"] = message;
    SetJsonResponse(res, out, 403);
}

std::vector<std::string> PollMessages(const std::string& sessionId) {
    std::unique_lock<std::mutex> lock(g_sessionMutex);
    const uint64_t now = NowMs();
    CleanupExpiredSessionsLocked(now);

    auto& session = g_sessions[sessionId];
    session.lastSeenMs = now;

    g_sessionCv.wait_for(lock, std::chrono::milliseconds(kPollWaitMs), [&]() {
        return !g_running.load() || !session.messages.empty();
    });

    std::vector<std::string> out;
    out.reserve(session.messages.size());
    while (!session.messages.empty()) {
        out.push_back(std::move(session.messages.front()));
        session.messages.pop_front();
    }
    session.lastSeenMs = NowMs();
    return out;
}

void EnsureSessionRegistered(const std::string& sessionId) {
    if (sessionId.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_sessionMutex);
    const uint64_t now = NowMs();
    CleanupExpiredSessionsLocked(now);
    auto& session = g_sessions[sessionId];
    session.lastSeenMs = now;
}

void BroadcastToSessions(const std::string& jsonMessage) {
    std::lock_guard<std::mutex> lock(g_sessionMutex);
    const uint64_t now = NowMs();
    CleanupExpiredSessionsLocked(now);
    for (auto& item : g_sessions) {
        auto& queue = item.second.messages;
        if (queue.size() >= kMaxQueuedMessagesPerSession) {
            queue.pop_front();
        }
        queue.push_back(jsonMessage);
    }
    g_sessionCv.notify_all();
}

void SetServerLastError(const std::string& errorText) {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    g_lastError = errorText;
}

void SetupRoutes(httplib::Server& server, const RemoteBrowserServer::Config& cfg, const std::string& html) {
    server.Get("/", [cfg, html](const httplib::Request& req, httplib::Response& res) {
        if (!IsAuthorized(req, cfg)) {
            res.status = 403;
            res.set_content("Forbidden: invalid token", "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(html, "text/html; charset=utf-8");
    });

    server.Get("/healthz", [](const httplib::Request&, httplib::Response& res) {
        json out;
        out["ok"] = true;
        out["running"] = g_running.load();
        SetJsonResponse(res, out, 200);
    });

    server.Post("/api/action", [cfg](const httplib::Request& req, httplib::Response& res) {
        if (!IsAuthorized(req, cfg)) {
            SetForbidden(res, "Invalid token");
            return;
        }

        if (!g_actionHandler) {
            json out;
            out["ok"] = false;
            out["error"] = "Backend action handler is not initialized";
            SetJsonResponse(res, out, 500);
            return;
        }

        json actionJson = json::parse(req.body, nullptr, false);
        if (actionJson.is_discarded() || !actionJson.is_object()) {
            json out;
            out["ok"] = false;
            out["error"] = "Request body is not valid JSON";
            SetJsonResponse(res, out, 400);
            return;
        }

        const std::string action = actionJson.value("action", "");
        if (action.empty()) {
            json out;
            out["ok"] = false;
            out["error"] = "Missing action";
            SetJsonResponse(res, out, 400);
            return;
        }

        if (action == "remote_browser_set_mode") {
            SetForbidden(res, "Remote session cannot change remote server config");
            return;
        }

        std::string sid;
        if (req.has_header("X-Remote-Sid")) {
            sid = req.get_header_value("X-Remote-Sid");
        } else if (req.has_param("sid")) {
            sid = req.get_param_value("sid");
        }
        sid = NormalizeSessionId(sid);
        if (!sid.empty()) {
            EnsureSessionRegistered(sid);
        }

        try {
            std::lock_guard<std::mutex> actionLock(g_actionExecMutex);
            g_actionHandler(req.body.c_str());
        } catch (const std::exception& e) {
            json out;
            out["ok"] = false;
            out["error"] = std::string("Action execution failed: ") + e.what();
            SetJsonResponse(res, out, 500);
            return;
        }

        json out;
        out["ok"] = true;
        SetJsonResponse(res, out, 200);
    });

    server.Get("/api/poll", [cfg](const httplib::Request& req, httplib::Response& res) {
        if (!IsAuthorized(req, cfg)) {
            SetForbidden(res, "Invalid token");
            return;
        }

        std::string sid;
        if (req.has_param("sid")) {
            sid = req.get_param_value("sid");
        } else if (req.has_header("X-Remote-Sid")) {
            sid = req.get_header_value("X-Remote-Sid");
        }
        sid = NormalizeSessionId(sid);
        if (sid.empty()) {
            json out;
            out["ok"] = false;
            out["error"] = "Missing valid sid";
            SetJsonResponse(res, out, 400);
            return;
        }

        const auto messages = PollMessages(sid);
        json out;
        out["ok"] = true;
        out["messages"] = json::array();
        for (const auto& message : messages) {
            json parsed = json::parse(message, nullptr, false);
            if (!parsed.is_discarded()) {
                out["messages"].push_back(parsed);
            }
        }

        SetJsonResponse(res, out, 200);
    });
}

} // namespace

namespace RemoteBrowserServer {

void SetActionHandler(void (*handler)(const char* jsonMessage)) {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    g_actionHandler = handler;
}

bool ApplyConfig(const Config& config, std::string& outError) {
    outError.clear();

    if (config.bindHost.empty()) {
        outError = "Bind host cannot be empty";
        SetServerLastError(outError);
        return false;
    }

    if (config.port <= 0 || config.port > 65535) {
        outError = "Invalid bind port";
        SetServerLastError(outError);
        return false;
    }

    if (config.enabled && config.accessToken.empty()) {
        outError = "Access token cannot be empty";
        SetServerLastError(outError);
        return false;
    }

    Stop();

    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_config = config;
    }

    if (!config.enabled) {
        SetServerLastError("");
        return true;
    }

    auto server = std::make_unique<httplib::Server>();
    server->set_read_timeout(5, 0);
    server->set_write_timeout(5, 0);
    server->set_idle_interval(0, 200000);
    server->set_keep_alive_max_count(100);
    server->set_keep_alive_timeout(10);
    server->set_payload_max_length(1024 * 1024 * 2);
    server->new_task_queue = [] {
        return new httplib::ThreadPool(8, 256);
    };

    const std::string html = BuildInlineHtml();
    SetupRoutes(*server, config, html);

    if (!server->bind_to_port(config.bindHost.c_str(), config.port)) {
        outError = "Failed to bind host/port";
        SetServerLastError(outError);
        return false;
    }

    httplib::Server* serverPtr = server.get();
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_server = std::move(server);
        g_running.store(true);
        g_lastError.clear();
    }

    g_serverThread = std::thread([serverPtr, config]() {
        AB_LOG_INFO("[RemoteBrowser] Server started: " + config.bindHost + ":" + std::to_string(config.port));
        serverPtr->listen_after_bind();
        AB_LOG_INFO("[RemoteBrowser] Server stopped");
        g_running.store(false);
        g_sessionCv.notify_all();
    });

    return true;
}

void Stop() {
    std::thread threadToJoin;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        if (g_server) {
            g_server->stop();
        }
        g_running.store(false);
        if (g_serverThread.joinable()) {
            threadToJoin = std::move(g_serverThread);
        }
    }

    g_sessionCv.notify_all();

    if (threadToJoin.joinable()) {
        if (threadToJoin.get_id() == std::this_thread::get_id()) {
            threadToJoin.detach();
        } else {
            threadToJoin.join();
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_server.reset();
    }

    {
        std::lock_guard<std::mutex> lock(g_sessionMutex);
        g_sessions.clear();
    }
}

bool IsRunning() {
    return g_running.load();
}

Config GetConfig() {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    return g_config;
}

std::string GetLastError() {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    return g_lastError;
}

bool IsPrivateIPv4Host(const std::string& host) {
    unsigned int a = 0, b = 0, c = 0, d = 0;
    if (sscanf_s(host.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
        return false;
    }
    if (a == 10 || a == 127 || a == 0) {
        return true;
    }
    if (a == 172 && b >= 16 && b <= 31) {
        return true;
    }
    if (a == 192 && b == 168) {
        return true;
    }
    if (a == 169 && b == 254) {
        return true;
    }
    return false;
}

std::string DetectPreferredIPv4Host() {
    ULONG bufferSize = 16 * 1024;
    std::vector<unsigned char> buffer(bufferSize);
    IP_ADAPTER_ADDRESSES* addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;

    ULONG result = GetAdaptersAddresses(AF_INET, flags, nullptr, addresses, &bufferSize);
    if (result == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(bufferSize);
        addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        result = GetAdaptersAddresses(AF_INET, flags, nullptr, addresses, &bufferSize);
    }

    auto scoreAdapter = [](const IP_ADAPTER_ADDRESSES* adapter, const std::string& ip) {
        int score = 0;
        if (adapter->OperStatus == IfOperStatusUp) {
            score += 1000;
        }
        if (adapter->FirstGatewayAddress != nullptr) {
            score += 300;
        }
        if (adapter->IfType == IF_TYPE_ETHERNET_CSMACD) {
            score += 220;
        } else if (adapter->IfType == IF_TYPE_IEEE80211) {
            score += 200;
        } else if (adapter->IfType == IF_TYPE_PPP) {
            score += 120;
        }
        if (!IsPrivateIPv4Host(ip)) {
            score += 80;
        }
        return score;
    };

    std::string bestIp;
    int bestScore = -1;
    std::string fallbackIp;

    if (result == NO_ERROR && addresses != nullptr) {
        for (auto* adapter = addresses; adapter != nullptr; adapter = adapter->Next) {
            if (adapter->OperStatus != IfOperStatusUp) {
                continue;
            }
            if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK || adapter->IfType == IF_TYPE_TUNNEL) {
                continue;
            }

            for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next) {
                if (!unicast->Address.lpSockaddr || unicast->Address.lpSockaddr->sa_family != AF_INET) {
                    continue;
                }

                auto* addr4 = reinterpret_cast<sockaddr_in*>(unicast->Address.lpSockaddr);
                char ipBuffer[INET_ADDRSTRLEN] = {};
                if (!inet_ntop(AF_INET, &addr4->sin_addr, ipBuffer, sizeof(ipBuffer))) {
                    continue;
                }

                std::string ip = ipBuffer;
                if (ip.empty() || ip == "127.0.0.1" || ip == "0.0.0.0" || (ip.rfind("169.254.", 0) == 0)) {
                    continue;
                }

                if (fallbackIp.empty()) {
                    fallbackIp = ip;
                }

                const int score = scoreAdapter(adapter, ip);
                if (score > bestScore) {
                    bestScore = score;
                    bestIp = ip;
                }
            }
        }
    }

    if (!bestIp.empty()) {
        return bestIp;
    }
    if (!fallbackIp.empty()) {
        return fallbackIp;
    }

    char hostname[256] = {};
    if (gethostname(hostname, sizeof(hostname)) != 0) {
        return "";
    }

    hostent* host = gethostbyname(hostname);
    if (!host || !host->h_addr_list) {
        return "";
    }

    for (int index = 0; host->h_addr_list[index] != nullptr; ++index) {
        auto* addr = reinterpret_cast<in_addr*>(host->h_addr_list[index]);
        const char* ipText = inet_ntoa(*addr);
        if (ipText && *ipText && std::string(ipText) != "127.0.0.1" && std::string(ipText) != "0.0.0.0") {
            return ipText;
        }
    }

    return "";
}

std::string BuildAccessUrl(const Config& config) {
    std::string host = config.bindHost;
    if (host == "0.0.0.0" || host == "::" || host == "[::]") {
        const std::string detectedHost = DetectPreferredIPv4Host();
        host = detectedHost.empty() ? "127.0.0.1" : detectedHost;
    }
    if (host.empty()) {
        host = "127.0.0.1";
    }

    return "http://" + host + ":" + std::to_string(config.port) + "/?token=" + config.accessToken;
}

void PushMessage(const std::string& jsonMessage) {
    if (!g_running.load() || jsonMessage.empty()) {
        return;
    }
    BroadcastToSessions(jsonMessage);
}

} // namespace RemoteBrowserServer


