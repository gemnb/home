#include "PoolCleanupServer.h"
#include "Logger.h"
#include <sstream>
#include "CloudIntegration.h"
#include <vector>

PoolCleanupServer::PoolCleanupServer(int port)
    : port(port), listenSocket(INVALID_SOCKET), isRunning(false) {
}

PoolCleanupServer::~PoolCleanupServer() {
    Stop();
}

bool PoolCleanupServer::Start() {
    if (isRunning) {
        AB_LOG_WARNING("[PoolCleanup] Server already running");
        return false;
    }

    if (CloudIntegration::IsLoggedIn()) {
        std::string denyReason;
        std::string contextJson = std::string("{\"op\":\"PoolCleanupServer.Start\",\"port\":") + std::to_string(port) + "}";
        if (!CloudIntegration::Checkpoint(3144, contextJson, denyReason)) {
            AB_LOG_ERROR("[云计算] PoolCleanupServer.Start被Checkpoint拒绝: " + denyReason);
            return false;
        }
    }

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        AB_LOG_ERROR("[PoolCleanup] WSAStartup failed");
        return false;
    }

    listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket == INVALID_SOCKET) {
        AB_LOG_ERROR("[PoolCleanup] Create socket failed");
        WSACleanup();
        return false;
    }

    int reuse = 1;
    setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(port);

    if (bind(listenSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        AB_LOG_ERROR("[PoolCleanup] Bind port failed: " + std::to_string(port));
        closesocket(listenSocket);
        WSACleanup();
        return false;
    }

    if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) {
        AB_LOG_ERROR("[PoolCleanup] Listen failed");
        closesocket(listenSocket);
        WSACleanup();
        return false;
    }

    isRunning = true;
    serverThread = std::thread(&PoolCleanupServer::ServerLoop, this);

    AB_LOG_INFO("[PoolCleanup] Server started on port: " + std::to_string(port));
    return true;
}

void PoolCleanupServer::Stop() {
    if (!isRunning) return;

    isRunning = false;

    if (listenSocket != INVALID_SOCKET) {
        closesocket(listenSocket);
        listenSocket = INVALID_SOCKET;
    }

    if (serverThread.joinable()) {
        serverThread.join();
    }

    AB_LOG_INFO("[PoolCleanup] Server stopped");
}

void PoolCleanupServer::ServerLoop() {
    while (isRunning) {
        sockaddr_in clientAddr;
        int clientAddrSize = sizeof(clientAddr);

        SOCKET clientSocket = accept(listenSocket, (sockaddr*)&clientAddr, &clientAddrSize);

        if (clientSocket == INVALID_SOCKET) {
            if (isRunning) {
                int error = WSAGetLastError();
                if (error != WSAEINTR && error != WSAENOTSOCK) {
                    AB_LOG_ERROR("[PoolCleanup] Accept failed, error: " + std::to_string(error));
                }
            }
            continue;
        }

        char clientIP[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, clientIP, INET_ADDRSTRLEN);

        std::thread(&PoolCleanupServer::HandleClient, this, clientSocket, std::string(clientIP)).detach();
    }
}

void PoolCleanupServer::HandleClient(SOCKET clientSocket, const std::string& clientIP) {
    if (CloudIntegration::IsLoggedIn()) {
        std::string denyReason;
        if (!CloudIntegration::Checkpoint(3145, "{\"op\":\"PoolCleanupServer.HandleClient\"}", denyReason)) {
            AB_LOG_ERROR("[云计算] PoolCleanupServer.HandleClient被Checkpoint拒绝: " + denyReason);
            closesocket(clientSocket);
            return;
        }
    }

    char buffer[4096];
    int bytesReceived = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);

    if (bytesReceived <= 0) {
        closesocket(clientSocket);
        return;
    }

    buffer[bytesReceived] = '\0';
    std::string request(buffer);

    std::string method, path;
    std::istringstream requestStream(request);
    requestStream >> method >> path;

    AB_LOG_INFO_CAT(LOG_CAT_API, "[PoolCleanup] " + clientIP + " " + method + " " + path);

    if (method == "GET" && (path == "/" || path == "/cleanup")) {
        SendHTML(clientSocket, GetLoginPageHTML());
    }
    else if (method == "POST" && path == "/cleanup") {
        std::string username = ParsePostData(request, "username");
        std::string password = ParsePostData(request, "password");

        if (username.empty() || password.empty()) {
            SendHTML(clientSocket, GetResultPageHTML(false, "Username and password required"));
            closesocket(clientSocket);
            return;
        }

        std::string gameID;
        if (validateCallback && validateCallback(username, password, gameID)) {
            if (gameID.empty()) {
                SendHTML(clientSocket, GetResultPageHTML(false,
                    "Account verified, but no GameID bound"));
            }
            else {
                if (clearPoolCallback) {
                    clearPoolCallback(gameID);
                    AB_LOG_INFO("[PoolCleanup] User [" + username + "] cleared GameID [" +
                        gameID + "] pool data");
                    SendHTML(clientSocket, GetResultPageHTML(true,
                        "Success! Pool data cleared for GameID", gameID));
                }
                else {
                    SendHTML(clientSocket, GetResultPageHTML(false, "Clear callback not set"));
                }
            }
        }
        else {
            AB_LOG_WARNING("[PoolCleanup] User [" + username + "] validation failed");
            SendHTML(clientSocket, GetResultPageHTML(false, "Invalid username or password"));
        }
    }
    else if (method == "POST" && path == "/api/cleanup") {
        std::string username = ParsePostData(request, "username");
        std::string password = ParsePostData(request, "password");

        if (username.empty() || password.empty()) {
            SendJSON(clientSocket, false, "Username and password required");
            closesocket(clientSocket);
            return;
        }

        std::string gameID;
        if (validateCallback && validateCallback(username, password, gameID)) {
            if (gameID.empty()) {
                SendJSON(clientSocket, false, "No GameID bound to this account");
            }
            else {
                if (clearPoolCallback) {
                    clearPoolCallback(gameID);
                    SendJSON(clientSocket, true, "Cleared GameID: " + gameID);
                }
                else {
                    SendJSON(clientSocket, false, "Clear callback not set");
                }
            }
        }
        else {
            SendJSON(clientSocket, false, "Invalid username or password");
        }
    }
    else {
        SendResponse(clientSocket, 404, "text/html", "<h1>404 Not Found</h1>");
    }

    closesocket(clientSocket);
}

void PoolCleanupServer::SendResponse(SOCKET socket, int statusCode,
    const std::string& contentType, const std::string& body) {

    std::string statusText;
    switch (statusCode) {
        case 200: statusText = "OK"; break;
        case 400: statusText = "Bad Request"; break;
        case 401: statusText = "Unauthorized"; break;
        case 404: statusText = "Not Found"; break;
        case 500: statusText = "Internal Server Error"; break;
        default: statusText = "Unknown"; break;
    }

    std::ostringstream response;
    response << "HTTP/1.1 " << statusCode << " " << statusText << "\r\n";
    response << "Content-Type: " << contentType << "; charset=utf-8\r\n";
    response << "Content-Length: " << body.length() << "\r\n";
    response << "Connection: close\r\n";
    response << "\r\n";
    response << body;

    std::string responseStr = response.str();
    send(socket, responseStr.c_str(), static_cast<int>(responseStr.length()), 0);
}

void PoolCleanupServer::SendHTML(SOCKET socket, const std::string& html) {
    SendResponse(socket, 200, "text/html", html);
}

void PoolCleanupServer::SendJSON(SOCKET socket, bool success, const std::string& message) {
    std::ostringstream json;
    json << "{\"success\":" << (success ? "true" : "false");
    json << ",\"message\":\"" << message << "\"}";
    SendResponse(socket, 200, "application/json", json.str());
}

std::string PoolCleanupServer::ParsePostData(const std::string& request, const std::string& key) {
    size_t bodyStart = request.find("\r\n\r\n");
    if (bodyStart == std::string::npos) return "";

    std::string body = request.substr(bodyStart + 4);

    std::string searchKey = key + "=";
    size_t keyPos = body.find(searchKey);
    if (keyPos == std::string::npos) return "";

    size_t valueStart = keyPos + searchKey.length();
    size_t valueEnd = body.find('&', valueStart);
    if (valueEnd == std::string::npos) {
        valueEnd = body.length();
    }

    return UrlDecode(body.substr(valueStart, valueEnd - valueStart));
}

std::string PoolCleanupServer::UrlDecode(const std::string& str) {
    std::string result;
    result.reserve(str.length());

    for (size_t i = 0; i < str.length(); ++i) {
        if (str[i] == '%' && i + 2 < str.length()) {
            int value;
            std::istringstream is(str.substr(i + 1, 2));
            if (is >> std::hex >> value) {
                result += static_cast<char>(value);
                i += 2;
            }
            else {
                result += str[i];
            }
        }
        else if (str[i] == '+') {
            result += ' ';
        }
        else {
            result += str[i];
        }
    }

    return result;
}

std::string PoolCleanupServer::GetLoginPageHTML() {
    return R"(<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Memory Pool Cleanup</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            min-height: 100vh;
            display: flex;
            justify-content: center;
            align-items: center;
            padding: 20px;
        }
        .container {
            background: white;
            padding: 40px;
            border-radius: 16px;
            box-shadow: 0 10px 40px rgba(0,0,0,0.2);
            width: 100%;
            max-width: 400px;
        }
        h1 {
            text-align: center;
            color: #333;
            margin-bottom: 10px;
            font-size: 24px;
        }
        .subtitle {
            text-align: center;
            color: #666;
            margin-bottom: 30px;
            font-size: 14px;
        }
        .form-group {
            margin-bottom: 20px;
        }
        label {
            display: block;
            margin-bottom: 8px;
            color: #555;
            font-weight: 500;
        }
        input[type="text"], input[type="password"] {
            width: 100%;
            padding: 12px 16px;
            border: 2px solid #e1e1e1;
            border-radius: 8px;
            font-size: 16px;
            transition: border-color 0.3s, box-shadow 0.3s;
        }
        input[type="text"]:focus, input[type="password"]:focus {
            outline: none;
            border-color: #667eea;
            box-shadow: 0 0 0 3px rgba(102, 126, 234, 0.2);
        }
        button {
            width: 100%;
            padding: 14px;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white;
            border: none;
            border-radius: 8px;
            font-size: 16px;
            font-weight: 600;
            cursor: pointer;
            transition: transform 0.2s, box-shadow 0.2s;
        }
        button:hover {
            transform: translateY(-2px);
            box-shadow: 0 4px 12px rgba(102, 126, 234, 0.4);
        }
        button:active {
            transform: translateY(0);
        }
        .note {
            margin-top: 20px;
            padding: 12px;
            background: #f8f9fa;
            border-radius: 8px;
            font-size: 12px;
            color: #666;
            text-align: center;
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>Memory Pool Cleanup</h1>
        <p class="subtitle">Enter SOCKS account to verify identity</p>
        <form method="POST" action="/cleanup">
            <div class="form-group">
                <label for="username">Username</label>
                <input type="text" id="username" name="username" placeholder="Enter SOCKS username" required>
            </div>
            <div class="form-group">
                <label for="password">Password</label>
                <input type="password" id="password" name="password" placeholder="Enter password" required>
            </div>
            <button type="submit">Clear My Pool Data</button>
        </form>
        <p class="note">This will clear all pool data for the GameID bound to your account</p>
    </div>
</body>
</html>)";
}

std::string PoolCleanupServer::GetResultPageHTML(bool success, const std::string& message,
    const std::string& gameID) {

    std::string iconColor = success ? "#28a745" : "#dc3545";
    std::string icon = success ? "&#10004;" : "&#10008;";
    std::string title = success ? "Success" : "Failed";

    std::ostringstream html;
    html << R"(<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>)" << title << R"(</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            min-height: 100vh;
            display: flex;
            justify-content: center;
            align-items: center;
            padding: 20px;
        }
        .container {
            background: white;
            padding: 40px;
            border-radius: 16px;
            box-shadow: 0 10px 40px rgba(0,0,0,0.2);
            width: 100%;
            max-width: 400px;
            text-align: center;
        }
        .icon {
            font-size: 60px;
            color: )" << iconColor << R"(;
            margin-bottom: 20px;
        }
        h1 {
            color: #333;
            margin-bottom: 15px;
            font-size: 24px;
        }
        .message {
            color: #666;
            margin-bottom: 20px;
            font-size: 16px;
            line-height: 1.5;
        }
        .gameid {
            background: #f8f9fa;
            padding: 12px;
            border-radius: 8px;
            font-family: monospace;
            font-size: 14px;
            color: #333;
            margin-bottom: 20px;
            word-break: break-all;
        }
        .back-btn {
            display: inline-block;
            padding: 12px 30px;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white;
            text-decoration: none;
            border-radius: 8px;
            font-weight: 600;
            transition: transform 0.2s, box-shadow 0.2s;
        }
        .back-btn:hover {
            transform: translateY(-2px);
            box-shadow: 0 4px 12px rgba(102, 126, 234, 0.4);
        }
    </style>
</head>
<body>
    <div class="container">
        <div class="icon">)" << icon << R"(</div>
        <h1>)" << title << R"(</h1>
        <p class="message">)" << message << R"(</p>)";

    if (!gameID.empty()) {
        html << R"(
        <div class="gameid">GameID: )" << gameID << R"(</div>)";
    }

    html << R"(
        <a href="/cleanup" class="back-btn">Back</a>
    </div>
</body>
</html>)";

    return html.str();
}
