#pragma once

#include <string>

namespace RemoteBrowserServer {

struct Config {
    bool enabled = false;
    std::string bindHost = "0.0.0.0";
    int port = 18080;
    std::string accessToken;
};

void SetActionHandler(void (*handler)(const char* jsonMessage));

bool ApplyConfig(const Config& config, std::string& outError);
void Stop();
bool IsRunning();
Config GetConfig();
std::string GetLastError();
std::string DetectPreferredIPv4Host();
bool IsPrivateIPv4Host(const std::string& host);
std::string BuildAccessUrl(const Config& config);

void PushMessage(const std::string& jsonMessage);

} // namespace RemoteBrowserServer



