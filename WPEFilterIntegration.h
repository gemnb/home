#pragma once

#include "WPEFilter.h"
#include <vector>

// 全局WPE滤镜管理器（由主程序创建/销毁）
extern WPEFilter::FilterManager* g_wpeFilterManager;

// 对数据包应用WPE滤镜，返回处理结果
// instanceId: 实例ID（用于判断滤镜是否对该实例生效）
// isCollector: true=采集端, false=伪心跳端
// isRequest: true=请求方向, false=响应方向
// phase: 当前执行阶段（用于判断执行顺序）
// username: 当前连接的账号名（用于账号级别的滤镜状态管理）
// userEnabledFilters: 用户启用的滤镜ID列表（用于用户滤镜模式，传入后会记录执行次数到数据库）
WPEFilter::FilterProcessResult ApplyWPEFilters(
    std::vector<uint8_t>& data,
    const std::string& instanceId,
    bool isCollector,
    bool isRequest,
    WPEFilter::FilterPriority phase,
    const std::string& username = "",
    const std::vector<int>* userEnabledFilters = nullptr);
