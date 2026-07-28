# 云计算性能优化详细方案

## 优化目标

减少加密后的卡顿问题，将 CPU 占用从 0.8%-3.8% 降低到 0.2%-1.0%，消除周期性微卡顿。

---

## 方案 1：降低 SProtect 自检频率

### 当前状态
- **频率：** 1.5 秒一次
- **CPU 占用：** 0.7%-3.3%
- **用户感知：** 每 1.5 秒有一次 10-50ms 的微卡顿

### 优化方案

#### 方案 1A：固定频率调整（简单）
**调整为 5 秒一次**

**代码位置：** `新伪心跳.cpp:7411-7414`

**修改前：**
```cpp
// 后台线程休眠 1500ms，不影响主线程渲染
for (int i = 0; i < 15 && g_sprotectCheckRunning.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}
```

**修改后：**
```cpp
// 后台线程休眠 5000ms（5秒），降低自检频率
for (int i = 0; i < 50 && g_sprotectCheckRunning.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}
```

**效果：**
- CPU 占用：0.7%-3.3% → 0.2%-1.0%（减少 70%）
- 卡顿频率：每 1.5 秒 → 每 5 秒
- 安全性：仍然保持较高的检测频率

---

#### 方案 1B：动态频率调整（推荐）
**根据用户活动状态动态调整**

**实现逻辑：**
```cpp
// 添加全局变量
static std::atomic<int> g_sprotectCheckIntervalMs(5000);  // 默认 5 秒
static std::atomic<uint64_t> g_lastUserActivityTime(0);   // 最后用户活动时间

// 用户活动检测（在主线程的消息循环中调用）
void UpdateUserActivity() {
    g_lastUserActivityTime.store(GetTickCount64(), std::memory_order_relaxed);
}

// 自检线程中动态调整
g_sprotectCheckThread = std::thread([hwnd]() {
    int wmTick = 0;
    while (g_sprotectCheckRunning.load()) {
        std::string reason;
        if (!SProtectSelfCheck::Tick(++wmTick, &reason)) {
            // 自检失败处理
            g_sprotectFailed.store(true);
            PostMessage(hwnd, WM_SPROTECT_FAILED, 0, 0);
            break;
        }

        // 动态调整检测间隔
        uint64_t now = GetTickCount64();
        uint64_t lastActivity = g_lastUserActivityTime.load(std::memory_order_relaxed);
        int intervalMs;

        if (now - lastActivity < 10000) {
            // 用户活跃（10秒内有操作）：5秒检测一次
            intervalMs = 5000;
        } else if (now - lastActivity < 60000) {
            // 用户空闲（1分钟内）：10秒检测一次
            intervalMs = 10000;
        } else {
            // 用户长时间空闲：30秒检测一次
            intervalMs = 30000;
        }

        // 休眠
        for (int i = 0; i < intervalMs / 100 && g_sprotectCheckRunning.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
});
```

**效果：**
- 活跃时：5 秒一次（保持安全性）
- 空闲时：10-30 秒一次（大幅降低 CPU 占用）
- 平均 CPU 占用：0.2%-0.8%（减少 75%-80%）

---

#### 方案 1C：智能跳过机制（高级）
**在特定场景下跳过自检**

**实现逻辑：**
```cpp
// 跳过条件
bool ShouldSkipCheck() {
    // 1. 如果正在进行大量数据传输，跳过
    if (g_collector && g_collector->GetCurrentConnections() > 100) {
        return true;
    }

    // 2. 如果 CPU 占用过高，跳过
    static uint64_t lastCpuCheck = 0;
    uint64_t now = GetTickCount64();
    if (now - lastCpuCheck > 5000) {
        lastCpuCheck = now;
        // 检查 CPU 占用（简化版）
        // 实际实现需要使用 GetProcessTimes
    }

    return false;
}

// 在自检线程中使用
while (g_sprotectCheckRunning.load()) {
    if (!ShouldSkipCheck()) {
        std::string reason;
        if (!SProtectSelfCheck::Tick(++wmTick, &reason)) {
            // 自检失败处理
        }
    }
    // 休眠...
}
```

**效果：**
- 在高负载时自动降低检测频率
- 避免"卡上加卡"的情况

---

## 方案 2：优化云心跳策略

### 当前状态
- **频率：** 固定 30 秒一次
- **耗时：** 20-100ms/次
- **问题：** 无论用户是否活跃，都以相同频率心跳

### 优化方案

#### 方案 2A：动态心跳间隔（推荐）

**实现逻辑：**
```cpp
void CloudAuthHeartbeatThread() {
    Logger::InfoCat(LOG_CAT_LOGIN_AUTH, "[云心跳] 线程已启动");

    int consecutiveFailures = 0;
    const int maxRetries = 3;

    while (g_cloudHeartbeatRunning) {
        std::string err;
        int errCode = 0;
        if (!CloudIntegration::BeatOnce(err, &errCode)) {
            consecutiveFailures++;
            // 失败处理...
        } else {
            consecutiveFailures = 0;
        }

        // 🔥 动态调整心跳间隔
        int intervalSeconds;
        uint64_t now = GetTickCount64();
        uint64_t lastActivity = g_lastUserActivityTime.load(std::memory_order_relaxed);

        if (consecutiveFailures > 0) {
            // 如果心跳失败，缩短间隔快速重试
            intervalSeconds = 10;
        } else if (now - lastActivity < 30000) {
            // 用户活跃（30秒内有操作）：30秒心跳
            intervalSeconds = 30;
        } else if (now - lastActivity < 300000) {
            // 用户空闲（5分钟内）：60秒心跳
            intervalSeconds = 60;
        } else {
            // 用户长时间空闲：120秒心跳
            intervalSeconds = 120;
        }

        // 等待
        for (int i = 0; i < intervalSeconds && g_cloudHeartbeatRunning; i++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}
```

**效果：**
- 活跃时：30 秒一次（保持会话活跃）
- 空闲时：60-120 秒一次（减少网络和 CPU 开销）
- 失败时：10 秒一次（快速恢复）
- 平均减少 50%-75% 的心跳次数

---

#### 方案 2B：心跳合并机制

**实现逻辑：**
```cpp
// 将心跳与其他云操作合并
static std::atomic<uint64_t> g_lastCloudRequestTime(0);

bool CloudIntegration::BeatOnce(std::string& outError, int* outErrorCode) {
    // 检查是否最近有其他云请求
    uint64_t now = GetTickCount64();
    uint64_t lastRequest = g_lastCloudRequestTime.load(std::memory_order_relaxed);

    if (now - lastRequest < 15000) {
        // 15秒内有其他云请求，跳过本次心跳
        return true;
    }

    // 执行心跳
    // ...

    // 更新最后请求时间
    g_lastCloudRequestTime.store(now, std::memory_order_relaxed);
    return true;
}

// 在所有云请求中更新时间戳
bool CloudIntegration::CloudRequestJson(...) {
    g_lastCloudRequestTime.store(GetTickCount64(), std::memory_order_relaxed);
    // 执行云请求...
}
```

**效果：**
- 避免在短时间内多次触发云计算校验
- 减少 30%-50% 的心跳次数

---

#### 方案 2C：心跳降级策略

**实现逻辑：**
```cpp
// 添加心跳降级机制
static std::atomic<int> g_heartbeatLevel(0);  // 0=正常, 1=降级, 2=最小

void CloudAuthHeartbeatThread() {
    while (g_cloudHeartbeatRunning) {
        int level = g_heartbeatLevel.load(std::memory_order_relaxed);
        int intervalSeconds;

        switch (level) {
            case 0:  // 正常模式
                intervalSeconds = 30;
                break;
            case 1:  // 降级模式（用户设置或自动检测到高负载）
                intervalSeconds = 60;
                break;
            case 2:  // 最小模式（仅保持连接）
                intervalSeconds = 120;
                break;
            default:
                intervalSeconds = 30;
        }

        // 执行心跳...
        // 等待...
    }
}

// 提供用户配置接口
void SetHeartbeatLevel(int level) {
    g_heartbeatLevel.store(level, std::memory_order_relaxed);
}
```

**效果：**
- 用户可以根据需要调整心跳频率
- 在低配置机器上可以选择降级模式

---

## 方案 3：合并云同步请求

### 当前状态
- **WPE 云同步：** 每 5 分钟一次
- **SOCKS 云同步：** 每 5 分钟一次
- **问题：** 两个独立线程，可能同时触发，导致双倍开销

### 优化方案

#### 方案 3A：统一云同步管理器（推荐）

**实现逻辑：**

**新建文件：** `UnifiedCloudSync.h`
```cpp
#pragma once

#include <string>
#include <functional>
#include <vector>

namespace UnifiedCloudSync {

// 同步任务类型
enum class SyncTaskType {
    WPE_FILTERS,
    SOCKS_INSTANCES,
    // 未来可扩展其他类型
};

// 同步任务
struct SyncTask {
    SyncTaskType type;
    std::function<bool(std::string&)> syncFunc;  // 同步函数
    std::string name;
};

// 启动统一云同步服务
void StartCloudSync();

// 停止统一云同步服务
void StopCloudSync();

// 注册同步任务
void RegisterSyncTask(const SyncTask& task);

// 立即执行同步
bool SyncNow(std::string& outError);

// 获取同步状态
bool IsRunning();

} // namespace UnifiedCloudSync
```

**新建文件：** `UnifiedCloudSync.cpp`
```cpp
#include "UnifiedCloudSync.h"
#include "CloudIntegration.h"
#include "Logger.h"
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <vector>

namespace {
    std::thread g_syncThread;
    std::atomic<bool> g_syncRunning(false);
    std::atomic<bool> g_stopRequested(false);
    std::atomic<int> g_syncIntervalSeconds(300);  // 5分钟

    std::mutex g_tasksMutex;
    std::vector<UnifiedCloudSync::SyncTask> g_syncTasks;

    void SyncThreadFunc() {
        while (!g_stopRequested.load()) {
            // 等待同步间隔
            int intervalSeconds = g_syncIntervalSeconds.load();
            for (int i = 0; i < intervalSeconds && !g_stopRequested.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }

            if (g_stopRequested.load()) {
                break;
            }

            // 🔥 批量执行所有同步任务
            std::vector<UnifiedCloudSync::SyncTask> tasks;
            {
                std::lock_guard<std::mutex> lock(g_tasksMutex);
                tasks = g_syncTasks;
            }

            for (const auto& task : tasks) {
                std::string error;
                if (!task.syncFunc(error)) {
                    Logger::Warning("[统一云同步] " + task.name + " 失败: " + error);
                }
            }
        }
    }
}

namespace UnifiedCloudSync {

void StartCloudSync() {
    if (g_syncRunning.load()) {
        return;
    }

    g_stopRequested.store(false);
    g_syncRunning.store(true);
    g_syncThread = std::thread(SyncThreadFunc);
}

void StopCloudSync() {
    if (!g_syncRunning.load()) {
        return;
    }

    g_stopRequested.store(true);
    if (g_syncThread.joinable()) {
        g_syncThread.join();
    }
    g_syncRunning.store(false);
}

void RegisterSyncTask(const SyncTask& task) {
    std::lock_guard<std::mutex> lock(g_tasksMutex);
    g_syncTasks.push_back(task);
}

bool SyncNow(std::string& outError) {
    if (!CloudIntegration::IsLoggedIn()) {
        outError = "未登录云计算账号";
        return false;
    }

    std::vector<SyncTask> tasks;
    {
        std::lock_guard<std::mutex> lock(g_tasksMutex);
        tasks = g_syncTasks;
    }

    bool allSuccess = true;
    for (const auto& task : tasks) {
        std::string error;
        if (!task.syncFunc(error)) {
            outError += task.name + " 失败: " + error + "; ";
            allSuccess = false;
        }
    }

    return allSuccess;
}

bool IsRunning() {
    return g_syncRunning.load();
}

} // namespace UnifiedCloudSync
```

**修改 WPECloudSync 和 SocksCloudSync：**
```cpp
// 在 WPECloudSync 中注册任务
void WPECloudSync::RegisterToUnifiedSync() {
    UnifiedCloudSync::SyncTask task;
    task.type = UnifiedCloudSync::SyncTaskType::WPE_FILTERS;
    task.name = "WPE滤镜同步";
    task.syncFunc = [](std::string& error) {
        return WPECloudSync::SyncNow(error);
    };
    UnifiedCloudSync::RegisterSyncTask(task);
}

// 在 SocksCloudSync 中注册任务
void SocksCloudSync::RegisterToUnifiedSync() {
    UnifiedCloudSync::SyncTask task;
    task.type = UnifiedCloudSync::SyncTaskType::SOCKS_INSTANCES;
    task.name = "SOCKS实例同步";
    task.syncFunc = [](std::string& error) {
        return SocksCloudSync::SyncNow(error);
    };
    UnifiedCloudSync::RegisterSyncTask(task);
}
```

**在主程序中使用：**
```cpp
// 登录成功后
if (CloudIntegration::Login(...)) {
    // 注册同步任务
    WPECloudSync::RegisterToUnifiedSync();
    SocksCloudSync::RegisterToUnifiedSync();

    // 启动统一云同步
    UnifiedCloudSync::StartCloudSync();
}

// 退出时
UnifiedCloudSync::StopCloudSync();
```

**效果：**
- 所有云同步在同一时间点执行
- 减少 50% 的云计算调用次数
- 更容易管理和扩展

---

#### 方案 3B：批量云请求接口

**实现逻辑：**
```cpp
// 在 CloudIntegration 中添加批量请求接口
namespace CloudIntegration {

struct BatchRequest {
    int cloudId;
    std::string requestJson;
};

struct BatchResponse {
    bool success;
    std::string responseJson;
    std::string error;
};

// 批量云请求（一次网络调用，多个业务请求）
bool CloudRequestBatch(
    const std::vector<BatchRequest>& requests,
    std::vector<BatchResponse>& outResponses,
    std::string& outError);

} // namespace CloudIntegration
```

**服务端支持：**
```json
// 请求格式
{
    "batch": true,
    "requests": [
        {"cloudId": 1010, "data": {...}},
        {"cloudId": 1011, "data": {...}}
    ]
}

// 响应格式
{
    "batch": true,
    "responses": [
        {"ok": true, "data": {...}},
        {"ok": true, "data": {...}}
    ]
}
```

**效果：**
- 一次网络调用完成多个业务请求
- 减少网络开销和云计算校验次数

---

## 方案 4：使用缓存机制

### 当前状态
- 每次云请求都触发完整性校验
- 没有本地缓存，重复请求相同数据

### 优化方案

#### 方案 4A：云请求结果缓存（推荐）

**实现逻辑：**
```cpp
// 缓存项
struct CacheEntry {
    std::string responseJson;
    uint64_t timestamp;
    int ttl;  // 生存时间（秒）
};

// 全局缓存
static std::mutex g_cacheMutex;
static std::unordered_map<std::string, CacheEntry> g_cloudCache;

// 生成缓存键
std::string MakeCacheKey(int cloudId, const std::string& requestJson) {
    return std::to_string(cloudId) + ":" + requestJson;
}

// 带缓存的云请求
bool CloudRequestJsonCached(
    int cloudId,
    const std::string& requestJson,
    std::string& outResponseJson,
    std::string& outError,
    int cacheTtl = 60)  // 默认缓存60秒
{
    std::string cacheKey = MakeCacheKey(cloudId, requestJson);
    uint64_t now = GetTickCount64() / 1000;

    // 检查缓存
    {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        auto it = g_cloudCache.find(cacheKey);
        if (it != g_cloudCache.end()) {
            if (now - it->second.timestamp < it->second.ttl) {
                // 缓存命中
                outResponseJson = it->second.responseJson;
                return true;
            } else {
                // 缓存过期，删除
                g_cloudCache.erase(it);
            }
        }
    }

    // 缓存未命中，执行云请求
    if (!CloudRequestJson(cloudId, requestJson, outResponseJson, outError)) {
        return false;
    }

    // 存入缓存
    {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        CacheEntry entry;
        entry.responseJson = outResponseJson;
        entry.timestamp = now;
        entry.ttl = cacheTtl;
        g_cloudCache[cacheKey] = entry;
    }

    return true;
}

// 清理过期缓存（定期调用）
void CleanExpiredCache() {
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    uint64_t now = GetTickCount64() / 1000;

    for (auto it = g_cloudCache.begin(); it != g_cloudCache.end();) {
        if (now - it->second.timestamp >= it->second.ttl) {
            it = g_cloudCache.erase(it);
        } else {
            ++it;
        }
    }
}
```

**使用示例：**
```cpp
// 获取 WPE 滤镜配置（缓存 5 分钟）
std::string response, error;
if (CloudRequestJsonCached(1003, "{}", response, error, 300)) {
    // 使用缓存的配置
}
```

**效果：**
- 减少 50%-80% 的重复云请求
- 大幅降低网络延迟和 CPU 占用

---

#### 方案 4B：Checkpoint 本地缓存

**实现逻辑：**
```cpp
// Checkpoint 缓存
struct CheckpointCache {
    bool allowed;
    uint64_t timestamp;
    int ttl;
};

static std::mutex g_checkpointCacheMutex;
static std::unordered_map<int, CheckpointCache> g_checkpointCache;

bool CheckpointCached(int checkpointCloudId, std::string& outError) {
    uint64_t now = GetTickCount64() / 1000;

    // 检查缓存
    {
        std::lock_guard<std::mutex> lock(g_checkpointCacheMutex);
        auto it = g_checkpointCache.find(checkpointCloudId);
        if (it != g_checkpointCache.end()) {
            if (now - it->second.timestamp < it->second.ttl) {
                // 缓存命中
                if (!it->second.allowed) {
                    outError = "Checkpoint 验证失败（缓存）";
                }
                return it->second.allowed;
            } else {
                // 缓存过期
                g_checkpointCache.erase(it);
            }
        }
    }

    // 缓存未命中，执行 Checkpoint
    bool allowed = Checkpoint(checkpointCloudId, "{}", outError);

    // 存入缓存
    {
        std::lock_guard<std::mutex> lock(g_checkpointCacheMutex);
        CheckpointCache cache;
        cache.allowed = allowed;
        cache.timestamp = now;
        cache.ttl = 60;  // 缓存 60 秒
        g_checkpointCache[checkpointCloudId] = cache;
    }

    return allowed;
}
```

**效果：**
- Checkpoint 验证结果缓存 60 秒
- 避免频繁触发云计算校验

---

#### 方案 4C：预加载机制

**实现逻辑：**
```cpp
// 在登录成功后预加载常用配置
void PreloadCloudConfigs() {
    std::thread([]{
        std::this_thread::sleep_for(std::chrono::seconds(2));  // 延迟2秒，避免影响登录流程

        std::string response, error;

        // 预加载 WPE 滤镜配置
        CloudRequestJsonCached(1003, "{}", response, error, 300);

        // 预加载 SOCKS 实例配置
        CloudRequestJsonCached(1012, "{}", response, error, 300);

        // 预加载其他常用配置...
    }).detach();
}

// 在登录成功后调用
if (CloudIntegration::Login(...)) {
    PreloadCloudConfigs();
}
```

**效果：**
- 用户实际使用时直接从缓存读取
- 消除首次使用时的延迟

---

## 综合优化效果预估

### 优化前
- **SProtect 自检：** 0.7%-3.3% CPU（每 1.5 秒）
- **云心跳：** 0.07%-0.33% CPU（每 30 秒）
- **云同步：** 0.04%-0.14% CPU（每 5 分钟）
- **总计：** 0.8%-3.8% CPU
- **卡顿：** 每 1.5 秒一次微卡顿

### 优化后
- **SProtect 自检：** 0.2%-1.0% CPU（每 5-30 秒动态）
- **云心跳：** 0.03%-0.15% CPU（每 30-120 秒动态）
- **云同步：** 0.02%-0.07% CPU（合并请求）
- **总计：** 0.25%-1.2% CPU
- **卡顿：** 每 5-30 秒一次微卡顿（用户几乎无感知）

### 性能提升
- **CPU 占用降低：** 68%-75%
- **卡顿频率降低：** 70%-95%
- **网络请求减少：** 50%-80%

---

## 实施建议

### 阶段 1：快速见效（1-2 小时）
1. 实施方案 1A：将 SProtect 自检频率从 1.5 秒调整为 5 秒
2. 实施方案 2A：云心跳动态间隔调整

### 阶段 2：深度优化（3-5 小时）
3. 实施方案 3A：统一云同步管理器
4. 实施方案 4A：云请求结果缓存

### 阶段 3：高级优化（可选）
5. 实施方案 1B：SProtect 自检动态频率
6. 实施方案 4B：Checkpoint 本地缓存
7. 实施方案 4C：预加载机制

---

## 风险评估

### 低风险
- 方案 1A：固定频率调整（5 秒仍然足够安全）
- 方案 4A：云请求缓存（不影响核心逻辑）

### 中风险
- 方案 2A：动态心跳间隔（需要测试会话超时）
- 方案 3A：统一云同步（需要重构现有代码）

### 高风险
- 方案 1B：动态自检频率（可能降低安全性）
- 方案 1C：智能跳过机制（可能被绕过）

---

## 测试建议

### 性能测试
1. 使用 Process Explorer 监控 CPU 占用
2. 记录优化前后的平均 CPU 占用
3. 测试不同场景（空闲、活跃、高负载）

### 功能测试
1. 验证云心跳是否正常
2. 验证云同步是否正常
3. 验证 SProtect 自检是否正常触发

### 安全测试
1. 测试调试器检测是否仍然有效
2. 测试完整性校验是否仍然有效
3. 测试会话超时是否正常处理

---

## 方案文档日期

2026-03-09
