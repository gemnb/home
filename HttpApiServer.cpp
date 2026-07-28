#include "HttpApiServer.h"
#include "Logger.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <algorithm>





// ========== ✅ GBK到UTF-8转换函数 ==========
std::string HttpApiServer::GBKToUTF8(const std::string& gbkStr) {
    if (gbkStr.empty()) return gbkStr;

    // 第一步：GBK -> Unicode (UTF-16)
    int len = MultiByteToWideChar(CP_ACP, 0, gbkStr.c_str(), -1, NULL, 0);
    if (len == 0) {
        AB_LOG_ERROR_CAT(LOG_CAT_API, "[HTTP API] GBK转Unicode失败");
        return gbkStr;
    }

    std::vector<wchar_t> wstr(len);
    MultiByteToWideChar(CP_ACP, 0, gbkStr.c_str(), -1, wstr.data(), len);

    // 第二步：Unicode (UTF-16) -> UTF-8
    len = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), -1, NULL, 0, NULL, NULL);
    if (len == 0) {
        AB_LOG_ERROR_CAT(LOG_CAT_API, "[HTTP API] Unicode转UTF-8失败");
        return gbkStr;
    }

    std::vector<char> utf8Str(len);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), -1, utf8Str.data(), len, NULL, NULL);

    return std::string(utf8Str.data());
}


// ========== ✅ 内置HTML模板 ==========
std::string HttpApiServer::GetEmbeddedHTMLTemplate() {
    return R"html(<!-- header -->
<html><head><title>CCProxy Account Manager</title>
<meta http-equiv="Content-Type" content="text/html; charset=utf-8">
<style>
body,td {font-family: "arial"; font-size: 9pt;}
.button {  font-family: "Arial", "Helvetica", "sans-serif"; font-size: 9px; font-style: _italic; height: 18px; width: 50px}
.editbox {font-family: "Arial", "Helvetica", "sans-serif"; font-size: 9px; height: 16px;}
</style></head><body>
<h3 align="center">CCProxy 帐号管理</h3>
<table width="100%" border="1" cellspacing="0" cellpadding="0" id="t$userid">
  <tr align="center"> 
    <td nowrap width="0">&nbsp; </td>
    <td nowrap width="0">&nbsp; </td>
    <td nowrap width="0">用户名</td>
    <td nowrap width="0" align="center">允许</td>
    <td nowrap width="0" align="center">密码</td>
    <td nowrap width="0" align="center">IP 地址</td>
    <td nowrap width="0">MAC 地址</td>
    <td nowrap width="0" colspan="2">连接数</td>
    <td nowrap width="0" colspan="2">带宽</td>
    <td nowrap width="0">自动过期</td>
  </tr>
  <form name="form" method="post" action="account">
    <tr align="center"> 
      <td nowrap width="0"> 
        <input type="submit" name="add" value="增加" class="button">
      </td>
      <td nowrap width="0">&nbsp;</td>
      <td nowrap width="0"> 
        <input type="text" name="username" size="12" value="" class="editbox">
      </td>
      <td nowrap width="0" align="center"> 
        <input type="checkbox" name="enable" value="1" checked>
      </td>
      <td nowrap width="0" align="center"> 
        <input type="checkbox" name="usepassword" value="1" $checkusepassword>
        <input type="text" name="password" size="8" value="" class="editbox">
      </td>
      <td nowrap width="0" align="center"> 
        <input type="checkbox" name="useipaddress" value="1" $checkuseipaddress>
        <input type="text" name="ipaddress" size="12" value="" class="editbox">
      </td>
      <td nowrap width="0"> 
        <input type="checkbox" name="usemacaddress" value="1" $checkusemacaddress>
        <input type="text" name="macaddress" size="12" value="" class="editbox">
      </td>
      <td nowrap width="0"> 
        <input type="text" name="connection" size="4" value="-1" class="editbox">
      </td>
      <td nowrap width="0">&nbsp;</td>
      <td nowrap width="0"> 
        <input type="text" name="bandwidth" size="4" value="-1" class="editbox">
      </td>
      <td nowrap width="0">&nbsp;</td>
      <td nowrap width="0"> 
        <input type="checkbox" name="autodisable" value="1" $checkautodisable>
        <input type="text" name="disabledate" size="10" value="$disabledate" class="editbox">
        <input type="text" name="disabletime" size="8" value="$disabletime" class="editbox">
      </td>
    </tr>
  </form>
  <!-- body -->
  <form name="form" method="post" action="account">
    <input type=hidden name="userid" value="$username">
    <tr align="center"> 
      <td nowrap width="0"> 
        <input type="submit" name="edit" value="编辑" class="button">
      </td>
      <td nowrap width="0"> 
        <input type="submit" name="delete" value="删除" class="button">
      </td>
      <td nowrap width="0"> 
        <input type="text" name="username" size="12" value="$username" class="editbox">
      </td>
      <td nowrap width="0" align="center"> 
        <input type="checkbox" name="enable" value="1" $checkenable>
      </td>
      <td nowrap width="0" align="center"> 
        <input type="checkbox" name="usepassword" value="1" $checkusepassword>
        <input type="text" name="password" size="8" value="$password" class="editbox">
      </td>
      <td nowrap width="0" align="center"> 
        <input type="checkbox" name="useipaddress" value="1" $checkuseipaddress>
        <input type="text" name="ipaddress" size="12" value="$ipaddress" class="editbox">
      </td>
      <td nowrap width="0"> 
        <input type="checkbox" name="usemacaddress" value="1" $checkusemacaddress>
        <input type="text" name="macaddress" size="12" value="$macaddress" class="editbox">
      </td>
      <td nowrap width="0"> 
        <input type="text" name="connection" size="4" value="$connection" class="editbox">
        </td>
      <td nowrap width="0">$activeconn</td>
      <td nowrap width="0"> 
        <input type="text" name="bandwidth" size="4" value="$bandwidth" class="editbox">
      </td>
      <td nowrap width="0">$curbandwidth </td>
      <td nowrap width="0"> 
        <input type="checkbox" name="autodisable" value="1" $checkautodisable>
        <input type="text" name="disabledate" size="10" value="$disabledate" class="editbox">
        <input type="text" name="disabletime" size="8" value="$disabletime" class="editbox">
      </td>
    </tr>
  </form>
  <!-- tail -->
</table>
<form name="form" method="post" action="account">
  <table width="100%" border="0" cellspacing="0" cellpadding="0">
    <tr> 
      <td nowrap> 管理员密码
<input type="text" name="adminpassword" size="8" value="$adminpassword" class="editbox">
        <input type="submit" name="changeadminpassword" value="修改" class="button">
        <input type="submit" name="refresh" value="刷新" class="button">
        <input type="submit" name="logout" value="退出" class="button">
      </td>
    </tr>
  </table>
</form>
<p>总连接数/总帐号数- $totalconn/$totaluser<br>
  在线总连接数/在线总帐号数- $totalactiveconn/$totalactiveuser<br>
  总带宽- $totalbandwidth<br>
  服务器时间: $disabledate $disabletime</p>
</body>
</html>)html";
}






HttpApiServer::HttpApiServer(int port, const std::string& user, const std::string& pass)
    : port(port), username(user), password(pass), adminPassword("admin"),
    listenSocket(INVALID_SOCKET), isRunning(false), socks5Server(nullptr) {}

HttpApiServer::~HttpApiServer() {
    Stop();
}

void HttpApiServer::SetAuth(const std::string& user, const std::string& pass) {
    username = user;
    password = pass;
    AB_LOG_INFO_CAT(LOG_CAT_API, "[HTTP API] 认证信息已更新");
}

// ========== Base64解码实现 ==========
std::string HttpApiServer::Base64Decode(const std::string& encoded) {
    static const std::string base64_chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789+/";

    std::string decoded;
    std::vector<int> T(256, -1);

    for (int i = 0; i < 64; i++) {
        T[base64_chars[i]] = i;
    }

    int val = 0, valb = -8;
    for (unsigned char c : encoded) {
        if (T[c] == -1) break;
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0) {
            decoded.push_back(char((val >> valb) & 0xFF));
            valb -= 8;
        }
    }

    return decoded;
}

// ========== 真正的认证检查 ==========
bool HttpApiServer::CheckAuth(const std::string& authHeader) {
    // 示例: Authorization: Basic YWRtaW46YWRtaW4=

    if (authHeader.empty()) {
        AB_LOG_WARNING("[HTTP API] 未提供认证信息");
        return false;
    }

    // 查找 "Basic " 前缀
    size_t basicPos = authHeader.find("Basic ");
    if (basicPos == std::string::npos) {
        AB_LOG_WARNING("[HTTP API] 认证头格式错误（缺少Basic前缀）");
        return false;
    }

    // 提取 Base64 编码的凭证
    std::string base64Credentials = authHeader.substr(basicPos + 6);

    // 移除可能的空格和换行符
    base64Credentials.erase(
        std::remove_if(base64Credentials.begin(), base64Credentials.end(),
            [](char c) { return c == ' ' || c == '\r' || c == '\n'; }),
        base64Credentials.end()
    );

    if (base64Credentials.empty()) {
        AB_LOG_WARNING("[HTTP API] 认证凭证为空");
        return false;
    }

    // Base64 解码
    std::string credentials;
    try {
        credentials = Base64Decode(base64Credentials);
    }
    catch (...) {
        AB_LOG_ERROR_CAT(LOG_CAT_API, "[HTTP API] Base64解码失败");
        return false;
    }

    if (credentials.empty()) {
        AB_LOG_WARNING("[HTTP API] 解码后的凭证为空");
        return false;
    }

    // 分离用户名和密码 (格式: username:password)
    size_t colonPos = credentials.find(':');
    if (colonPos == std::string::npos) {
        AB_LOG_WARNING("[HTTP API] 凭证格式错误（缺少冒号分隔符）");
        return false;
    }

    std::string providedUsername = credentials.substr(0, colonPos);
    std::string providedPassword = credentials.substr(colonPos + 1);

    // 验证用户名和密码
    if (providedUsername == username && providedPassword == password) {
        AB_LOG_INFO_CAT(LOG_CAT_API, "[HTTP API] 认证成功: " + providedUsername);
        return true;
    }
    else {
        AB_LOG_WARNING("[HTTP API] 认证失败: 用户名或密码错误 (提供的用户名: " +
            providedUsername + ")");
        return false;
    }
}

std::string HttpApiServer::LoadHTMLTemplate() {
    // ========== ✅ 直接使用内置模板（源文件已是UTF-8编码）==========
    AB_LOG_INFO_CAT(LOG_CAT_API, "[HTTP API] 使用内置HTML模板");
    return GetEmbeddedHTMLTemplate();
}




std::string HttpApiServer::GetCurrentDate() {
    time_t now = time(nullptr);
    struct tm timeinfo;
    localtime_s(&timeinfo, &now);
    char buffer[32];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d", &timeinfo);
    return std::string(buffer);
}

std::string HttpApiServer::GetCurrentTime() {
    time_t now = time(nullptr);
    struct tm timeinfo;
    localtime_s(&timeinfo, &now);
    char buffer[32];
    strftime(buffer, sizeof(buffer), "%H:%M:%S", &timeinfo);
    return std::string(buffer);
}

std::string HttpApiServer::ReplaceVariables(const std::string& html,
    const std::map<std::string, std::string>& vars) {
    std::string result = html;

    for (const auto& pair : vars) {
        std::string placeholder = "$" + pair.first;
        size_t pos = 0;
        while ((pos = result.find(placeholder, pos)) != std::string::npos) {
            result.replace(pos, placeholder.length(), pair.second);
            pos += pair.second.length();
        }
    }

    return result;
}

std::string HttpApiServer::RenderAccountListHTML() {
    if (htmlTemplate.empty()) {
        htmlTemplate = LoadHTMLTemplate();
        if (htmlTemplate.empty()) {
            return "<html><body>Error: Template not found</body></html>";
        }
    }

    size_t headerEnd = htmlTemplate.find("<!-- body -->");
    size_t bodyStart = headerEnd;
    size_t bodyEnd = htmlTemplate.find("<!-- tail -->");
    size_t tailStart = bodyEnd;

    if (headerEnd == std::string::npos || bodyEnd == std::string::npos) {
        AB_LOG_ERROR_CAT(LOG_CAT_API, "[HTTP API] HTML模板格式错误");
        return "<html><body>Error: Invalid template</body></html>";
    }

    std::string header = htmlTemplate.substr(0, headerEnd);
    std::string bodyTemplate = htmlTemplate.substr(bodyStart, bodyEnd - bodyStart);
    std::string tail = htmlTemplate.substr(tailStart);

    std::map<std::string, std::string> headerVars;
    headerVars["checkusepassword"] = "checked";
    headerVars["checkuseipaddress"] = "";
    headerVars["checkusemacaddress"] = "";
    headerVars["checkautodisable"] = "checked";
    headerVars["disabledate"] = GetCurrentDate();
    headerVars["disabletime"] = GetCurrentTime();

    header = ReplaceVariables(header, headerVars);

    std::string accountsHTML;
    if (socks5Server) {
        auto accounts = socks5Server->GetAllAccounts();

        for (const auto& account : accounts) {
            std::map<std::string, std::string> accountVars;
            accountVars["username"] = account.username;
            accountVars["password"] = account.password;
            accountVars["checkenable"] = account.isEnabled ? "checked" : "";
            accountVars["checkusepassword"] = "checked";
            accountVars["checkuseipaddress"] = "";
            accountVars["checkusemacaddress"] = "";
            accountVars["ipaddress"] = "";
            accountVars["macaddress"] = "";
            accountVars["connection"] = std::to_string(account.maxConnections);
            accountVars["activeconn"] = std::to_string(account.currentConnections);
            accountVars["bandwidth"] = "-1";
            accountVars["curbandwidth"] = "0";
            accountVars["checkautodisable"] = account.expireTime.empty() ? "" : "checked";

            std::string expireDate, expireTime;
            if (!account.expireTime.empty()) {
                size_t spacePos = account.expireTime.find(' ');
                if (spacePos != std::string::npos) {
                    expireDate = account.expireTime.substr(0, spacePos);
                    expireTime = account.expireTime.substr(spacePos + 1);
                }
                else {
                    expireDate = account.expireTime;
                    expireTime = "23:59:59";
                }
            }
            else {
                expireDate = GetCurrentDate();
                expireTime = GetCurrentTime();
            }

            accountVars["disabledate"] = expireDate;
            accountVars["disabletime"] = expireTime;

            accountsHTML += ReplaceVariables(bodyTemplate, accountVars);
        }
    }

    std::map<std::string, std::string> tailVars;
    tailVars["adminpassword"] = adminPassword;

    int totalUser = 0;
    int totalActiveUser = 0;
    int totalConn = 0;
    int totalActiveConn = 0;

    if (socks5Server) {
        auto accounts = socks5Server->GetAllAccounts();
        totalUser = (int)accounts.size();

        for (const auto& account : accounts) {
            if (account.isEnabled) {
                totalActiveUser++;
            }
            totalConn += account.maxConnections;
            totalActiveConn += account.currentConnections;
        }
    }

    tailVars["totaluser"] = std::to_string(totalUser);
    tailVars["totalactiveuser"] = std::to_string(totalActiveUser);
    tailVars["totalconn"] = std::to_string(totalConn);
    tailVars["totalactiveconn"] = std::to_string(totalActiveConn);
    tailVars["totalbandwidth"] = "0";
    tailVars["disabledate"] = GetCurrentDate();
    tailVars["disabletime"] = GetCurrentTime();

    tail = ReplaceVariables(tail, tailVars);

    return header + accountsHTML + tail;
}

std::string HttpApiServer::UrlDecode(const std::string& str) {
    std::string result;
    for (size_t i = 0; i < str.length(); i++) {
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

std::map<std::string, std::string> HttpApiServer::ParsePostData(const std::string& postData) {
    std::map<std::string, std::string> params;

    if (postData.empty()) {
        AB_LOG_WARNING("[HTTP API] POST数据为空");
        return params;
    }

    std::istringstream stream(postData);
    std::string pair;

    while (std::getline(stream, pair, '&')) {
        size_t pos = pair.find('=');
        if (pos != std::string::npos) {
            std::string key = UrlDecode(pair.substr(0, pos));
            std::string value = UrlDecode(pair.substr(pos + 1));
            params[key] = value;
        }
    }

    return params;
}

bool HttpApiServer::HandlePostRequest(const std::string& postData, std::string& errorMsg) {
    if (!socks5Server) {
        AB_LOG_ERROR_CAT(LOG_CAT_API, "[HTTP API] socks5Server 为空");
        errorMsg = "服务器内部错误";
        return false;
    }

    AB_LOG_INFO_CAT(LOG_CAT_API, "[HTTP API] 开始解析POST数据...");

    auto params = ParsePostData(postData);

    AB_LOG_INFO_CAT(LOG_CAT_API, "[HTTP API] 解析出 " + std::to_string(params.size()) + " 个参数:");
    for (const auto& pair : params) {
        if (pair.first == "password") {
            AB_LOG_INFO_CAT(LOG_CAT_API, "[HTTP API]   " + pair.first + " = ****");
        }
        else {
            AB_LOG_INFO_CAT(LOG_CAT_API, "[HTTP API]   " + pair.first + " = " + pair.second);
        }
    }

    if (params.find("add") != params.end()) {
        AB_LOG_INFO_CAT(LOG_CAT_API, "[HTTP API] 检测到添加账号操作");

        std::string username = params["username"];
        std::string password = params["password"];
        std::string expireDate = params["disabledate"];
        std::string expireTime = params["disabletime"];
        std::string expireDateTime = expireDate + " " + expireTime;

        if (username.empty()) {
            errorMsg = "用户名不能为空";
            return false;
        }

        // 检查账号是否已存在
        if (socks5Server->AccountExists(username)) {
            errorMsg = "账号已存在: " + username;
            AB_LOG_WARNING("[HTTP API] 添加失败 - 账号已存在: " + username);
            return false;
        }

        int maxConn = 0;
        try {
            if (params.find("connection") != params.end() && !params["connection"].empty()) {
                std::string connStr = params["connection"];
                size_t slashPos = connStr.find('/');
                if (slashPos != std::string::npos) {
                    connStr = connStr.substr(0, slashPos);
                }
                maxConn = std::stoi(connStr);
                if (maxConn < 0) maxConn = 0;
            }
        }
        catch (const std::exception& e) {
            AB_LOG_WARNING("[HTTP API] connection 参数解析失败: " + std::string(e.what()));
        }

        AB_LOG_INFO_CAT(LOG_CAT_API, "[HTTP API] 准备添加账号: username=" + username +
            ", expire=" + expireDateTime + ", maxConn=" + std::to_string(maxConn));

        bool success = socks5Server->AddAccount(username, password, expireDateTime, maxConn);
        if (!success) {
            errorMsg = "添加账号失败";
        }
        else if (onAccountChanged) {
            // 添加成功后保存到数据库
            onAccountChanged();
            AB_LOG_INFO_CAT(LOG_CAT_API, "[HTTP API] 账号变更已保存到数据库");
        }
        AB_LOG_INFO_CAT(LOG_CAT_API, "[HTTP API] 添加账号: " + username + " - " + (success ? "成功" : "失败"));
        return success;
    }

    if (params.find("edit") != params.end()) {
        AB_LOG_INFO_CAT(LOG_CAT_API, "[HTTP API] 检测到编辑账号操作");

        std::string username = params["username"];
        std::string password = params["password"];
        std::string expireDate = params["disabledate"];
        std::string expireTime = params["disabletime"];
        std::string expireDateTime = expireDate + " " + expireTime;

        if (username.empty()) {
            errorMsg = "用户名不能为空";
            return false;
        }

        int maxConn = 0;
        try {
            if (params.find("connection") != params.end() && !params["connection"].empty()) {
                std::string connStr = params["connection"];
                size_t slashPos = connStr.find('/');
                if (slashPos != std::string::npos) {
                    connStr = connStr.substr(0, slashPos);
                }
                maxConn = std::stoi(connStr);
                if (maxConn < 0) maxConn = 0;
            }
        }
        catch (const std::exception& e) {
            AB_LOG_WARNING("[HTTP API] connection 参数解析失败: " + std::string(e.what()));
        }

        bool enabled = (params.find("enable") != params.end());

        AB_LOG_INFO_CAT(LOG_CAT_API, "[HTTP API] 准备编辑账号: username=" + username +
            ", expire=" + expireDateTime +
            ", maxConn=" + std::to_string(maxConn) +
            ", enabled=" + (enabled ? "true" : "false"));

        bool success = socks5Server->UpdateAccount(username, password,
            expireDateTime, maxConn, enabled);
        if (!success) {
            errorMsg = "编辑账号失败，账号可能不存在";
        }
        else if (onAccountChanged) {
            // 编辑成功后保存到数据库
            onAccountChanged();
            AB_LOG_INFO_CAT(LOG_CAT_API, "[HTTP API] 账号变更已保存到数据库");
        }
        AB_LOG_INFO_CAT(LOG_CAT_API, "[HTTP API] 编辑账号: " + username + " - " + (success ? "成功" : "失败"));
        return success;
    }

    if (params.find("delete") != params.end()) {
        AB_LOG_INFO_CAT(LOG_CAT_API, "[HTTP API] 检测到删除账号操作");
        std::string username = params["userid"];
        AB_LOG_INFO_CAT(LOG_CAT_API, "[HTTP API] 准备删除账号: " + username);
        bool success = socks5Server->RemoveAccount(username);
        if (!success) {
            errorMsg = "删除账号失败，账号可能不存在";
        }
        else if (onAccountChanged) {
            // 删除成功后保存到数据库
            onAccountChanged();
            AB_LOG_INFO_CAT(LOG_CAT_API, "[HTTP API] 账号变更已保存到数据库");
        }
        AB_LOG_INFO_CAT(LOG_CAT_API, "[HTTP API] 删除账号: " + username + " - " + (success ? "成功" : "失败"));
        return success;
    }

    if (params.find("changeadminpassword") != params.end()) {
        AB_LOG_INFO_CAT(LOG_CAT_API, "[HTTP API] 检测到修改管理员密码操作");
        adminPassword = params["adminpassword"];
        AB_LOG_INFO_CAT(LOG_CAT_API, "[HTTP API] 管理员密码已修改");
        return true;
    }

    if (params.find("refresh") != params.end()) {
        AB_LOG_INFO_CAT(LOG_CAT_API, "[HTTP API] 检测到刷新操作");
        return true;
    }

    if (params.find("logout") != params.end()) {
        AB_LOG_INFO_CAT(LOG_CAT_API, "[HTTP API] 检测到登出操作");
        return true;
    }

    AB_LOG_WARNING("[HTTP API] 未识别的POST操作");
    errorMsg = "未识别的操作";
    return false;
}

bool HttpApiServer::Start(PacketCollector* server) {
    if (isRunning) {
        AB_LOG_WARNING("[HTTP API] 服务器已在运行");
        return false;
    }

    socks5Server = server;
    if (!socks5Server) {
        AB_LOG_ERROR_CAT(LOG_CAT_API, "[HTTP API] SOCKS5服务器对象为空");
        return false;
    }

    htmlTemplate = LoadHTMLTemplate();
    if (htmlTemplate.empty()) {
        AB_LOG_ERROR_CAT(LOG_CAT_API, "[HTTP API] HTML模板加载失败");
        return false;
    }

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        AB_LOG_ERROR_CAT(LOG_CAT_API, "[HTTP API] WSAStartup失败");
        return false;
    }

    listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket == INVALID_SOCKET) {
        AB_LOG_ERROR_CAT(LOG_CAT_API, "[HTTP API] 创建Socket失败");
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
        AB_LOG_ERROR_CAT(LOG_CAT_API, "[HTTP API] 绑定端口失败: " + std::to_string(port));
        closesocket(listenSocket);
        WSACleanup();
        return false;
    }

    if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) {
        AB_LOG_ERROR_CAT(LOG_CAT_API, "[HTTP API] 监听失败");
        closesocket(listenSocket);
        WSACleanup();
        return false;
    }

    isRunning = true;
    serverThread = std::thread(&HttpApiServer::ServerLoop, this);

    AB_LOG_INFO_CAT(LOG_CAT_API, "[HTTP API] CCProxy兼容服务启动成功，端口: " + std::to_string(port));
    return true;
}

void HttpApiServer::ServerLoop() {
    while (isRunning) {
        sockaddr_in clientAddr;
        int clientAddrSize = sizeof(clientAddr);

        SOCKET clientSocket = accept(listenSocket, (sockaddr*)&clientAddr, &clientAddrSize);

        if (clientSocket == INVALID_SOCKET) {
            if (isRunning) {
                AB_LOG_ERROR_CAT(LOG_CAT_API, "[HTTP API] 接受连接失败");
            }
            continue;
        }

        std::thread(&HttpApiServer::HandleClient, this, clientSocket).detach();
    }
}

void HttpApiServer::HandleClient(SOCKET clientSocket) {
    DWORD timeout = 10000;
    setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
    setsockopt(clientSocket, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));

    std::string request;
    char buffer[8192];
    int headerEndPos = -1;
    int contentLength = 0;
    std::string method, path;

    while (true) {
        int bytesReceived = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);

        if (bytesReceived <= 0) {
            AB_LOG_ERROR_CAT(LOG_CAT_API, "[HTTP API] 接收请求头失败");
            closesocket(clientSocket);
            return;
        }

        buffer[bytesReceived] = '\0';
        request.append(buffer, bytesReceived);

        headerEndPos = request.find("\r\n\r\n");
        if (headerEndPos != std::string::npos) {
            break;
        }

        if (request.length() > 16384) {
            AB_LOG_ERROR_CAT(LOG_CAT_API, "[HTTP API] 请求头过大");
            std::string errorResponse =
                "HTTP/1.1 400 Bad Request\r\n"
                "Content-Length: 0\r\n"
                "Connection: close\r\n\r\n";
            send(clientSocket, errorResponse.c_str(), errorResponse.length(), 0);
            closesocket(clientSocket);
            return;
        }
    }

    std::istringstream iss(request);
    std::string version;
    iss >> method >> path >> version;

    AB_LOG_INFO_CAT(LOG_CAT_API, "[HTTP API] 收到请求: " + method + " " + path);

    size_t clPos = request.find("Content-Length:");
    if (clPos == std::string::npos) {
        clPos = request.find("content-length:");
    }

    if (clPos != std::string::npos) {
        size_t lineEnd = request.find("\r\n", clPos);
        std::string clLine = request.substr(clPos, lineEnd - clPos);
        size_t colonPos = clLine.find(':');
        if (colonPos != std::string::npos) {
            std::string clValue = clLine.substr(colonPos + 1);
            clValue.erase(0, clValue.find_first_not_of(" \t"));
            contentLength = std::atoi(clValue.c_str());
            AB_LOG_INFO_CAT(LOG_CAT_API, "[HTTP API] Content-Length: " + std::to_string(contentLength));
        }
    }

    std::string authHeader;
    size_t authPos = request.find("Authorization:");
    if (authPos == std::string::npos) {
        authPos = request.find("authorization:");
    }

    if (authPos != std::string::npos) {
        size_t lineEnd = request.find("\r\n", authPos);
        authHeader = request.substr(authPos, lineEnd - authPos);
    }

    // ✅ 验证认证
    if (!CheckAuth(authHeader)) {
        AB_LOG_WARNING("[HTTP API] 认证失败，返回401");
        std::string response =
            "HTTP/1.1 401 Unauthorized\r\n"
            "WWW-Authenticate: Basic realm=\"CCProxy\"\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n";
        send(clientSocket, response.c_str(), response.length(), 0);
        closesocket(clientSocket);
        return;
    }

    std::string postData;

    if (method == "POST" && contentLength > 0) {
        int alreadyReceived = request.length() - (headerEndPos + 4);
        if (alreadyReceived > 0) {
            postData = request.substr(headerEndPos + 4);
        }

        int remaining = contentLength - alreadyReceived;

        AB_LOG_INFO_CAT(LOG_CAT_API, "[HTTP API] 开始接收POST数据，期望: " + std::to_string(contentLength) +
            " 字节，已接收: " + std::to_string(alreadyReceived) + " 字节");

        while (remaining > 0) {
            int toReceive = (remaining < sizeof(buffer)) ? remaining : sizeof(buffer);
            int bytesReceived = recv(clientSocket, buffer, toReceive, 0);

            if (bytesReceived <= 0) {
                AB_LOG_ERROR_CAT(LOG_CAT_API, "[HTTP API] 接收POST数据失败");
                closesocket(clientSocket);
                return;
            }

            postData.append(buffer, bytesReceived);
            remaining -= bytesReceived;
        }

        AB_LOG_INFO_CAT(LOG_CAT_API, "[HTTP API] POST数据接收完成: " + std::to_string(postData.length()) + " 字节");

        if (postData.length() < 2048) {
            AB_LOG_INFO_CAT(LOG_CAT_API, "[HTTP API] POST数据内容: " + postData);
        }
        else {
            AB_LOG_INFO_CAT(LOG_CAT_API, "[HTTP API] POST数据内容: " + postData.substr(0, 200) + "...");
        }
    }

    std::string response;
    std::string htmlContent;

    // ========== 根路径重定向到 /account ==========
    if (path == "/" || path.empty()) {
        AB_LOG_INFO_CAT(LOG_CAT_API, "[HTTP API] 重定向到 /account");
        response = "HTTP/1.1 302 Found\r\n"
            "Location: /account\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n";
        send(clientSocket, response.c_str(), response.length(), 0);
        closesocket(clientSocket);
        return;
    }

    if (path == "/account") {
        if (method == "POST" && !postData.empty()) {
            AB_LOG_INFO_CAT(LOG_CAT_API, "[HTTP API] 开始处理POST请求...");

            bool success = false;
            std::string errorMsg;

            try {
                success = HandlePostRequest(postData, errorMsg);

                if (success) {
                    AB_LOG_INFO_CAT(LOG_CAT_API, "[HTTP API] POST请求处理成功");
                }
                else {
                    AB_LOG_ERROR_CAT(LOG_CAT_API, "[HTTP API] POST请求处理失败: " + errorMsg);
                }
            }
            catch (const std::exception& e) {
                AB_LOG_ERROR_CAT(LOG_CAT_API, "[HTTP API] POST处理异常: " + std::string(e.what()));
                errorMsg = std::string("异常: ") + e.what();
                success = false;
            }
            catch (...) {
                AB_LOG_ERROR_CAT(LOG_CAT_API, "[HTTP API] POST处理未知异常");
                errorMsg = "未知异常";
                success = false;
            }

            // ========== ✅ 生成响应HTML（GBK编码）==========
            if (success) {
                htmlContent =
                    "<html><head><meta charset=\"utf-8\"></head><body>"
                    "<h3>操作成功</h3>"
                    "<script>setTimeout(function(){window.location.href='/account';}, 500);</script>"
                    "</body></html>";
            }
            else {
                htmlContent =
                    "<html><head><meta charset=\"utf-8\"></head><body>"
                    "<h3>操作失败</h3>"
                    "<p>" + errorMsg + "</p>"
                    "<p><a href=\"/account\">返回账号列表</a></p>"
                    "<script>setTimeout(function(){window.location.href='/account';}, 2000);</script>"
                    "</body></html>";
            }

            // ========== ✅ 源文件已是UTF-8编码，无需转换 ==========
            response = "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/html; charset=utf-8\r\n"
                "Content-Length: " + std::to_string(htmlContent.length()) + "\r\n"
                "Connection: close\r\n\r\n" + htmlContent;

            send(clientSocket, response.c_str(), response.length(), 0);

        }
        else if (method == "GET") {
            AB_LOG_INFO_CAT(LOG_CAT_API, "[HTTP API] 处理GET请求，生成账号列表...");

            try {
                // ========== ✅ 生成HTML（源文件已是UTF-8编码）==========
                htmlContent = RenderAccountListHTML();

                response = "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/html; charset=utf-8\r\n"
                    "Content-Length: " + std::to_string(htmlContent.length()) + "\r\n"
                    "Connection: close\r\n\r\n" + htmlContent;

                send(clientSocket, response.c_str(), response.length(), 0);

                AB_LOG_INFO_CAT(LOG_CAT_API, "[HTTP API] GET请求处理成功");
            }
            catch (const std::exception& e) {
                AB_LOG_ERROR_CAT(LOG_CAT_API, "[HTTP API] 生成HTML异常: " + std::string(e.what()));

                htmlContent = "<html><body><h3>Internal Server Error</h3></body></html>";

                response = "HTTP/1.1 500 Internal Server Error\r\n"
                    "Content-Type: text/html; charset=utf-8\r\n"
                    "Content-Length: " + std::to_string(htmlContent.length()) + "\r\n"
                    "Connection: close\r\n\r\n" + htmlContent;

                send(clientSocket, response.c_str(), response.length(), 0);
            }
        }
    }
    else {
        // ========== 其他路径重定向到 /account ==========
        AB_LOG_INFO_CAT(LOG_CAT_API, "[HTTP API] 未知路径: " + path + "，重定向到 /account");
        response = "HTTP/1.1 302 Found\r\n"
            "Location: /account\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n";
        send(clientSocket, response.c_str(), response.length(), 0);
    }

    closesocket(clientSocket);
}




void HttpApiServer::Stop() {
    if (!isRunning) return;

    isRunning = false;

    if (listenSocket != INVALID_SOCKET) {
        closesocket(listenSocket);
        listenSocket = INVALID_SOCKET;
    }

    if (serverThread.joinable()) {
        serverThread.join();
    }

    WSACleanup();
    AB_LOG_INFO_CAT(LOG_CAT_API, "[HTTP API] 服务已停止");
}
