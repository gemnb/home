#include "SSLMitmContext.h"
#include "Logger.h"
#include <chrono>
#include <filesystem>
#include <iostream>

int main() {
    Logger::SetEnabled(true);

    const auto start = std::chrono::steady_clock::now();
    std::cout << "[probe] calling SSLMitmContext::InitGlobalCA()" << std::endl;

    const bool initOk = SSLMitmContext::InitGlobalCA();
    const auto afterInit = std::chrono::steady_clock::now();
    const auto initMs = std::chrono::duration_cast<std::chrono::milliseconds>(afterInit - start).count();

    std::cout << "[probe] InitGlobalCA result=" << (initOk ? "true" : "false")
              << " elapsed_ms=" << initMs << std::endl;
    std::cout << "[probe] last_error=" << SSLMitmContext::GetLastErrorDetail() << std::endl;

    const std::filesystem::path exportPath = std::filesystem::current_path() / "sslmitm_probe_export.cer";
    std::cout << "[probe] exporting to " << exportPath.string() << std::endl;

    const bool exportOk = SSLMitmContext::ExportCACert(exportPath.string());
    const auto afterExport = std::chrono::steady_clock::now();
    const auto exportMs = std::chrono::duration_cast<std::chrono::milliseconds>(afterExport - afterInit).count();

    std::cout << "[probe] ExportCACert result=" << (exportOk ? "true" : "false")
              << " elapsed_ms=" << exportMs << std::endl;
    std::cout << "[probe] last_error=" << SSLMitmContext::GetLastErrorDetail() << std::endl;

    return (initOk && exportOk) ? 0 : 1;
}
