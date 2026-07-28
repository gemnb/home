#include "Logger.h"
#include <windows.h>
#include <unordered_set>

namespace {
bool Utf8ToWide(const std::string& input, std::wstring& output) {
    output.clear();
    if (input.empty()) {
        return true;
    }

    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), nullptr, 0);
    if (length <= 0) {
        return false;
    }

    output.resize(length);
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), output.data(), length) > 0;
}

bool WideToUtf8(const std::wstring& input, std::string& output) {
    output.clear();
    if (input.empty()) {
        return true;
    }

    const int length = WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return false;
    }

    output.resize(length);
    return WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), output.data(), length, nullptr, nullptr) > 0;
}

bool WideToCodePageBytes(const std::wstring& input, UINT codePage, std::string& output) {
    output.clear();
    if (input.empty()) {
        return true;
    }

    const int length = WideCharToMultiByte(codePage, 0, input.data(), static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return false;
    }

    output.resize(length);
    return WideCharToMultiByte(codePage, 0, input.data(), static_cast<int>(input.size()), output.data(), length, nullptr, nullptr) > 0;
}

int CountCjkCharacters(const std::wstring& text) {
    int count = 0;
    for (const wchar_t ch : text) {
        if (ch >= 0x4E00 && ch <= 0x9FFF) {
            ++count;
        }
    }
    return count;
}

int CountChinesePunctuation(const std::wstring& text) {
    static const std::wstring punctuation = L"，。！？：；（）【】《》、‘’“”";
    int count = 0;
    for (const wchar_t ch : text) {
        if (punctuation.find(ch) != std::wstring::npos) {
            ++count;
        }
    }
    return count;
}

int CountKanaCharacters(const std::wstring& text) {
    int count = 0;
    for (const wchar_t ch : text) {
        // Hiragana (3040-309F), Katakana (30A0-30FF)
        if ((ch >= 0x3040 && ch <= 0x309F) || (ch >= 0x30A0 && ch <= 0x30FF)) {
            ++count;
        }
    }
    return count;
}

int CountPrivateUseCharacters(const std::wstring& text) {
    int count = 0;
    for (const wchar_t ch : text) {
        // Basic Private Use Area (E000-F8FF)
        if (ch >= 0xE000 && ch <= 0xF8FF) {
            ++count;
        }
    }
    return count;
}

int CountSuspiciousMarkers(const std::wstring& text) {
    static const std::wstring suspicious = L"鍦鐧瀵缁鏇鍚寮璇鏈鎴妫閮闂閫夋暟鏃堕棿";
    int count = 0;
    for (const wchar_t ch : text) {
        if (suspicious.find(ch) != std::wstring::npos) {
            ++count;
        }
    }
    return count;
}

int CountReplacementCharacters(const std::wstring& text) {
    int count = 0;
    for (const wchar_t ch : text) {
        if (ch == 0xFFFD || ch == L'?') {
            ++count;
        }
    }
    return count;
}

int ComputeReadabilityScore(const std::wstring& text) {
    return CountCjkCharacters(text) * 2
        + CountChinesePunctuation(text) * 3
        - CountSuspiciousMarkers(text) * 4
        - CountKanaCharacters(text) * 5
        - CountPrivateUseCharacters(text) * 8
        - CountReplacementCharacters(text) * 8;
}

bool NeedsMojibakeRepair(const std::wstring& text) {
    return CountSuspiciousMarkers(text) >= 2
        || CountKanaCharacters(text) > 0
        || CountPrivateUseCharacters(text) > 0
        || CountReplacementCharacters(text) > 0;
}

bool TryRepairUtf8Mojibake(const std::string& input, UINT codePage, std::string& output) {
    std::wstring sourceWide;
    if (!Utf8ToWide(input, sourceWide)) {
        return false;
    }

    std::string codePageBytes;
    if (!WideToCodePageBytes(sourceWide, codePage, codePageBytes)) {
        return false;
    }

    std::wstring repairedWide;
    if (!Utf8ToWide(codePageBytes, repairedWide)) {
        return false;
    }

    return WideToUtf8(repairedWide, output) && !output.empty();
}

std::string NormalizeMojibakeText(const std::string& text) {
    std::wstring sourceWide;
    if (!Utf8ToWide(text, sourceWide) || !NeedsMojibakeRepair(sourceWide)) {
        return text;
    }

    std::string bestText = text;
    int bestScore = ComputeReadabilityScore(sourceWide);
    const int originalSuspiciousCount = CountSuspiciousMarkers(sourceWide);
    const int originalKanaCount = CountKanaCharacters(sourceWide);
    const int originalPrivateUseCount = CountPrivateUseCharacters(sourceWide);
    const int originalReplacementCount = CountReplacementCharacters(sourceWide);

    std::vector<UINT> codePages = { 936 };
    if (CP_ACP != 936) {
        codePages.push_back(CP_ACP);
    }

    for (const UINT codePage : codePages) {
        std::string repairedText;
        if (!TryRepairUtf8Mojibake(text, codePage, repairedText)) {
            continue;
        }

        std::wstring repairedWide;
        if (!Utf8ToWide(repairedText, repairedWide)) {
            continue;
        }

        const int repairedSuspiciousCount = CountSuspiciousMarkers(repairedWide);
        const int repairedKanaCount = CountKanaCharacters(repairedWide);
        const int repairedPrivateUseCount = CountPrivateUseCharacters(repairedWide);
        const int repairedReplacementCount = CountReplacementCharacters(repairedWide);
        const int repairedScore = ComputeReadabilityScore(repairedWide);

        const bool improvedCorruption =
            repairedSuspiciousCount < originalSuspiciousCount
            || repairedKanaCount < originalKanaCount
            || repairedPrivateUseCount < originalPrivateUseCount
            || repairedReplacementCount < originalReplacementCount;

        if (improvedCorruption && repairedScore > bestScore) {
            bestText = repairedText;
            bestScore = repairedScore;
        }
    }

    return bestText;
}
}
std::vector<LogEntry> Logger::logs;
std::mutex Logger::logMutex;
std::atomic<bool> Logger::enableLogging(true);

std::string Logger::GetTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::stringstream ss;
    struct tm timeinfo;
    localtime_s(&timeinfo, &time);
    ss << std::put_time(&timeinfo, "%Y-%m-%d %H:%M:%S");
    ss << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return ss.str();
}

std::string Logger::GetCategoryName(LogCategory cat) {
    switch (cat) {
    case LOG_CAT_ALL:
        return "[ALL]";
    case LOG_CAT_HEARTBEAT_LOAD:
        return "[HeartbeatLoad]";
    case LOG_CAT_HEARTBEAT_CLEANUP:
        return "[HeartbeatCleanup]";
    case LOG_CAT_HEARTBEAT_REPLACE:
        return "[HeartbeatReplace]";
    case LOG_CAT_LOGIN_AUTH:
        return "[LoginAuth]";
    case LOG_CAT_ANTICC:
        return "[AntiCC]";
    case LOG_CAT_COLLECTOR:
        return "[Collector]";
    case LOG_CAT_API:
        return "[API]";
    case LOG_CAT_SOCKS_ACCOUNT:
        return "[SocksAccount]";
    default:
        return "[Unknown]";
    }
}

std::string Logger::NormalizeTextForDisplay(const std::string& text) {
    return NormalizeMojibakeText(text);
}

void Logger::Log(LogLevel level, LogCategory category, const std::string& message) {
    if (!enableLogging.load(std::memory_order_relaxed)) {
        return;
    }

    std::lock_guard<std::mutex> lock(logMutex);
    logs.emplace_back(GetTimestamp(), level, category, NormalizeTextForDisplay(message));

    if (logs.size() > MAX_LOGS) {
        logs.erase(logs.begin());
    }
}

void Logger::Info(const std::string& message) {
    Log(LOG_INFO, LOG_CAT_ALL, message);
}

void Logger::Warning(const std::string& message) {
    Log(LOG_WARNING, LOG_CAT_ALL, message);
}

void Logger::Error(const std::string& message) {
    Log(LOG_ERROR, LOG_CAT_ALL, message);
}

void Logger::InfoCat(LogCategory category, const std::string& message) {
    Log(LOG_INFO, category, message);
}

void Logger::WarningCat(LogCategory category, const std::string& message) {
    Log(LOG_WARNING, category, message);
}

void Logger::ErrorCat(LogCategory category, const std::string& message) {
    Log(LOG_ERROR, category, message);
}

std::vector<LogEntry> Logger::GetLogs() {
    std::lock_guard<std::mutex> lock(logMutex);
    return logs;
}

std::vector<LogEntry> Logger::GetLogsByCategory(LogCategory cat) {
    std::lock_guard<std::mutex> lock(logMutex);
    if (cat == LOG_CAT_ALL) {
        return logs;
    }

    std::vector<LogEntry> filteredLogs;
    for (const auto& log : logs) {
        if (log.category == cat) {
            filteredLogs.push_back(log);
        }
    }
    return filteredLogs;
}

std::vector<LogEntry> Logger::GetErrorLogs() {
    std::lock_guard<std::mutex> lock(logMutex);

    std::vector<LogEntry> errorLogs;
    for (const auto& log : logs) {
        if (log.level == LOG_ERROR) {
            errorLogs.push_back(log);
        }
    }
    return errorLogs;
}

void Logger::Clear() {
    std::lock_guard<std::mutex> lock(logMutex);
    logs.clear();
}

void Logger::SetEnabled(bool enabled) {
    enableLogging.store(enabled, std::memory_order_relaxed);
}

bool Logger::IsEnabled() {
    return enableLogging.load(std::memory_order_relaxed);
}
