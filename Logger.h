#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <atomic>

// 日志级别
enum LogLevel {
    LOG_INFO = 0,
    LOG_WARNING = 1,
    LOG_ERROR = 2
};

// 日志分类
enum LogCategory {
    LOG_CAT_ALL = 0,           // 全部日志
    LOG_CAT_HEARTBEAT_LOAD,    // 心跳加载
    LOG_CAT_HEARTBEAT_CLEANUP, // 心跳清理
    LOG_CAT_HEARTBEAT_REPLACE, // 伪心跳替换
    LOG_CAT_LOGIN_AUTH,        // 登录验证
    LOG_CAT_ANTICC,            // 防CC
    LOG_CAT_COLLECTOR,         // 采集日志
    LOG_CAT_API,               // API日志
    LOG_CAT_SOCKS_ACCOUNT      // SOCKS账号日志
};

// 日志条目结构
struct LogEntry {
    std::string timestamp;
    LogLevel level;
    LogCategory category;
    std::string message;

    LogEntry(const std::string& ts, LogLevel lvl, LogCategory cat, const std::string& msg)
        : timestamp(ts), level(lvl), category(cat), message(msg) {}
};

class Logger {
private:
    // 静态成员变量（只能在类外定义）
    static std::vector<LogEntry> logs;
    static std::mutex logMutex;
    static const int MAX_LOGS = 5000;  // 增加日志上限
    static std::atomic<bool> enableLogging;  // 是否启用日志记录

public:
    // 基础日志方法（带分类）
    static void Log(LogLevel level, LogCategory category, const std::string& message);

    // 便捷方法 - 默认分类为全部
    static void Info(const std::string& message);
    static void Warning(const std::string& message);
    static void Error(const std::string& message);

    // 便捷方法 - 指定分类
    static void InfoCat(LogCategory category, const std::string& message);
    static void WarningCat(LogCategory category, const std::string& message);
    static void ErrorCat(LogCategory category, const std::string& message);

    // 获取日志
    static std::vector<LogEntry> GetLogs();                           // 获取全部日志
    static std::vector<LogEntry> GetLogsByCategory(LogCategory cat);  // 按分类获取
    static std::vector<LogEntry> GetErrorLogs();                      // 仅错误日志

    // 控制方法
    static void Clear();                              // 清空日志
    static void SetEnabled(bool enabled);             // 启用/停止日志记录
    static bool IsEnabled();                          // 获取日志记录状态

    // 辅助方法
    static std::string GetTimestamp();
    static std::string NormalizeTextForDisplay(const std::string& text);
    static std::string GetCategoryName(LogCategory cat);  // 获取分类名称
};

#ifndef AB_LOG_MACROS
#define AB_LOG_MACROS 1

// 注意：这些宏用于避免在日志被禁用时构造字符串（参数不会被求值）。
// 极速模式本质上会关闭 Logger，因此可显著降低热路径日志的额外开销。
#define AB_LOG_INFO(message)          do { if (Logger::IsEnabled()) { Logger::Info((message)); } } while (0)
#define AB_LOG_WARNING(message)       do { if (Logger::IsEnabled()) { Logger::Warning((message)); } } while (0)
#define AB_LOG_ERROR(message)         do { if (Logger::IsEnabled()) { Logger::Error((message)); } } while (0)

#define AB_LOG_INFO_CAT(category, message)    do { if (Logger::IsEnabled()) { Logger::InfoCat((category), (message)); } } while (0)
#define AB_LOG_WARNING_CAT(category, message) do { if (Logger::IsEnabled()) { Logger::WarningCat((category), (message)); } } while (0)
#define AB_LOG_ERROR_CAT(category, message)   do { if (Logger::IsEnabled()) { Logger::ErrorCat((category), (message)); } } while (0)

#endif  // AB_LOG_MACROS
