/*
 * 使用示例 - 如何在您的C++程序中集成云端授权
 */

#include "CloudAuth.h"
#include <iostream>

// 云端服务器地址 (修改为您的服务器地址)
#define AUTH_SERVER_URL "http://your-server.com"

// 全局授权对象
CloudAuth* g_auth = nullptr;

// 强制下线回调
void OnForceOffline(const std::string& reason) {
    std::cout << "[授权] 您已被强制下线: " << reason << std::endl;
    // 这里应该关闭程序或返回登录界面
    // 例如: exit(0);
}

// VIP过期回调
void OnVipExpired() {
    std::cout << "[授权] 您的会员已到期，请续费后使用" << std::endl;
    // 这里应该关闭程序或返回登录界面
    // 例如: exit(0);
}

// 初始化授权
bool InitAuth() {
    g_auth = new CloudAuth(AUTH_SERVER_URL);
    g_auth->SetOnForceOffline(OnForceOffline);
    g_auth->SetOnVipExpired(OnVipExpired);
    return true;
}

// 登录示例
bool DoLogin(const std::string& username, const std::string& password) {
    if (!g_auth) {
        std::cout << "[授权] 授权系统未初始化" << std::endl;
        return false;
    }

    // 获取客户端信息 (可选)
    std::string clientInfo = "Windows 10 x64";

    // 调用登录接口
    CloudAuth::AuthResult result = g_auth->Login(username, password, clientInfo);

    if (result.success) {
        std::cout << "[授权] 登录成功!" << std::endl;
        std::cout << "  用户名: " << result.username << std::endl;
        std::cout << "  昵称: " << result.nickname << std::endl;
        std::cout << "  VIP到期: " << result.vipExpireTime << std::endl;
        std::cout << "  剩余天数: " << result.vipDaysLeft << std::endl;
        std::cout << "  当前连接: " << result.currentConnections << "/" << result.maxConnections << std::endl;

        // 启动心跳线程 (每30秒发送一次心跳)
        g_auth->StartHeartbeatThread(30);

        return true;
    }
    else {
        std::cout << "[授权] 登录失败: " << result.msg << " (错误码: " << result.code << ")" << std::endl;
        return false;
    }
}

// 登出示例
void DoLogout() {
    if (g_auth) {
        g_auth->StopHeartbeatThread();
        g_auth->Logout();
        std::cout << "[授权] 已登出" << std::endl;
    }
}

// 清理
void CleanupAuth() {
    if (g_auth) {
        DoLogout();
        delete g_auth;
        g_auth = nullptr;
    }
}

/*
 * 使用流程:
 *
 * 1. 程序启动时调用 InitAuth() 初始化授权系统
 * 2. 用户输入用户名密码后调用 DoLogin() 进行验证
 * 3. 登录成功后自动启动心跳线程保持在线
 * 4. 程序退出时调用 CleanupAuth() 清理资源
 *
 * 错误码说明:
 * 0     - 成功
 * 1001  - 用户名或密码为空
 * 1002  - 用户名或密码错误
 * 1003  - 账号已被禁用
 * 1004  - 会员已到期
 * 1005  - IP未绑定
 * 1006  - 连接数已达上限
 * 2001  - 会话token为空
 * 2002  - 会话不存在或已过期
 * 2003  - 被强制下线
 * 2004  - 账号已被禁用
 * 2005  - 会员已到期
 */

// ============ 集成到现有SOCKS5代理中的示例 ============

/*
在 PacketCollector.cpp 的 HandleSocks5Auth 函数中集成:

bool PacketCollector::HandleSocks5Auth(SOCKET clientSocket, std::string& username,
    const std::string& clientIP) {

    // ... 原有的接收用户名密码逻辑 ...

    // 调用云端验证
    if (!g_auth) {
        InitAuth();
    }

    CloudAuth::AuthResult result = g_auth->Login(username, password);

    if (!result.success) {
        Log("[SOCKS5-Auth] 云端验证失败: " + result.msg);
        char response[2] = { 1, 1 };
        send(clientSocket, response, 2, 0);
        return false;
    }

    // 保存会话token供心跳使用
    m_sessionToken = result.sessionToken;

    // 启动心跳
    g_auth->StartHeartbeatThread(30);

    Log("[SOCKS5-Auth] 云端验证成功: " + username);
    char response[2] = { 1, 0 };
    send(clientSocket, response, 2, 0);
    return true;
}
*/
