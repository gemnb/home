# 死锁问题修复说明

## 问题描述

项目启动时出现以下错误：
```
[WARNING] 绑定账号库时发生锁冲突: resource deadlock would occur
[ERROR] [UI Bridge] 处理消息失败: resource deadlock would occur
```

## 根本原因

**死锁场景（嵌套锁导致的死锁）：**

1. `InstanceManager::StartInstance()` 在第 2035 行获取了 `InstanceManager::m_mutex` 锁
2. 在持有锁的情况下，第 2106 行调用 `socksForwardIt->second->Start()`
3. `SocksForwardInstance::Start()` 调用 `ApplyConfigToCollector()`
4. `ApplyConfigToCollector()` 在第 3467 行调用 `InstanceManager::GetInstance().GetSocks5PoolInstance()`
5. `GetSocks5PoolInstance()` 在第 3233 行尝试再次获取 `InstanceManager::m_mutex` 锁
6. **同一线���尝试重复获取同一个非递归锁 → 死锁！**

**死锁类型：** 同一线程重复获取非递归互斥锁（Self-Deadlock / Recursive Lock Attempt）

## 修复方案

### 修改 1：`InstanceManager::StartInstance()` - 第 2026-2140 行

**核心思路：** 在锁内查找实例指针，释放锁后再调用实例的 `Start()` 方法

**修改前：**
```cpp
bool InstanceManager::StartInstance(const std::string& instanceId) {
    std::lock_guard<std::mutex> lock(m_mutex);  // 持有锁

    auto socksForwardIt = m_socksForwards.find(instanceId);
    if (socksForwardIt != m_socksForwards.end()) {
        return socksForwardIt->second->Start();  // ❌ 在持有锁时调用，导致嵌套锁
    }
    // ...
}
```

**修改后：**
```cpp
bool InstanceManager::StartInstance(const std::string& instanceId) {
    // 查找实例指针
    SocksForwardInstance* socksForwardInstance = nullptr;
    // ... 其他实例类型 ...

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto socksForwardIt = m_socksForwards.find(instanceId);
        if (socksForwardIt != m_socksForwards.end()) {
            socksForwardInstance = socksForwardIt->second.get();
        }
        // ... 查找其他实例 ...
    } // ✅ 释放 InstanceManager::m_mutex

    // ✅ 在锁外调用 Start()
    if (socksForwardInstance) {
        return socksForwardInstance->Start();
    }
    // ...
}
```

### 修改 2：`InstanceManager::StopInstance()` - 第 2142-2220 行

**同样的问题和修复方案**，在锁外调用实例的 `Stop()` 方法。

### 修改 3：`SocksForwardInstance::Start()` - 第 3645-3852 行

**核心思路：** 将 `ApplyConfigToCollector()` 调用移到锁外（这是第一次修复尝试，但不够彻底）

## 关键改进点

1. **避免嵌套锁**：`InstanceManager` 的方法在调用实例方法前释放全局锁
2. **缩小锁的持有范围**：只在查找实例时持有锁，实际操作在锁外进行
3. **保持线程安全**：使用裸指针在锁外调用是安全的，因为实例由 `unique_ptr` 管理，不会在运行时被删除

## 修改文件

- `InstanceManager.cpp`
  - `InstanceManager::StartInstance()`: 第 2026-2140 行
  - `InstanceManager::StopInstance()`: 第 2142-2220 行
  - `SocksForwardInstance::Start()`: 第 3645-3852 行

## 验证方法

重新编译并运行程序，检查日志中是否还有以下错误：
- `resource deadlock would occur`
- `绑定账号库时发生锁冲突`
- `[UI Bridge] 处理消息失败`

如果这些错误消失，说明死锁问题已解决。

## 死锁调用链分析

**修复前：**
```
InstanceManager::StartInstance()
  └─ 持有 InstanceManager::m_mutex
      └─ SocksForwardInstance::Start()
          └─ ApplyConfigToCollector()
              └─ InstanceManager::GetSocks5PoolInstance()
                  └─ 尝试获取 InstanceManager::m_mutex  ❌ 死锁！
```

**修复后：**
```
InstanceManager::StartInstance()
  └─ { 持有 InstanceManager::m_mutex
       └─ 查找实例指针
     } 释放锁
  └─ SocksForwardInstance::Start()  ✅ 在锁外调用
      └─ ApplyConfigToCollector()
          └─ InstanceManager::GetSocks5PoolInstance()
              └─ 获取 InstanceManager::m_mutex  ✅ 成功！
```

## 注意事项

1. **实例生命周期**：实例由 `unique_ptr` 管理，在 `InstanceManager` 的生命周期内不会被删除，因此在锁外使用裸指针是安全的
2. **异常安全性**：已添加 try-catch 块确保异常情况下状态正确更新
3. **其他潜在死锁点**：如果还有其他地方在持有 `InstanceManager::m_mutex` 时调用实例方法，需要类似修复

## 相关代码位置

- `InstanceManager::StartInstance()`: InstanceManager.cpp:2026
- `InstanceManager::StopInstance()`: InstanceManager.cpp:2142
- `InstanceManager::GetSocks5PoolInstance()`: InstanceManager.cpp:3232
- `SocksForwardInstance::Start()`: InstanceManager.cpp:3645
- `SocksForwardInstance::ApplyConfigToCollector()`: InstanceManager.cpp:3426
- UI Bridge 错误处理: ui_bridge.cpp:3123
