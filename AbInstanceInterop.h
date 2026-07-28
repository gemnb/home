#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "CollectedPacketPool.h"
#include "PacketCollector.h"

// 说明：
// - 实现位于 `新伪心跳.cpp`，供 InstanceManager 的 “ab多实例” 复用独立运行那套采集/伪心跳核心逻辑。
// - 通过传入 pool 指针实现内存隔离；伪心跳侧通过 atomic 指针支持运行中动态切换绑定的采集内存池。
namespace AbInstanceInterop {
    // instanceId: 用于定位该ab实例的独立UI/配置上下文（实现位于 新伪心跳.cpp）
    bool StartCollector(std::unique_ptr<PacketCollector>& collector, const std::string& instanceId, int port, CollectedPacketPool* pool);
    void StopCollector(std::unique_ptr<PacketCollector>& collector);

    bool StartHeartbeatForwarder(std::unique_ptr<PacketCollector>& forwarder, const std::string& instanceId, int port, std::atomic<CollectedPacketPool*>* poolPtr);
    void StopHeartbeatForwarder(std::unique_ptr<PacketCollector>& forwarder);

    // 上下文生命周期（可选调用；未调用也会在首次Start/渲染时惰性创建）
    void DestroyCollectorContext(const std::string& instanceId);
    void DestroyHeartbeatContext(const std::string& instanceId);
} // namespace AbInstanceInterop
