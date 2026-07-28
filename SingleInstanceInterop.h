#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "DatabaseManager.h"
#include "PacketCollector.h"

class CollectorInstance;
class HeartbeatInstance;
class HeartbeatCore;

// 说明：
// - 实现位于 `新伪心跳.cpp`（通过 include 的 .inl 代码编译进同一目标）
// - 用于 InstanceManager 的“单伪多实例”复用单伪项目的采集/伪心跳核心与UI，并做到每实例数据隔离
namespace SingleInstanceInterop {
    // desiredMode: MEMORY=写入内存池；HOURLY/DAILY=写入指定目录下的按小时/按天db文件
    bool StartCollector(std::unique_ptr<PacketCollector>& collector, CollectorInstance* instance, const std::string& instanceId, int port, DatabaseManager* db, ::StorageMode desiredMode);
    void StopCollector(std::unique_ptr<PacketCollector>& collector);

    // useBoundCollectorDb: true=从 boundDbPtr 读取数据（绑定采集），false=走全局数据库（兼容“数据库模式”）
    bool StartHeartbeatForwarder(std::unique_ptr<PacketCollector>& forwarder, HeartbeatInstance* instance, const std::string& instanceId, int port, std::atomic<DatabaseManager*>* boundDbPtr, bool useBoundCollectorDb, HeartbeatCore* core);
    void StopHeartbeatForwarder(std::unique_ptr<PacketCollector>& forwarder);

    // UI：在实例配置页渲染“单伪项目”的完整采集/伪心跳页面
    void RenderCollectorInstanceUi(CollectorInstance* instance, bool& needsRedraw);
    void RenderHeartbeatInstanceUi(HeartbeatInstance* instance, bool& needsRedraw);

    // 上下文生命周期（删除实例时调用）
    void DestroyCollectorContext(const std::string& instanceId);
    void DestroyHeartbeatContext(const std::string& instanceId);
} // namespace SingleInstanceInterop
