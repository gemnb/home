#pragma once

#include <string>
#include <vector>
#include <map>
#include <set>
#include <mutex>
#include <memory>
#include "DatabaseManager.h"

// 前向声明
class DatabaseManager;

struct AuthorizedFilterState {
    std::set<int> authorizedFilterIds;
    std::set<int> defaultEnabledFilterIds;
    std::vector<int> groupIds;
    bool hasUserConfig = false;
    std::set<int> userEnabledFilterIds;
    std::set<int> effectiveFilterIds;
};

// ==================== 用户滤镜管理器 ====================
// 管理每个实例中每个SOCKS用户启用的WPE滤镜配置
class UserFilterManager {
public:
    UserFilterManager();
    ~UserFilterManager();

    // 设置数据库管理器（用于持久化）
    void SetDatabase(DatabaseManager* db);

    // 加载指定实例的所有用户配置到内存
    void LoadInstanceConfig(const std::string& instanceId);

    // 获取用户启用的滤镜ID列表
    std::set<int> GetUserEnabledFilters(const std::string& instanceId, const std::string& username);

    // 检查用户是否有配置记录（用于区分"未配置"和"配置为空"）
    bool HasUserConfig(const std::string& instanceId, const std::string& username);

    // 更新用户滤镜配置（同时更新内存和数据库）
    bool UpdateUserFilters(const std::string& instanceId, const std::string& username,
                          const std::vector<int>& filterIds);

    bool UpdateUserFilterGroups(const std::string& instanceId, const std::string& username,
                                const std::vector<int>& groupIds);
    std::vector<int> GetUserFilterGroups(const std::string& instanceId, const std::string& username);
    AuthorizedFilterState GetAuthorizedFilters(const std::string& instanceId, const std::string& username);

    // 检查用户是否启用了指定滤镜
    bool IsFilterEnabledForUser(const std::string& instanceId, const std::string& username, int filterId);

    // 删除用户配置
    bool DeleteUserConfig(const std::string& instanceId, const std::string& username);

    // 清空指定实例的所有用户配置
    bool ClearInstanceConfigs(const std::string& instanceId);

    // 获取指定实例的所有用户列表
    std::vector<std::string> GetInstanceUsers(const std::string& instanceId);

    // 获取指定实例的用户配置数量
    int GetInstanceUserCount(const std::string& instanceId);

    // ===== 默认滤镜配置管理 =====
    // 保存实例的默认滤镜配置
    bool SaveDefaultFilters(const std::string& instanceId, const std::vector<int>& filterIds);

    // 加载实例的默认滤镜配置
    std::vector<int> LoadDefaultFilters(const std::string& instanceId);

    // 获取用户的有效滤镜列表（如果用户未配置，返回默认配置）
    std::set<int> GetEffectiveUserFilters(const std::string& instanceId, const std::string& username);

    // 将默认配置应用到所有现有用户（覆盖所有用户的配置）
    bool ApplyDefaultFiltersToAllUsers(const std::string& instanceId, const std::vector<int>& filterIds);

private:
    // 内存缓存：userFilterMap[instanceId][username] = {filterId1, filterId2, ...}
    std::map<std::string, std::map<std::string, std::set<int>>> userFilterMap;
    std::map<std::string, std::set<int>> defaultFilterMap;
    std::map<std::string, std::map<std::string, std::set<int>>> userFilterGroupMap;
    std::map<int, WPEFilterGroupRecord> filterGroupMap;
    std::mutex mapMutex;

    // 数据库管理器指针
    DatabaseManager* database;
};

// 全局单例
extern UserFilterManager* g_userFilterManager;
