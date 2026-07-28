# WPE 用户态滤镜组权限化 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 ab3 项目中实现 WPE 滤镜按卡密/账号授权的用户态滤镜组权限、组默认值、用户 Web 控制台分类开关、用户 Web 显示名称、GameID 展示和按当前 SOCKS 账号清空采集数据池。

**Architecture:** 保留现有全局 WPE 滤镜编辑和运行链路，在 `UserFilterManager` 上新增“授权滤镜组 + 用户自定义开关 + 组默认开关”的解析层。运行时只把当前 SOCKS 账号实际允许且启用的滤镜 ID 传给 `WPEFilter::FilterManager::ProcessPacket`，Web 端只返回授权滤镜，管理端负责维护滤镜组、组默认值、用户 Web 显示名称和卡密/账号绑定关系。管理端把 HTTP/API 地址配置与用户态权限配置拆成子菜单，用户态相关 UI 统一放在“Web用户态设置”中。

**Tech Stack:** C++17、WinSock HTTP 服务、SQLite、nlohmann/json、WebView2 前端 `web/app.js`、MSBuild/Visual Studio 2022。

---

## 当前代码基线

- `WPEFilter.h/.cpp` 已支持 `ProcessPacket(data, instanceId, isRequest, isCollector, phase, username, userEnabledFilters)`，传入 `userEnabledFilters` 后只执行指定滤镜 ID。
- `UserFilterManager.h/.cpp` 已有按 `instanceId + username` 保存用户滤镜开关，以及实例级默认滤镜 `defaultFilterMap`。
- `UserFilterWebServer.h/.cpp` 已有用户 Web 登录、获取滤镜、保存滤镜开关、重置执行次数。
- `PacketCollector.cpp` 的响应 WPE 路径已经按 `conn->authenticatedUser` 获取用户有效滤镜；部分采集/伪心跳请求路径仍从 `新伪心跳.cpp` 调用 `ApplyWPEFilters(..., "", ...)`，需要补齐真实 username。
- `CollectedPacketPool.h` 已有 `ClearByUsername`、`GetCountByUsername`、`PeekPacketByUsername`，可直接支撑“只清当前登录 SOCKS 账号数据池”。
- `web/app.js` 和 `ui_bridge.cpp` 已有管理端“用户滤镜配置”页面、HTTP 服务器配置、用户访问地址和实例默认滤镜配置，需要改造成“API地址 / Web用户态设置”子菜单，并把滤镜组、组默认值、用户 Web 显示名放进 Web 用户态设置。

## 权限模型

本功能新增三层数据：

1. **WPE 滤镜组**
   - 管理员创建。
   - 绑定一批可用滤镜 ID。
   - 每个绑定滤镜还有默认启用状态。

2. **账号/卡密授权**
   - 卡密绑定一个或多个滤镜组。
   - 账号由卡密激活或注册后，继承该卡密的滤镜组授权。
   - 已激活用户不复制滤镜 ID 明细，只保存组授权关系；因此管理员后续给组追加滤镜后，老用户自动获得新滤镜权限。

3. **用户自定义开关**
   - 用户只可保存自己有权限的滤镜 ID。
   - 如果用户从未保存过开关配置，则使用授权滤镜组的默认启用配置。
   - 如果用户保存过开关配置，则用用户配置和授权滤镜集合取交集，防止旧配置或接口绕过启用无权限滤镜。

运行时有效滤镜计算：

```text
authorizedFilterIds = 用户绑定的所有滤镜组中允许的滤镜 ID 并集
groupDefaultEnabledIds = 用户绑定的所有滤镜组中默认启用的滤镜 ID 并集

if 用户存在自定义开关配置:
    effectiveIds = userEnabledIds ∩ authorizedFilterIds
else:
    effectiveIds = groupDefaultEnabledIds ∩ authorizedFilterIds
```

如果一个滤镜同时属于多个组：
- 任意组授权即可使用。
- 任意组把该滤镜默认启用，则默认有效。

## 管理端 UI 放置约定

- 在实例配置的用户滤镜区域中，把现有 HTTP 服务端口、启动/停止服务、用户访问 URL 归入 **API地址** 子菜单。
- 在 **API地址** 上方或同级子菜单旁新增 **Web用户态设置**。
- **Web用户态设置** 承载：
  - 是否启用用户滤镜模式。
  - 是否在用户 Web 控制台显示执行次数。
  - WPE 滤镜组管理。
  - 滤镜组默认启用配置。
  - 卡密/账号绑定滤镜组。
  - 每个 WPE 滤镜在用户 Web 页面显示的名称。
- WPE 滤镜原始 `name` 继续用于管理端、日志和内部识别；新增 `webDisplayName` 只用于用户 Web 控制台展示。`webDisplayName` 为空时，用户 Web 回退显示 `name`。

---

### Task 1: 新增数据库表和持久化接口

**Files:**
- Modify: `DatabaseManager.h`
- Modify: `DatabaseManager.cpp`
- Test: `tests/WPEFilterGroupDatabaseCoverageTest.ps1`

- [ ] **Step 1: 新建失败测试**

创建 `tests/WPEFilterGroupDatabaseCoverageTest.ps1`，检查数据库接口声明、表名、SQL 字段、启动初始化调用都存在。

```powershell
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

$h = Get-Content -Raw -Encoding UTF8 (Join-Path $root 'DatabaseManager.h')
$cpp = Get-Content -Raw -Encoding UTF8 (Join-Path $root 'DatabaseManager.cpp')

function Assert-Contains($text, $pattern, $message) {
    if ($text -notmatch [regex]::Escape($pattern)) {
        throw $message
    }
}

Assert-Contains $h 'struct WPEFilterGroupRecord' 'DatabaseManager.h must define WPEFilterGroupRecord.'
Assert-Contains $h 'struct WPEFilterGroupItemRecord' 'DatabaseManager.h must define WPEFilterGroupItemRecord.'
Assert-Contains $h 'CreateWPEFilterGroupTables' 'DatabaseManager.h must declare CreateWPEFilterGroupTables.'
Assert-Contains $h 'SaveWPEFilterGroup' 'DatabaseManager.h must declare SaveWPEFilterGroup.'
Assert-Contains $h 'LoadWPEFilterGroups' 'DatabaseManager.h must declare LoadWPEFilterGroups.'
Assert-Contains $h 'SaveUserWPEFilterGroups' 'DatabaseManager.h must declare SaveUserWPEFilterGroups.'
Assert-Contains $h 'LoadUserWPEFilterGroups' 'DatabaseManager.h must declare LoadUserWPEFilterGroups.'
Assert-Contains $h 'SaveCardWPEFilterGroups' 'DatabaseManager.h must declare SaveCardWPEFilterGroups.'
Assert-Contains $h 'LoadCardWPEFilterGroups' 'DatabaseManager.h must declare LoadCardWPEFilterGroups.'

Assert-Contains $cpp 'CREATE TABLE IF NOT EXISTS wpe_filter_groups' 'DatabaseManager.cpp must create wpe_filter_groups.'
Assert-Contains $cpp 'CREATE TABLE IF NOT EXISTS wpe_filter_group_items' 'DatabaseManager.cpp must create wpe_filter_group_items.'
Assert-Contains $cpp 'CREATE TABLE IF NOT EXISTS user_wpe_filter_groups' 'DatabaseManager.cpp must create user_wpe_filter_groups.'
Assert-Contains $cpp 'CREATE TABLE IF NOT EXISTS card_wpe_filter_groups' 'DatabaseManager.cpp must create card_wpe_filter_groups.'
Assert-Contains $cpp 'default_enabled INTEGER NOT NULL DEFAULT 0' 'Group item table must persist default enabled state.'
Assert-Contains $cpp 'CreateWPEFilterGroupTables()' 'InitializeConfig must call CreateWPEFilterGroupTables.'

Write-Host 'WPE filter group database coverage is complete.'
```

- [ ] **Step 2: 运行测试确认失败**

Run:

```powershell
powershell -ExecutionPolicy Bypass -File .\tests\WPEFilterGroupDatabaseCoverageTest.ps1
```

Expected: FAIL，提示缺少 `WPEFilterGroupRecord` 或表名。

- [ ] **Step 3: 添加头文件声明**

在 `DatabaseManager.h` 的用户滤镜配置区域前后加入结构体和方法声明。

```cpp
struct WPEFilterGroupItemRecord {
    int filterId = 0;
    bool defaultEnabled = false;
};

struct WPEFilterGroupRecord {
    int id = 0;
    std::string name;
    std::string description;
    bool enabled = true;
    std::vector<WPEFilterGroupItemRecord> items;
};

bool CreateWPEFilterGroupTables();
int SaveWPEFilterGroup(const WPEFilterGroupRecord& group);
bool DeleteWPEFilterGroup(int groupId);
std::vector<WPEFilterGroupRecord> LoadWPEFilterGroups(bool includeDisabled = false);
WPEFilterGroupRecord LoadWPEFilterGroup(int groupId);

bool SaveUserWPEFilterGroups(const std::string& instanceId, const std::string& username,
                             const std::vector<int>& groupIds);
std::vector<int> LoadUserWPEFilterGroups(const std::string& instanceId, const std::string& username);
std::map<std::string, std::vector<int>> LoadAllUserWPEFilterGroups(const std::string& instanceId);

bool SaveCardWPEFilterGroups(const std::string& cardKey, const std::vector<int>& groupIds);
std::vector<int> LoadCardWPEFilterGroups(const std::string& cardKey);
bool DeleteCardWPEFilterGroups(const std::string& cardKey);
```

- [ ] **Step 4: 添加建表 SQL**

在 `DatabaseManager.cpp` 中新增 `CreateWPEFilterGroupTables()`，并在 `InitializeConfig()` 中继 `CreateUserFilterConfigTable()` 后调用。

```cpp
bool DatabaseManager::CreateWPEFilterGroupTables() {
    std::lock_guard<std::mutex> lock(configMutex);
    if (!configDb) return false;

    const char* groupsSql = R"SQL(
        CREATE TABLE IF NOT EXISTS wpe_filter_groups (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL,
            description TEXT DEFAULT '',
            enabled INTEGER NOT NULL DEFAULT 1,
            created_at TEXT DEFAULT CURRENT_TIMESTAMP,
            updated_at TEXT DEFAULT CURRENT_TIMESTAMP
        );
        CREATE INDEX IF NOT EXISTS idx_wpe_filter_groups_enabled ON wpe_filter_groups(enabled);
    )SQL";

    const char* itemsSql = R"SQL(
        CREATE TABLE IF NOT EXISTS wpe_filter_group_items (
            group_id INTEGER NOT NULL,
            filter_id INTEGER NOT NULL,
            default_enabled INTEGER NOT NULL DEFAULT 0,
            PRIMARY KEY(group_id, filter_id)
        );
        CREATE INDEX IF NOT EXISTS idx_wpe_filter_group_items_filter ON wpe_filter_group_items(filter_id);
    )SQL";

    const char* userSql = R"SQL(
        CREATE TABLE IF NOT EXISTS user_wpe_filter_groups (
            instance_id TEXT NOT NULL,
            username TEXT NOT NULL,
            group_id INTEGER NOT NULL,
            PRIMARY KEY(instance_id, username, group_id)
        );
        CREATE INDEX IF NOT EXISTS idx_user_wpe_filter_groups_user
            ON user_wpe_filter_groups(instance_id, username);
    )SQL";

    const char* cardSql = R"SQL(
        CREATE TABLE IF NOT EXISTS card_wpe_filter_groups (
            card_key TEXT NOT NULL,
            group_id INTEGER NOT NULL,
            PRIMARY KEY(card_key, group_id)
        );
        CREATE INDEX IF NOT EXISTS idx_card_wpe_filter_groups_card
            ON card_wpe_filter_groups(card_key);
    )SQL";

    char* errMsg = nullptr;
    for (const char* sql : {groupsSql, itemsSql, userSql, cardSql}) {
        if (sqlite3_exec(configDb, sql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
            DbLog("创建WPE滤镜组表失败: " + std::string(errMsg ? errMsg : ""));
            sqlite3_free(errMsg);
            return false;
        }
    }
    DbLog("WPE滤镜组表创建成功");
    return true;
}
```

在初始化处加入：

```cpp
if (!CreateWPEFilterGroupTables()) {
    DbLog("创建WPE滤镜组表失败，但继续运行");
}
```

- [ ] **Step 5: 实现保存和加载滤镜组**

实现 `SaveWPEFilterGroup` 时使用事务：

```cpp
int DatabaseManager::SaveWPEFilterGroup(const WPEFilterGroupRecord& group) {
    std::lock_guard<std::mutex> lock(configMutex);
    if (!configDb) return 0;

    sqlite3_exec(configDb, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);

    int groupId = group.id;
    sqlite3_stmt* stmt = nullptr;
    if (groupId > 0) {
        const char* sql =
            "UPDATE wpe_filter_groups SET name=?, description=?, enabled=?, updated_at=CURRENT_TIMESTAMP WHERE id=?;";
        if (sqlite3_prepare_v2(configDb, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            sqlite3_exec(configDb, "ROLLBACK;", nullptr, nullptr, nullptr);
            return 0;
        }
        sqlite3_bind_text(stmt, 1, group.name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, group.description.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, group.enabled ? 1 : 0);
        sqlite3_bind_int(stmt, 4, groupId);
    } else {
        const char* sql =
            "INSERT INTO wpe_filter_groups (name, description, enabled) VALUES (?, ?, ?);";
        if (sqlite3_prepare_v2(configDb, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            sqlite3_exec(configDb, "ROLLBACK;", nullptr, nullptr, nullptr);
            return 0;
        }
        sqlite3_bind_text(stmt, 1, group.name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, group.description.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, group.enabled ? 1 : 0);
    }

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        sqlite3_exec(configDb, "ROLLBACK;", nullptr, nullptr, nullptr);
        return 0;
    }
    sqlite3_finalize(stmt);
    if (groupId <= 0) {
        groupId = static_cast<int>(sqlite3_last_insert_rowid(configDb));
    }

    sqlite3_stmt* del = nullptr;
    sqlite3_prepare_v2(configDb, "DELETE FROM wpe_filter_group_items WHERE group_id=?;", -1, &del, nullptr);
    sqlite3_bind_int(del, 1, groupId);
    sqlite3_step(del);
    sqlite3_finalize(del);

    sqlite3_stmt* itemStmt = nullptr;
    sqlite3_prepare_v2(configDb,
        "INSERT INTO wpe_filter_group_items (group_id, filter_id, default_enabled) VALUES (?, ?, ?);",
        -1, &itemStmt, nullptr);
    for (const auto& item : group.items) {
        if (item.filterId <= 0) continue;
        sqlite3_reset(itemStmt);
        sqlite3_clear_bindings(itemStmt);
        sqlite3_bind_int(itemStmt, 1, groupId);
        sqlite3_bind_int(itemStmt, 2, item.filterId);
        sqlite3_bind_int(itemStmt, 3, item.defaultEnabled ? 1 : 0);
        sqlite3_step(itemStmt);
    }
    sqlite3_finalize(itemStmt);

    sqlite3_exec(configDb, "COMMIT;", nullptr, nullptr, nullptr);
    return groupId;
}
```

加载时先读 `wpe_filter_groups`，再读 `wpe_filter_group_items` 填充 `items`。删除组时同时删除 `wpe_filter_group_items`、`user_wpe_filter_groups`、`card_wpe_filter_groups` 对应行。

- [ ] **Step 6: 实现账号/卡密组绑定读写**

`SaveUserWPEFilterGroups` 和 `SaveCardWPEFilterGroups` 都采用先删后插，保证最新绑定。

```cpp
bool DatabaseManager::SaveUserWPEFilterGroups(
    const std::string& instanceId,
    const std::string& username,
    const std::vector<int>& groupIds) {
    std::lock_guard<std::mutex> lock(configMutex);
    if (!configDb || instanceId.empty() || username.empty()) return false;

    sqlite3_exec(configDb, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);
    sqlite3_stmt* del = nullptr;
    sqlite3_prepare_v2(configDb,
        "DELETE FROM user_wpe_filter_groups WHERE instance_id=? AND username=?;",
        -1, &del, nullptr);
    sqlite3_bind_text(del, 1, instanceId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(del, 2, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(del);
    sqlite3_finalize(del);

    sqlite3_stmt* ins = nullptr;
    sqlite3_prepare_v2(configDb,
        "INSERT OR IGNORE INTO user_wpe_filter_groups (instance_id, username, group_id) VALUES (?, ?, ?);",
        -1, &ins, nullptr);
    for (int groupId : groupIds) {
        if (groupId <= 0) continue;
        sqlite3_reset(ins);
        sqlite3_clear_bindings(ins);
        sqlite3_bind_text(ins, 1, instanceId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ins, 2, username.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(ins, 3, groupId);
        sqlite3_step(ins);
    }
    sqlite3_finalize(ins);
    sqlite3_exec(configDb, "COMMIT;", nullptr, nullptr, nullptr);
    return true;
}
```

`LoadUserWPEFilterGroups` 查询：

```sql
SELECT group_id FROM user_wpe_filter_groups
WHERE instance_id = ? AND username = ?
ORDER BY group_id;
```

`SaveCardWPEFilterGroups` 使用 `card_wpe_filter_groups(card_key, group_id)`。

- [ ] **Step 7: 运行数据库覆盖测试**

Run:

```powershell
powershell -ExecutionPolicy Bypass -File .\tests\WPEFilterGroupDatabaseCoverageTest.ps1
```

Expected: PASS。

---

### Task 2: 扩展 UserFilterManager 为“滤镜组权限解析器”

**Files:**
- Modify: `UserFilterManager.h`
- Modify: `UserFilterManager.cpp`
- Test: `tests/WPEFilterGroupEffectiveStateCoverageTest.ps1`

- [ ] **Step 1: 新建失败测试**

创建 `tests/WPEFilterGroupEffectiveStateCoverageTest.ps1`。

```powershell
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$h = Get-Content -Raw -Encoding UTF8 (Join-Path $root 'UserFilterManager.h')
$cpp = Get-Content -Raw -Encoding UTF8 (Join-Path $root 'UserFilterManager.cpp')

function Need($text, $needle, $message) {
    if ($text -notmatch [regex]::Escape($needle)) { throw $message }
}

Need $h 'struct AuthorizedFilterState' 'UserFilterManager.h must define AuthorizedFilterState.'
Need $h 'GetAuthorizedFilters' 'UserFilterManager.h must expose GetAuthorizedFilters.'
Need $h 'GetEffectiveUserFilters' 'UserFilterManager.h must keep GetEffectiveUserFilters.'
Need $h 'UpdateUserFilterGroups' 'UserFilterManager.h must expose UpdateUserFilterGroups.'
Need $h 'GetUserFilterGroups' 'UserFilterManager.h must expose GetUserFilterGroups.'
Need $cpp 'authorizedFilterIds' 'UserFilterManager.cpp must compute authorized filter ids.'
Need $cpp 'defaultEnabledFilterIds' 'UserFilterManager.cpp must compute default enabled ids.'
Need $cpp 'std::set_intersection' 'UserFilterManager.cpp must intersect user choices with authorization.'

Write-Host 'WPE filter group effective state coverage is complete.'
```

- [ ] **Step 2: 运行测试确认失败**

Run:

```powershell
powershell -ExecutionPolicy Bypass -File .\tests\WPEFilterGroupEffectiveStateCoverageTest.ps1
```

Expected: FAIL，提示缺少 `AuthorizedFilterState`。

- [ ] **Step 3: 添加授权状态结构和接口**

在 `UserFilterManager.h` 添加：

```cpp
struct AuthorizedFilterState {
    std::set<int> authorizedFilterIds;
    std::set<int> defaultEnabledFilterIds;
    std::vector<int> groupIds;
    bool hasUserConfig = false;
    std::set<int> userEnabledFilterIds;
    std::set<int> effectiveFilterIds;
};

bool UpdateUserFilterGroups(const std::string& instanceId, const std::string& username,
                            const std::vector<int>& groupIds);
std::vector<int> GetUserFilterGroups(const std::string& instanceId, const std::string& username);
AuthorizedFilterState GetAuthorizedFilters(const std::string& instanceId, const std::string& username);
```

新增缓存成员：

```cpp
std::map<std::string, std::map<std::string, std::set<int>>> userFilterGroupMap;
std::map<int, WPEFilterGroupRecord> filterGroupMap;
```

- [ ] **Step 4: 加载实例配置时同时加载组配置**

修改 `LoadInstanceConfig`：

```cpp
auto groups = database->LoadWPEFilterGroups(false);
auto userGroups = database->LoadAllUserWPEFilterGroups(instanceId);

std::lock_guard<std::mutex> lock(mapMutex);
filterGroupMap.clear();
for (const auto& group : groups) {
    filterGroupMap[group.id] = group;
}
userFilterGroupMap[instanceId].clear();
for (const auto& pair : userGroups) {
    userFilterGroupMap[instanceId][pair.first] = std::set<int>(pair.second.begin(), pair.second.end());
}
```

- [ ] **Step 5: 实现组授权解析**

在 `UserFilterManager.cpp` 添加：

```cpp
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
```

修改 `GetEffectiveUserFilters`：

```cpp
std::set<int> UserFilterManager::GetEffectiveUserFilters(
    const std::string& instanceId,
    const std::string& username) {
    AuthorizedFilterState state = GetAuthorizedFilters(instanceId, username);
    if (!state.authorizedFilterIds.empty() || !state.groupIds.empty()) {
        return state.effectiveFilterIds;
    }

    // 兼容旧数据：账号未绑定任何滤镜组时，继续使用旧实例默认配置。
    std::lock_guard<std::mutex> lock(mapMutex);
    auto instanceIt = userFilterMap.find(instanceId);
    if (instanceIt != userFilterMap.end()) {
        auto userIt = instanceIt->second.find(username);
        if (userIt != instanceIt->second.end()) return userIt->second;
    }
    auto defaultIt = defaultFilterMap.find(instanceId);
    if (defaultIt != defaultFilterMap.end()) return defaultIt->second;
    return {};
}
```

- [ ] **Step 6: 保存用户开关时过滤无权限 ID**

修改 `UpdateUserFilters`，保存前先算授权集合：

```cpp
AuthorizedFilterState auth = GetAuthorizedFilters(instanceId, username);
std::vector<int> sanitized;
for (int filterId : filterIds) {
    if (auth.authorizedFilterIds.empty() && auth.groupIds.empty()) {
        sanitized.push_back(filterId); // 旧兼容模式
    } else if (auth.authorizedFilterIds.count(filterId) > 0) {
        sanitized.push_back(filterId);
    }
}
```

后续数据库保存和内存缓存都使用 `sanitized`。

- [ ] **Step 7: 实现用户组绑定接口**

```cpp
bool UserFilterManager::UpdateUserFilterGroups(
    const std::string& instanceId,
    const std::string& username,
    const std::vector<int>& groupIds) {
    if (!database) return false;
    if (!database->SaveUserWPEFilterGroups(instanceId, username, groupIds)) return false;
    std::lock_guard<std::mutex> lock(mapMutex);
    userFilterGroupMap[instanceId][username] = std::set<int>(groupIds.begin(), groupIds.end());
    return true;
}

std::vector<int> UserFilterManager::GetUserFilterGroups(
    const std::string& instanceId,
    const std::string& username) {
    std::lock_guard<std::mutex> lock(mapMutex);
    auto instIt = userFilterGroupMap.find(instanceId);
    if (instIt == userFilterGroupMap.end()) return {};
    auto userIt = instIt->second.find(username);
    if (userIt == instIt->second.end()) return {};
    return std::vector<int>(userIt->second.begin(), userIt->second.end());
}
```

- [ ] **Step 8: 运行覆盖测试**

Run:

```powershell
powershell -ExecutionPolicy Bypass -File .\tests\WPEFilterGroupEffectiveStateCoverageTest.ps1
```

Expected: PASS。

---

### Task 3: 管理端 WebView 增加 WPE 滤镜组维护

**Files:**
- Modify: `ui_bridge.cpp`
- Modify: `web/app.js`
- Modify: `web/style.css`
- Test: `tests/WPEFilterGroupAdminUiCoverageTest.ps1`

- [ ] **Step 1: 新建失败测试**

创建 `tests/WPEFilterGroupAdminUiCoverageTest.ps1`。

```powershell
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bridge = Get-Content -Raw -Encoding UTF8 (Join-Path $root 'ui_bridge.cpp')
$app = Get-Content -Raw -Encoding UTF8 (Join-Path $root 'web\app.js')

function Need($text, $needle, $message) {
    if ($text -notmatch [regex]::Escape($needle)) { throw $message }
}

Need $bridge 'get_wpe_filter_groups' 'ui_bridge.cpp must handle get_wpe_filter_groups.'
Need $bridge 'save_wpe_filter_group' 'ui_bridge.cpp must handle save_wpe_filter_group.'
Need $bridge 'delete_wpe_filter_group' 'ui_bridge.cpp must handle delete_wpe_filter_group.'
Need $app 'renderWebUserStateSettings' 'web/app.js must render Web用户态设置 submenu.'
Need $app 'renderApiAddressSettings' 'web/app.js must render API地址 submenu.'
Need $app 'renderWpeFilterGroups' 'web/app.js must render WPE filter groups.'
Need $app 'saveWpeFilterGroup' 'web/app.js must save WPE filter groups.'
Need $app 'defaultEnabled' 'web/app.js must expose default enabled per group filter item.'

Write-Host 'WPE filter group admin UI coverage is complete.'
```

- [ ] **Step 2: 运行测试确认失败**

Run:

```powershell
powershell -ExecutionPolicy Bypass -File .\tests\WPEFilterGroupAdminUiCoverageTest.ps1
```

Expected: FAIL，提示缺少 action 或 `renderWebUserStateSettings`。

- [ ] **Step 3: 后端 action 获取滤镜组**

在 `ui_bridge.cpp` 的 WPE Filters action 附近新增：

```cpp
else if (action == "get_wpe_filter_groups") {
    json response;
    response["type"] = "wpe_filter_groups";
    response["groups"] = json::array();
    if (g_database) {
        auto groups = g_database->LoadWPEFilterGroups(true);
        for (const auto& group : groups) {
            json gj;
            gj["id"] = group.id;
            gj["name"] = group.name;
            gj["description"] = group.description;
            gj["enabled"] = group.enabled;
            gj["items"] = json::array();
            for (const auto& item : group.items) {
                gj["items"].push_back({
                    {"filterId", item.filterId},
                    {"defaultEnabled", item.defaultEnabled}
                });
            }
            response["groups"].push_back(gj);
        }
    }
    UIBridge_PushMessage(response.dump());
}
```

- [ ] **Step 4: 后端 action 保存/删除滤镜组**

```cpp
else if (action == "save_wpe_filter_group") {
    if (!g_database) {
        UIBridge_Toast("error", "保存失败", "数据库未初始化");
        return;
    }
    WPEFilterGroupRecord group;
    group.id = msg.value("groupId", 0);
    group.name = UIBridge_TrimCopy(msg.value("name", ""));
    group.description = msg.value("description", "");
    group.enabled = msg.value("enabled", true);
    if (group.name.empty()) {
        UIBridge_Toast("warning", "保存失败", "滤镜组名称不能为空");
        return;
    }
    if (msg.contains("items") && msg["items"].is_array()) {
        for (const auto& itemJson : msg["items"]) {
            WPEFilterGroupItemRecord item;
            item.filterId = itemJson.value("filterId", 0);
            item.defaultEnabled = itemJson.value("defaultEnabled", false);
            if (item.filterId > 0) group.items.push_back(item);
        }
    }
    int savedId = g_database->SaveWPEFilterGroup(group);
    if (savedId > 0) {
        if (g_userFilterManager) {
            g_userFilterManager->LoadInstanceConfig("");
        }
        UIBridge_Toast("success", "保存成功", "WPE滤镜组已保存");
        UIBridge_HandleMessage("{\"action\":\"get_wpe_filter_groups\"}");
    } else {
        UIBridge_Toast("error", "保存失败", "无法保存WPE滤镜组");
    }
}
else if (action == "delete_wpe_filter_group") {
    int groupId = msg.value("groupId", 0);
    if (g_database && groupId > 0 && g_database->DeleteWPEFilterGroup(groupId)) {
        UIBridge_Toast("success", "删除成功", "WPE滤镜组已删除");
        UIBridge_HandleMessage("{\"action\":\"get_wpe_filter_groups\"}");
    } else {
        UIBridge_Toast("error", "删除失败", "无法删除WPE滤镜组");
    }
}
```

实现后应改为刷新所有实例缓存；如果没有全实例枚举接口，至少在实例启动/保存用户配置时重新加载。

- [ ] **Step 5: 前端拆分 API地址 / Web用户态设置子菜单**

在 `web/app.js` 当前实例配置的用户滤镜区域中，把原来的 `renderUserFilterConfig(inst, isRunning)` 拆成两个子菜单渲染函数。`API地址` 放 HTTP 端口、启动/停止服务和访问 URL；`Web用户态设置` 放用户态开关、执行次数显示、滤镜组和用户 Web 显示名。

```javascript
let currentUserFilterSubTab = 'webUserState';

function renderUserFilterConfig(inst, isRunning) {
    return `
        <h3>用户滤镜配置</h3>
        <div class="sub-tabs">
            <button class="sub-tab ${currentUserFilterSubTab === 'webUserState' ? 'active' : ''}"
                onclick="switchUserFilterSubTab('webUserState')">Web用户态设置</button>
            <button class="sub-tab ${currentUserFilterSubTab === 'apiAddress' ? 'active' : ''}"
                onclick="switchUserFilterSubTab('apiAddress')">API地址</button>
        </div>
        <div id="userFilterSubTabContent">
            ${currentUserFilterSubTab === 'apiAddress'
                ? renderApiAddressSettings()
                : renderWebUserStateSettings()}
        </div>
    `;
}

function switchUserFilterSubTab(tab) {
    currentUserFilterSubTab = tab;
    const content = $('userFilterSubTabContent');
    if (content) {
        content.innerHTML = tab === 'apiAddress'
            ? renderApiAddressSettings()
            : renderWebUserStateSettings();
        if (window.__lastUserFilterConfigData) {
            handleUserFilterConfigData(window.__lastUserFilterConfigData);
        }
    }
}

function renderApiAddressSettings() {
    return `
        <h4 class="section-title">HTTP服务器配置</h4>
        <div class="form-row">
            <div class="form-group">
                <label>监听端口</label>
                <input type="number" id="httpPort" class="input" value="8080">
            </div>
            <button class="btn btn-primary" onclick="saveHttpPort()">保存端口</button>
        </div>
        <div id="httpServerStatus"></div>
        <h4 class="section-title">用户访问地址</h4>
        <div id="accessUrls"></div>
    `;
}

function renderWebUserStateSettings() {
    return `
        <div class="info-hint">
            说明：启用后，每个SOCKS用户只能管理自己有权限的WPE滤镜。
        </div>
        <div class="checkbox-group">
            <input type="checkbox" id="enableUserFilterMode">
            <label for="enableUserFilterMode">启用用户滤镜模式</label>
        </div>
        <div class="checkbox-group">
            <input type="checkbox" id="showUserFilterCounts">
            <label for="showUserFilterCounts">远程控制台显示执行次数</label>
        </div>
        <h4 class="section-title">用户Web显示名称</h4>
        <div id="wpeWebDisplayNameList"></div>
        <h4 class="section-title">WPE滤镜组</h4>
        <div id="wpeFilterGroupList"></div>
        <div id="wpeFilterGroupEditor"></div>
    `;
}
```

在 `handleUserFilterConfigData(data)` 开始处保存最后一次数据，切换子菜单后可重放：

```javascript
window.__lastUserFilterConfigData = data;
```

- [ ] **Step 6: 前端新增滤镜组页面**

在 `web/app.js` WPE 滤镜页面附近新增全局状态：

```javascript
let wpeFilterGroups = [];
let currentWpeFilterGroupId = 0;
```

新增渲染函数：

```javascript
function renderWpeFilterGroups(groups) {
    wpeFilterGroups = groups || [];
    const container = $('wpeFilterGroupList');
    if (!container) return;
    if (!wpeFilterGroups.length) {
        container.innerHTML = '<div class="empty-state">暂无滤镜组</div>';
        return;
    }
    container.innerHTML = wpeFilterGroups.map(g => `
        <div class="filter-group-card ${g.enabled ? '' : 'is-disabled'}">
            <div class="filter-group-head">
                <strong>${escapeHtmlInline(g.name)}</strong>
                <span>${g.items ? g.items.length : 0} 个滤镜</span>
            </div>
            <div class="filter-group-actions">
                <button class="btn btn-sm" onclick="editWpeFilterGroup(${g.id})">编辑</button>
                <button class="btn btn-sm btn-danger" onclick="deleteWpeFilterGroup(${g.id})">删除</button>
            </div>
        </div>
    `).join('');
}
```

新增保存函数，`items` 中每个滤镜要带 `defaultEnabled`：

```javascript
function saveWpeFilterGroup() {
    const name = $('wpeGroupName').value.trim();
    const description = $('wpeGroupDescription').value.trim();
    const enabled = $('wpeGroupEnabled').checked;
    const rows = document.querySelectorAll('#wpeGroupFilterList .wpe-group-filter-row');
    const items = [];
    rows.forEach(row => {
        const allowed = row.querySelector('.wpe-group-filter-allowed');
        const def = row.querySelector('.wpe-group-filter-default');
        if (allowed && allowed.checked) {
            items.push({
                filterId: parseInt(row.dataset.filterId, 10),
                defaultEnabled: !!(def && def.checked)
            });
        }
    });
    postAction('save_wpe_filter_group', {
        groupId: currentWpeFilterGroupId || 0,
        name,
        description,
        enabled,
        items
    });
}
```

- [ ] **Step 7: 前端消息分发**

在 `dispatchBackendPayload` 或 switch 中加入：

```javascript
case 'wpe_filter_groups':
    renderWpeFilterGroups(data.groups);
    break;
```

- [ ] **Step 8: 加样式**

在 `web/style.css` 添加：

```css
.filter-group-card {
    border: 1px solid var(--border);
    border-radius: 8px;
    padding: 12px;
    margin-bottom: 8px;
    background: var(--surface);
}
.filter-group-card.is-disabled {
    opacity: .55;
}
.filter-group-head {
    display: flex;
    justify-content: space-between;
    gap: 12px;
    align-items: center;
}
.filter-group-actions {
    display: flex;
    gap: 8px;
    margin-top: 10px;
}
.wpe-group-filter-row {
    display: grid;
    grid-template-columns: 1fr auto auto;
    align-items: center;
    gap: 12px;
    min-height: 36px;
}
.sub-tabs {
    display: flex;
    gap: 8px;
    margin: 12px 0;
}
.sub-tab {
    width: auto;
    min-height: 34px;
    padding: 0 14px;
    border-radius: 8px;
}
.sub-tab.active {
    background: var(--primary);
    color: #fff;
}
```

- [ ] **Step 9: 运行覆盖测试**

Run:

```powershell
powershell -ExecutionPolicy Bypass -File .\tests\WPEFilterGroupAdminUiCoverageTest.ps1
```

Expected: PASS。

---

### Task 4: WPE 滤镜增加用户 Web 显示名称

**Files:**
- Modify: `WPEFilter.h`
- Modify: `WPEFilter.cpp`
- Modify: `ui_bridge.cpp`
- Modify: `web/app.js`
- Test: `tests/WPEFilterWebDisplayNameCoverageTest.ps1`

- [ ] **Step 1: 新建失败测试**

创建 `tests/WPEFilterWebDisplayNameCoverageTest.ps1`。

```powershell
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$wpeH = Get-Content -Raw -Encoding UTF8 (Join-Path $root 'WPEFilter.h')
$wpeCpp = Get-Content -Raw -Encoding UTF8 (Join-Path $root 'WPEFilter.cpp')
$bridge = Get-Content -Raw -Encoding UTF8 (Join-Path $root 'ui_bridge.cpp')
$app = Get-Content -Raw -Encoding UTF8 (Join-Path $root 'web\app.js')
$webServer = Get-Content -Raw -Encoding UTF8 (Join-Path $root 'UserFilterWebServer.cpp')

function Need($text, $needle, $message) {
    if ($text -notmatch [regex]::Escape($needle)) { throw $message }
}

Need $wpeH 'webDisplayName' 'WPEFilter.h must define webDisplayName.'
Need $wpeCpp 'item["webDisplayName"]' 'WPEFilter.cpp must export webDisplayName.'
Need $wpeCpp 'f.webDisplayName' 'WPEFilter.cpp must import webDisplayName.'
Need $bridge 'webDisplayName' 'ui_bridge.cpp must expose webDisplayName to WebView.'
Need $app 'wpeWebDisplayNameList' 'web/app.js must render Web display name settings.'
Need $app 'saveWpeWebDisplayNames' 'web/app.js must save Web display names.'
Need $webServer 'webDisplayName.empty() ? filter.name : filter.webDisplayName' 'UserFilterWebServer flow must use webDisplayName fallback.'

Write-Host 'WPE filter web display name coverage is complete.'
```

- [ ] **Step 2: 运行测试确认失败**

Run:

```powershell
powershell -ExecutionPolicy Bypass -File .\tests\WPEFilterWebDisplayNameCoverageTest.ps1
```

Expected: FAIL，提示缺少 `webDisplayName`。

- [ ] **Step 3: WPEFilter 数据结构新增字段**

在 `WPEFilter.h` 的 `FilterInfoData` 中 `name` 后添加：

```cpp
std::string webDisplayName;
```

构造函数初始化：

```cpp
webDisplayName()
```

字段语义：
- `name`：管理端内部名称、日志、导入导出识别。
- `webDisplayName`：用户 Web 控制台展示名称。
- `webDisplayName` 为空时，用户 Web 控制台显示 `name`。

- [ ] **Step 4: 导入导出保存 webDisplayName**

在 `WPEFilter.cpp` 的 `ExportToJson()` 和 `ExportToJsonForLocalSave()` 中写入：

```cpp
item["webDisplayName"] = f.webDisplayName;
```

在 `ImportFromJson`、`ImportFromJsonMerge`、`ImportFromJsonOverwrite` 读入：

```cpp
f.webDisplayName = item.get("webDisplayName", "").asString();
```

兼容旧 JSON：缺少字段时为空，用户 Web 自动回退原名称。

- [ ] **Step 5: WebView 后端返回和保存字段**

在 `ui_bridge.cpp` 的 `get_wpe_filters` 和 `wpe_filter_get` 响应中加入：

```cpp
fj["webDisplayName"] = filter.webDisplayName;
```

在 `wpe_filter_create_full` 和 `wpe_filter_update` 解析中加入：

```cpp
filter.webDisplayName = filterJson.value("webDisplayName", "");
```

新增批量保存 action：

```cpp
else if (action == "save_wpe_web_display_names") {
    if (!g_wpeFilterManager) {
        UIBridge_Toast("error", "保存失败", "WPE滤镜管理器未初始化");
        return;
    }
    if (!msg.contains("items") || !msg["items"].is_array()) {
        UIBridge_Toast("warning", "保存失败", "缺少显示名称列表");
        return;
    }
    int updated = 0;
    for (const auto& item : msg["items"]) {
        int filterId = item.value("filterId", 0);
        std::string displayName = item.value("webDisplayName", "");
        auto* filter = g_wpeFilterManager->GetFilter(filterId);
        if (!filter) continue;
        WPEFilter::FilterInfo copy = *filter;
        copy.webDisplayName = displayName;
        if (g_wpeFilterManager->UpdateFilter(filterId, copy)) {
            ++updated;
        }
    }
    UIBridge_SaveWPEConfigWithFeedback(false, "", "");
    UIBridge_Toast("success", "保存成功", "已更新 " + std::to_string(updated) + " 个用户Web显示名称");
    UIBridge_HandleMessage("{\"action\":\"get_wpe_filters\"}");
}
```

- [ ] **Step 6: 管理端 Web用户态设置渲染显示名列表**

在 `web/app.js` 中新增：

```javascript
function renderWpeWebDisplayNameSettings(filters) {
    const container = $('wpeWebDisplayNameList');
    if (!container) return;
    if (!filters || filters.length === 0) {
        container.innerHTML = '<p style="color: var(--text-muted);">暂无WPE滤镜</p>';
        return;
    }
    container.innerHTML = filters.map(f => `
        <div class="form-row wpe-web-name-row" data-filter-id="${f.id}">
            <div class="form-group">
                <label>[${f.id}] ${escapeHtmlInline(f.name)}</label>
                <input class="input wpe-web-display-name-input"
                    value="${escapeHtmlInline(f.webDisplayName || '')}"
                    placeholder="用户Web显示名称，留空则显示原名称">
            </div>
        </div>
    `).join('') + `
        <div class="toolbar">
            <button class="btn btn-primary" onclick="saveWpeWebDisplayNames()">保存显示名称</button>
        </div>
    `;
}

function saveWpeWebDisplayNames() {
    const rows = document.querySelectorAll('.wpe-web-name-row');
    const items = [];
    rows.forEach(row => {
        const input = row.querySelector('.wpe-web-display-name-input');
        items.push({
            filterId: parseInt(row.dataset.filterId, 10),
            webDisplayName: input ? input.value.trim() : ''
        });
    });
    postAction('save_wpe_web_display_names', { items });
}
```

在收到 `wpe_filters` 后，如果 `wpeWebDisplayNameList` 存在，调用：

```javascript
renderWpeWebDisplayNameSettings(data.filters || []);
```

- [ ] **Step 7: 用户 Web 控制台优先显示 webDisplayName**

扩展 `AuthorizedWebFilter`：

```cpp
std::string webDisplayName;
```

在 `PacketCollector` 和 `IOCPThreadPool` 的 `SetAuthorizedFilterListGetter` 中：

```cpp
item.name = filter.webDisplayName.empty() ? filter.name : filter.webDisplayName;
item.webDisplayName = filter.webDisplayName;
```

在 `UserFilterWebServer.cpp` 构造 JSON 时使用 `item.name`，确保用户 Web 看到的是显示名。

- [ ] **Step 8: 运行覆盖测试**

Run:

```powershell
powershell -ExecutionPolicy Bypass -File .\tests\WPEFilterWebDisplayNameCoverageTest.ps1
```

Expected: PASS。

---

### Task 5: 卡密/账号绑定滤镜组

**Files:**
- Modify: `ui_bridge.cpp`
- Modify: `web/app.js`
- Modify: `CloudIntegration.cpp`
- Modify: `ABProtectIntegration.cpp` 或本地卡密接入点
- Test: `tests/WPEFilterGroupCardBindingCoverageTest.ps1`

- [ ] **Step 1: 新建失败测试**

创建 `tests/WPEFilterGroupCardBindingCoverageTest.ps1`。

```powershell
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bridge = Get-Content -Raw -Encoding UTF8 (Join-Path $root 'ui_bridge.cpp')
$cloud = Get-Content -Raw -Encoding UTF8 (Join-Path $root 'CloudIntegration.cpp')
$abp = Get-Content -Raw -Encoding UTF8 (Join-Path $root 'ABProtectIntegration.cpp')

function NeedAny($texts, $needle, $message) {
    foreach ($text in $texts) {
        if ($text -match [regex]::Escape($needle)) { return }
    }
    throw $message
}

NeedAny @($bridge) 'save_card_wpe_filter_groups' 'ui_bridge.cpp must save card filter group binding.'
NeedAny @($bridge) 'save_user_wpe_filter_groups' 'ui_bridge.cpp must save user filter group binding.'
NeedAny @($cloud, $abp, $bridge) 'LoadCardWPEFilterGroups' 'Activation/login flow must load card filter groups.'
NeedAny @($cloud, $abp, $bridge) 'SaveUserWPEFilterGroups' 'Activation/login flow must persist user inherited filter groups.'

Write-Host 'WPE filter group card binding coverage is complete.'
```

- [ ] **Step 2: 运行测试确认失败**

Run:

```powershell
powershell -ExecutionPolicy Bypass -File .\tests\WPEFilterGroupCardBindingCoverageTest.ps1
```

Expected: FAIL。

- [ ] **Step 3: 管理端新增卡密绑定 action**

如果现有内置开卡数据在本项目内只通过 ABProtect 云端处理，先在本地提供映射表维护接口，后续和云端同步时按同一语义传递。

```cpp
else if (action == "save_card_wpe_filter_groups") {
    const std::string cardKey = UIBridge_TrimCopy(msg.value("cardKey", ""));
    std::vector<int> groupIds;
    if (msg.contains("groupIds") && msg["groupIds"].is_array()) {
        for (const auto& idVal : msg["groupIds"]) {
            if (idVal.is_number_integer()) groupIds.push_back(idVal.get<int>());
        }
    }
    if (cardKey.empty()) {
        UIBridge_Toast("warning", "保存失败", "卡密不能为空");
        return;
    }
    if (g_database && g_database->SaveCardWPEFilterGroups(cardKey, groupIds)) {
        UIBridge_Toast("success", "保存成功", "卡密滤镜组权限已保存");
    } else {
        UIBridge_Toast("error", "保存失败", "无法保存卡密滤镜组权限");
    }
}
```

- [ ] **Step 4: 管理端新增账号绑定 action**

```cpp
else if (action == "save_user_wpe_filter_groups") {
    const std::string instanceId = msg.value("instanceId", "");
    const std::string username = UIBridge_TrimCopy(msg.value("username", ""));
    std::vector<int> groupIds;
    if (msg.contains("groupIds") && msg["groupIds"].is_array()) {
        for (const auto& idVal : msg["groupIds"]) {
            if (idVal.is_number_integer()) groupIds.push_back(idVal.get<int>());
        }
    }
    if (instanceId.empty() || username.empty()) {
        UIBridge_Toast("warning", "保存失败", "实例和账号不能为空");
        return;
    }
    if (g_userFilterManager && g_userFilterManager->UpdateUserFilterGroups(instanceId, username, groupIds)) {
        UIBridge_Toast("success", "保存成功", "账号滤镜组权限已保存");
    } else {
        UIBridge_Toast("error", "保存失败", "无法保存账号滤镜组权限");
    }
}
```

- [ ] **Step 5: 卡密激活继承组权限**

在账号由卡密注册/激活成功的路径后加入：

```cpp
static void ApplyCardWPEFilterGroupsToUser(
    const std::string& instanceId,
    const std::string& username,
    const std::string& cardKey) {
    if (!g_database || !g_userFilterManager) return;
    if (instanceId.empty() || username.empty() || cardKey.empty()) return;
    std::vector<int> groupIds = g_database->LoadCardWPEFilterGroups(cardKey);
    if (!groupIds.empty()) {
        g_userFilterManager->UpdateUserFilterGroups(instanceId, username, groupIds);
    }
}
```

调用点：
- 卡密注册账号成功后：`ApplyCardWPEFilterGroupsToUser(instanceId, username, cardKey)`
- 卡密登录且用户名就是卡密时：`ApplyCardWPEFilterGroupsToUser(instanceId, cardKey, cardKey)`
- 如果当前项目没有本地开卡流程，先在管理端账号绑定 action 中提供手动绑定，云端字段接入作为后续同步点。

- [ ] **Step 6: 前端开卡/账号页增加滤镜组选择**

在管理端账号或卡密编辑区域增加多选框：

```javascript
function buildWpeGroupCheckboxes(selectedGroupIds) {
    const selected = new Set((selectedGroupIds || []).map(Number));
    return (wpeFilterGroups || []).map(group => `
        <label class="checkbox-group">
            <input type="checkbox" class="wpe-group-bind-cb" value="${group.id}" ${selected.has(group.id) ? 'checked' : ''}>
            <span>${escapeHtmlInline(group.name)}</span>
        </label>
    `).join('');
}

function collectSelectedWpeGroupIds(rootSelector) {
    return Array.from(document.querySelectorAll(`${rootSelector} .wpe-group-bind-cb:checked`))
        .map(cb => parseInt(cb.value, 10))
        .filter(Number.isFinite);
}
```

保存卡密时调用 `save_card_wpe_filter_groups`；保存账号权限时调用 `save_user_wpe_filter_groups`。

- [ ] **Step 7: 运行覆盖测试**

Run:

```powershell
powershell -ExecutionPolicy Bypass -File .\tests\WPEFilterGroupCardBindingCoverageTest.ps1
```

Expected: PASS。

---

### Task 6: 用户 Web 控制台只展示授权滤镜并分类

**Files:**
- Modify: `UserFilterWebServer.h`
- Modify: `UserFilterWebServer.cpp`
- Modify: `PacketCollector.cpp`
- Modify: `IOCPThreadPool.cpp`
- Test: `tests\UserFilterWebAuthorizationCoverageTest.ps1`

- [ ] **Step 1: 新建失败测试**

创建 `tests\UserFilterWebAuthorizationCoverageTest.ps1`。

```powershell
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$h = Get-Content -Raw -Encoding UTF8 (Join-Path $root 'UserFilterWebServer.h')
$cpp = Get-Content -Raw -Encoding UTF8 (Join-Path $root 'UserFilterWebServer.cpp')

function Need($text, $needle, $message) {
    if ($text -notmatch [regex]::Escape($needle)) { throw $message }
}

Need $h 'AuthorizedWebFilter' 'UserFilterWebServer.h must define AuthorizedWebFilter.'
Need $h 'SetAuthorizedFilterListGetter' 'UserFilterWebServer.h must expose authorized filter getter.'
Need $cpp '"collectorFilters"' 'UserFilterWebServer.cpp must return collectorFilters.'
Need $cpp '"heartbeatFilters"' 'UserFilterWebServer.cpp must return heartbeatFilters.'
Need $cpp 'authorizedFilterIds' 'UserFilterWebServer.cpp must enforce authorization.'
Need $cpp 'defaultEnabled' 'UserFilterWebServer.cpp must return default enabled state.'
Need $cpp 'data-auth-filter-id' 'HTML must render authorized filter ids for saving.'

Write-Host 'User filter web authorization coverage is complete.'
```

- [ ] **Step 2: 运行测试确认失败**

Run:

```powershell
powershell -ExecutionPolicy Bypass -File .\tests\UserFilterWebAuthorizationCoverageTest.ps1
```

Expected: FAIL。

- [ ] **Step 3: 替换滤镜列表回调数据结构**

在 `UserFilterWebServer.h` 添加：

```cpp
struct AuthorizedWebFilter {
    int id = 0;
    std::string name;
    bool applyToCollector = false;
    bool applyToHeartbeat = false;
};

void SetAuthorizedFilterListGetter(
    std::function<std::vector<AuthorizedWebFilter>(const std::string& username)> getter);
```

成员：

```cpp
std::function<std::vector<AuthorizedWebFilter>(const std::string& username)> authorizedFilterListGetter;
```

保留旧 `SetFilterListGetter` 兼容老代码。

- [ ] **Step 4: PacketCollector 设置授权滤镜获取器**

在 `PacketCollector::StartUserFilterHttpServer()` 中新增：

```cpp
userFilterHttpServer->SetAuthorizedFilterListGetter([this](const std::string& username) {
    std::vector<AuthorizedWebFilter> result;
    if (!g_wpeFilterManager || !g_userFilterManager) return result;

    const auto state = g_userFilterManager->GetAuthorizedFilters(m_instanceId, username);
    const auto allFilters = g_wpeFilterManager->GetAllFilters();
    for (const auto& filter : allFilters) {
        if (state.authorizedFilterIds.count(filter.id) == 0) continue;
        if (!filter.target.applyToAllInstances) {
            const auto& ids = filter.target.targetInstanceIds;
            if (!ids.empty() && std::find(ids.begin(), ids.end(), m_instanceId) == ids.end()) {
                continue;
            }
        }
        AuthorizedWebFilter item;
        item.id = filter.id;
        item.name = filter.name;
        item.applyToCollector = filter.target.applyToCollector || filter.target.applyToAllInstances;
        item.applyToHeartbeat = filter.target.applyToHeartbeat || filter.target.applyToAllInstances;
        result.push_back(item);
    }
    return result;
});
```

IOCP 同样设置该回调。

- [ ] **Step 5: 修改 `/api/filters` JSON**

在 `HandleGetFilters` 中：

```cpp
AuthorizedFilterState authState;
if (g_userFilterManager) {
    authState = g_userFilterManager->GetAuthorizedFilters(instanceId, username);
}

std::vector<AuthorizedWebFilter> allFilters;
if (authorizedFilterListGetter) {
    allFilters = authorizedFilterListGetter(username);
} else if (filterListGetter) {
    for (const auto& pair : filterListGetter()) {
        AuthorizedWebFilter item;
        item.id = pair.first;
        item.name = pair.second;
        item.applyToCollector = true;
        item.applyToHeartbeat = true;
        allFilters.push_back(item);
    }
}

std::set<int> enabledFilters = authState.effectiveFilterIds;
```

响应结构：

```json
{
  "success": true,
  "username": "u1",
  "expireTime": "...",
  "hasUserConfig": false,
  "collectorFilters": [],
  "heartbeatFilters": []
}
```

每个滤镜项：

```json
{
  "id": 1,
  "name": "xxx",
  "enabled": true,
  "defaultEnabled": true,
  "executionCount": 0
}
```

- [ ] **Step 6: 保存用户开关时服务端强制权限过滤**

`HandleUpdateFilters` 中解析 `filterIds` 后：

```cpp
if (g_userFilterManager) {
    auto authState = g_userFilterManager->GetAuthorizedFilters(instanceId, username);
    std::vector<int> sanitized;
    for (int filterId : filterIds) {
        if (authState.authorizedFilterIds.count(filterId) > 0) {
            sanitized.push_back(filterId);
        }
    }
    if (g_userFilterManager->UpdateUserFilters(instanceId, username, sanitized)) {
        return "{\"success\":true,\"message\":\"保存成功\"}";
    }
}
```

- [ ] **Step 7: HTML 分成采集/伪心跳分类**

替换 JS 渲染逻辑：

```javascript
function renderFilterSection(title, filters) {
    if (!filters || filters.length === 0) {
        return '<div class="user-info">' + title + '：暂无可用滤镜</div>';
    }
    return '<h3 class="filter-section-title">' + title + '</h3>' + filters.map(f => {
        return '<div class="filter-item">' +
            '<input type="checkbox" class="filter-checkbox" data-auth-filter-id="' + f.id + '" id="filter_' + f.id + '"' + (f.enabled ? ' checked' : '') + '>' +
            '<label for="filter_' + f.id + '">' + f.name +
            (f.defaultEnabled ? ' <span class="filter-badge">默认开启</span>' : '') +
            (f.executionCount !== undefined ? ' <span class="filter-count">(执行: ' + f.executionCount + '次)</span>' : '') +
            '</label></div>';
    }).join('');
}

document.getElementById('filterList').innerHTML =
    renderFilterSection('采集监听端口滤镜', data.collectorFilters || []) +
    renderFilterSection('伪心跳监听端口滤镜', data.heartbeatFilters || []);
```

保存时只收集授权 checkbox：

```javascript
const checkboxes = document.querySelectorAll('.filter-checkbox[data-auth-filter-id]');
```

- [ ] **Step 8: 运行覆盖测试**

Run:

```powershell
powershell -ExecutionPolicy Bypass -File .\tests\UserFilterWebAuthorizationCoverageTest.ps1
```

Expected: PASS。

---

### Task 7: 用户 Web 控制台显示 GameID 和清空当前账号数据池

**Files:**
- Modify: `UserFilterWebServer.h`
- Modify: `UserFilterWebServer.cpp`
- Modify: `PacketCollector.cpp`
- Modify: `IOCPThreadPool.cpp`
- Test: `tests\UserFilterWebPoolControlCoverageTest.ps1`

- [ ] **Step 1: 新建失败测试**

创建 `tests\UserFilterWebPoolControlCoverageTest.ps1`。

```powershell
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$h = Get-Content -Raw -Encoding UTF8 (Join-Path $root 'UserFilterWebServer.h')
$cpp = Get-Content -Raw -Encoding UTF8 (Join-Path $root 'UserFilterWebServer.cpp')
$pc = Get-Content -Raw -Encoding UTF8 (Join-Path $root 'PacketCollector.cpp')

function Need($text, $needle, $message) {
    if ($text -notmatch [regex]::Escape($needle)) { throw $message }
}

Need $h 'SetUserGameInfoGetter' 'UserFilterWebServer.h must expose game info getter.'
Need $h 'SetUserPoolClearer' 'UserFilterWebServer.h must expose pool clearer.'
Need $cpp '/api/clear-pool' 'UserFilterWebServer.cpp must add clear pool endpoint.'
Need $cpp '"gameId"' 'UserFilterWebServer.cpp must return gameId.'
Need $cpp '"poolCount"' 'UserFilterWebServer.cpp must return poolCount.'
Need $cpp 'HandleClearPool' 'UserFilterWebServer.cpp must implement HandleClearPool.'
Need $pc 'ClearByUsername' 'PacketCollector.cpp must wire ClearByUsername to web callback.'

Write-Host 'User filter web pool control coverage is complete.'
```

- [ ] **Step 2: 运行测试确认失败**

Run:

```powershell
powershell -ExecutionPolicy Bypass -File .\tests\UserFilterWebPoolControlCoverageTest.ps1
```

Expected: FAIL。

- [ ] **Step 3: WebServer 增加回调**

在 `UserFilterWebServer.h` 添加：

```cpp
struct UserGameInfo {
    std::string gameId;
    size_t poolCount = 0;
};

void SetUserGameInfoGetter(std::function<UserGameInfo(const std::string& username)> getter);
void SetUserPoolClearer(std::function<size_t(const std::string& username)> clearer);
```

成员：

```cpp
std::function<UserGameInfo(const std::string& username)> userGameInfoGetter;
std::function<size_t(const std::string& username)> userPoolClearer;
```

- [ ] **Step 4: PacketCollector 接入回调**

在 `PacketCollector::StartUserFilterHttpServer()` 中：

```cpp
userFilterHttpServer->SetUserGameInfoGetter([this](const std::string& username) {
    UserGameInfo info;
    {
        std::lock_guard<std::mutex> lock(accountsMutex);
        auto it = accounts.find(username);
        if (it != accounts.end()) {
            info.gameId = it->second.currentGameID;
        }
    }
    if (info.gameId.empty()) {
        info.gameId = GetLastGameIDForUser(username);
    }
    info.poolCount = collectedPool.GetCountByUsername(username);
    return info;
});

userFilterHttpServer->SetUserPoolClearer([this](const std::string& username) {
    if (username.empty()) return static_cast<size_t>(0);
    return collectedPool.ClearByUsername(username);
});
```

如果实际成员名不是 `collectedPool` 或已有全局池，按本项目实际池对象替换，但必须调用 `ClearByUsername(username)`。

- [ ] **Step 5: GET filters 返回 GameID 和数据池数量**

在 `HandleGetFilters` 中：

```cpp
UserGameInfo gameInfo;
if (userGameInfoGetter) {
    gameInfo = userGameInfoGetter(username);
}
oss << ",\"gameId\":\"" << JsonEscape(gameInfo.gameId) << "\"";
oss << ",\"poolCount\":" << static_cast<unsigned long long>(gameInfo.poolCount);
```

如果当前文件没有 JSON 转义函数，新增：

```cpp
static std::string JsonEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}
```

- [ ] **Step 6: 新增清空接口**

路由：

```cpp
else if (method == "POST" && path == "/api/clear-pool") {
    std::string body = ParseRequestBody(request);
    response = BuildHttpResponse(200, "application/json; charset=utf-8", HandleClearPool(body));
}
```

实现：

```cpp
std::string UserFilterWebServer::HandleClearPool(const std::string& body) {
    std::string username = ExtractJsonString(body, "username");
    if (username.empty()) {
        return "{\"success\":false,\"message\":\"用户名不能为空\"}";
    }
    if (!userPoolClearer) {
        return "{\"success\":false,\"message\":\"系统未配置数据池清理器\"}";
    }
    size_t cleared = userPoolClearer(username);
    AB_LOG_INFO("[用户滤镜Web] 清空用户数据池: " + username + ", 数量: " + std::to_string(cleared));
    return "{\"success\":true,\"message\":\"清空成功\",\"cleared\":" + std::to_string(cleared) + "}";
}
```

- [ ] **Step 7: 前端显示 GameID 和清空按钮**

用户信息区域加入：

```javascript
userInfoHtml += '<div class="user-info-row">';
userInfoHtml += '<span class="user-info-label">GameID</span>';
userInfoHtml += '<span class="user-info-value">' + (data.gameId || '未绑定') + '</span>';
userInfoHtml += '</div>';
userInfoHtml += '<div class="user-info-row">';
userInfoHtml += '<span class="user-info-label">采集数据池</span>';
userInfoHtml += '<span class="user-info-value">' + (data.poolCount || 0) + ' 条</span>';
userInfoHtml += '</div>';
```

按钮：

```html
<button class="button-warning" onclick="clearMyPool()">清空我的数据池</button>
```

JS：

```javascript
function clearMyPool() {
    if (!confirm('确定清空当前账号的采集数据池吗？')) return;
    fetch('/api/clear-pool', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({username: currentUser})
    })
    .then(r => r.json())
    .then(data => {
        showMessage('saveMsg', data.message || (data.success ? '清空成功' : '清空失败'), data.success ? 'success' : 'error');
        if (data.success) loadFilters();
    })
    .catch(() => showMessage('saveMsg', '网络错误', 'error'));
}
```

- [ ] **Step 8: 运行覆盖测试**

Run:

```powershell
powershell -ExecutionPolicy Bypass -File .\tests\UserFilterWebPoolControlCoverageTest.ps1
```

Expected: PASS。

---

### Task 8: 补齐运行时 username 传递，确保采集/伪心跳请求和响应都按用户权限生效

**Files:**
- Modify: `PacketParser.h`
- Modify: `PacketCollector.cpp`
- Modify: `IOCPThreadPool.cpp`
- Modify: `新伪心跳.cpp`
- Test: `tests\WPEUserRuntimeCoverageTest.ps1`

- [ ] **Step 1: 新建失败测试**

创建 `tests/WPEUserRuntimeCoverageTest.ps1`。

```powershell
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$parser = Get-Content -Raw -Encoding UTF8 (Join-Path $root 'PacketParser.h')
$pc = Get-Content -Raw -Encoding UTF8 (Join-Path $root 'PacketCollector.cpp')
$iocp = Get-Content -Raw -Encoding UTF8 (Join-Path $root 'IOCPThreadPool.cpp')
$main = Get-Content -Raw -Encoding UTF8 (Join-Path $root '新伪心跳.cpp')

function Need($text, $needle, $message) {
    if ($text -notmatch [regex]::Escape($needle)) { throw $message }
}

Need $parser 'socksUsername' 'PacketInfo must carry socksUsername.'
Need $pc 'packetInfo.socksUsername = conn->authenticatedUser' 'PacketCollector must set PacketInfo socksUsername from connection.'
Need $iocp 'packetInfo.socksUsername = conn->authenticatedUser' 'IOCP must set PacketInfo socksUsername from connection.'
Need $main 'packet.socksUsername' 'Main WPE callbacks must pass packet.socksUsername.'
Need $main 'GetEffectiveUserFilters' 'Main WPE callbacks must get effective user filters.'
Need $main 'ApplyWPEFilters(workingData' 'Main WPE callbacks must still call ApplyWPEFilters.'

Write-Host 'WPE user runtime coverage is complete.'
```

- [ ] **Step 2: 运行测试确认失败**

Run:

```powershell
powershell -ExecutionPolicy Bypass -File .\tests\WPEUserRuntimeCoverageTest.ps1
```

Expected: 可能部分 PASS，失败点通常是 `新伪心跳.cpp` 仍传空 username。

- [ ] **Step 3: 确认 PacketInfo 有账号字段**

如果 `PacketParser.h` 尚无字段，添加：

```cpp
std::string socksUsername;
```

在构造函数中保持默认空字符串。

- [ ] **Step 4: PacketCollector 传统模式设置 username**

所有由连接生成的 `PacketInfo packetInfo` 后补：

```cpp
packetInfo.socksUsername = conn->authenticatedUser;
```

请求和响应都要补齐。

- [ ] **Step 5: IOCP 模式设置 username**

在 `IOCPThreadPool.cpp` 的客户端请求和服务端响应解析点补：

```cpp
packetInfo.socksUsername = conn->authenticatedUser;
```

- [ ] **Step 6: 新伪心跳.cpp 回调传用户有效滤镜**

把类似调用：

```cpp
ApplyWPEFilters(workingData, "", true, true, WPEFilter::FilterPriority::BeforeHeartbeat, "");
```

替换为：

```cpp
std::vector<int> userFilters;
const std::vector<int>* userEnabledFiltersPtr = nullptr;
if (g_userFilterManager && !packet.socksUsername.empty()) {
    std::set<int> filterSet = g_userFilterManager->GetEffectiveUserFilters(instanceId, packet.socksUsername);
    userFilters.assign(filterSet.begin(), filterSet.end());
    userEnabledFiltersPtr = &userFilters;
}
auto filterResult = ApplyWPEFilters(
    workingData,
    instanceId,
    true,
    true,
    WPEFilter::FilterPriority::BeforeHeartbeat,
    packet.socksUsername,
    userEnabledFiltersPtr);
```

对采集端和伪心跳端、替换前和替换后、请求和响应路径全部应用。

- [ ] **Step 7: 未启用用户滤镜模式时保持旧逻辑**

只有实例开启用户滤镜模式时才传 `userEnabledFiltersPtr`，否则传 `nullptr`：

```cpp
if (!IsUserFilterModeEnabledForInstance(instanceId)) {
    userEnabledFiltersPtr = nullptr;
}
```

如果没有现成函数，读取配置：

```cpp
const bool userModeEnabled = g_database &&
    g_database->GetConfigValue(InstanceManager::MakeInstanceConfigKey(instanceId, "enableUserFilterMode"), "0") == "1";
```

- [ ] **Step 8: 运行覆盖测试**

Run:

```powershell
powershell -ExecutionPolicy Bypass -File .\tests\WPEUserRuntimeCoverageTest.ps1
```

Expected: PASS。

---

### Task 9: 配置导入导出兼容滤镜组

**Files:**
- Modify: `ui_bridge.cpp`
- Test: `tests\WPEFilterGroupImportExportCoverageTest.ps1`

- [ ] **Step 1: 新建失败测试**

创建 `tests/WPEFilterGroupImportExportCoverageTest.ps1`。

```powershell
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bridge = Get-Content -Raw -Encoding UTF8 (Join-Path $root 'ui_bridge.cpp')

function Need($text, $needle, $message) {
    if ($text -notmatch [regex]::Escape($needle)) { throw $message }
}

Need $bridge '"wpeFilterGroups"' 'Instance export must include wpeFilterGroups.'
Need $bridge 'SaveWPEFilterGroup' 'Instance import must restore WPE filter groups.'
Need $bridge '"userFilterGroups"' 'Instance export must include userFilterGroups.'
Need $bridge 'UpdateUserFilterGroups' 'Instance import must restore user group bindings.'

Write-Host 'WPE filter group import/export coverage is complete.'
```

- [ ] **Step 2: 运行测试确认失败**

Run:

```powershell
powershell -ExecutionPolicy Bypass -File .\tests\WPEFilterGroupImportExportCoverageTest.ps1
```

Expected: FAIL。

- [ ] **Step 3: 导出实例时写入滤镜组**

在实例导出 JSON 中加入：

```cpp
if (g_database) {
    json groupsArray = json::array();
    for (const auto& group : g_database->LoadWPEFilterGroups(true)) {
        json gj;
        gj["id"] = group.id;
        gj["name"] = group.name;
        gj["description"] = group.description;
        gj["enabled"] = group.enabled;
        gj["items"] = json::array();
        for (const auto& item : group.items) {
            gj["items"].push_back({
                {"filterId", item.filterId},
                {"defaultEnabled", item.defaultEnabled}
            });
        }
        groupsArray.push_back(gj);
    }
    instConfig["wpeFilterGroups"] = groupsArray;
}
```

导出用户组绑定：

```cpp
json userFilterGroupsObj = json::object();
if (g_userFilterManager) {
    for (const std::string& username : users) {
        json arr = json::array();
        for (int groupId : g_userFilterManager->GetUserFilterGroups(instanceId, username)) {
            arr.push_back(groupId);
        }
        userFilterGroupsObj[username] = arr;
    }
}
instConfig["userFilterGroups"] = userFilterGroupsObj;
```

- [ ] **Step 4: 导入实例时恢复滤镜组**

导入时如果存在 `wpeFilterGroups`：

```cpp
std::map<int, int> oldToNewGroupId;
if (g_database && instJson.contains("wpeFilterGroups") && instJson["wpeFilterGroups"].is_array()) {
    for (const auto& groupJson : instJson["wpeFilterGroups"]) {
        WPEFilterGroupRecord group;
        const int oldId = groupJson.value("id", 0);
        group.name = groupJson.value("name", "");
        group.description = groupJson.value("description", "");
        group.enabled = groupJson.value("enabled", true);
        if (groupJson.contains("items") && groupJson["items"].is_array()) {
            for (const auto& itemJson : groupJson["items"]) {
                WPEFilterGroupItemRecord item;
                item.filterId = itemJson.value("filterId", 0);
                item.defaultEnabled = itemJson.value("defaultEnabled", false);
                if (item.filterId > 0) group.items.push_back(item);
            }
        }
        int newGroupId = g_database->SaveWPEFilterGroup(group);
        if (oldId > 0 && newGroupId > 0) oldToNewGroupId[oldId] = newGroupId;
    }
}
```

导入用户绑定时映射组 ID：

```cpp
if (g_userFilterManager && instJson.contains("userFilterGroups") && instJson["userFilterGroups"].is_object()) {
    for (auto it = instJson["userFilterGroups"].begin(); it != instJson["userFilterGroups"].end(); ++it) {
        std::vector<int> groupIds;
        for (const auto& idVal : it.value()) {
            int oldId = idVal.get<int>();
            auto mapIt = oldToNewGroupId.find(oldId);
            groupIds.push_back(mapIt == oldToNewGroupId.end() ? oldId : mapIt->second);
        }
        g_userFilterManager->UpdateUserFilterGroups(newId, it.key(), groupIds);
    }
}
```

- [ ] **Step 5: 运行覆盖测试**

Run:

```powershell
powershell -ExecutionPolicy Bypass -File .\tests\WPEFilterGroupImportExportCoverageTest.ps1
```

Expected: PASS。

---

### Task 10: 最终验证

**Files:**
- No code changes unless verification exposes a defect.

- [ ] **Step 1: 跑全部新增覆盖脚本**

Run:

```powershell
powershell -ExecutionPolicy Bypass -File .\tests\WPEFilterGroupDatabaseCoverageTest.ps1
powershell -ExecutionPolicy Bypass -File .\tests\WPEFilterGroupEffectiveStateCoverageTest.ps1
powershell -ExecutionPolicy Bypass -File .\tests\WPEFilterGroupAdminUiCoverageTest.ps1
powershell -ExecutionPolicy Bypass -File .\tests\WPEFilterWebDisplayNameCoverageTest.ps1
powershell -ExecutionPolicy Bypass -File .\tests\WPEFilterGroupCardBindingCoverageTest.ps1
powershell -ExecutionPolicy Bypass -File .\tests\UserFilterWebAuthorizationCoverageTest.ps1
powershell -ExecutionPolicy Bypass -File .\tests\UserFilterWebPoolControlCoverageTest.ps1
powershell -ExecutionPolicy Bypass -File .\tests\WPEUserRuntimeCoverageTest.ps1
powershell -ExecutionPolicy Bypass -File .\tests\WPEFilterGroupImportExportCoverageTest.ps1
```

Expected: 每个脚本输出 `... coverage is complete.`。

- [ ] **Step 2: Release x64 构建**

Run:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\amd64\MSBuild.exe' '.\新伪心跳.sln' /p:Configuration=Release /p:Platform=x64 /m
```

Expected:

```text
已成功生成。
0 个警告
0 个错误
```

- [ ] **Step 3: 手工联调场景**

1. 新建两个 WPE 滤镜：
   - `C1`：只对采集监听端口生效。
   - `H1`：只对伪心跳监听端口生效。
2. 新建滤镜组 `基础组`：
   - 授权 `C1`，默认开启。
   - 授权 `H1`，默认关闭。
3. 给账号 `user_a` 绑定 `基础组`。
4. 在“Web用户态设置”中把 `C1` 的用户 Web 显示名称设置为 `采集过滤A`。
5. 到“API地址”中启动用户滤镜 HTTP 服务。
6. 用 `user_a` 登录 Web 控制台。
7. 确认页面显示：
   - 采集监听端口滤镜分类中有 `C1`。
   - 伪心跳监听端口滤镜分类中有 `H1`。
   - `C1` 在用户 Web 中显示为 `采集过滤A`。
   - `C1` 默认勾选。
   - `H1` 默认不勾选。
   - 显示当前 GameID 或“未绑定”。
   - 显示当前采集数据池条数。
8. 勾选 `H1` 并保存。
9. 发送伪心跳链路数据，确认 `H1` 对该用户生效。
10. 使用无权限账号登录，确认看不到 `C1/H1`。
11. 点击“清空我的数据池”，确认只清 `user_a` 的数据，其他账号数据池不变。
12. 给 `基础组` 追加新滤镜 `C2`，默认开启。
13. 不让 `user_a` 重新配置，刷新 Web 控制台，确认 `C2` 出现且按默认状态生效。

---

## 兼容性要求

- 未启用用户滤镜模式的实例继续走全局 WPE 逻辑。
- 已有 `user_filter_config` 和实例默认滤镜配置继续兼容；账号未绑定任何滤镜组时允许旧逻辑生效。
- 一旦账号绑定了滤镜组，用户 Web 和运行时必须严格使用组授权，不允许用户启用组外滤镜。
- 清空数据池必须按 `username` 调用 `ClearByUsername`，不能调用全局 `Clear`。
- 用户 Web 控制台不接收客户端传来的权限结果，只接收用户名并在服务端重新计算授权。
- 用户 Web 显示名称仅影响用户控制台展示，不改变滤镜内部 `name`、日志、管理端列表和执行统计归属。

## 风险点

- 当前项目里部分源码存在编码损坏注释，新增代码尽量使用 ASCII 注释或中文字符串保持 UTF-8 编译设置，避免 MSVC 默认代码页误读。
- 用户 Web 服务当前手写 JSON 解析，新增接口字段应尽量复用小型 `ExtractJsonString`、`ExtractJsonIntArray`、`JsonEscape` 辅助函数，避免散落字符串解析。
- `LoadInstanceConfig("")` 不能代表所有实例；实现时应在实例启动、用户滤镜配置页保存、滤镜组保存后对实际实例 ID 重载缓存。
- 卡密绑定如果最终由 ABProtect 云端管理，本地 `card_wpe_filter_groups` 应作为兼容缓存；云端返回字段接入后应落到同一张用户组绑定表。

## 完成定义

- 管理端能创建/编辑/删除 WPE 滤镜组，配置授权滤镜和默认开启值。
- 管理端实例配置中存在“Web用户态设置”和“API地址”两个子菜单；用户态权限 UI 位于“Web用户态设置”，HTTP 端口和访问 URL 位于“API地址”。
- 卡密或账号能绑定滤镜组。
- 已绑定组的老用户在组新增滤镜后自动获得权限。
- 管理端能给每个 WPE 滤镜设置用户 Web 显示名称；用户 Web 显示名称为空时回退原滤镜名称。
- 用户 Web 控制台只显示授权滤镜，并按采集监听端口/伪心跳监听端口分类。
- 用户未保存开关时使用组默认值；保存后使用用户自定义开关和授权交集。
- Web 控制台显示当前 GameID 和采集数据池条数。
- Web 控制台可清空当前登录 SOCKS 账号的内存采集数据，不影响其他账号。
- Release x64 构建通过，新增覆盖脚本全部通过。
