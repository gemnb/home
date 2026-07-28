#include "UserFilterManager.h"
#include "DatabaseManager.h"
#include "Logger.h"
#include <algorithm>
#include <iterator>

// 鍏ㄥ眬鍗曚緥
UserFilterManager* g_userFilterManager = nullptr;

UserFilterManager::UserFilterManager()
    : database(nullptr) {
}

UserFilterManager::~UserFilterManager() {
}

void UserFilterManager::SetDatabase(DatabaseManager* db) {
    database = db;
}

void UserFilterManager::LoadInstanceConfig(const std::string& instanceId) {
    if (!database) {
        AB_LOG_WARNING("[用户滤镜] 数据库未初始化，无法加载配置");
        return;
    }

    auto configs = database->LoadAllUserFilterConfigs(instanceId);
    auto defaultFilters = database->LoadDefaultFilterConfig(instanceId);
    auto groups = database->LoadWPEFilterGroups(false);
    auto userGroups = database->LoadAllUserWPEFilterGroups(instanceId);

    std::lock_guard<std::mutex> lock(mapMutex);
    userFilterMap[instanceId].clear();
    for (const auto& pair : configs) {
        userFilterMap[instanceId][pair.first] = std::set<int>(pair.second.begin(), pair.second.end());
    }
    defaultFilterMap[instanceId] = std::set<int>(defaultFilters.begin(), defaultFilters.end());
    filterGroupMap.clear();
    for (const auto& group : groups) {
        filterGroupMap[group.id] = group;
    }
    userFilterGroupMap[instanceId].clear();
    for (const auto& pair : userGroups) {
        userFilterGroupMap[instanceId][pair.first] = std::set<int>(pair.second.begin(), pair.second.end());
    }

    AB_LOG_INFO("[用户滤镜] 加载实例配置: " + instanceId + ", 用户数: " + std::to_string(configs.size()));
}

std::set<int> UserFilterManager::GetUserEnabledFilters(const std::string& instanceId, const std::string& username) {
    std::lock_guard<std::mutex> lock(mapMutex);

    auto instanceIt = userFilterMap.find(instanceId);
    if (instanceIt == userFilterMap.end()) {
        return std::set<int>();  // 瀹炰緥涓嶅瓨鍦紝杩斿洖绌洪泦鍚?
    }

    auto userIt = instanceIt->second.find(username);
    if (userIt == instanceIt->second.end()) {
        return std::set<int>();  // 鐢ㄦ埛涓嶅瓨鍦紝杩斿洖绌洪泦鍚?
    }

    return userIt->second;
}

bool UserFilterManager::HasUserConfig(const std::string& instanceId, const std::string& username) {
    std::lock_guard<std::mutex> lock(mapMutex);

    auto instanceIt = userFilterMap.find(instanceId);
    if (instanceIt == userFilterMap.end()) {
        return false;  // 瀹炰緥涓嶅瓨鍦?
    }

    auto userIt = instanceIt->second.find(username);
    return userIt != instanceIt->second.end();  // 鐢ㄦ埛瀛樺湪鍒欒繑鍥瀟rue
}

bool UserFilterManager::UpdateUserFilters(const std::string& instanceId, const std::string& username,
                                          const std::vector<int>& filterIds) {
    if (!database) {
        AB_LOG_ERROR("[用户滤镜] 数据库未初始化，无法保存配置");
        return false;
    }

    AuthorizedFilterState auth = GetAuthorizedFilters(instanceId, username);
    std::vector<int> sanitized;
    sanitized.reserve(filterIds.size());
    for (int filterId : filterIds) {
        if (auth.authorizedFilterIds.empty() && auth.groupIds.empty()) {
            sanitized.push_back(filterId);
        } else if (auth.authorizedFilterIds.count(filterId) > 0) {
            sanitized.push_back(filterId);
        }
    }

    // 馃敟 鍏堜繚瀛樺埌鏁版嵁搴擄紙鏁版嵁搴撴搷浣滀細鑾峰彇 configMutex锛?
    // 娉ㄦ剰锛氳繖閲屼笉鎸佹湁 mapMutex锛岄伩鍏嶆閿?
    if (!database->SaveUserFilterConfig(instanceId, username, sanitized)) {
        AB_LOG_ERROR("[用户滤镜] 保存配置到数据库失败: " + instanceId + "/" + username);
        return false;
    }

    // 馃敟 鏁版嵁搴撴搷浣滃畬鎴愬悗锛屽啀鏇存柊鍐呭瓨缂撳瓨锛堣幏鍙?mapMutex锛?
    {
        std::lock_guard<std::mutex> lock(mapMutex);
        std::set<int> filterSet(sanitized.begin(), sanitized.end());
        userFilterMap[instanceId][username] = filterSet;
    }

    AB_LOG_INFO("[用户滤镜] 更新用户配置: " + instanceId + "/" + username +
                 ", 滤镜数: " + std::to_string(sanitized.size()));
    return true;
}

bool UserFilterManager::UpdateUserFilterGroups(
    const std::string& instanceId,
    const std::string& username,
    const std::vector<int>& groupIds) {
    if (!database) {
        AB_LOG_ERROR("[用户滤镜] 数据库未初始化，无法保存用户滤镜组");
        return false;
    }

    if (!database->SaveUserWPEFilterGroups(instanceId, username, groupIds)) {
        AB_LOG_ERROR("[用户滤镜] 保存用户滤镜组失败: " + instanceId + "/" + username);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mapMutex);
        userFilterGroupMap[instanceId][username] = std::set<int>(groupIds.begin(), groupIds.end());
    }

    AB_LOG_INFO("[用户滤镜] 更新用户滤镜组: " + instanceId + "/" + username +
                 ", 组数: " + std::to_string(groupIds.size()));
    return true;
}

std::vector<int> UserFilterManager::GetUserFilterGroups(
    const std::string& instanceId,
    const std::string& username) {
    std::lock_guard<std::mutex> lock(mapMutex);

    auto instIt = userFilterGroupMap.find(instanceId);
    if (instIt == userFilterGroupMap.end()) {
        return {};
    }

    auto userIt = instIt->second.find(username);
    if (userIt == instIt->second.end()) {
        return {};
    }

    return std::vector<int>(userIt->second.begin(), userIt->second.end());
}

AuthorizedFilterState UserFilterManager::GetAuthorizedFilters(
    const std::string& instanceId,
    const std::string& username) {
    std::lock_guard<std::mutex> lock(mapMutex);
    AuthorizedFilterState state;

    auto userGroupInstIt = userFilterGroupMap.find(instanceId);
    if (userGroupInstIt != userFilterGroupMap.end()) {
        auto userGroupIt = userGroupInstIt->second.find(username);
        if (userGroupIt != userGroupInstIt->second.end()) {
            state.groupIds.assign(userGroupIt->second.begin(), userGroupIt->second.end());
        }
    }

    for (int groupId : state.groupIds) {
        auto groupIt = filterGroupMap.find(groupId);
        if (groupIt == filterGroupMap.end()) continue;

        const auto& group = groupIt->second;
        if (!group.enabled) continue;

        for (const auto& item : group.items) {
            if (item.filterId <= 0) continue;

            state.authorizedFilterIds.insert(item.filterId);
            if (item.defaultEnabled) {
                state.defaultEnabledFilterIds.insert(item.filterId);
            }
        }
    }

    auto instanceIt = userFilterMap.find(instanceId);
    if (instanceIt != userFilterMap.end()) {
        auto userIt = instanceIt->second.find(username);
        if (userIt != instanceIt->second.end()) {
            state.hasUserConfig = true;
            state.userEnabledFilterIds = userIt->second;
        }
    }

    const std::set<int>& base = state.hasUserConfig
        ? state.userEnabledFilterIds
        : state.defaultEnabledFilterIds;

    std::set_intersection(
        base.begin(), base.end(),
        state.authorizedFilterIds.begin(), state.authorizedFilterIds.end(),
        std::inserter(state.effectiveFilterIds, state.effectiveFilterIds.begin()));

    return state;
}

bool UserFilterManager::IsFilterEnabledForUser(const std::string& instanceId, const std::string& username, int filterId) {
    std::lock_guard<std::mutex> lock(mapMutex);

    auto instanceIt = userFilterMap.find(instanceId);
    if (instanceIt == userFilterMap.end()) {
        return false;
    }

    auto userIt = instanceIt->second.find(username);
    if (userIt == instanceIt->second.end()) {
        return false;
    }

    return userIt->second.find(filterId) != userIt->second.end();
}

bool UserFilterManager::DeleteUserConfig(const std::string& instanceId, const std::string& username) {
    if (!database) {
        AB_LOG_ERROR("[用户滤镜] 数据库未初始化，无法删除配置");
        return false;
    }

    // 馃敟 鍏堜粠鏁版嵁搴撳垹闄わ紙鏁版嵁搴撴搷浣滀細鑾峰彇 configMutex锛?
    // 娉ㄦ剰锛氳繖閲屼笉鎸佹湁 mapMutex锛岄伩鍏嶆閿?
    if (!database->DeleteUserFilterConfig(instanceId, username)) {
        AB_LOG_ERROR("[用户滤镜] 从数据库删除配置失败: " + instanceId + "/" + username);
        return false;
    }

    // 馃敟 鏁版嵁搴撴搷浣滃畬鎴愬悗锛屽啀浠庡唴瀛樺垹闄わ紙鑾峰彇 mapMutex锛?
    {
        std::lock_guard<std::mutex> lock(mapMutex);
        auto instanceIt = userFilterMap.find(instanceId);
        if (instanceIt != userFilterMap.end()) {
            instanceIt->second.erase(username);
        }
    }

    AB_LOG_INFO("[用户滤镜] 删除用户配置: " + instanceId + "/" + username);
    return true;
}

bool UserFilterManager::ClearInstanceConfigs(const std::string& instanceId) {
    if (!database) {
        AB_LOG_ERROR("[用户滤镜] 数据库未初始化，无法清空配置");
        return false;
    }

    // 馃敟 鍏堜粠鏁版嵁搴撴竻绌猴紙鏁版嵁搴撴搷浣滀細鑾峰彇 configMutex锛?
    // 娉ㄦ剰锛氳繖閲屼笉鎸佹湁 mapMutex锛岄伩鍏嶆閿?
    if (!database->ClearInstanceUserFilterConfigs(instanceId)) {
        AB_LOG_ERROR("[用户滤镜] 从数据库清空实例配置失败: " + instanceId);
        return false;
    }

    // 馃敟 鏁版嵁搴撴搷浣滃畬鎴愬悗锛屽啀浠庡唴瀛樻竻绌猴紙鑾峰彇 mapMutex锛?
    {
        std::lock_guard<std::mutex> lock(mapMutex);
        userFilterMap.erase(instanceId);
    }

    AB_LOG_INFO("[用户滤镜] 清空实例配置: " + instanceId);
    return true;
}

std::vector<std::string> UserFilterManager::GetInstanceUsers(const std::string& instanceId) {
    std::lock_guard<std::mutex> lock(mapMutex);
    std::vector<std::string> result;

    auto instanceIt = userFilterMap.find(instanceId);
    if (instanceIt != userFilterMap.end()) {
        for (const auto& pair : instanceIt->second) {
            result.push_back(pair.first);
        }
    }

    return result;
}

int UserFilterManager::GetInstanceUserCount(const std::string& instanceId) {
    std::lock_guard<std::mutex> lock(mapMutex);

    auto instanceIt = userFilterMap.find(instanceId);
    if (instanceIt == userFilterMap.end()) {
        return 0;
    }

    return static_cast<int>(instanceIt->second.size());
}

// ========== 榛樿婊ら暅閰嶇疆绠＄悊 ==========
bool UserFilterManager::SaveDefaultFilters(const std::string& instanceId, const std::vector<int>& filterIds) {
    if (!database) {
        AB_LOG_ERROR("[用户滤镜] 数据库未初始化，无法保存默认配置");
        return false;
    }

    if (!database->SaveDefaultFilterConfig(instanceId, filterIds)) {
        AB_LOG_ERROR("[用户滤镜] 保存默认配置到数据库失败: " + instanceId);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mapMutex);
        defaultFilterMap[instanceId] = std::set<int>(filterIds.begin(), filterIds.end());
    }

    AB_LOG_INFO("[用户滤镜] 保存默认配置: " + instanceId + ", 滤镜数: " + std::to_string(filterIds.size()));
    return true;
}

std::vector<int> UserFilterManager::LoadDefaultFilters(const std::string& instanceId) {
    {
        std::lock_guard<std::mutex> lock(mapMutex);
        auto it = defaultFilterMap.find(instanceId);
        if (it != defaultFilterMap.end()) {
            return std::vector<int>(it->second.begin(), it->second.end());
        }
    }

    if (!database) {
        AB_LOG_WARNING("[用户滤镜] 数据库未初始化，无法加载默认配置");
        return std::vector<int>();
    }

    auto filters = database->LoadDefaultFilterConfig(instanceId);
    {
        std::lock_guard<std::mutex> lock(mapMutex);
        defaultFilterMap[instanceId] = std::set<int>(filters.begin(), filters.end());
    }
    return filters;
}

std::set<int> UserFilterManager::GetEffectiveUserFilters(const std::string& instanceId, const std::string& username) {
    AuthorizedFilterState state = GetAuthorizedFilters(instanceId, username);
    if (!state.authorizedFilterIds.empty() || !state.groupIds.empty()) {
        return state.effectiveFilterIds;
    }

    std::lock_guard<std::mutex> lock(mapMutex);
    auto instanceIt = userFilterMap.find(instanceId);
    if (instanceIt != userFilterMap.end()) {
        auto userIt = instanceIt->second.find(username);
        if (userIt != instanceIt->second.end()) {
            return userIt->second;
        }
    }

    auto defaultIt = defaultFilterMap.find(instanceId);
    if (defaultIt != defaultFilterMap.end()) {
        return defaultIt->second;
    }

    return std::set<int>();
}

bool UserFilterManager::ApplyDefaultFiltersToAllUsers(const std::string& instanceId, const std::vector<int>& filterIds) {
    if (!database) {
        AB_LOG_ERROR("[用户滤镜] 数据库未初始化，无法应用配置到所有用户");
        return false;
    }

    // 鑾峰彇璇ュ疄渚嬬殑鎵€鏈夌敤鎴峰垪琛?
    auto users = GetInstanceUsers(instanceId);

    if (users.empty()) {
        AB_LOG_INFO("[用户滤镜] 实例没有用户配置，仅保存默认配置: " + instanceId);
        return SaveDefaultFilters(instanceId, filterIds);
    }

    // 鏇存柊鎵€鏈夌敤鎴风殑閰嶇疆
    int successCount = 0;
    for (const auto& username : users) {
        if (UpdateUserFilters(instanceId, username, filterIds)) {
            successCount++;
        } else {
            AB_LOG_WARNING("[用户滤镜] 更新用户配置失败: " + username);
        }
    }

    // 鍚屾椂淇濆瓨榛樿閰嶇疆
    if (!SaveDefaultFilters(instanceId, filterIds)) {
        AB_LOG_WARNING("[用户滤镜] 保存默认配置失败");
    }

    AB_LOG_INFO("[用户滤镜] 已将配置应用到所有用户: " + instanceId +
                 ", 成功: " + std::to_string(successCount) + "/" + std::to_string(users.size()));

    return successCount == users.size();
}


