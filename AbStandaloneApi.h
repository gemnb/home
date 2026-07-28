#pragma once

#include <cstdint>

// 说明：
// - 这里提供“独立运行（新伪心跳.cpp）”那套采集/伪心跳的对外调用接口，
//   供 InstanceManager 以“ab采集 / ab伪心跳”内置实例的形式接入。
// - 具体实现位于 新伪心跳.cpp（因其内部存在 static 全局对象）。

namespace AbStandaloneApi {
// ===== 采集（ab采集）=====
int GetCollectorDesiredPort();             // 从 g_portBuffer 解析
bool StartCollector(int port);             // 等价于 UI 点击“启动代理”
bool StartCollectorFromBuffer();           // 使用当前 g_portBuffer
void StopCollector();                      // 等价于 UI 点击“停止代理”
bool IsCollectorRunning();
int GetCollectorConnections();
std::uint64_t GetCollectorTotalPackets();
std::uint64_t GetCollectorTotalBytes();

// ===== 伪心跳（ab伪心跳）=====
int GetHeartbeatDesiredPort();             // 从 g_heartbeatPortBuffer 解析
bool StartHeartbeatForwarder(int port);    // 等价于 UI 点击“启动转发”
bool StartHeartbeatForwarderFromBuffer();  // 使用当前 g_heartbeatPortBuffer
void StopHeartbeatForwarder();             // 等价于 UI 点击“停止转发”
bool IsHeartbeatForwarderRunning();
int GetHeartbeatConnections();
std::uint64_t GetHeartbeatTotalPackets();
std::uint64_t GetHeartbeatTotalBytes();
} // namespace AbStandaloneApi

