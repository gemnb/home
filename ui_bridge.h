#pragma once

#include <string>
#include <mutex>
#include <queue>
#include <vector>
#include <functional>

// Forward declarations
class InstanceManager;

// Initialize UI bridge (call after webview2_create and webview2_setup_message_handler)
void UIBridge_Init();

// Handle incoming JSON message from JavaScript frontend
void UIBridge_HandleMessage(const char* jsonMessage);

// Push JSON message to frontend (thread-safe, queued)
void UIBridge_PushMessage(const std::string& jsonMessage);

// Flush queued messages to WebView2 (call from main thread / message loop)
void UIBridge_FlushMessages();

// Helper: push a toast notification to frontend
void UIBridge_Toast(const std::string& type, const std::string& title, const std::string& message);

// Helper: push status update
void UIBridge_PushStatus(bool force = true);

// Helper: push instance list
void UIBridge_PushInstances(bool force = true);

// Proxy data realtime subscription
bool UIBridge_ShouldPushProxyPacket(const std::string& instanceId);
bool UIBridge_ShouldPushProxyPacketData(const std::string& instanceId, const std::vector<uint8_t>& data);
void UIBridge_SetProxydataSubscription(const std::string& instanceId, bool enabled);

// Start background push thread (stats, etc.)
void UIBridge_StartPushThread();

// Stop background push thread
void UIBridge_StopPushThread();

