#include "PoolQueryServer.h"
#include "Logger.h"
#include <sstream>
#include <vector>

PoolQueryServer::PoolQueryServer(int port)
    : port(port), listenSocket(INVALID_SOCKET), isRunning(false) {
}

PoolQueryServer::~PoolQueryServer() {
    Stop();
}

bool PoolQueryServer::Start() {
    if (isRunning) {
        AB_LOG_WARNING("[PoolQuery] Server already running");
        return false;
    }

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        AB_LOG_ERROR("[PoolQuery] WSAStartup failed");
        return false;
    }

    listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket == INVALID_SOCKET) {
        AB_LOG_ERROR("[PoolQuery] Create socket failed");
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
        AB_LOG_ERROR("[PoolQuery] Bind port failed: " + std::to_string(port));
        closesocket(listenSocket);
        WSACleanup();
        return false;
    }

    if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) {
        AB_LOG_ERROR("[PoolQuery] Listen failed");
        closesocket(listenSocket);
        WSACleanup();
        return false;
    }

    isRunning = true;
    serverThread = std::thread(&PoolQueryServer::ServerLoop, this);

    AB_LOG_INFO("[PoolQuery] Server started on port: " + std::to_string(port));
    return true;
}

void PoolQueryServer::Stop() {
    if (!isRunning) return;

    isRunning = false;

    if (listenSocket != INVALID_SOCKET) {
        closesocket(listenSocket);
        listenSocket = INVALID_SOCKET;
    }

    if (serverThread.joinable()) {
        serverThread.join();
    }

    AB_LOG_INFO("[PoolQuery] Server stopped");
}

void PoolQueryServer::ServerLoop() {
    while (isRunning) {
        sockaddr_in clientAddr;
        int clientAddrSize = sizeof(clientAddr);

        SOCKET clientSocket = accept(listenSocket, (sockaddr*)&clientAddr, &clientAddrSize);

        if (clientSocket == INVALID_SOCKET) {
            if (isRunning) {
                int error = WSAGetLastError();
                if (error != WSAEINTR && error != WSAENOTSOCK) {
                    AB_LOG_ERROR("[PoolQuery] Accept failed, error: " + std::to_string(error));
                }
            }
            continue;
        }

        char clientIP[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, clientIP, INET_ADDRSTRLEN);

        std::thread(&PoolQueryServer::HandleClient, this, clientSocket, std::string(clientIP)).detach();
    }
}

void PoolQueryServer::HandleClient(SOCKET clientSocket, const std::string& clientIP) {
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

    AB_LOG_INFO_CAT(LOG_CAT_API, "[PoolQuery] " + clientIP + " " + method + " " + path);

    if (method == "GET" && (path == "/" || path == "/query")) {
        SendHTML(clientSocket, GetLoginPageHTML());
    }
    else if (method == "GET" && path.find("/result?gameid=") == 0) {
        // 直接通过GameID查询（用于自动刷新）
        std::string gameID = path.substr(15);  // 去掉 "/result?gameid="

        // URL解码
        gameID = UrlDecode(gameID);

        if (gameID.empty()) {
            SendHTML(clientSocket, GetResultPageHTML(false, "\xe8\xaf\xb7\xe6\x8f\x90\xe4\xbe\x9bGameID"));
        }
        else {
            size_t packetCount = 0;
            if (getPacketCountCallback) {
                packetCount = getPacketCountCallback(gameID);
            }
            SendHTML(clientSocket, GetResultPageHTML(true, "\xe6\x9f\xa5\xe8\xaf\xa2\xe6\x88\x90\xe5\x8a\x9f", gameID, packetCount));
        }
    }
    else if (method == "POST" && path == "/query") {
        std::string username = ParsePostData(request, "username");
        std::string password = ParsePostData(request, "password");

        if (username.empty() || password.empty()) {
            SendHTML(clientSocket, GetResultPageHTML(false, "\xe8\xaf\xb7\xe8\xbe\x93\xe5\x85\xa5\xe7\x94\xa8\xe6\x88\xb7\xe5\x90\x8d\xe5\x92\x8c\xe5\xaf\x86\xe7\xa0\x81"));
        }
        else {
            std::string gameID;
            if (validateCallback && validateCallback(username, password, gameID)) {
                if (gameID.empty()) {
                    SendHTML(clientSocket, GetResultPageHTML(false,
                        "\xe8\xb4\xa6\xe5\x8f\xb7\xe9\xaa\x8c\xe8\xaf\x81\xe6\x88\x90\xe5\x8a\x9f\xef\xbc\x8c\xe4\xbd\x86\xe6\x9c\xaa\xe7\xbb\x91\xe5\xae\x9aGameID", "", 0));
                }
                else {
                    size_t packetCount = 0;
                    if (getPacketCountCallback) {
                        packetCount = getPacketCountCallback(gameID);
                    }
                    AB_LOG_INFO("[PoolQuery] User [" + username + "] queried GameID [" +
                        gameID + "] packet count: " + std::to_string(packetCount));
                    SendHTML(clientSocket, GetResultPageHTML(true,
                        "\xe6\x9f\xa5\xe8\xaf\xa2\xe6\x88\x90\xe5\x8a\x9f", gameID, packetCount, username, password));
                }
            }
            else {
                AB_LOG_WARNING("[PoolQuery] User [" + username + "] validation failed");
                SendHTML(clientSocket, GetResultPageHTML(false, "\xe7\x94\xa8\xe6\x88\xb7\xe5\x90\x8d\xe6\x88\x96\xe5\xaf\x86\xe7\xa0\x81\xe9\x94\x99\xe8\xaf\xaf"));
            }
        }
    }
    else if (method == "POST" && path == "/api/query") {
        std::string username = ParsePostData(request, "username");
        std::string password = ParsePostData(request, "password");

        if (username.empty() || password.empty()) {
            SendJSON(clientSocket, false, "\xe8\xaf\xb7\xe8\xbe\x93\xe5\x85\xa5\xe7\x94\xa8\xe6\x88\xb7\xe5\x90\x8d\xe5\x92\x8c\xe5\xaf\x86\xe7\xa0\x81");
        }
        else {
            std::string gameID;
            if (validateCallback && validateCallback(username, password, gameID)) {
                if (gameID.empty()) {
                    SendJSON(clientSocket, false, "\xe8\xaf\xa5\xe8\xb4\xa6\xe5\x8f\xb7\xe6\x9c\xaa\xe7\xbb\x91\xe5\xae\x9aGameID");
                }
                else {
                    size_t packetCount = 0;
                    if (getPacketCountCallback) {
                        packetCount = getPacketCountCallback(gameID);
                    }
                    SendJSON(clientSocket, true, "\xe6\x9f\xa5\xe8\xaf\xa2\xe6\x88\x90\xe5\x8a\x9f", gameID, packetCount);
                }
            }
            else {
                SendJSON(clientSocket, false, "\xe7\x94\xa8\xe6\x88\xb7\xe5\x90\x8d\xe6\x88\x96\xe5\xaf\x86\xe7\xa0\x81\xe9\x94\x99\xe8\xaf\xaf");
            }
        }
    }
    else if (method == "POST" && path == "/api/query/gameid") {
        // Direct GameID query for JavaScript polling (no auth required once logged in)
        std::string gameID = ParsePostData(request, "gameid");

        if (gameID.empty()) {
            SendJSON(clientSocket, false, "\xe8\xaf\xb7\xe6\x8f\x90\xe4\xbe\x9bGameID");
        }
        else {
            size_t packetCount = 0;
            if (getPacketCountCallback) {
                packetCount = getPacketCountCallback(gameID);
            }
            SendJSON(clientSocket, true, "\xe6\x9f\xa5\xe8\xaf\xa2\xe6\x88\x90\xe5\x8a\x9f", gameID, packetCount);
        }
    }
    else if (method == "POST" && path == "/clear") {
        // 清理数据包（需要验证账号）
        std::string username = ParsePostData(request, "username");
        std::string password = ParsePostData(request, "password");

        if (username.empty() || password.empty()) {
            SendHTML(clientSocket, GetResultPageHTML(false, "\xe8\xaf\xb7\xe8\xbe\x93\xe5\x85\xa5\xe7\x94\xa8\xe6\x88\xb7\xe5\x90\x8d\xe5\x92\x8c\xe5\xaf\x86\xe7\xa0\x81"));
        }
        else {
            std::string gameID;
            if (validateCallback && validateCallback(username, password, gameID)) {
                if (gameID.empty()) {
                    SendHTML(clientSocket, GetResultPageHTML(false,
                        "\xe8\xb4\xa6\xe5\x8f\xb7\xe9\xaa\x8c\xe8\xaf\x81\xe6\x88\x90\xe5\x8a\x9f\xef\xbc\x8c\xe4\xbd\x86\xe6\x9c\xaa\xe7\xbb\x91\xe5\xae\x9aGameID", "", 0));
                }
                else {
                    size_t clearedCount = 0;
                    if (clearPacketsCallback) {
                        clearedCount = clearPacketsCallback(gameID);
                    }
                    AB_LOG_INFO("[PoolQuery] User [" + username + "] cleared GameID [" +
                        gameID + "] packets: " + std::to_string(clearedCount));
                    // 清理后获取当前数量（应该是0）
                    size_t currentCount = 0;
                    if (getPacketCountCallback) {
                        currentCount = getPacketCountCallback(gameID);
                    }
                    SendHTML(clientSocket, GetClearResultPageHTML(true, gameID, clearedCount, currentCount));
                }
            }
            else {
                AB_LOG_WARNING("[PoolQuery] User [" + username + "] clear validation failed");
                SendHTML(clientSocket, GetResultPageHTML(false, "\xe7\x94\xa8\xe6\x88\xb7\xe5\x90\x8d\xe6\x88\x96\xe5\xaf\x86\xe7\xa0\x81\xe9\x94\x99\xe8\xaf\xaf"));
            }
        }
    }
    else if (method == "POST" && path == "/api/clear") {
        // JSON API清理数据包
        std::string username = ParsePostData(request, "username");
        std::string password = ParsePostData(request, "password");

        if (username.empty() || password.empty()) {
            SendJSON(clientSocket, false, "\xe8\xaf\xb7\xe8\xbe\x93\xe5\x85\xa5\xe7\x94\xa8\xe6\x88\xb7\xe5\x90\x8d\xe5\x92\x8c\xe5\xaf\x86\xe7\xa0\x81");
        }
        else {
            std::string gameID;
            if (validateCallback && validateCallback(username, password, gameID)) {
                if (gameID.empty()) {
                    SendJSON(clientSocket, false, "\xe8\xaf\xa5\xe8\xb4\xa6\xe5\x8f\xb7\xe6\x9c\xaa\xe7\xbb\x91\xe5\xae\x9aGameID");
                }
                else {
                    size_t clearedCount = 0;
                    if (clearPacketsCallback) {
                        clearedCount = clearPacketsCallback(gameID);
                    }
                    SendClearJSON(clientSocket, true, "\xe6\xb8\x85\xe7\x90\x86\xe6\x88\x90\xe5\x8a\x9f", gameID, clearedCount);
                }
            }
            else {
                SendJSON(clientSocket, false, "\xe7\x94\xa8\xe6\x88\xb7\xe5\x90\x8d\xe6\x88\x96\xe5\xaf\x86\xe7\xa0\x81\xe9\x94\x99\xe8\xaf\xaf");
            }
        }
    }
    else {
        SendResponse(clientSocket, 404, "text/html", "<h1>404 Not Found</h1>");
    }

    // 优雅关闭连接：先关闭发送端，等待数据发送完毕
    shutdown(clientSocket, SD_SEND);

    // 等待客户端关闭连接或超时
    char tempBuf[64];
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(clientSocket, &readSet);
    timeval timeout = { 1, 0 };  // 1秒超时
    if (select(0, &readSet, NULL, NULL, &timeout) > 0) {
        recv(clientSocket, tempBuf, sizeof(tempBuf), 0);
    }

    closesocket(clientSocket);
}

void PoolQueryServer::SendResponse(SOCKET socket, int statusCode,
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

void PoolQueryServer::SendHTML(SOCKET socket, const std::string& html) {
    SendResponse(socket, 200, "text/html", html);
}

void PoolQueryServer::SendJSON(SOCKET socket, bool success, const std::string& message,
    const std::string& gameID, size_t packetCount) {
    std::ostringstream json;
    json << "{\"success\":" << (success ? "true" : "false");
    json << ",\"message\":\"" << message << "\"";
    if (!gameID.empty()) {
        json << ",\"gameID\":\"" << gameID << "\"";
        json << ",\"packetCount\":" << packetCount;
    }
    json << "}";
    SendResponse(socket, 200, "application/json", json.str());
}

void PoolQueryServer::SendClearJSON(SOCKET socket, bool success, const std::string& message,
    const std::string& gameID, size_t clearedCount) {
    std::ostringstream json;
    json << "{\"success\":" << (success ? "true" : "false");
    json << ",\"message\":\"" << message << "\"";
    json << ",\"gameID\":\"" << gameID << "\"";
    json << ",\"clearedCount\":" << clearedCount;
    json << "}";
    SendResponse(socket, 200, "application/json", json.str());
}

std::string PoolQueryServer::ParsePostData(const std::string& request, const std::string& key) {
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

std::string PoolQueryServer::UrlDecode(const std::string& str) {
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

std::string PoolQueryServer::GetLoginPageHTML() {
    return R"(<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>&#20869;&#23384;&#27744;&#25968;&#25454;&#21253;&#26597;&#35810;</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, "Microsoft YaHei", sans-serif;
            background: linear-gradient(135deg, #11998e 0%, #38ef7d 100%);
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
            border-color: #11998e;
            box-shadow: 0 0 0 3px rgba(17, 153, 142, 0.2);
        }
        button {
            width: 100%;
            padding: 14px;
            background: linear-gradient(135deg, #11998e 0%, #38ef7d 100%);
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
            box-shadow: 0 4px 12px rgba(17, 153, 142, 0.4);
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
        <h1>&#20869;&#23384;&#27744;&#25968;&#25454;&#21253;&#26597;&#35810;</h1>
        <p class="subtitle">&#36755;&#20837;SOCKS&#36134;&#21495;&#26597;&#35810;&#25968;&#25454;&#21253;&#25968;&#37327;</p>
        <form method="POST" action="/query">
            <div class="form-group">
                <label for="username">&#29992;&#25143;&#21517;</label>
                <input type="text" id="username" name="username" placeholder="&#35831;&#36755;&#20837;SOCKS&#29992;&#25143;&#21517;" required>
            </div>
            <div class="form-group">
                <label for="password">&#23494;&#30721;</label>
                <input type="password" id="password" name="password" placeholder="&#35831;&#36755;&#20837;&#23494;&#30721;" required>
            </div>
            <button type="submit">&#26597;&#35810;&#25968;&#25454;&#21253;&#25968;&#37327;</button>
        </form>
        <p class="note">&#26597;&#35810;&#24744;&#32465;&#23450;&#30340;GameID&#22312;&#20869;&#23384;&#27744;&#20013;&#30340;&#25968;&#25454;&#21253;&#25968;&#37327;</p>
    </div>
</body>
</html>)";
}

std::string PoolQueryServer::GetResultPageHTML(bool success, const std::string& message,
    const std::string& gameID, size_t packetCount,
    const std::string& username, const std::string& password) {

    std::string iconColor = success ? "#28a745" : "#dc3545";
    std::string icon = success ? "&#128202;" : "&#10008;";
    // 查询结果 / 查询失败
    std::string title = success ? "&#26597;&#35810;&#32467;&#26524;" : "&#26597;&#35810;&#22833;&#36133;";

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
            font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, "Microsoft YaHei", sans-serif;
            background: linear-gradient(135deg, #11998e 0%, #38ef7d 100%);
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
            max-width: 450px;
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
        .info-card {
            background: #f8f9fa;
            padding: 20px;
            border-radius: 12px;
            margin-bottom: 20px;
        }
        .info-row {
            display: flex;
            justify-content: space-between;
            align-items: center;
            padding: 10px 0;
        }
        .info-label {
            color: #666;
            font-size: 14px;
        }
        .info-value {
            font-family: monospace;
            font-size: 14px;
            color: #333;
            font-weight: 600;
            word-break: break-all;
        }
        .packet-count {
            font-size: 64px;
            font-weight: bold;
            color: #11998e;
            margin: 20px 0;
        }
        .packet-label {
            color: #666;
            font-size: 14px;
            margin-bottom: 20px;
        }
        .refresh-note {
            color: #999;
            font-size: 12px;
            margin-bottom: 15px;
        }
        .status-dot {
            display: inline-block;
            width: 8px;
            height: 8px;
            border-radius: 50%;
            background: #28a745;
            margin-right: 5px;
            animation: pulse 1s infinite;
        }
        @keyframes pulse {
            0%, 100% { opacity: 1; }
            50% { opacity: 0.5; }
        }
        .back-btn {
            display: inline-block;
            padding: 12px 30px;
            background: linear-gradient(135deg, #11998e 0%, #38ef7d 100%);
            color: white;
            text-decoration: none;
            border-radius: 8px;
            font-weight: 600;
            transition: transform 0.2s, box-shadow 0.2s;
            margin: 5px;
        }
        .back-btn:hover {
            transform: translateY(-2px);
            box-shadow: 0 4px 12px rgba(17, 153, 142, 0.4);
        }
        .refresh-btn {
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
        }
        .clear-btn {
            background: linear-gradient(135deg, #dc3545 0%, #c82333 100%);
            border: none;
            cursor: pointer;
            font-size: 16px;
        }
        .clear-btn:hover {
            box-shadow: 0 4px 12px rgba(220, 53, 69, 0.4);
        }
        .btn-group {
            margin-top: 15px;
        }
    </style>
</head>
<body>
    <div class="container">
        <div class="icon">)" << icon << R"(</div>
        <h1>)" << title << R"(</h1>)";

    if (success && !gameID.empty()) {
        // 游戏ID / 内存池中的数据包数量 / 每3秒自动刷新 / 立即刷新
        html << R"HTML(
        <div class="info-card">
            <div class="info-row">
                <span class="info-label">&#28216;&#25103;ID</span>
                <span class="info-value">)HTML" << gameID << R"HTML(</span>
            </div>
        </div>
        <div class="packet-count" id="packetCount">)HTML" << packetCount << R"HTML(</div>
        <p class="packet-label">&#20869;&#23384;&#27744;&#20013;&#30340;&#25968;&#25454;&#21253;&#25968;&#37327;</p>
        <p class="refresh-note"><span class="status-dot"></span>&#27599;3&#31186;&#33258;&#21160;&#21047;&#26032;</p>
        <div class="btn-group">
            <button onclick="refreshNow()" class="back-btn refresh-btn">&#31435;&#21363;&#21047;&#26032;</button>)HTML";

        // 如果有用户名密码，显示清理按钮
        if (!username.empty() && !password.empty()) {
            html << R"HTML(
            <form method="POST" action="/clear" style="display:inline;">
                <input type="hidden" name="username" value=")HTML" << username << R"HTML(">
                <input type="hidden" name="password" value=")HTML" << password << R"HTML(">
                <button type="submit" class="back-btn clear-btn" onclick="return confirm('&#30830;&#23450;&#35201;&#28165;&#29702;&#25152;&#26377;&#25968;&#25454;&#21253;&#21527;&#65311;');">&#28165;&#29702;&#25968;&#25454;&#21253;</button>
            </form>)HTML";
        }
        html << R"HTML(
        </div>
        <script>
            var gameID = ')HTML" << gameID << R"HTML(';
            function refreshCount() {
                fetch('/api/query/gameid', {
                    method: 'POST',
                    headers: {'Content-Type': 'application/x-www-form-urlencoded'},
                    body: 'gameid=' + encodeURIComponent(gameID)
                })
                .then(response => response.json())
                .then(data => {
                    if (data.success) {
                        document.getElementById('packetCount').textContent = data.packetCount;
                    }
                })
                .catch(err => console.log('Refresh error:', err));
            }
            function refreshNow() {
                refreshCount();
            }
            setInterval(refreshCount, 3000);
        </script>)HTML";
    }
    else {
        html << R"(
        <p class="message">)" << message << R"(</p>)";
    }

    // 返回登录
    html << R"(
        <a href="/" class="back-btn">&#36820;&#22238;&#30331;&#24405;</a>
    </div>
</body>
</html>)";

    return html.str();
}

std::string PoolQueryServer::GetClearResultPageHTML(bool success, const std::string& gameID,
    size_t clearedCount, size_t currentCount) {

    std::string iconColor = success ? "#28a745" : "#dc3545";
    std::string icon = success ? "&#10004;" : "&#10008;";
    // 清理完成 / 清理失败
    std::string title = success ? "&#28165;&#29702;&#23436;&#25104;" : "&#28165;&#29702;&#22833;&#36133;";

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
            font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, "Microsoft YaHei", sans-serif;
            background: linear-gradient(135deg, #11998e 0%, #38ef7d 100%);
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
            max-width: 450px;
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
        .info-card {
            background: #f8f9fa;
            padding: 20px;
            border-radius: 12px;
            margin-bottom: 20px;
        }
        .info-row {
            display: flex;
            justify-content: space-between;
            align-items: center;
            padding: 10px 0;
            border-bottom: 1px solid #e9ecef;
        }
        .info-row:last-child {
            border-bottom: none;
        }
        .info-label {
            color: #666;
            font-size: 14px;
        }
        .info-value {
            font-family: monospace;
            font-size: 14px;
            color: #333;
            font-weight: 600;
            word-break: break-all;
        }
        .cleared-count {
            font-size: 48px;
            font-weight: bold;
            color: #dc3545;
            margin: 10px 0;
        }
        .current-count {
            font-size: 32px;
            font-weight: bold;
            color: #28a745;
            margin: 10px 0;
        }
        .count-label {
            color: #666;
            font-size: 14px;
            margin-bottom: 15px;
        }
        .back-btn {
            display: inline-block;
            padding: 12px 30px;
            background: linear-gradient(135deg, #11998e 0%, #38ef7d 100%);
            color: white;
            text-decoration: none;
            border-radius: 8px;
            font-weight: 600;
            transition: transform 0.2s, box-shadow 0.2s;
            margin: 5px;
        }
        .back-btn:hover {
            transform: translateY(-2px);
            box-shadow: 0 4px 12px rgba(17, 153, 142, 0.4);
        }
    </style>
</head>
<body>
    <div class="container">
        <div class="icon">)" << icon << R"(</div>
        <h1>)" << title << R"(</h1>
        <div class="info-card">
            <div class="info-row">
                <span class="info-label">&#28216;&#25103;ID</span>
                <span class="info-value">)" << gameID << R"(</span>
            </div>
        </div>
        <div class="cleared-count">)" << clearedCount << R"(</div>
        <p class="count-label">&#24050;&#28165;&#29702;&#30340;&#25968;&#25454;&#21253;&#25968;&#37327;</p>
        <div class="current-count">)" << currentCount << R"(</div>
        <p class="count-label">&#24403;&#21069;&#21097;&#20313;&#25968;&#25454;&#21253;&#25968;&#37327;</p>
        <a href="/" class="back-btn">&#36820;&#22238;&#30331;&#24405;</a>
    </div>
</body>
</html>)";

    return html.str();
}
