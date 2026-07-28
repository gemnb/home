#pragma once

#include <algorithm>
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "DatabaseManager.h"
#include "Logger.h"

// 采集内存数据池（同时支持按 SOCKS 账号、GameID 索引）
// 说明：从 `新伪心跳.cpp` 抽离，供“ab多实例”复用并隔离各自内存池。
struct CollectedPacketPool {
    std::map<std::string, std::deque<HeartbeatRecord>> dataByUsername;  // username -> records
    std::map<std::string, std::deque<HeartbeatRecord>> dataByGameID;    // gameID -> records
    std::mutex poolMutex;
    std::atomic<int> totalCount{0};

    void AddPacket(const HeartbeatRecord& record) {
        std::lock_guard<std::mutex> lock(poolMutex);
        dataByUsername[record.socksUsername].push_back(record);
        dataByGameID[record.gameID].push_back(record);
        totalCount.fetch_add(1);
    }

    // ascending=true: FIFO；ascending=false: LIFO
    bool PopPacketByUsername(const std::string& username, HeartbeatRecord& outRecord, bool ascending = true) {
        std::lock_guard<std::mutex> lock(poolMutex);
        auto it = dataByUsername.find(username);
        if (it == dataByUsername.end() || it->second.empty()) return false;

        if (ascending) {
            outRecord = it->second.front();
            it->second.pop_front();
        } else {
            outRecord = it->second.back();
            it->second.pop_back();
        }

        // 同步从 GameID 索引移除
        auto gameIt = dataByGameID.find(outRecord.gameID);
        if (gameIt != dataByGameID.end()) {
            auto& q = gameIt->second;
            auto recIt = std::find(q.begin(), q.end(), outRecord);
            if (recIt != q.end()) {
                q.erase(recIt);
                if (q.empty()) dataByGameID.erase(gameIt);
            }
        }

        if (it->second.empty()) dataByUsername.erase(it);
        totalCount.fetch_sub(1);
        return true;
    }

    // ascending=true: FIFO；ascending=false: LIFO
    bool PopPacketByGameID(const std::string& gameID, HeartbeatRecord& outRecord, bool ascending = true) {
        std::lock_guard<std::mutex> lock(poolMutex);
        auto it = dataByGameID.find(gameID);
        if (it == dataByGameID.end() || it->second.empty()) return false;

        if (ascending) {
            outRecord = it->second.front();
            it->second.pop_front();
        } else {
            outRecord = it->second.back();
            it->second.pop_back();
        }

        // 同步从 Username 索引移除
        auto userIt = dataByUsername.find(outRecord.socksUsername);
        if (userIt != dataByUsername.end()) {
            auto& q = userIt->second;
            auto recIt = std::find(q.begin(), q.end(), outRecord);
            if (recIt != q.end()) {
                q.erase(recIt);
                if (q.empty()) dataByUsername.erase(userIt);
            }
        }

        if (it->second.empty()) dataByGameID.erase(it);
        totalCount.fetch_sub(1);
        return true;
    }

    bool PeekPacketByUsername(const std::string& username, HeartbeatRecord& outRecord) {
        std::lock_guard<std::mutex> lock(poolMutex);
        auto it = dataByUsername.find(username);
        if (it == dataByUsername.end() || it->second.empty()) return false;
        outRecord = it->second.front();
        return true;
    }

    bool PeekPacketByGameID(const std::string& gameID, HeartbeatRecord& outRecord) {
        std::lock_guard<std::mutex> lock(poolMutex);
        auto it = dataByGameID.find(gameID);
        if (it == dataByGameID.end() || it->second.empty()) return false;
        outRecord = it->second.front();
        return true;
    }

    size_t GetCountByUsername(const std::string& username) {
        std::lock_guard<std::mutex> lock(poolMutex);
        auto it = dataByUsername.find(username);
        return it == dataByUsername.end() ? 0 : it->second.size();
    }

    size_t GetCountByGameID(const std::string& gameID) {
        std::lock_guard<std::mutex> lock(poolMutex);
        auto it = dataByGameID.find(gameID);
        return it == dataByGameID.end() ? 0 : it->second.size();
    }

    std::vector<std::string> GetAllUsernames() {
        std::lock_guard<std::mutex> lock(poolMutex);
        std::vector<std::string> result;
        result.reserve(dataByUsername.size());
        for (const auto& pair : dataByUsername) result.push_back(pair.first);
        return result;
    }

    std::vector<std::string> GetAllGameIDs() {
        std::lock_guard<std::mutex> lock(poolMutex);
        std::vector<std::string> result;
        result.reserve(dataByGameID.size());
        for (const auto& pair : dataByGameID) result.push_back(pair.first);
        return result;
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(poolMutex);
        dataByUsername.clear();
        dataByGameID.clear();
        totalCount.store(0);
    }

    size_t ClearByUsername(const std::string& username) {
        std::lock_guard<std::mutex> lock(poolMutex);
        auto it = dataByUsername.find(username);
        if (it == dataByUsername.end()) return 0;

        const size_t clearedCount = it->second.size();

        // 同步从 GameID 索引移除
        for (const auto& record : it->second) {
            auto gameIt = dataByGameID.find(record.gameID);
            if (gameIt == dataByGameID.end()) continue;
            auto& q = gameIt->second;
            auto recIt = std::find(q.begin(), q.end(), record);
            if (recIt != q.end()) q.erase(recIt);
            if (q.empty()) dataByGameID.erase(gameIt);
        }

        dataByUsername.erase(it);
        totalCount.fetch_sub(static_cast<int>(clearedCount));
        AB_LOG_INFO("[内存池] 已清空SOCKS用户[" + username + "] 的 " + std::to_string(clearedCount) + " 条数据");
        return clearedCount;
    }

    size_t ClearByGameID(const std::string& gameID) {
        std::lock_guard<std::mutex> lock(poolMutex);
        auto it = dataByGameID.find(gameID);
        if (it == dataByGameID.end()) return 0;

        const size_t clearedCount = it->second.size();

        // 同步从 Username 索引移除
        for (const auto& record : it->second) {
            auto userIt = dataByUsername.find(record.socksUsername);
            if (userIt == dataByUsername.end()) continue;
            auto& q = userIt->second;
            auto recIt = std::find(q.begin(), q.end(), record);
            if (recIt != q.end()) q.erase(recIt);
            if (q.empty()) dataByUsername.erase(userIt);
        }

        dataByGameID.erase(it);
        totalCount.fetch_sub(static_cast<int>(clearedCount));
        AB_LOG_INFO("[内存池] 已清空GameID[" + gameID + "] 的 " + std::to_string(clearedCount) + " 条数据");
        return clearedCount;
    }

    int GetTotalCount() { return totalCount.load(); }

    size_t GetUsernameCount() {
        std::lock_guard<std::mutex> lock(poolMutex);
        return dataByUsername.size();
    }

    size_t GetGameIDCount() {
        std::lock_guard<std::mutex> lock(poolMutex);
        return dataByGameID.size();
    }

    struct PoolStats {
        int totalPackets = 0;
        int usernameCount = 0;
        int gameIDCount = 0;
    };

    PoolStats GetStats() {
        std::lock_guard<std::mutex> lock(poolMutex);
        PoolStats stats;
        stats.totalPackets = totalCount.load();
        stats.usernameCount = static_cast<int>(dataByUsername.size());
        stats.gameIDCount = static_cast<int>(dataByGameID.size());
        return stats;
    }
};

