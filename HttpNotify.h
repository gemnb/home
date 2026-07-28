#pragma once

#include <string>

/**
 * HTTP通知模块
 * 用于云计算服务端登录成功后向ThinkPHP推送用户信息
 */

// 通知配置
struct HttpNotifyConfig {
    std::string thinkphpUrl;      // ThinkPHP服务器地址
    std::string apiKey;           // API密钥
    std::string registerPath = "/api/user/register";
    bool enabled = false;
    int timeoutMs = 5000;
};

/**
 * 加载通知配置
 * @param configPath 配置文件路径
 * @return 配置结构
 */
HttpNotifyConfig LoadHttpNotifyConfig(const std::string& configPath = "thinkphp_notify_config.json");

/**
 * 保存通知配置
 * @param config 配置结构
 * @param configPath 配置文件路径
 * @return 是否保存成功
 */
bool SaveHttpNotifyConfig(const HttpNotifyConfig& config, const std::string& configPath = "thinkphp_notify_config.json");

/**
 * 通知登录成功
 * 在云计算门禁验证成功后调用，向ThinkPHP推送用户信息
 *
 * @param config 通知配置
 * @param account 云计算账号
 * @param pid 应用PID
 * @param publicIp 客户端公网IP
 * @param apiPort API监听端口
 * @param rc4Key RC4加密密钥（可选）
 * @param expiredAt 账号过期时间（可选，格式：YYYY-MM-DD HH:MM:SS）
 * @return 是否通知成功
 */
bool NotifyLoginSuccess(
    const HttpNotifyConfig& config,
    const std::string& account,
    const std::string& pid,
    const std::string& publicIp,
    int apiPort,
    const std::string& rc4Key = "",
    const std::string& expiredAt = ""
);

/**
 * 通知登出
 * 在用户登出或会话过期时调用
 *
 * @param config 通知配置
 * @param pid 应用PID
 * @return 是否通知成功
 */
bool NotifyLogout(const HttpNotifyConfig& config, const std::string& pid);
