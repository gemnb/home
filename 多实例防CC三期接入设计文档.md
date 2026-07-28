# 多实例防CC三期接入设计文档

## 1. 文档目的

本文档用于在当前项目中落地一套适用于“多实例 Socks 转发”的防 CC 体系。

目标不是简单把参考项目 `新防cc` 的实现搬过来，而是结合当前项目的真实架构，设计一套：

1. 支持多 `SocksForwardInstance` 独立运行的实例级防护。
2. 同时具备跨实例共享的全局封禁、全局防火墙、全局 IP 信誉与全局攻击态。
3. 明确区分白名单、黑名单、连接数、压力判断、认证优先队列的作用域。
4. 能逐期接入，不破坏当前 `PacketCollector + IOCP + InstanceManager` 的现有能力。

本文档只输出设计与接入方案，不在本轮继续实施剩余大改动。

---

## 2. 当前项目的真实现状

### 2.1 实例结构

当前项目中：

1. 每个 `SocksForwardInstance` 内部持有一个独立的 `PacketCollector`。
2. 每个 `PacketCollector` 内部持有一个独立的 `AntiCC`。
3. `SocksForwardInstance::CachedConfig` 目前不包含防 CC 配置。
4. `SocksForwardInstance::ApplyConfigToCollector()` 当前不会把防 CC 配置应用到实例。
5. `ui_bridge.cpp` 当前仍通过全局 `g_antiCC` 读写防 CC 配置，而不是按 `instanceId` 操作实例。

也就是说：

1. 数据面上，防 CC 已经“天然每实例一份”。
2. 控制面上，防 CC 还没有完成“多实例化”。
3. 当前 UI / 配置 / 持久化 / 作用域模型都还是旧的全局思路。

### 2.2 当前项目已有的防 CC 基础

当前项目已有这些基础能力：

1. `AntiCC` 已支持：
   - 黑白名单
   - 限速
   - 非 SOCKS5 检测
   - 基于请求窗口的限制
   - 认证成功加白名单
   - 白名单持久化
   - 防火墙规则
2. `PacketCollector` 已支持：
   - 传统模式
   - 阻塞线程池模式
   - IOCP 模式
   - SOXKS5 认证
   - 外部账号源绑定
   - 按实例 ID 运行
3. `InstanceManager` 已支持：
   - 多 `SocksForwardInstance`
   - 实例配置缓存与数据库持久化
   - 启停和自动启动
   - 账号库绑定

### 2.3 当前项目已发现且已修复的基础工程问题

在本轮文档前，已经完成以下基础修复：

1. IOCP 路径中的 `packetReceived` 回调已改为统一进入回调线程队列，不再直接阻塞 IOCP 工作线程。
2. IOCP 路径里重复 `CheckAntiCC()` 的问题已去除。
3. `AntiCC::currentConnectionCount` 已开始由调用方喂入，不再长期为无效值。
4. 传统模式下 `CheckAntiCC()` 成功后、后续握手失败导致的 `currentConnections` 泄漏已修复。
5. 攻击状态检测 `UpdateAttackStatus()` 已开始被驱动。
6. 白名单状态在 `stats->isWhitelisted` 与 `ipListManager` 之间的不同步问题已修复。
7. `AntiCC` 对单 IP 的部分状态访问已增加更细粒度互斥保护。

这些修复是后续三期接入的基础，不需要回滚。

---

## 3. 当前项目与参考项目的关键差异

参考项目 `新防cc` 的核心思想是正确的，但它是“单程序、单防护域、全局状态驱动”的写法；当前项目则是“多实例、同进程、同工作区共享资源”的写法。

因此两者差异如下：

### 3.1 参考项目的优势

`新防cc` 值得借鉴的点：

1. 高负载时的认证优先接入。
2. 基于信誉分的连接优先级。
3. 压力下踢低优先级连接给真实用户让路。
4. 黑名单、防火墙、踢指定 IP 连接这些全局手段。
5. 后台维护线程：
   - 攻击态切换
   - 白名单过期清理
   - 统计清理
6. 更细的协议探测。

### 3.2 参考项目不能直接照搬的点

不适合直接搬入当前项目的点：

1. 单一全局 `g_ipStats / g_connectionCount / g_ipListManager`。
2. 每个程序只有一个防护域的默认假设。
3. 自动白名单默认全局生效。
4. `QuickAuthCheck` 被当成高可信信号的倾向。
5. 白名单专属 IOCP 直接复制到每个实例。

原因很简单：

1. 当前项目是多 `SocksForwardInstance` 共存。
2. 同一个 IP 可能同时攻击多个实例。
3. 同一个 IP 也可能在 A 实例认证成功，但不应该自动放通 B 实例。
4. 同进程里如果每个实例都各自完整复制参考项目的全局逻辑，会出现资源重复、封禁状态分裂、压力判断失真。

---

## 4. 本次设计必须满足的硬约束

以下约束以本轮用户要求为准，视为必须满足：

### 4.1 白名单必须分作用域

1. 手动白名单必须支持作用域。
2. 认证成功自动白名单默认是“实例作用域”。
3. 不能出现“在实例 A 认证成功后，实例 B 也自动放行”的行为。

### 4.2 黑名单和防火墙必须全局共享

1. 一个 IP 攻击多个实例时，应一次命中、全局拦截。
2. 防火墙规则是系统级资源，必须由全局统一管理。

### 4.3 连接数必须分三类

1. 全局总连接数。
2. 单实例连接数。
3. 单 IP 跨实例总连接数。

### 4.4 压力判断必须分两类

1. 单实例压力：保护本实例不被拖垮。
2. 全局压力：保护整个进程不被拖垮。

### 4.5 认证优先只能是调度 hint

1. `QuickAuthCheck` 只能表示“可能是会做密码认证的客户端”。
2. 不能因为 quick auth hint 就增加信誉分。
3. 不能因为 quick auth hint 就进入正式白名单。
4. 真正加信誉、加白名单，必须等上游认证成功后再做。

### 4.6 防 CC 功能也要实例化管理

1. 防 CC 配置必须进入 `SocksForwardInstance` 的实例配置体系。
2. 防 CC UI / 持久化 / 更新接口要按 `instanceId` 操作。
3. 对于需要作用域选择的防 CC 条目，应支持“选择生效的 Socks 转发实例”。

---

## 5. 目标架构

本设计采用“双层防护架构”。

### 5.1 第一层：全局协调层

新增组件：

1. `GlobalAntiCCCoordinator`

职责：

1. 维护全局 IP 封禁状态。
2. 维护全局黑名单。
3. 维护全局防火墙规则。
4. 维护全局 IP 信誉视图。
5. 维护跨实例连接统计。
6. 维护全局攻击态。
7. 提供跨实例按 IP 踢连接能力。

这个层是“共享资源和共享视角”的唯一真实来源。

### 5.2 第二层：实例执行层

保留当前每个 `PacketCollector` 持有一个 `AntiCC` 的模型，但职责收缩为：

1. 本实例入口处的快速检查。
2. 本实例连接上限控制。
3. 本实例协议探测。
4. 本实例局部速率限制。
5. 本实例 IOCP / 线程池 admission。
6. 本实例连接关闭执行。

这个层负责“执行”，不负责跨实例共享事实。

### 5.3 第三层：实例配置与作用域层

新增或扩展：

1. `SocksForwardInstance::CachedAntiCCConfig`
2. 作用域条目模型：
   - 手动白名单条目
   - 黑名单条目
   - 规则绑定项

这一层负责表达：

1. 某个配置属于哪个实例。
2. 某个白名单条目作用于哪些实例。
3. 哪些功能是全局共享、不可按实例拆开的。

---

## 6. 作用域模型

### 6.1 作用域分类

定义统一作用域枚举：

1. `GlobalShared`
2. `InstanceOnly`
3. `SelectedInstances`

### 6.2 黑名单作用域

黑名单固定为：

1. `GlobalShared`

原因：

1. 黑名单的目标是快速拦攻击者。
2. 黑名单一旦仅作用于单实例，攻击流量仍可继续打其他实例。
3. 防火墙规则天然也是系统级资源，必须跟随全局黑名单。

### 6.3 防火墙作用域

防火墙固定为：

1. `GlobalShared`

防火墙规则不允许按实例拆分，因为系统防火墙拦的是端口与 IP，属于进程外系统资源，不应该由多个实例分头维护。

### 6.4 手动白名单作用域

手动白名单允许两种：

1. `SelectedInstances`
2. `GlobalShared`

默认建议：

1. UI 新建手动白名单时默认 `SelectedInstances`
2. 用户可显式切换为 `GlobalShared`

条目结构示例：

```cpp
struct ScopedWhitelistEntry {
    std::string ip;
    ScopeType scopeType;
    std::vector<std::string> targetInstanceIds;
    std::chrono::steady_clock::time_point expireAt;
    std::string source; // manual / auth_auto / import
};
```

### 6.5 自动白名单作用域

认证成功自动白名单固定为：

1. `InstanceOnly`

行为定义：

1. 在实例 A 中上游认证成功，只增加实例 A 的自动白名单状态。
2. 不修改其他实例的自动白名单状态。
3. 不写入全局白名单集合。
4. 仅在全局信誉模型中增加“认证成功事件”，但不直接全局放行。

### 6.6 信誉分作用域

信誉分采用“双视角”：

1. `globalScore`
2. `instanceScore`

解释：

1. `globalScore` 反映“这个 IP 在整个进程里的总体行为”。
2. `instanceScore` 反映“这个 IP 在某个实例内的行为”。
3. 实例 admission 使用混合评分，而不是单看全局或单看实例。

建议公式：

1. `effectiveScore = 0.6 * instanceScore + 0.4 * globalScore`
2. 若命中实例自动白名单，则直接按实例白名单路径放大阈值。

---

## 7. 计数与指标模型

### 7.1 必须维护的三类连接数

#### 7.1.1 全局总连接数

定义：

1. 所有 `SocksForwardInstance` 活跃连接总和。

用途：

1. 判断整个进程是否接近资源上限。
2. 全局攻击态切换。

#### 7.1.2 单实例连接数

定义：

1. 某个 `instanceId` 当前活跃连接数。

用途：

1. 单实例 admission。
2. 单实例高负载判断。

#### 7.1.3 单 IP 跨实例总连接数

定义：

1. 同一个 IP 在所有实例上的总活跃连接数。

用途：

1. 判断某个 IP 是否在“水平打所有实例”。
2. 决定是否触发全局拦截或跨实例踢连接。

### 7.2 建议维护的附加计数

1. 单 IP 在单实例的连接数。
2. 单 IP 在全局的短时间新建连接次数。
3. 单实例 IOCP 活跃连接数。
4. 单实例线程池队列积压。
5. 全局回调队列积压总量。

---

## 8. 压力判断模型

### 8.1 单实例压力

单实例压力只保护本实例，建议由 `PacketCollector` 本地上报：

指标建议：

1. 实例活跃连接数占实例上限比例。
2. 实例线程池队列长度。
3. 实例 callback 队列长度。
4. 实例 send queue 累积长度。
5. 实例 `AntiCC::IsOverloaded()` 状态。

输出：

1. `InstancePressureState { Normal, Busy, Overloaded }`

### 8.2 全局压力

全局压力保护整个进程，建议由 `GlobalAntiCCCoordinator` 统一计算：

指标建议：

1. 全局总连接数占全局上限比例。
2. 所有实例 callback 队列总长度。
3. 所有实例 IOCP 活跃连接数总和。
4. 全局单位时间新连接数。
5. 单位时间被拦截数。

输出：

1. `GlobalPressureState { Normal, Busy, UnderAttack, Critical }`

### 8.3 使用规则

1. 单实例压力高：
   - 收缩本实例阈值
   - 本实例优先拒绝低优先级连接
2. 全局压力高：
   - 启动全局 admission 收缩
   - 更激进地执行跨实例 IP 拦截
   - 允许协调器要求实例踢低优先级连接

---

## 9. 认证优先模型

### 9.1 QuickAuthCheck 的角色

`QuickAuthCheck` 只保留一个角色：

1. “调度 hint”

它可以决定：

1. 这个连接是否进认证优先 admission 队列。
2. 这个连接是否在高负载时获得更长的握手生存时间。

它不可以决定：

1. 是否加信誉分。
2. 是否加入正式白名单。
3. 是否跳过全局防护。
4. 是否直接进入全局高信任路径。

### 9.2 认证优先队列的正确用法

在高负载或攻击态下：

1. 普通连接走正常 admission。
2. `QuickAuthCheck == true` 的连接进入实例认证优先队列。
3. 上游认证成功后，才上报：
   - 实例自动白名单
   - 实例信誉提升
   - 全局认证成功事件

### 9.3 当前项目的接入方式

当前项目不建议复制参考项目的整套全局认证优先线程，而是：

1. 在 `PacketCollector` 内增加“实例认证优先 admission 队列”。
2. 队列只负责早期握手调度。
3. 数据转发阶段继续沿用现有 IOCP。

原因：

1. 当前项目已经有实例级 `PacketCollector` 和 IOCP。
2. 多实例下更适合“每实例 admission 队列 + 全局协调限流”，不适合做单一全局握手线程。

---

## 10. 目标数据模型

### 10.1 全局协调器数据

建议新增：

```cpp
struct GlobalIpMetrics {
    std::atomic<int> totalActiveConnections{0};
    std::atomic<int> recentConnections{0};
    std::atomic<int> recentBlocks{0};
    std::atomic<int> recentAuthSuccess{0};
    std::atomic<int> recentAuthHints{0};
    float globalScore = 50.0f;
    bool globallyBanned = false;
    bool firewallBlocked = false;
    std::chrono::steady_clock::time_point globalBanUntil{};
};

struct InstanceIpMetrics {
    std::atomic<int> activeConnections{0};
    std::atomic<int> recentConnections{0};
    std::atomic<int> recentBlocks{0};
    float instanceScore = 50.0f;
    bool autoWhitelisted = false;
    std::chrono::steady_clock::time_point autoWhitelistUntil{};
};
```

### 10.2 实例配置数据

建议给 `SocksForwardInstance::CachedConfig` 增加：

```cpp
struct CachedAntiCCConfig {
    bool enabled = false;
    int timeWindowSeconds = 10;
    int maxRequestsInWindow = 20;
    int banTimeSeconds = 300;
    int maxConnections = 100;
    int authFailBanTime = 60;
    int noAuthBanTime = 30;
    int whitelistDuration = 3600;
    bool useBlacklist = true;
    bool useWhitelist = true;
    bool blockNonSocks = false;
    bool rateLimitEnabled = false;
    int rateLimit = 100;
    int rateTimeWindow = 1;
    bool enableAuthPriorityAdmission = true;
    int authPriorityQueueLimit = 128;
    bool enableLowPriorityEviction = true;
    int lowPriorityEvictionThreshold = 80; // 百分比
    bool enableCoordinator = true;
};
```

### 10.3 作用域条目数据

建议新增：

```cpp
enum class ScopeType {
    GlobalShared,
    InstanceOnly,
    SelectedInstances
};

struct ScopedWhitelistEntry {
    std::string id;
    std::string ip;
    ScopeType scopeType = ScopeType::SelectedInstances;
    std::vector<std::string> targetInstanceIds;
    std::string source; // manual / auth_auto / import
    std::chrono::steady_clock::time_point expireAt{};
};
```

黑名单不需要这个模型，直接全局共享。

---

## 11. 新连接接入流程

建议未来新的入站流程如下：

1. 监听层接收连接。
2. 调用 `GlobalAntiCCCoordinator::PreCheck(instanceId, clientIP)`：
   - 查全局黑名单
   - 查全局防火墙封禁
   - 看单 IP 跨实例总连接数
   - 看全局压力态
3. 若通过，再调用实例本地 `AntiCC::CheckInstanceAdmission(instanceId, clientIP)`：
   - 查实例白名单
   - 查实例速率限制
   - 查实例连接上限
   - 计算混合信誉分
4. 若实例高压且 `QuickAuthCheck == true`，进入实例认证优先 admission 队列。
5. 完成 SOCKS5 握手并向上游认证。
6. 上游认证成功：
   - 实例级自动白名单生效
   - 实例分数提升
   - 全局记录认证成功事件
7. 隧道建立成功后：
   - 计入全局连接数
   - 计入实例连接数
   - 计入单 IP 跨实例连接数
8. 连接关闭：
   - 三类计数都回收
   - 维护实例最后活跃时间
   - 上报协调器更新全局视图

---

## 12. 三期接入方案

## 12.1 第一期：实例化控制面

### 12.1.1 目标

先把“当前已经按实例存在的数据面 AntiCC”真正接入实例控制面。

### 12.1.2 必做项

1. 扩展 `SocksForwardInstance::CachedConfig`
   - 新增 `CachedAntiCCConfig`
2. 扩展 `LoadConfigFromDatabase() / SaveConfigToDatabase()`
   - 把防 CC 配置按 `instanceId` 存入数据库
3. 扩展 `ApplyConfigToCollector()`
   - 对当前实例的 `PacketCollector` 调用：
     - `SetAntiCCConfig()`
     - `SetAntiCCEnabled()`
     - `SetAntiCCDatabaseManager()`
4. 把 `ui_bridge.cpp` 的全局 `g_antiCC` 入口全部改为 `instanceId` 版：
   - `anticc_get_config(instanceId)`
   - `anticc_save_config(instanceId)`
   - `anticc_reset_config(instanceId)`
   - `anticc_get_stats(instanceId)`
5. 在 UI 上把防 CC 面板变成“当前选中 SocksForwardInstance 的防 CC 配置”

### 12.1.3 第一期不做的事

1. 不引入全局协调器。
2. 不引入跨实例共享黑名单。
3. 不做认证优先 admission 改造。

### 12.1.4 第一期完成标准

1. 每个 `SocksForwardInstance` 都能独立保存、加载、修改防 CC 配置。
2. A 实例改防 CC 参数，不影响 B 实例。
3. UI 所见即该实例真实配置。

---

## 12.2 第二期：全局协调器与共享资源

### 12.2.1 目标

把必须全局共享的能力抽离出来。

### 12.2.2 必做项

1. 新增 `GlobalAntiCCCoordinator.h/.cpp`
2. 全局共享：
   - 黑名单
   - 防火墙
   - 全局 IP 信誉
   - 三类连接数聚合
   - 全局压力态
3. `InstanceManager` 负责注册所有运行中的 `SocksForwardInstance`
4. `PacketCollector` 连接生命周期事件上报给协调器：
   - pre-check
   - auth success
   - auth failure
   - protocol violation
   - connection opened
   - connection closed
5. 新增“跨实例按 IP 断开连接”能力

### 12.2.3 作用域落地规则

1. 黑名单：全局共享
2. 防火墙：全局共享
3. 自动白名单：实例作用域
4. 手动白名单：支持选择实例，也支持全局

### 12.2.4 第二期完成标准

1. 攻击 IP 打多个实例时，只要一次命中全局黑名单，所有实例都拒绝。
2. 认证成功自动白名单只作用于认证成功的实例。
3. 能查看：
   - 全局总连接数
   - 单实例连接数
   - 单 IP 跨实例连接数

---

## 12.3 第三期：调度与抗压增强

### 12.3.1 目标

把参考项目里最有价值的“调度型能力”迁进来，但采用当前项目适合的写法。

### 12.3.2 必做项

1. 实例认证优先 admission 队列
2. `QuickAuthCheck` 仅作调度 hint
3. 混合信誉分优先级
4. 高压下拒绝低优先级新连接
5. 高压下踢低优先级存量连接
6. 更细的协议识别：
   - HTTP
   - HTTPS/TLS
   - SSH
   - SOCKS4
7. 维护线程分层：
   - 全局维护线程
   - 实例维护线程

### 12.3.3 不建议在第三期做的事

1. 不做每实例双 IOCP（普通 + 白名单专属 IOCP）的完全复制。

原因：

1. 当前项目已经有 whitelist / normal 上限拆分。
2. 多实例下每实例再复制一套白名单专属 IOCP，线程资源会膨胀。
3. 当前更适合做“普通 admission + auth-priority admission + 现有 IOCP 数据面”。

### 12.3.4 第三期完成标准

1. 高压下真实认证用户的接入成功率显著高于未认证流量。
2. `QuickAuthCheck` 不会直接产生白名单或信誉提升。
3. 低信誉攻击流量会优先被拒绝或踢出。

---

## 13. 文件改动清单

## 13.1 第一期预计改动

1. `InstanceManager.h`
   - 增加 `CachedAntiCCConfig`
2. `InstanceManager.cpp`
   - `LoadConfigFromDatabase()`
   - `SaveConfigToDatabase()`
   - `ApplyConfigToCollector()`
3. `PacketCollector.h`
   - 如有必要增加实例化白名单作用域接口
4. `PacketCollector.cpp`
   - 读取实例化 AntiCC 配置
5. `ui_bridge.cpp`
   - 全部 AntiCC 接口改为带 `instanceId`
6. 前端或 ImGui UI
   - AntiCC 面板实例化

## 13.2 第二期预计新增文件

1. `GlobalAntiCCCoordinator.h`
2. `GlobalAntiCCCoordinator.cpp`
3. 可选：`AntiCCScopeTypes.h`

## 13.3 第三期预计改动

1. `PacketCollector.cpp`
   - admission 队列
   - quick auth hint
   - 低优先级驱逐
2. `IOCPThreadPool.cpp`
   - admission 接口与队列配合
3. `AntiCC.cpp`
   - 混合信誉分
   - 全局协调器接入

---

## 14. 数据库存储建议

建议按实例存储：

1. `instance_<id>_anticc_enabled`
2. `instance_<id>_anticc_timeWindowSeconds`
3. `instance_<id>_anticc_maxRequestsInWindow`
4. `instance_<id>_anticc_banTimeSeconds`
5. `instance_<id>_anticc_maxConnections`
6. `instance_<id>_anticc_authFailBanTime`
7. `instance_<id>_anticc_noAuthBanTime`
8. `instance_<id>_anticc_whitelistDuration`
9. `instance_<id>_anticc_rateLimitEnabled`
10. `instance_<id>_anticc_rateLimit`
11. `instance_<id>_anticc_rateTimeWindow`
12. `instance_<id>_anticc_blockNonSocks`
13. `instance_<id>_anticc_enableAuthPriorityAdmission`
14. `instance_<id>_anticc_authPriorityQueueLimit`
15. `instance_<id>_anticc_enableLowPriorityEviction`
16. `instance_<id>_anticc_lowPriorityEvictionThreshold`

全局存储：

1. `global_anticc_blacklist`
2. `global_anticc_firewall_rules`
3. `global_anticc_manual_whitelist_entries`

自动白名单建议不直接写入全局配置键，而是维护专用结构：

1. `instance_<id>_anticc_auto_whitelist_<ip>`

---

## 15. UI 方案

### 15.1 实例级 AntiCC 面板

以当前选中的 `SocksForwardInstance` 为主。

展示：

1. 启用状态
2. 时间窗口
3. 请求限制
4. 封禁时间
5. 单实例连接限制
6. 实例速率限制
7. 非 SOCKS5 检测
8. 实例认证优先 admission
9. 低优先级驱逐开关

### 15.2 全局 AntiCC 面板

新增全局面板，用来展示和操作：

1. 全局黑名单
2. 全局防火墙状态
3. 全局总连接数
4. 全局攻击态
5. 单 IP 跨实例连接统计

### 15.3 白名单面板

白名单新增作用域字段：

1. `作用域类型`
2. `目标 SocksForwardInstance 列表`
3. `来源`
4. `过期时间`

自动白名单来源显示为：

1. `auth_auto`

并且默认不可切换为全局。

---

## 16. 验收标准

以下为最终验收条件：

1. 两个 `SocksForwardInstance` 同时运行时，实例 A 的防 CC 参数变化不影响实例 B。
2. 某个 IP 在实例 A 认证成功后，仅实例 A 对其自动白名单放行。
3. 同一攻击 IP 同时打多个实例时，命中一次全局黑名单后，所有实例都拒绝。
4. 系统能正确显示：
   - 全局总连接数
   - 单实例连接数
   - 单 IP 跨实例总连接数
5. 系统能正确区分：
   - 单实例高压
   - 全局高压
6. 高压下 `QuickAuthCheck` 仅影响调度，不直接提升信任。
7. 真正的白名单/信誉提升只发生在上游认证成功后。
8. 三期完成后，系统仍保持当前多实例架构，不回退成单实例全局写法。

---

## 17. 建议实施顺序

建议实际开发顺序如下：

1. 先落第一期
   - 实例级 AntiCC 配置
   - UI / DB / ApplyConfigToCollector
2. 再落第二期
   - 全局协调器
   - 全局黑名单 / 防火墙 / 计数器
3. 最后落第三期
   - admission 队列
   - quick auth hint
   - 低优先级驱逐

原因：

1. 第一期是控制面修正，风险最低。
2. 第二期开始引入跨实例共享事实，价值最大。
3. 第三期是调度增强，最容易影响行为，必须在前两期稳定后做。

---

## 18. 本轮建议

本轮不继续直接写三期代码，先按本文档确认以下几点：

1. 手动白名单是否允许全局作用域，还是也必须只能选实例。
2. 全局信誉分与实例信誉分的混合比例是否接受。
3. 是否需要单独做“防 CC 规则模板”，还是只做实例内配置即可。
4. UI 上是否接受拆成：
   - 实例级 AntiCC 面板
   - 全局共享 AntiCC 面板

如果以上方向确认，就按本文档进入实际接入阶段。
