# CCProxy API 自启动问题修复说明

## 问题描述

SOCKS5 账号库的 CCProxy API 兼容服务在软件关闭前是开启的，但重启软件后不会自动启动。

## 问题根源

### 问题链条

1. 软件关闭时，`Socks5PoolInstance` 对象被销毁
2. 析构函数调用 `StopApiServer()`
3. `StopApiServer()` 将 `m_apiEnabled` 设置为 `false`
4. `StopApiServer()` 调用 `SaveToDatabase()` 保存状态到数据库
5. 数据库中 `api_enabled` 被设置为 `0`
6. 下次启动时，`LoadFromDatabase()` 读取到 `api_enabled = 0`
7. `AutoStartApiIfEnabled()` 检查失败，不会自动启动

### 核心问题

**析构函数不应该修改用户配置！**

析构函数调用 `StopApiServer()` 是为了释放资源，但不应该将"API 启用状态"设置为 `false`。这导致用户的配置被意外修改。

## 修复方案

### 修改 1：`InstanceManager.h` 第 667 行

**修改前：**
```cpp
void StopApiServer();
```

**修改后：**
```cpp
void StopApiServer(bool updateEnabledState = true);  // updateEnabledState: 是否更新启用状态（析构时传false）
```

### 修改 2：`InstanceManager.cpp` 第 3142-3153 行

**修改前：**
```cpp
void Socks5PoolInstance::StopApiServer() {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_apiServer) {
        m_apiServer->Stop();
        delete m_apiServer;
        m_apiServer = nullptr;
        m_apiEnabled = false;  // ❌ 总是设置为 false
        SaveToDatabase();
        Logger::Info("[SOCKS5Pool " + m_id + "] API服务已停止");
    }
}
```

**修改后：**
```cpp
void Socks5PoolInstance::StopApiServer(bool updateEnabledState) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_apiServer) {
        m_apiServer->Stop();
        delete m_apiServer;
        m_apiServer = nullptr;

        // 🔥 只有在用户手动停止时才更新启用状态
        // 析构函数调用时不更新，保持原有的启用状态
        if (updateEnabledState) {
            m_apiEnabled = false;  // 标记为禁用
            SaveToDatabase();
        }

        Logger::Info("[SOCKS5Pool " + m_id + "] API服务已停止");
    }
}
```

### 修改 3：`InstanceManager.cpp` 第 2669-2675 行

**修改前：**
```cpp
Socks5PoolInstance::~Socks5PoolInstance() {
    // 停止API服务
    StopApiServer();  // ❌ 会修改 m_apiEnabled

    // 保存账号到数据库
    SaveToDatabase();
}
```

**修改后：**
```cpp
Socks5PoolInstance::~Socks5PoolInstance() {
    // 停止API服务（但不更新启用状态，保持原有配置）
    StopApiServer(false);  // ✅ 传递 false，不修改 m_apiEnabled

    // 保存账号到数据库
    SaveToDatabase();
}
```

## 修复效果

### 修复前的行为

1. 用户启动 API 服务 → `api_enabled = 1` 保存到数据库
2. 软件正常关闭 → 析构函数调用 `StopApiServer()` → `api_enabled = 0` 保存到数据库 ❌
3. 重启软件 → 读取 `api_enabled = 0` → API 不会自动启动 ❌

### 修复后的行为

1. 用户启动 API 服务 → `api_enabled = 1` 保存到数据库
2. 软件正常关闭 → 析构函数调用 `StopApiServer(false)` → `api_enabled` 保持为 `1` ✅
3. 重启软件 → 读取 `api_enabled = 1` → `AutoStartApiIfEnabled()` 自动启动 API ✅

### 用户手动停止的行为（不受影响）

1. 用户手动点击"停止 API"按钮 → 调用 `StopApiServer(true)` → `api_enabled = 0` 保存
2. 重启软件 → 读取 `api_enabled = 0` → API 不会自动启动 ✅（符合预期）

## 设计原则

**析构函数应该只负责资源清理，不应该修改用户配置。**

- ✅ 释放内存、关闭连接、停止线程 → 析构函数的职责
- ❌ 修改用户设置、更新配置状态 → 不应该在析构函数中做

## 验证方法

1. 启动软件
2. 进入 SOCKS5 账号库配置
3. 启动 CCProxy API 服务
4. 关闭软件
5. 重新启动软件
6. 检查 API 服务是否自动启动 ✅

## 相关代码位置

- `InstanceManager.h`: 第 667 行
- `InstanceManager.cpp`:
  - `StopApiServer()`: 第 3142-3159 行
  - `~Socks5PoolInstance()`: 第 2669-2675 行
  - `AutoStartApiIfEnabled()`: 第 3168-3174 行
  - `LoadFromDatabase()`: 第 2982-3088 行

## 修复日期

2026-03-09
