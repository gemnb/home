// ==================== 实例管理页面实现 ====================
// 此文件包含实例管理页面的GUI实现

// 包含实例配置UI
#include "CollectorInstanceUI.inl"
#include "HeartbeatInstanceUI.inl"

// 实例管理页面的全局变量
static char g_newInstanceName[256] = "";
static int g_newInstanceType = 0;  // 0=单伪采集, 1=单伪伪心跳, 2=ab采集, 3=ab伪心跳
static int g_newInstancePort = 1080;
static int g_newStorageMode = 0;   // 0=按小时, 1=按天, 2=内存
static int g_newPoolSource = 0;    // 0=数据库, 1=绑定的采集实例
static int g_newPacketOrder = 0;   // 0=升序, 1=降序
static std::string g_selectedInstanceId = "";
static std::string g_bindCollectorId = "";
static std::string g_bindHeartbeatId = "";
static std::string g_bindAbCollectorId = "";
static std::string g_bindAbHeartbeatId = "";
static bool g_showInstanceConfig = false;
static std::string g_configInstanceId = "";

// ===== 实例导入导出相关 =====
static std::set<std::string> g_instanceExportSelectedIds;
static bool g_showInstanceImportModal = false;
static int g_instanceImportMode = 0; // 0=合并, 1=覆盖
static std::string g_instancePendingImportPath;
static std::string g_instancePendingImportJson;
static std::string g_instanceLastIoMessage;

namespace {
struct AbCollectorUiScope {
    AbCollectorCtx* prevCtx = nullptr;
    AbCollectorInstance* prevInst = nullptr;
    PacketCollector* prevCollector = nullptr;
    CollectedPacketPool* prevPool = nullptr;

    AbCollectorUiScope(AbCollectorCtx* ctx, AbCollectorInstance* inst, PacketCollector* collector, CollectedPacketPool* pool) {
        prevCtx = g_uiActiveAbCollectorCtx;
        prevInst = g_uiActiveAbCollectorInstance;
        prevCollector = g_uiActiveAbCollector;
        prevPool = g_uiActiveAbCollectorPool;

        g_uiActiveAbCollectorCtx = ctx;
        g_uiActiveAbCollectorInstance = inst;
        g_uiActiveAbCollector = collector;
        g_uiActiveAbCollectorPool = pool;
    }

    ~AbCollectorUiScope() {
        g_uiActiveAbCollectorCtx = prevCtx;
        g_uiActiveAbCollectorInstance = prevInst;
        g_uiActiveAbCollector = prevCollector;
        g_uiActiveAbCollectorPool = prevPool;
    }
};

struct AbHeartbeatUiScope {
    AbHeartbeatCtx* prevCtx = nullptr;
    AbHeartbeatInstance* prevInst = nullptr;
    PacketCollector* prevForwarder = nullptr;
    CollectedPacketPool* prevBoundPool = nullptr;

    AbHeartbeatUiScope(AbHeartbeatCtx* ctx, AbHeartbeatInstance* inst, PacketCollector* forwarder, CollectedPacketPool* boundPool) {
        prevCtx = g_uiActiveAbHeartbeatCtx;
        prevInst = g_uiActiveAbHeartbeatInstance;
        prevForwarder = g_uiActiveAbHeartbeatForwarder;
        prevBoundPool = g_uiActiveAbHeartbeatBoundPool;

        g_uiActiveAbHeartbeatCtx = ctx;
        g_uiActiveAbHeartbeatInstance = inst;
        g_uiActiveAbHeartbeatForwarder = forwarder;
        g_uiActiveAbHeartbeatBoundPool = boundPool;
    }

    ~AbHeartbeatUiScope() {
        g_uiActiveAbHeartbeatCtx = prevCtx;
        g_uiActiveAbHeartbeatInstance = prevInst;
        g_uiActiveAbHeartbeatForwarder = prevForwarder;
        g_uiActiveAbHeartbeatBoundPool = prevBoundPool;
    }
};
} // namespace

static void RenderAbCollectorInstanceConfig(AbCollectorInstance* instance, bool& needsRedraw) {
    if (!instance) {
        ImGuiGBK::Text("实例不存在");
        return;
    }

    const InstanceInfo info = instance->GetInfo();

    // 复用"独立运行采集页"的完整UI：通过本帧UI上下文把数据源切到该ab实例
    auto ctx = GetOrCreateAbCollectorCtx(info.id);
    if (ctx) {
        // 同步端口显示（避免初次进入时仍显示默认1080）
        ctx->listenPort = info.port;
        sprintf_s(ctx->portBuffer, "%d", info.port);
    }

    AbCollectorUiScope scope(ctx.get(), instance, instance->GetCollector(), instance->GetPool());

    int currentPacketCount = 0;
    if (ctx) {
        std::lock_guard<std::mutex> lock(ctx->packetsMutex);
        currentPacketCount = static_cast<int>(ctx->packets.size());
    }

    int cachedTotalPackets = 0;
    int cachedTotalConnections = 0;
    double cachedTotalBytes = 0.0;
    int cachedFragmentedPackets = 0;
    int cachedMultiPackets = 0;

    if (g_uiActiveAbCollector) {
        cachedTotalPackets = g_uiActiveAbCollector->GetTotalPackets();
        cachedTotalConnections = g_uiActiveAbCollector->GetTotalConnections();
        cachedTotalBytes = g_uiActiveAbCollector->GetTotalBytes() / 1024.0;
        cachedFragmentedPackets = g_uiActiveAbCollector->GetFragmentedPackets();
        cachedMultiPackets = g_uiActiveAbCollector->GetMultiPackets();
    }

    RenderCollectorPage(needsRedraw, currentPacketCount,
        cachedTotalPackets, cachedTotalConnections, cachedTotalBytes,
        cachedFragmentedPackets, cachedMultiPackets);
}

static void RenderAbHeartbeatInstanceConfig(AbHeartbeatInstance* instance, bool& needsRedraw) {
    if (!instance) {
        ImGuiGBK::Text("实例不存在");
        return;
    }

    const InstanceInfo info = instance->GetInfo();

    auto ctx = GetOrCreateAbHeartbeatCtx(info.id);
    if (ctx) {
        ctx->heartbeatPort = info.port;
        sprintf_s(ctx->heartbeatPortBuffer, "%d", info.port);
    }

    AbHeartbeatUiScope scope(ctx.get(), instance, instance->GetForwarder(), instance->GetBoundCollectorPool());

    RenderHeartbeatPage(needsRedraw);
}

static void RenderSocksForwardInstanceConfig(SocksForwardInstance* instance, bool& needsRedraw) {
    if (!instance) {
        ImGuiGBK::Text("实例不存在");
        return;
    }

    const InstanceInfo info = instance->GetInfo();
    PacketCollector* collector = instance->GetCollector();

    if (!collector) {
        ImGuiGBK::Text("PacketCollector未初始化");
        return;
    }

    // ===== 左右布局：左侧菜单（2列）+ 右侧内容（8列）=====
    static int selectedMenu = 0;  // 0=基本信息, 1=二级代理, 2=线程模型, 3=SOCKS5认证, 4=分包处理, 5=流量过滤, 6=用户滤镜

    // 左侧菜单（2/10宽度）
    ImGui::BeginChild("SocksMenuLeft", ImVec2(ImGui::GetContentRegionAvail().x * 0.2f, 0), true);
    ImGuiGBK::Text("配置菜单");
    ImGui::Separator();

    if (ImGui::Selectable("基本信息##menu0", selectedMenu == 0)) {
        selectedMenu = 0;
        needsRedraw = true;
    }
    if (ImGui::Selectable("二级代理##menu1", selectedMenu == 1)) {
        selectedMenu = 1;
        needsRedraw = true;
    }
    if (ImGui::Selectable("线程模型##menu2", selectedMenu == 2)) {
        selectedMenu = 2;
        needsRedraw = true;
    }
    if (ImGui::Selectable("SOCKS5认证##menu3", selectedMenu == 3)) {
        selectedMenu = 3;
        needsRedraw = true;
    }
    if (ImGui::Selectable("分包处理##menu4", selectedMenu == 4)) {
        selectedMenu = 4;
        needsRedraw = true;
    }
    if (ImGuiGBK::Selectable("流量过滤##menu5", selectedMenu == 5)) {
        selectedMenu = 5;
        needsRedraw = true;
    }
    if (ImGuiGBK::Selectable("用户滤镜##menu6", selectedMenu == 6)) {
        selectedMenu = 6;
        needsRedraw = true;
    }
    if (ImGuiGBK::Selectable("账号滤镜##menu7", selectedMenu == 7)) {
        selectedMenu = 7;
        needsRedraw = true;
    }

    ImGui::EndChild();

    ImGui::SameLine();

    // 右侧内容（8/10宽度）
    ImGui::BeginChild("SocksContentRight", ImVec2(0, 0), true);

    // 根据选中的菜单显示对应内容
    if (selectedMenu == 0) {
        // ===== 基本信息 =====
        ImGuiGBK::Text("基本信息");
        ImGui::Separator();

        ImGuiGBK::Text("实例ID: %s", info.id.c_str());
        ImGuiGBK::Text("实例名称: %s", info.name.c_str());
        ImGuiGBK::Text("实例类型: Socks转发 (支持WPE滤镜和二级代理)");
        ImGuiGBK::Text("监听端口: %d", info.port);

        const char* stateText = "";
        ImVec4 stateColor = ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
        switch (info.state) {
            case InstanceState::Running:
                stateText = "运行中";
                stateColor = ImVec4(0.0f, 1.0f, 0.0f, 1.0f);
                break;
            case InstanceState::Stopped:
                stateText = "已停止";
                stateColor = ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
                break;
            case InstanceState::Error:
                stateText = "错误";
                stateColor = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
                break;
            default:
                stateText = "未知";
                break;
        }
        ImGuiGBK::Text("状态: ");
        ImGui::SameLine();
        ImGui::TextColored(stateColor, "%s", stateText);

        ImGuiGBK::Text("当前连接数: %d", info.currentConnections);
        ImGuiGBK::Text("总连接数: %llu", info.totalPackets);
        ImGuiGBK::Text("总流量: %.2f KB", info.totalBytes / 1024.0);

        ImGui::Spacing();
        ImGui::TextDisabled("提示: WPE滤镜规则请在WPE滤镜页面中配置并指定生效实例");
    }
    else if (selectedMenu == 1) {
        // ===== 二级代理配置 =====
        ImGuiGBK::Text("二级代理配置");
        ImGui::Separator();

        static std::string lastInstanceId = "";
        static bool enableSecondaryProxy = false;
        static char secondaryProxyHost[256] = "";
        static int secondaryProxyPort = 1080;
        static char secondaryProxyUsername[256] = "";
        static char secondaryProxyPassword[256] = "";

        // 如果切换了实例，重新初始化
        if (lastInstanceId != info.id) {
            lastInstanceId = info.id;

            // 从数据库加载配置
            if (g_database) {
                std::string enableStr = g_database->GetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(info.id, "enableSecondaryProxy"), "0");
                enableSecondaryProxy = (enableStr == "1");

                std::string host = g_database->GetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(info.id, "secondaryProxyHost"), "127.0.0.1");
                strncpy_s(secondaryProxyHost, host.c_str(), sizeof(secondaryProxyHost) - 1);

                std::string portStr = g_database->GetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(info.id, "secondaryProxyPort"), "1080");
                secondaryProxyPort = std::stoi(portStr);

                std::string username = g_database->GetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(info.id, "secondaryProxyUsername"), "");
                strncpy_s(secondaryProxyUsername, username.c_str(), sizeof(secondaryProxyUsername) - 1);

                std::string password = g_database->GetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(info.id, "secondaryProxyPassword"), "");
                strncpy_s(secondaryProxyPassword, password.c_str(), sizeof(secondaryProxyPassword) - 1);
            } else {
                // 如果没有数据库，从 collector 读取
                enableSecondaryProxy = collector->IsSecondaryProxyEnabled();
                strncpy_s(secondaryProxyHost, collector->GetSecondaryProxyHost().c_str(), sizeof(secondaryProxyHost) - 1);
                secondaryProxyPort = collector->GetSecondaryProxyPort();
                strncpy_s(secondaryProxyUsername, collector->GetSecondaryProxyUsername().c_str(), sizeof(secondaryProxyUsername) - 1);
                strncpy_s(secondaryProxyPassword, collector->GetSecondaryProxyPassword().c_str(), sizeof(secondaryProxyPassword) - 1);
            }
        }

        if (ImGuiGBK::Checkbox("启用二级代理", &enableSecondaryProxy)) {
            collector->SetSecondaryProxy(enableSecondaryProxy, secondaryProxyHost, secondaryProxyPort,
                                         secondaryProxyUsername, secondaryProxyPassword);

            // 保存到数据库
            if (g_database) {
                g_database->SetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(info.id, "enableSecondaryProxy"),
                    enableSecondaryProxy ? "1" : "0");
                g_database->SetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(info.id, "secondaryProxyHost"),
                    secondaryProxyHost);
                g_database->SetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(info.id, "secondaryProxyPort"),
                    std::to_string(secondaryProxyPort));
                g_database->SetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(info.id, "secondaryProxyUsername"),
                    secondaryProxyUsername);
                g_database->SetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(info.id, "secondaryProxyPassword"),
                    secondaryProxyPassword);
            }

            needsRedraw = true;
        }

        ImGui::BeginDisabled(!enableSecondaryProxy);

        ImGuiGBK::Text("代理地址:");
        ImGui::SameLine();
        if (ImGui::InputText("##SecondaryProxyHost", secondaryProxyHost, sizeof(secondaryProxyHost))) {
            collector->SetSecondaryProxy(enableSecondaryProxy, secondaryProxyHost, secondaryProxyPort,
                                         secondaryProxyUsername, secondaryProxyPassword);
            if (g_database) {
                g_database->SetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(info.id, "secondaryProxyHost"),
                    secondaryProxyHost);
            }
        }

        ImGuiGBK::Text("代理端口:");
        ImGui::SameLine();
        if (ImGui::InputInt("##SecondaryProxyPort", &secondaryProxyPort)) {
            collector->SetSecondaryProxy(enableSecondaryProxy, secondaryProxyHost, secondaryProxyPort,
                                         secondaryProxyUsername, secondaryProxyPassword);
            if (g_database) {
                g_database->SetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(info.id, "secondaryProxyPort"),
                    std::to_string(secondaryProxyPort));
            }
        }

        ImGuiGBK::Text("用户名:");
        ImGui::SameLine();
        if (ImGui::InputText("##SecondaryProxyUsername", secondaryProxyUsername, sizeof(secondaryProxyUsername))) {
            collector->SetSecondaryProxy(enableSecondaryProxy, secondaryProxyHost, secondaryProxyPort,
                                         secondaryProxyUsername, secondaryProxyPassword);
            if (g_database) {
                g_database->SetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(info.id, "secondaryProxyUsername"),
                    secondaryProxyUsername);
            }
        }

        ImGuiGBK::Text("密码:");
        ImGui::SameLine();
        if (ImGui::InputText("##SecondaryProxyPassword", secondaryProxyPassword, sizeof(secondaryProxyPassword), ImGuiInputTextFlags_Password)) {
            collector->SetSecondaryProxy(enableSecondaryProxy, secondaryProxyHost, secondaryProxyPort,
                                         secondaryProxyUsername, secondaryProxyPassword);
            if (g_database) {
                g_database->SetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(info.id, "secondaryProxyPassword"),
                    secondaryProxyPassword);
            }
        }

        ImGui::EndDisabled();
    }
    else if (selectedMenu == 2) {
        // ===== 线程模型配置 =====
        ImGuiGBK::Text("线程模型配置");
        ImGui::Separator();

        const bool isRunning = (info.state == InstanceState::Running);

        // 线程模型选择
        static int threadPoolMode = 0; // 0=Traditional, 1=Blocking, 2=IOCP
        static bool threadModeInitialized = false;
        if (!threadModeInitialized) {
            auto currentMode = collector->GetThreadPoolMode();
            if (currentMode == PacketCollector::ThreadPoolMode::TRADITIONAL) threadPoolMode = 0;
            else if (currentMode == PacketCollector::ThreadPoolMode::BLOCKING) threadPoolMode = 1;
            else if (currentMode == PacketCollector::ThreadPoolMode::IOCP) threadPoolMode = 2;
            threadModeInitialized = true;
        }

        const char* modes[] = { "传统模式（每连接一个线程）", "阻塞式线程池", "IOCP高性能模式" };
    ImGuiGBK::Text("线程模型:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(250);

    ImGui::BeginDisabled(isRunning);
    if (ImGui::Combo("##SocksThreadMode", &threadPoolMode, modes, 3)) {
        needsRedraw = true;
    }
    ImGui::EndDisabled();

    if (isRunning) {
        ImGui::SameLine();
        ImGuiGBK::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "运行中无法更改");
    }

    ImGui::Spacing();

    // 应用线程模型按钮
    ImGui::BeginDisabled(isRunning);
    if (ImGuiGBK::Button("应用线程模型##ApplySocksThreadMode", ImVec2(160, 0))) {
        PacketCollector::ThreadPoolMode mode;
        if (threadPoolMode == 0) mode = PacketCollector::ThreadPoolMode::TRADITIONAL;
        else if (threadPoolMode == 1) mode = PacketCollector::ThreadPoolMode::BLOCKING;
        else mode = PacketCollector::ThreadPoolMode::IOCP;

        collector->SetThreadPoolMode(mode);

        // 保存到数据库
        if (g_database) {
            g_database->SetConfigValue(
                InstanceManager::MakeInstanceConfigKey(info.id, "threadPoolMode"),
                std::to_string(threadPoolMode));
        }

        AB_LOG_INFO("[Socks转发] 已应用线程模型: " + std::string(modes[threadPoolMode]));
        needsRedraw = true;
    }
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // 根据不同模式显示不同的参数配置
    if (threadPoolMode == 0) {
        // 传统模式说明
        ImGuiGBK::TextColored(ImVec4(0.8f, 0.8f, 1.0f, 1.0f), "传统模式特性:");
        ImGuiGBK::TextWrapped("• 每个连接创建独立线程");
        ImGuiGBK::TextWrapped("• 适合连接数较少的场景（< 100）");
        ImGuiGBK::TextWrapped("• 实现简单，调试方便");
        ImGuiGBK::TextWrapped("• 大量连接时会消耗较多系统资源");
    }
    else if (threadPoolMode == 1) {
        // 阻塞式线程池参数
        ImGuiGBK::TextColored(ImVec4(0.8f, 0.8f, 1.0f, 1.0f), "阻塞式线程池参数:");

        static int whitelistPoolSize = 10;
        static int normalPoolSize = 50;
        static bool poolSizeInitialized = false;
        if (!poolSizeInitialized) {
            whitelistPoolSize = 10;
            normalPoolSize = 50;
            poolSizeInitialized = true;
        }

        ImGuiGBK::Text("白名单线程数:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::BeginDisabled(isRunning);
        ImGui::InputInt("##SocksWlPool", &whitelistPoolSize);
        ImGui::EndDisabled();

        ImGuiGBK::Text("普通线程数:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::BeginDisabled(isRunning);
        ImGui::InputInt("##SocksNlPool", &normalPoolSize);
        ImGui::EndDisabled();

        ImGui::BeginDisabled(isRunning);
        if (ImGuiGBK::Button("应用线程池大小##ApplySocksPoolSize", ImVec2(160, 0))) {
            collector->SetThreadPoolSizes((std::max)(1, whitelistPoolSize), (std::max)(1, normalPoolSize));

            // 保存到数据库
            if (g_database) {
                g_database->SetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(info.id, "whitelistPoolSize"),
                    std::to_string(whitelistPoolSize));
                g_database->SetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(info.id, "normalPoolSize"),
                    std::to_string(normalPoolSize));
            }

            AB_LOG_INFO("[Socks转发] 已应用线程池大小");
            needsRedraw = true;
        }
        ImGui::EndDisabled();

        if (isRunning) {
            ImGuiGBK::Text("白名单处理: %d, 队列: %d",
                collector->GetWhitelistPoolProcessed(),
                collector->GetWhitelistQueueSize());
            ImGuiGBK::Text("普通处理: %d, 队列: %d",
                collector->GetNormalPoolProcessed(),
                collector->GetNormalQueueSize());
        }
    }
    else if (threadPoolMode == 2) {
        // IOCP参数
        ImGuiGBK::TextColored(ImVec4(0.8f, 0.8f, 1.0f, 1.0f), "IOCP高性能模式参数:");

        static int iocpMaxWhitelist = 2000;
        static int iocpMaxNormal = 500;
        static bool iocpParamsInitialized = false;
        if (!iocpParamsInitialized) {
            iocpMaxWhitelist = 2000;
            iocpMaxNormal = 500;
            iocpParamsInitialized = true;
        }

        ImGuiGBK::Text("白名单最大连接:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::BeginDisabled(isRunning);
        ImGui::InputInt("##SocksIocpWlMax", &iocpMaxWhitelist);
        ImGui::EndDisabled();

        ImGuiGBK::Text("普通最大连接:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::BeginDisabled(isRunning);
        ImGui::InputInt("##SocksIocpNlMax", &iocpMaxNormal);
        ImGui::EndDisabled();

        ImGui::BeginDisabled(isRunning);
        if (ImGuiGBK::Button("应用IOCP连接上限##ApplySocksIocpMax", ImVec2(180, 0))) {
            collector->SetIOCPMaxConnections((std::max)(1, iocpMaxWhitelist), (std::max)(1, iocpMaxNormal));

            // 保存到数据库
            if (g_database) {
                g_database->SetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(info.id, "iocpMaxWhitelist"),
                    std::to_string(iocpMaxWhitelist));
                g_database->SetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(info.id, "iocpMaxNormal"),
                    std::to_string(iocpMaxNormal));
            }

            AB_LOG_INFO("[Socks转发] 已应用IOCP连接上限");
            needsRedraw = true;
        }
        ImGui::EndDisabled();

        if (isRunning) {
            ImGuiGBK::Text("白名单连接: %d / %d",
                collector->GetIOCPWhitelistConnCount(),
                iocpMaxWhitelist);
            ImGuiGBK::Text("普通连接: %d / %d",
                collector->GetIOCPNormalConnCount(),
                iocpMaxNormal);
        }

        ImGui::Spacing();
        ImGuiGBK::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "推荐用于代理转发场景");
        ImGuiGBK::TextWrapped("• 异步I/O，高并发性能");
        ImGuiGBK::TextWrapped("• 支持数千个并发连接");
        ImGuiGBK::TextWrapped("• 自动处理长连接保活");
        ImGuiGBK::TextWrapped("• 二级代理连接自动超时控制");
    }
    }
    else if (selectedMenu == 3) {
        // ===== SOCKS5认证配置 =====
        ImGuiGBK::Text("SOCKS5认证配置");
        ImGui::Separator();

        static bool enableSocks5Auth = collector->IsSocks5AuthEnabled();
        static std::string selectedSocks5PoolId = "";

        // 初始化：从配置中读取已保存的账号库ID和认证状态
        static bool socks5AuthInitialized = false;
        if (!socks5AuthInitialized) {
            if (g_database) {
                // 🔥 从数据库读取认证状态
                std::string authEnabledStr = g_database->GetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(info.id, "enableSocks5Auth"), "0");
                enableSocks5Auth = (authEnabledStr == "1");
                collector->SetSocks5Auth(enableSocks5Auth);  // 同步到collector

                selectedSocks5PoolId = g_database->GetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(info.id, "accountSourceInstanceId"), "");
            }
            socks5AuthInitialized = true;
        }

        if (ImGuiGBK::Checkbox("启用SOCKS5认证", &enableSocks5Auth)) {
            collector->SetSocks5Auth(enableSocks5Auth);  // 🔥 同步设置认证状态

            // 🔥 保存认证状态到数据库
            if (g_database) {
                g_database->SetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(info.id, "enableSocks5Auth"),
                    enableSocks5Auth ? "1" : "0");
            }

            if (!enableSocks5Auth) {
                // 禁用认证时，清除外部账号源
                collector->SetExternalAccountSource(nullptr);
                selectedSocks5PoolId.clear();

                // 清除配置
                if (g_database) {
                    g_database->SetConfigValue(
                        InstanceManager::MakeInstanceConfigKey(info.id, "accountSourceInstanceId"), "");
                }
            } else {
                // 启用认证时，如果已选择账号库，重新绑定
                if (!selectedSocks5PoolId.empty()) {
                    auto socks5Pools = InstanceManager::GetInstance().GetSocks5PoolInstances();
                    for (auto* pool : socks5Pools) {
                        if (pool && pool->GetId() == selectedSocks5PoolId) {
                            PacketCollector* poolCollector = pool->GetInternalCollector();
                            if (poolCollector) {
                                collector->SetExternalAccountSource(poolCollector);
                            }
                            break;
                        }
                    }
                }
            }
            needsRedraw = true;
        }

        // 🔥 移除禁用逻辑，允许用户随时选择账号库
        // ImGui::BeginDisabled(!enableSocks5Auth);

        ImGuiGBK::Text("选择账号实例:");
        ImGui::SameLine();

        // 获取所有SOCKS5账号库实例
        auto socks5Pools = InstanceManager::GetInstance().GetSocks5PoolInstances();

        if (ImGui::BeginCombo("##Socks5PoolSelect", selectedSocks5PoolId.empty() ? "请选择账号实例" : selectedSocks5PoolId.c_str())) {
            for (auto* pool : socks5Pools) {
                if (!pool) continue;

                std::string poolId = pool->GetId();
                std::string poolName = pool->GetName();
                std::string displayText = poolId + " (" + poolName + ")";

                bool isSelected = (selectedSocks5PoolId == poolId);
                if (ImGui::Selectable(displayText.c_str(), isSelected)) {
                    selectedSocks5PoolId = poolId;

                    // 设置外部账号源：从选中的账号库实例获取账号
                    PacketCollector* poolCollector = pool->GetInternalCollector();
                    if (poolCollector) {
                        collector->SetExternalAccountSource(poolCollector);
                        // 🔥 不自动启用认证，由用户通过复选框控制
                        // collector->SetSocks5Auth(true);  // 已删除

                        // 保存账号库ID到配置（持久化）
                        if (g_database) {
                            g_database->SetConfigValue(
                                InstanceManager::MakeInstanceConfigKey(info.id, "accountSourceInstanceId"), poolId);
                        }

                        AB_LOG_INFO("[实例管理] Socks转发实例 " + info.id + " 绑定到账号库: " + poolId);
                        needsRedraw = true;
                }
            }
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    if (enableSocks5Auth) {
        if (!selectedSocks5PoolId.empty()) {
            ImGuiGBK::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "认证已启用");
            ImGuiGBK::Text("绑定账号库: %s", selectedSocks5PoolId.c_str());

            // 显示账号数量
            auto* externalSource = collector->GetExternalAccountSource();
            if (externalSource) {
                ImGuiGBK::Text("可用账号数: %d", static_cast<int>(externalSource->GetAllAccounts().size()));
            }
        } else {
            ImGuiGBK::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "请选择账号实例");
        }
    } else {
        ImGuiGBK::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "认证已禁用");
    }

    // 🔥 移除禁用逻辑的结束标记
    // ImGui::EndDisabled();
    }
    else if (selectedMenu == 4) {
        // ===== 分包处理配置 =====
        ImGuiGBK::Text("分包处理配置");
        ImGui::Separator();

        static bool enablePacketSplit = true;
        static std::vector<int> targetPorts;
        static char newPortInput[32] = "";
        static bool packetSplitInitialized = false;

        // 初始化：从配置中读取
        if (!packetSplitInitialized) {
            if (g_database) {
                std::string enabledStr = g_database->GetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(info.id, "enablePacketSplit"), "1");
                enablePacketSplit = (enabledStr == "1");

                std::string portsStr = g_database->GetConfigValue(
                InstanceManager::MakeInstanceConfigKey(info.id, "packetSplitPorts"), "");

            // ���析端口列表（逗号分隔）
            targetPorts.clear();
            if (!portsStr.empty()) {
                size_t pos = 0;
                while (pos < portsStr.size()) {
                    size_t comma = portsStr.find(',', pos);
                    if (comma == std::string::npos) comma = portsStr.size();
                    std::string portStr = portsStr.substr(pos, comma - pos);
                    if (!portStr.empty()) {
                        try {
                            int port = std::stoi(portStr);
                            if (port > 0 && port <= 65535) {
                                targetPorts.push_back(port);
                            }
                        } catch (...) {}
                    }
                    pos = comma + 1;
                }
            }
        }
        packetSplitInitialized = true;
    }

    if (ImGuiGBK::Checkbox("启用分包处理", &enablePacketSplit)) {
        // 应用到运行中的实例
        if (g_database) {
            g_database->SetConfigValue(
                InstanceManager::MakeInstanceConfigKey(info.id, "enablePacketSplit"),
                enablePacketSplit ? "1" : "0");
        }

        // 🔥 通知collector更新配置
        auto* instance = InstanceManager::GetInstance().GetSocksForwardInstance(info.id);
        if (instance && instance->GetCollector()) {
            instance->GetCollector()->SetPacketSplitEnabled(enablePacketSplit);
        }

        AB_LOG_INFO("[Socks转发] 分包处理已" + std::string(enablePacketSplit ? "启用" : "禁用"));
        needsRedraw = true;
    }

    ImGui::Spacing();
    ImGuiGBK::TextWrapped("说明: 分包处理用于解析游戏协议数据包，启用后WPE滤镜和触发器才能正常工作。");
    ImGui::Spacing();

    // 🔥 新增：对不分包流量也应用WPE滤镜
    static std::string lastInstanceIdForWpeOnNonSplit = "";
    static bool applyWpeOnNonSplitTraffic = false;

    if (lastInstanceIdForWpeOnNonSplit != info.id) {
        lastInstanceIdForWpeOnNonSplit = info.id;
        if (g_database) {
            std::string value = g_database->GetConfigValue(
                InstanceManager::MakeInstanceConfigKey(info.id, "applyWpeOnNonSplitTraffic"), "0");
            applyWpeOnNonSplitTraffic = (value == "1");
        }
    }

    if (ImGuiGBK::Checkbox("对不分包流量也应用WPE滤镜", &applyWpeOnNonSplitTraffic)) {
        if (g_database) {
            g_database->SetConfigValue(
                InstanceManager::MakeInstanceConfigKey(info.id, "applyWpeOnNonSplitTraffic"),
                applyWpeOnNonSplitTraffic ? "1" : "0");
        }

        // 🔥 应用到运行中的实例
        auto* instance = InstanceManager::GetInstance().GetSocksForwardInstance(info.id);
        if (instance && instance->GetCollector()) {
            instance->GetCollector()->SetApplyWpeOnNonSplitTraffic(applyWpeOnNonSplitTraffic);
        }

        AB_LOG_INFO("[Socks转发] 对不分包流量应用WPE滤镜已" + std::string(applyWpeOnNonSplitTraffic ? "启用" : "禁用"));
        needsRedraw = true;
    }

    ImGui::Spacing();
    ImGuiGBK::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
        "勾选后，即使不进行分包处理的流量（如HTTPS）也会应用WPE滤镜");
    ImGui::Spacing();

    ImGui::BeginDisabled(!enablePacketSplit);

    ImGuiGBK::Text("生效的目标端口（仅对这些端口启用分包处理）:");
    ImGui::Spacing();

    // 端口列表
    if (ImGui::BeginTable("PacketSplitPortsTable", 3,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
        ImVec2(0, 120)))
    {
        ImGui::TableSetupColumn(ImGuiText::U("启用"), ImGuiTableColumnFlags_WidthFixed, 50);
        ImGui::TableSetupColumn(ImGuiText::U("端口"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(ImGuiText::U("操作"), ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < targetPorts.size(); i++) {
            ImGui::TableNextRow();
            ImGui::PushID(static_cast<int>(i));

            // 启用状态（暂时都是启用的）
            ImGui::TableSetColumnIndex(0);
            bool enabled = true;
            ImGui::Checkbox("##enabled", &enabled);

            // 端口号
            ImGui::TableSetColumnIndex(1);
            ImGuiGBK::Text("%d", targetPorts[i]);

            // 删除按钮
            ImGui::TableSetColumnIndex(2);
            if (ImGuiGBK::Button("删除##del", ImVec2(50, 0))) {
                targetPorts.erase(targetPorts.begin() + i);

                // 保存到数据库
                if (g_database) {
                    std::string portsStr;
                    for (size_t j = 0; j < targetPorts.size(); j++) {
                        if (j > 0) portsStr += ",";
                        portsStr += std::to_string(targetPorts[j]);
                    }
                    g_database->SetConfigValue(
                        InstanceManager::MakeInstanceConfigKey(info.id, "packetSplitPorts"),
                        portsStr);
                }

                // 🔥 应用到运行中的实例
                auto* instance = InstanceManager::GetInstance().GetSocksForwardInstance(info.id);
                if (instance && instance->GetCollector()) {
                    instance->GetCollector()->SetPacketSplitPorts(targetPorts);
                }

                needsRedraw = true;
            }

            ImGui::PopID();
        }

        ImGui::EndTable();
    }

    ImGui::Spacing();

    // 添加端口
    ImGuiGBK::Text("添加端口:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::InputText("##NewPort", newPortInput, sizeof(newPortInput), ImGuiInputTextFlags_CharsDecimal);
    ImGui::SameLine();
    if (ImGuiGBK::Button("添加##addPort", ImVec2(60, 0))) {
        try {
            int port = std::stoi(newPortInput);
            if (port > 0 && port <= 65535) {
                // 检查是否已存在
                bool exists = false;
                for (int p : targetPorts) {
                    if (p == port) {
                        exists = true;
                        break;
                    }
                }

                if (!exists) {
                    targetPorts.push_back(port);

                    // 保存到数据库
                    if (g_database) {
                        std::string portsStr;
                        for (size_t j = 0; j < targetPorts.size(); j++) {
                            if (j > 0) portsStr += ",";
                            portsStr += std::to_string(targetPorts[j]);
                        }
                        g_database->SetConfigValue(
                            InstanceManager::MakeInstanceConfigKey(info.id, "packetSplitPorts"),
                            portsStr);
                    }

                    // 🔥 应用到运行中的实例
                    auto* instance = InstanceManager::GetInstance().GetSocksForwardInstance(info.id);
                    if (instance && instance->GetCollector()) {
                        instance->GetCollector()->SetPacketSplitPorts(targetPorts);
                    }

                    newPortInput[0] = '\0';
                    needsRedraw = true;
                } else {
                    MessageBoxA(g_mainHwnd, "该端口已存在", "提示", MB_OK | MB_ICONINFORMATION);
                }
            } else {
                MessageBoxA(g_mainHwnd, "端口号必须在1-65535之间", "错误", MB_OK | MB_ICONERROR);
            }
        } catch (...) {
            MessageBoxA(g_mainHwnd, "请输入有效的端口号", "错误", MB_OK | MB_ICONERROR);
        }
    }

    ImGui::Spacing();
    ImGuiGBK::TextColored(ImVec4(0.8f, 0.8f, 0.0f, 1.0f), "提示:");
    ImGuiGBK::TextWrapped("• 如果端口列表为空，则对所有端口都启用分包处理");
    ImGuiGBK::TextWrapped("• 如果端口列表不为空，则只对列表中的端口启用分包处理");
    ImGuiGBK::TextWrapped("• 对于HTTPS/HTTP等非游戏流量，建议不添加到列表中，以提高转发性能");

    ImGui::EndDisabled();
    }
    else if (selectedMenu == 5) {
        // ===== 流量过滤配置 =====
        ImGuiGBK::Text("流量过滤配置");
        ImGui::Separator();

        static bool enableTrafficFilter = false;
        static bool enableSniSniffing = false;
        static std::vector<TrafficFilterRule> filterRules;
        static int selectedRuleIndex = -1;
        static bool trafficFilterInitialized = false;
        static std::string lastInitializedInstanceId = "";  // 🔥 新增：记录上次初始化的实例ID
        static bool showAddRuleWindow = false;
        static bool showEditRuleWindow = false;

        // 🔥 辅助函数：保存规则到数据库（使用 JSON 库）
        auto SaveRulesToDatabase = [&]() {
            if (!g_database) return;

            try {
                // 使用 nlohmann/json 库序列化规则
                nlohmann::json rulesJson = nlohmann::json::array();

                for (const auto& rule : filterRules) {
                    nlohmann::json ruleJson;
                    ruleJson["id"] = rule.id;
                    ruleJson["type"] = static_cast<int>(rule.type);
                    ruleJson["value1"] = rule.value1;
                    ruleJson["value2"] = rule.value2;
                    ruleJson["enabled"] = rule.enabled;
                    ruleJson["description"] = rule.description;
                    rulesJson.push_back(ruleJson);
                }

                std::string rulesJsonStr = rulesJson.dump();
                g_database->SetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(info.id, "trafficFilterRules"),
                    rulesJsonStr);

                AB_LOG_INFO("[流量过滤] 规则已保存到数据库，共 " + std::to_string(filterRules.size()) + " 条");
            } catch (const std::exception& e) {
                AB_LOG_ERROR("[流量过滤] 保存规则失败: " + std::string(e.what()));
            }
        };

        // 初始化：从配置中读取（🔥 修复：切换实例时重新加载）
        if (!trafficFilterInitialized || lastInitializedInstanceId != info.id) {
            enableTrafficFilter = collector->IsTrafficFilterEnabled();
            enableSniSniffing = collector->IsSNISniffingEnabled();
            filterRules = collector->GetAllTrafficRules();
            trafficFilterInitialized = true;
            lastInitializedInstanceId = info.id;  // 🔥 记录当前实例ID
        }

        // 启用流量过滤开关
        if (ImGuiGBK::Checkbox("启用流量过滤", &enableTrafficFilter)) {
            collector->SetTrafficFilterEnabled(enableTrafficFilter);

            if (g_database) {
                g_database->SetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(info.id, "enableTrafficFilter"),
                    enableTrafficFilter ? "1" : "0");
            }

            AB_LOG_INFO("[流量过滤] " + std::string(enableTrafficFilter ? "已启用" : "已禁用"));
            needsRedraw = true;
        }

        ImGui::SameLine();
        ImGui::Spacing();
        ImGui::SameLine();

        // 启用SNI嗅探开关
        if (ImGuiGBK::Checkbox("启用SNI嗅探", &enableSniSniffing)) {
            collector->SetSNISniffingEnabled(enableSniSniffing);

            if (g_database) {
                g_database->SetConfigValue(
                    InstanceManager::MakeInstanceConfigKey(info.id, "enableSniSniffing"),
                    enableSniSniffing ? "1" : "0");
            }

            AB_LOG_INFO("[流量过滤] SNI嗅探" + std::string(enableSniSniffing ? "已启用" : "已禁用"));
            needsRedraw = true;
        }

        ImGui::Spacing();
        ImGuiGBK::TextColored(ImVec4(0.8f, 0.8f, 0.0f, 1.0f), "说明:");
        ImGuiGBK::TextWrapped("• 启用流量过滤后，只有匹配规则的流量才能通过");
        ImGuiGBK::TextWrapped("• SNI嗅探用于识别HTTPS流量的真实域名（需配合'指定端口嗅探指定域名'规则使用）");
        ImGui::Separator();

        // 操作按钮
        if (ImGuiGBK::Button("添加规则##addRule", ImVec2(100, 0))) {
            showAddRuleWindow = true;
            needsRedraw = true;
        }

        ImGui::SameLine();
        ImGui::BeginDisabled(selectedRuleIndex < 0 || selectedRuleIndex >= static_cast<int>(filterRules.size()));
        if (ImGuiGBK::Button("编辑##editRule", ImVec2(80, 0))) {
            showEditRuleWindow = true;
            needsRedraw = true;
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled(selectedRuleIndex < 0 || selectedRuleIndex >= static_cast<int>(filterRules.size()));
        if (ImGuiGBK::Button("删除##deleteRule", ImVec2(80, 0))) {
            if (MessageBoxA(g_mainHwnd, "确定要删除选中的规则吗?", "确认删除", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                int ruleId = filterRules[selectedRuleIndex].id;
                collector->RemoveTrafficRule(ruleId);
                filterRules = collector->GetAllTrafficRules();
                selectedRuleIndex = -1;

                // 🔥 保存到数据库
                SaveRulesToDatabase();

                AB_LOG_INFO("[流量过滤] 规则已删除");
                needsRedraw = true;
            }
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGuiGBK::Button("清空所有##clearRules", ImVec2(100, 0))) {
            if (MessageBoxA(g_mainHwnd, "确定要清空所有规则吗?", "确认清空", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                collector->ClearAllTrafficRules();
                filterRules.clear();
                selectedRuleIndex = -1;

                // 🔥 保存到数据库
                SaveRulesToDatabase();

                AB_LOG_INFO("[流量过滤] 所有规则已清空");
                needsRedraw = true;
            }
        }

        ImGui::Spacing();

        // 规则列表
        ImGuiGBK::Text("过滤规则列表 (共 %d 条):", static_cast<int>(filterRules.size()));

        if (ImGui::BeginTable("TrafficFilterRulesTable", 5,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
            ImVec2(0, 300)))
        {
            ImGui::TableSetupColumn(ImGuiText::U("启用"), ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableSetupColumn(ImGuiText::U("类型"), ImGuiTableColumnFlags_WidthFixed, 120);
            ImGui::TableSetupColumn(ImGuiText::U("值"), ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn(ImGuiText::U("SNI域名"), ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn(ImGuiText::U("ID"), ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableHeadersRow();

            for (size_t i = 0; i < filterRules.size(); i++) {
                ImGui::TableNextRow();
                ImGui::PushID(static_cast<int>(i));

                bool isSelected = (selectedRuleIndex == static_cast<int>(i));

                // 启用状态
                ImGui::TableSetColumnIndex(0);
                bool enabled = filterRules[i].enabled;
                if (ImGui::Checkbox("##enabled", &enabled)) {
                    filterRules[i].enabled = enabled;
                    collector->UpdateTrafficRule(filterRules[i]);

                    // 🔥 保存到数据库
                    SaveRulesToDatabase();

                    needsRedraw = true;
                }

                // 类型
                ImGui::TableSetColumnIndex(1);
                const char* typeText = "";
                switch (filterRules[i].type) {
                    case TrafficRuleType::PORT_MATCH: typeText = "端口匹配"; break;
                    case TrafficRuleType::DOMAIN_MATCH: typeText = "域名匹配"; break;
                    case TrafficRuleType::IP_MATCH: typeText = "IP匹配"; break;
                    case TrafficRuleType::PORT_DOMAIN_SNI: typeText = "端口+SNI域名"; break;
                }
                if (ImGui::Selectable(typeText, isSelected, ImGuiSelectableFlags_SpanAllColumns)) {
                    selectedRuleIndex = static_cast<int>(i);
                    needsRedraw = true;
                }

                // 值
                ImGui::TableSetColumnIndex(2);
                ImGuiGBK::Text("%s", filterRules[i].value1.c_str());

                // SNI域名（仅对 PORT_DOMAIN_SNI 显示）
                ImGui::TableSetColumnIndex(3);
                if (filterRules[i].type == TrafficRuleType::PORT_DOMAIN_SNI) {
                    ImGuiGBK::Text("%s", filterRules[i].value2.c_str());
                } else {
                    ImGuiGBK::Text("-");
                }

                // ID
                ImGui::TableSetColumnIndex(4);
                ImGuiGBK::Text("%d", filterRules[i].id);

                ImGui::PopID();
            }

            ImGui::EndTable();
        }

        // ===== 添加规则弹窗 =====
        if (showAddRuleWindow) {
            ImGui::OpenPopup("添加流量过滤规则");
            showAddRuleWindow = false;
        }

        static int newRuleType = 0;  // 0=端口, 1=域名, 2=IP, 3=端口+SNI域名
        static char newRuleValue[256] = "";
        static char newRuleSniDomain[256] = "";

        if (ImGui::BeginPopupModal("添加流量过滤规则", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGuiGBK::Text("规则类型:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(200);
            const char* ruleTypes[] = { "端口匹配", "域名匹配", "IP匹配", "端口+SNI域名" };
            ImGui::Combo("##RuleType", &newRuleType, ruleTypes, IM_ARRAYSIZE(ruleTypes));

            ImGui::Spacing();

            if (newRuleType == 0) {
                ImGuiGBK::Text("端口号:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(200);
                ImGui::InputText("##RuleValue", newRuleValue, sizeof(newRuleValue), ImGuiInputTextFlags_CharsDecimal);
            } else if (newRuleType == 1) {
                ImGuiGBK::Text("域名:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(300);
                ImGui::InputText("##RuleValue", newRuleValue, sizeof(newRuleValue));
                ImGuiGBK::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "例如: example.com");
            } else if (newRuleType == 2) {
                ImGuiGBK::Text("IP地址:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(200);
                ImGui::InputText("##RuleValue", newRuleValue, sizeof(newRuleValue));
                ImGuiGBK::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "例如: 192.168.1.1");
            } else if (newRuleType == 3) {
                ImGuiGBK::Text("端口号:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(200);
                ImGui::InputText("##RuleValue", newRuleValue, sizeof(newRuleValue), ImGuiInputTextFlags_CharsDecimal);

                ImGuiGBK::Text("SNI域名:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(300);
                ImGui::InputText("##RuleSniDomain", newRuleSniDomain, sizeof(newRuleSniDomain));
                ImGuiGBK::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "例如: example.com (需启用SNI嗅探)");
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            if (ImGuiGBK::Button("确定##OkAddRule", ImVec2(120, 0))) {
                bool valid = true;
                std::string errorMsg;

                // 验证输入
                if (strlen(newRuleValue) == 0) {
                    valid = false;
                    errorMsg = "请输入规则值";
                } else if (newRuleType == 0 || newRuleType == 3) {
                    // 验证端口号
                    try {
                        int port = std::stoi(newRuleValue);
                        if (port < 1 || port > 65535) {
                            valid = false;
                            errorMsg = "端口号必须在1-65535之间";
                        }
                    } catch (...) {
                        valid = false;
                        errorMsg = "请输入有效的端口号";
                    }

                    if (valid && newRuleType == 3 && strlen(newRuleSniDomain) == 0) {
                        valid = false;
                        errorMsg = "请输入SNI域名";
                    }
                }

                if (valid) {
                    TrafficFilterRule rule;
                    rule.id = static_cast<int>(filterRules.size()) + 1;
                    rule.type = static_cast<TrafficRuleType>(newRuleType);
                    rule.value1 = newRuleValue;
                    rule.value2 = (newRuleType == 3) ? newRuleSniDomain : "";
                    rule.enabled = true;

                    collector->AddTrafficRule(rule);
                    filterRules = collector->GetAllTrafficRules();

                    // 🔥 保存到数据库
                    SaveRulesToDatabase();

                    AB_LOG_INFO("[流量过滤] 已添加规则: " + rule.value1);

                    // 清空输入
                    newRuleValue[0] = '\0';
                    newRuleSniDomain[0] = '\0';
                    newRuleType = 0;

                    ImGui::CloseCurrentPopup();
                    needsRedraw = true;
                } else {
                    MessageBoxA(g_mainHwnd, errorMsg.c_str(), "输入错误", MB_OK | MB_ICONERROR);
                }
            }

            ImGui::SameLine();
            if (ImGuiGBK::Button("取消##CancelAddRule", ImVec2(120, 0))) {
                newRuleValue[0] = '\0';
                newRuleSniDomain[0] = '\0';
                newRuleType = 0;
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }

        // ===== 编辑规则弹窗 =====
        static bool editDataLoaded = false;
        if (showEditRuleWindow && selectedRuleIndex >= 0 && selectedRuleIndex < static_cast<int>(filterRules.size())) {
            ImGui::OpenPopup("编辑流量过滤规则");

            // 加载当前规则数据
            if (!editDataLoaded) {
                const auto& rule = filterRules[selectedRuleIndex];
                newRuleType = static_cast<int>(rule.type);
                strncpy_s(newRuleValue, rule.value1.c_str(), sizeof(newRuleValue) - 1);
                strncpy_s(newRuleSniDomain, rule.value2.c_str(), sizeof(newRuleSniDomain) - 1);
                editDataLoaded = true;
            }

            showEditRuleWindow = false;
        }

        if (ImGui::BeginPopupModal("编辑流量过滤规则", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGuiGBK::Text("规则类型:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(200);
            const char* ruleTypes[] = { "端口匹配", "域名匹配", "IP匹配", "端口+SNI域名" };
            ImGui::Combo("##EditRuleType", &newRuleType, ruleTypes, IM_ARRAYSIZE(ruleTypes));

            ImGui::Spacing();

            if (newRuleType == 0) {
                ImGuiGBK::Text("端口号:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(200);
                ImGui::InputText("##EditRuleValue", newRuleValue, sizeof(newRuleValue), ImGuiInputTextFlags_CharsDecimal);
            } else if (newRuleType == 1) {
                ImGuiGBK::Text("域名:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(300);
                ImGui::InputText("##EditRuleValue", newRuleValue, sizeof(newRuleValue));
            } else if (newRuleType == 2) {
                ImGuiGBK::Text("IP地址:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(200);
                ImGui::InputText("##EditRuleValue", newRuleValue, sizeof(newRuleValue));
            } else if (newRuleType == 3) {
                ImGuiGBK::Text("端口号:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(200);
                ImGui::InputText("##EditRuleValue", newRuleValue, sizeof(newRuleValue), ImGuiInputTextFlags_CharsDecimal);

                ImGuiGBK::Text("SNI域名:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(300);
                ImGui::InputText("##EditRuleSniDomain", newRuleSniDomain, sizeof(newRuleSniDomain));
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            if (ImGuiGBK::Button("确定##OkEditRule", ImVec2(120, 0))) {
                bool valid = true;
                std::string errorMsg;

                if (strlen(newRuleValue) == 0) {
                    valid = false;
                    errorMsg = "请输入规则值";
                } else if (newRuleType == 0 || newRuleType == 3) {
                    try {
                        int port = std::stoi(newRuleValue);
                        if (port < 1 || port > 65535) {
                            valid = false;
                            errorMsg = "端口号必须在1-65535之间";
                        }
                    } catch (...) {
                        valid = false;
                        errorMsg = "请输入有效的端口号";
                    }

                    if (valid && newRuleType == 3 && strlen(newRuleSniDomain) == 0) {
                        valid = false;
                        errorMsg = "请输入SNI域名";
                    }
                }

                if (valid && selectedRuleIndex >= 0 && selectedRuleIndex < static_cast<int>(filterRules.size())) {
                    TrafficFilterRule& rule = filterRules[selectedRuleIndex];
                    rule.type = static_cast<TrafficRuleType>(newRuleType);
                    rule.value1 = newRuleValue;
                    rule.value2 = (newRuleType == 3) ? newRuleSniDomain : "";

                    collector->UpdateTrafficRule(rule);
                    filterRules = collector->GetAllTrafficRules();

                    // 🔥 保存到数据库
                    SaveRulesToDatabase();

                    AB_LOG_INFO("[流量过滤] 已更新规则: " + rule.value1);

                    newRuleValue[0] = '\0';
                    newRuleSniDomain[0] = '\0';
                    newRuleType = 0;
                    editDataLoaded = false;  // 重置标志

                    ImGui::CloseCurrentPopup();
                    needsRedraw = true;
                } else if (!valid) {
                    MessageBoxA(g_mainHwnd, errorMsg.c_str(), "输入错误", MB_OK | MB_ICONERROR);
                }
            }

            ImGui::SameLine();
            if (ImGuiGBK::Button("取消##CancelEditRule", ImVec2(120, 0))) {
                newRuleValue[0] = '\0';
                newRuleSniDomain[0] = '\0';
                newRuleType = 0;
                editDataLoaded = false;  // 重置标志
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }
    else if (selectedMenu == 6) {
        // ===== 用户滤镜配置 =====
        ImGuiGBK::Text("用户滤镜配置");
        ImGui::Separator();

        // 🔥 检查实例是否已启动
        if (!collector) {
            ImGuiGBK::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "请先启动实例才能配置用户滤镜");
            ImGui::Spacing();
            if (ImGuiGBK::Button("返回##BackFromUserFilter", ImVec2(100, 0))) {
                selectedMenu = 0;
                needsRedraw = true;
            }
        } else {
            static bool enableUserFilterMode = false;
            static int httpPort = 8080;
            static bool userFilterInitialized = false;
            static bool httpServerRunning = false;
            static std::string lastInstanceId = "";  // 🔥 记录上次的实例ID

            // 初始化配置（当切换实例时重新初始化）
            if (!userFilterInitialized || lastInstanceId != info.id) {
                // 🔥 直接从数据库读取配置，而不是从 collector 读取
                // 因为实例可能未启动，或者配置还没有加载到 collector
                if (g_database) {
                    std::string enableStr = g_database->GetConfigValue(
                        "instance_" + info.id + "_enableUserFilterMode", "0");
                    enableUserFilterMode = (enableStr == "1");

                    std::string portStr = g_database->GetConfigValue(
                        "instance_" + info.id + "_userFilterHttpPort", "8080");
                    httpPort = std::stoi(portStr);
                }

                // 如果实例已启动，同步运行状态
                if (collector) {
                    httpServerRunning = collector->IsUserFilterHttpServerRunning();
                } else {
                    httpServerRunning = false;
                }

                userFilterInitialized = true;
                lastInstanceId = info.id;  // 🔥 更新实例ID
            }

        ImGuiGBK::Text("说明：启用用户滤镜模式后，每个SOCKS用户可以通过网页管理自己的WPE滤镜配置。");
        ImGui::Spacing();

        // 启用用户滤镜模式
        if (ImGui::Checkbox("##EnableUserFilterMode", &enableUserFilterMode)) {
            collector->SetUserFilterMode(enableUserFilterMode);
            g_database->SetConfigValue("instance_" + info.id + "_enableUserFilterMode",
                                      enableUserFilterMode ? "1" : "0");

            // 🔥 方案1：启用时自动启动HTTP服务器，禁用时自动停止
            if (enableUserFilterMode) {
                // 启用用户滤镜模式，自动启动HTTP服务器
                if (collector->StartUserFilterHttpServer()) {
                    httpServerRunning = true;
                    AB_LOG_INFO("[用户滤镜] 已启用用户滤镜模式并启动HTTP服务器，端口: " + std::to_string(httpPort));
                } else {
                    MessageBoxA(g_mainHwnd, "HTTP服务器启动失败，请检查端口是否被占用", "错误", MB_OK | MB_ICONERROR);
                }
            } else {
                // 禁用用户滤镜模式，自动停止HTTP服务器
                if (httpServerRunning) {
                    collector->StopUserFilterHttpServer();
                    httpServerRunning = false;
                    AB_LOG_INFO("[用户滤镜] 已禁用用户滤镜模式并停止HTTP服务器");
                }
            }

            needsRedraw = true;
        }
        ImGui::SameLine();
        ImGuiGBK::Text("启用用户滤镜模式");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // HTTP服务器配置
        ImGuiGBK::Text("HTTP服务器配置");
        ImGui::Spacing();

        ImGuiGBK::Text("监听端口:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150);
        if (ImGui::InputInt("##HttpPort", &httpPort, 1, 100)) {
            if (httpPort < 1) httpPort = 1;
            if (httpPort > 65535) httpPort = 65535;
        }
        ImGui::SameLine();
        if (ImGuiGBK::Button("保存端口", ImVec2(100, 0))) {
            // 保存端口到数据库
            collector->SetUserFilterHttpPort(httpPort);
            g_database->SetConfigValue("instance_" + info.id + "_userFilterHttpPort",
                                      std::to_string(httpPort));

            // 🔥 如果用户滤镜模式已启用且HTTP服务器正在运行，需要重启服务器以应用新端口
            if (enableUserFilterMode && httpServerRunning) {
                collector->StopUserFilterHttpServer();
                if (collector->StartUserFilterHttpServer()) {
                    httpServerRunning = true;
                    AB_LOG_INFO("[用户滤镜] HTTP服务器已用新端口重启: " + std::to_string(httpPort));
                } else {
                    httpServerRunning = false;
                    MessageBoxA(g_mainHwnd, "HTTP服务器重启失败，请检查端口是否被占用", "错误", MB_OK | MB_ICONERROR);
                }
            } else {
                AB_LOG_INFO("[用户滤镜] 端口已保存: " + std::to_string(httpPort) + "，将在下次启动时生效");
            }

            needsRedraw = true;
        }

        ImGui::Spacing();

        // HTTP服务器状态和控制按钮
        ImGuiGBK::Text("服务器状态: %s", httpServerRunning ? "运行中" : "已停止");

        ImGui::SameLine();

        // 🔥 添加启动/停止按钮
        if (httpServerRunning) {
            if (ImGuiGBK::Button("停止服务器##StopHttpServer", ImVec2(100, 0))) {
                collector->StopUserFilterHttpServer();
                httpServerRunning = false;
                AB_LOG_INFO("[用户滤镜] HTTP服务器已手动停止");
                needsRedraw = true;
            }
        } else {
            if (ImGuiGBK::Button("启动服务器##StartHttpServer", ImVec2(100, 0))) {
                if (collector->StartUserFilterHttpServer()) {
                    httpServerRunning = true;
                    AB_LOG_INFO("[用户滤镜] HTTP服务器已手动启动，端口: " + std::to_string(httpPort));
                } else {
                    MessageBoxA(g_mainHwnd, "HTTP服务器启动失败，请检查端口是否被占用", "错误", MB_OK | MB_ICONERROR);
                }
                needsRedraw = true;
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // 访问地址
        ImGuiGBK::Text("用户访问地址:");
        ImGui::Spacing();

        std::string accessUrl = "http://127.0.0.1:" + std::to_string(httpPort);
        ImGuiGBK::Text("本地访问: %s", accessUrl.c_str());

        ImGui::SameLine();
        if (ImGuiGBK::Button("复制##CopyLocalUrl", ImVec2(60, 0))) {
            if (OpenClipboard(g_mainHwnd)) {
                EmptyClipboard();
                HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, accessUrl.size() + 1);
                if (hMem) {
                    memcpy(GlobalLock(hMem), accessUrl.c_str(), accessUrl.size() + 1);
                    GlobalUnlock(hMem);
                    SetClipboardData(CF_TEXT, hMem);
                }
                CloseClipboard();
            }
        }

        ImGui::Spacing();

        // 获取本机IP地址
        char hostname[256];
        if (gethostname(hostname, sizeof(hostname)) == 0) {
            struct hostent* host = gethostbyname(hostname);
            if (host && host->h_addr_list[0]) {
                struct in_addr addr;
                memcpy(&addr, host->h_addr_list[0], sizeof(struct in_addr));
                std::string remoteUrl = "http://" + std::string(inet_ntoa(addr)) + ":" + std::to_string(httpPort);
                ImGuiGBK::Text("远程访问: %s", remoteUrl.c_str());

                ImGui::SameLine();
                if (ImGuiGBK::Button("复制##CopyRemoteUrl", ImVec2(60, 0))) {
                    if (OpenClipboard(g_mainHwnd)) {
                        EmptyClipboard();
                        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, remoteUrl.size() + 1);
                        if (hMem) {
                            memcpy(GlobalLock(hMem), remoteUrl.c_str(), remoteUrl.size() + 1);
                            GlobalUnlock(hMem);
                            SetClipboardData(CF_TEXT, hMem);
                        }
                        CloseClipboard();
                    }
                }
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // 默认滤镜配置
        ImGuiGBK::Text("默认滤镜配置（对所有账号生效）");
        ImGui::Spacing();
        ImGuiGBK::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "说明：未配置的用户将自动使用这些默认滤镜");
        ImGui::Spacing();

        static std::vector<int> defaultFilters;
        static bool defaultFiltersInitialized = false;
        static std::string lastDefaultFilterInstanceId = "";  // 🔥 记录上次的实例ID

        // 初始化默认滤镜配置（当切换实例时重新初始化）
        if (!defaultFiltersInitialized || lastDefaultFilterInstanceId != info.id) {
            if (g_userFilterManager) {
                defaultFilters = g_userFilterManager->LoadDefaultFilters(info.id);
            }
            defaultFiltersInitialized = true;
            lastDefaultFilterInstanceId = info.id;  // 🔥 更新实例ID
        }

        // 获取对当前实例生效的所有滤镜
        std::vector<std::pair<int, std::string>> availableFilters;
        if (g_wpeFilterManager) {
            auto allFilters = g_wpeFilterManager->GetAllFilters();
            for (const auto& filter : allFilters) {
                // 只显示对当前实例生效的滤镜
                if (filter.target.applyToAllInstances) {
                    availableFilters.push_back({filter.id, filter.name});
                } else {
                    auto& targetIds = filter.target.targetInstanceIds;
                    if (std::find(targetIds.begin(), targetIds.end(), info.id) != targetIds.end()) {
                        availableFilters.push_back({filter.id, filter.name});
                    }
                }
            }
        }

        // 显示滤镜列表
        ImGui::BeginChild("DefaultFilterList", ImVec2(0, 200), true);
        for (const auto& filterPair : availableFilters) {
            int filterId = filterPair.first;
            const std::string& filterName = filterPair.second;

            bool isEnabled = std::find(defaultFilters.begin(), defaultFilters.end(), filterId) != defaultFilters.end();

            if (ImGui::Checkbox(("##default_" + std::to_string(filterId)).c_str(), &isEnabled)) {
                if (isEnabled) {
                    // 添加到默认列表
                    if (std::find(defaultFilters.begin(), defaultFilters.end(), filterId) == defaultFilters.end()) {
                        defaultFilters.push_back(filterId);
                    }
                } else {
                    // 从默认列表移除
                    defaultFilters.erase(std::remove(defaultFilters.begin(), defaultFilters.end(), filterId), defaultFilters.end());
                }
            }
            ImGui::SameLine();
            ImGuiGBK::Text("[%d] %s", filterId, filterName.c_str());
        }
        ImGui::EndChild();

        ImGui::Spacing();

        // 添加复选框：是否应用到所有用户
        static std::map<std::string, bool> applyToAllUsersMap;
        bool& applyToAllUsers = applyToAllUsersMap[info.id];
        ImGui::Checkbox("##applyToAll", &applyToAllUsers);
        ImGui::SameLine();
        ImGuiGBK::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "应用到所有现有用户");
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGuiGBK::Text("勾选：覆盖所有用户的配置为此默认值");
            ImGuiGBK::Text("不勾选：仅对新用户和未配置的用户生效");
            ImGui::EndTooltip();
        }

        ImGui::Spacing();

        // 保存按钮
        if (ImGuiGBK::Button("保存默认配置", ImVec2(150, 0))) {
            if (g_userFilterManager) {
                bool success = false;
                if (applyToAllUsers) {
                    // 应用到所有用户
                    success = g_userFilterManager->ApplyDefaultFiltersToAllUsers(info.id, defaultFilters);
                    if (success) {
                        AB_LOG_INFO("[用户滤镜] 默认配置已应用到所有用户: " + info.id + ", 滤镜数: " + std::to_string(defaultFilters.size()));
                        MessageBoxA(g_mainHwnd, "默认配置已保存并应用到所有现有用户", "成功", MB_OK | MB_ICONINFORMATION);
                    } else {
                        MessageBoxA(g_mainHwnd, "应用配置到所有用户失败", "错误", MB_OK | MB_ICONERROR);
                    }
                } else {
                    // 仅保存默认配置
                    success = g_userFilterManager->SaveDefaultFilters(info.id, defaultFilters);
                    if (success) {
                        AB_LOG_INFO("[用户滤镜] 默认配置已保存: " + info.id + ", 滤镜数: " + std::to_string(defaultFilters.size()));
                    } else {
                        MessageBoxA(g_mainHwnd, "保存默认配置失败", "错误", MB_OK | MB_ICONERROR);
                    }
                }
            }
        }

        ImGui::SameLine();

        // 全选按钮
        if (ImGuiGBK::Button("全选", ImVec2(80, 0))) {
            defaultFilters.clear();
            for (const auto& filterPair : availableFilters) {
                defaultFilters.push_back(filterPair.first);
            }
        }

        ImGui::SameLine();

        // 清空按钮
        if (ImGuiGBK::Button("清空", ImVec2(80, 0))) {
            defaultFilters.clear();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // 使用说明
        ImGuiGBK::Text("使用说明:");
        ImGui::Spacing();
        ImGuiGBK::Text("1. 启用用户滤镜模式后，每个SOCKS用户可以独立配置自己的WPE滤镜");
        ImGuiGBK::Text("2. 用户通过浏览器访问上述地址，使用SOCKS账号密码登录");
        ImGuiGBK::Text("3. 登录后可以查看所有滤镜，并选择启用/禁用特定滤镜");
        ImGuiGBK::Text("4. 未配置的用户将自动使用上面设置的默认滤镜");
        ImGuiGBK::Text("5. 用户的滤镜配置会自动保存到数据库");
        ImGui::Spacing();
        ImGuiGBK::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "关于\"应用到所有现有用户\"选项:");
        ImGuiGBK::Text("  - 不勾选：仅保存默认配置，已配置的用户不受影响");
        ImGuiGBK::Text("  - 勾选：将默认配置强制应用到所有现有用户（覆盖他们的配置）");
        }  // 关闭 else 块
    }  // 关闭 else if (selectedMenu == 6) 块
    else if (selectedMenu == 7) {
        // ===== 账号滤镜 =====
        ImGuiGBK::Text("账号滤镜配置查看");
        ImGui::Separator();

        // 检查实例是否启动
        if (!collector) {
            ImGuiGBK::Text("请先启动实例才能查看账号滤镜配置");
            if (ImGuiGBK::Button("返回", ImVec2(100, 0))) {
                selectedMenu = 0;
                needsRedraw = true;
            }
            ImGui::EndChild();
            return;
        }

        // 检查是否启用了用户滤镜模式
        if (!collector->IsUserFilterModeEnabled()) {
            ImGuiGBK::Text("当前实例未启用用户滤镜模式");
            ImGui::Spacing();
            ImGuiGBK::Text("请在\"用户滤镜\"菜单中启用用户滤镜模式");
            if (ImGuiGBK::Button("返回", ImVec2(100, 0))) {
                selectedMenu = 0;
                needsRedraw = true;
            }
            ImGui::EndChild();
            return;
        }

        ImGui::Spacing();

        // 获取所有 SOCKS 账号
        std::vector<std::string> socksAccounts;
        auto* externalSource = collector->GetExternalAccountSource();
        if (externalSource) {
            auto allAccounts = externalSource->GetAllAccounts();
            for (const auto& account : allAccounts) {
                socksAccounts.push_back(account.username);
            }
        } else {
            ImGuiGBK::TextColored(ImVec4(0.8f, 0.4f, 0.4f, 1.0f), "当前实例未绑定账号库");
            ImGui::Spacing();
            ImGuiGBK::Text("请在\"SOCKS5认证\"菜单中绑定账号库");
            if (ImGuiGBK::Button("返回", ImVec2(100, 0))) {
                selectedMenu = 0;
                needsRedraw = true;
            }
            ImGui::EndChild();
            return;
        }

        // 获取默认滤镜配置
        std::vector<int> defaultFilters = g_userFilterManager->LoadDefaultFilters(info.id);

        // 获取对当前实例生效的所有滤镜（用于显示名称）
        std::map<int, std::string> filterIdToName;
        if (g_wpeFilterManager) {
            auto allFilters = g_wpeFilterManager->GetAllFilters();
            for (const auto& filter : allFilters) {
                if (filter.target.applyToAllInstances) {
                    filterIdToName[filter.id] = filter.name;
                } else {
                    auto& targetIds = filter.target.targetInstanceIds;
                    if (std::find(targetIds.begin(), targetIds.end(), info.id) != targetIds.end()) {
                        filterIdToName[filter.id] = filter.name;
                    }
                }
            }
        }

        ImGuiGBK::Text("共 %d 个 SOCKS 账号", socksAccounts.size());
        ImGui::Spacing();

        // 显示账号列表和对应的滤镜配置
        ImGui::BeginChild("AccountFilterList", ImVec2(0, 0), true);

        for (const auto& username : socksAccounts) {
            ImGui::PushID(username.c_str());

            // 检查用户是否有自定义配置
            bool hasUserConfig = g_userFilterManager->HasUserConfig(info.id, username);

            // 获取有效的滤镜列表
            auto effectiveFilters = g_userFilterManager->GetEffectiveUserFilters(info.id, username);
            std::vector<int> filterList(effectiveFilters.begin(), effectiveFilters.end());

            // 显示账号名称
            if (hasUserConfig) {
                ImGuiGBK::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1.0f), "[自定义] %s", username.c_str());
            } else {
                ImGuiGBK::TextColored(ImVec4(0.8f, 0.8f, 0.4f, 1.0f), "[默认配置] %s", username.c_str());
            }

            ImGui::Indent(20.0f);

            // 显示生效的滤镜
            if (filterList.empty()) {
                ImGuiGBK::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "无生效滤镜");
            } else {
                ImGuiGBK::Text("生效滤镜 (%d 个):", filterList.size());
                ImGui::Indent(20.0f);

                for (int filterId : filterList) {
                    auto it = filterIdToName.find(filterId);
                    if (it != filterIdToName.end()) {
                        ImGuiGBK::Text("• [%d] %s", filterId, it->second.c_str());
                    } else {
                        ImGuiGBK::TextColored(ImVec4(0.8f, 0.4f, 0.4f, 1.0f), "• [%d] (滤镜不存在或已删除)", filterId);
                    }
                }

                ImGui::Unindent(20.0f);
            }

            ImGui::Unindent(20.0f);
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::PopID();
        }

        if (socksAccounts.empty()) {
            ImGuiGBK::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "暂无 SOCKS 账号");
        }

        ImGui::EndChild();
    }

    // 右侧内容区域结束
    ImGui::EndChild();
}

// ===== 实例导出：构建单个实例的完整配置JSON =====
static Json::Value ExportInstanceConfig(const std::string& instanceId) {
    Json::Value config(Json::objectValue);

    auto* instance = InstanceManager::GetInstance().GetSocksForwardInstance(instanceId);
    if (!instance) return config;

    InstanceInfo info = instance->GetInfo();

    // 基本信息
    config["id"] = instanceId;
    config["name"] = info.name;
    config["port"] = info.port;
    config["createTime"] = info.createTime;

    // 从数据库读取详细配置（与SocksCloudSync.cpp完全一致）
    config["threadPoolMode"] = g_database->GetConfigValue(
        InstanceManager::MakeInstanceConfigKey(instanceId, "threadPoolMode"), "0");
    config["whitelistPoolSize"] = g_database->GetConfigValue(
        InstanceManager::MakeInstanceConfigKey(instanceId, "whitelistPoolSize"), "10");
    config["normalPoolSize"] = g_database->GetConfigValue(
        InstanceManager::MakeInstanceConfigKey(instanceId, "normalPoolSize"), "50");
    config["iocpMaxWhitelist"] = g_database->GetConfigValue(
        InstanceManager::MakeInstanceConfigKey(instanceId, "iocpMaxWhitelist"), "2000");
    config["iocpMaxNormal"] = g_database->GetConfigValue(
        InstanceManager::MakeInstanceConfigKey(instanceId, "iocpMaxNormal"), "500");

    // SOCKS5认证配置
    config["enableSocks5Auth"] = g_database->GetConfigValue(
        InstanceManager::MakeInstanceConfigKey(instanceId, "enableSocks5Auth"), "0");
    config["accountSourceInstanceId"] = g_database->GetConfigValue(
        InstanceManager::MakeInstanceConfigKey(instanceId, "accountSourceInstanceId"), "");

    // 二级代理配置
    config["enableSecondaryProxy"] = g_database->GetConfigValue(
        InstanceManager::MakeInstanceConfigKey(instanceId, "enableSecondaryProxy"), "0");
    config["secondaryProxyHost"] = g_database->GetConfigValue(
        InstanceManager::MakeInstanceConfigKey(instanceId, "secondaryProxyHost"), "127.0.0.1");
    config["secondaryProxyPort"] = g_database->GetConfigValue(
        InstanceManager::MakeInstanceConfigKey(instanceId, "secondaryProxyPort"), "1080");
    config["secondaryProxyUsername"] = g_database->GetConfigValue(
        InstanceManager::MakeInstanceConfigKey(instanceId, "secondaryProxyUsername"), "");
    config["secondaryProxyPassword"] = g_database->GetConfigValue(
        InstanceManager::MakeInstanceConfigKey(instanceId, "secondaryProxyPassword"), "");

    // 分包处理配置
    config["enablePacketSplit"] = g_database->GetConfigValue(
        InstanceManager::MakeInstanceConfigKey(instanceId, "enablePacketSplit"), "1");
    config["packetSplitPorts"] = g_database->GetConfigValue(
        InstanceManager::MakeInstanceConfigKey(instanceId, "packetSplitPorts"), "");
    config["applyWpeOnNonSplitTraffic"] = g_database->GetConfigValue(
        InstanceManager::MakeInstanceConfigKey(instanceId, "applyWpeOnNonSplitTraffic"), "0");

    // 流量过滤配置
    config["enableTrafficFilter"] = g_database->GetConfigValue(
        InstanceManager::MakeInstanceConfigKey(instanceId, "enableTrafficFilter"), "0");
    config["enableSniSniffing"] = g_database->GetConfigValue(
        InstanceManager::MakeInstanceConfigKey(instanceId, "enableSniSniffing"), "0");
    config["trafficFilterRules"] = g_database->GetConfigValue(
        InstanceManager::MakeInstanceConfigKey(instanceId, "trafficFilterRules"), "");
    config["enableSSLMitm"] = g_database->GetConfigValue(
        InstanceManager::MakeInstanceConfigKey(instanceId, "enableSSLMitm"), "0");
    config["sslMitmRules"] = g_database->GetConfigValue(
        InstanceManager::MakeInstanceConfigKey(instanceId, "sslMitmRules"), "[]");
    config["enableHttpLocalMap"] = g_database->GetConfigValue(
        InstanceManager::MakeInstanceConfigKey(instanceId, "enableHttpLocalMap"), "0");
    config["httpLocalMapRules"] = g_database->GetConfigValue(
        InstanceManager::MakeInstanceConfigKey(instanceId, "httpLocalMapRules"), "[]");

    // 自动启动配置
    config["autoStart"] = g_database->GetConfigValue(
        InstanceManager::MakeInstanceConfigKey(instanceId, "autoStart"), "0");

    // 用户滤镜配置
    config["enableUserFilterMode"] = g_database->GetConfigValue(
        InstanceManager::MakeInstanceConfigKey(instanceId, "enableUserFilterMode"), "0");
    config["userFilterHttpPort"] = g_database->GetConfigValue(
        InstanceManager::MakeInstanceConfigKey(instanceId, "userFilterHttpPort"), "8080");

    // 导出默认滤镜配置
    if (g_userFilterManager) {
        std::vector<int> defaultFilters = g_userFilterManager->LoadDefaultFilters(instanceId);
        Json::Value defaultFiltersArray(Json::arrayValue);
        for (int filterId : defaultFilters) {
            defaultFiltersArray.append(filterId);
        }
        config["defaultFilters"] = defaultFiltersArray;

        // 导出所有用户的滤镜配置
        std::vector<std::string> users = g_userFilterManager->GetInstanceUsers(instanceId);
        Json::Value userFiltersObj(Json::objectValue);
        for (const std::string& username : users) {
            std::set<int> userFilters = g_userFilterManager->GetUserEnabledFilters(instanceId, username);
            Json::Value userFiltersArray(Json::arrayValue);
            for (int filterId : userFilters) {
                userFiltersArray.append(filterId);
            }
            userFiltersObj[username] = userFiltersArray;
        }
        config["userFilters"] = userFiltersObj;
    }

    return config;
}

// ===== 实例导入：将配置写入数据库 =====
static void ApplyInstanceConfigFromJson(const std::string& instanceId, const Json::Value& config) {
    if (!g_database) return;

    // 写入所有配置键到数据库
    auto setKey = [&](const std::string& key, const std::string& defaultVal) {
        std::string val = config.get(key, defaultVal).asString();
        g_database->SetConfigValue(InstanceManager::MakeInstanceConfigKey(instanceId, key), val);
    };

    // 线程模型
    setKey("threadPoolMode", "0");
    setKey("whitelistPoolSize", "10");
    setKey("normalPoolSize", "50");
    setKey("iocpMaxWhitelist", "2000");
    setKey("iocpMaxNormal", "500");

    // SOCKS5认证
    setKey("enableSocks5Auth", "0");
    setKey("accountSourceInstanceId", "");

    // 二级代理
    setKey("enableSecondaryProxy", "0");
    setKey("secondaryProxyHost", "127.0.0.1");
    setKey("secondaryProxyPort", "1080");
    setKey("secondaryProxyUsername", "");
    setKey("secondaryProxyPassword", "");

    // 分包处理
    setKey("enablePacketSplit", "1");
    setKey("packetSplitPorts", "");
    setKey("applyWpeOnNonSplitTraffic", "0");

    // 流量过滤
    setKey("enableTrafficFilter", "0");
    setKey("enableSniSniffing", "0");
    setKey("trafficFilterRules", "");
    setKey("enableSSLMitm", "0");
    setKey("sslMitmRules", "[]");
    setKey("enableHttpLocalMap", "0");
    setKey("httpLocalMapRules", "[]");

    // 自动启动
    setKey("autoStart", "0");

    // 用户滤镜模式
    setKey("enableUserFilterMode", "0");
    setKey("userFilterHttpPort", "8080");

    // 恢复用户滤镜配置
    if (g_userFilterManager) {
        // 恢复默认滤镜
        if (config.isMember("defaultFilters") && config["defaultFilters"].isArray()) {
            std::vector<int> defaultFilters;
            for (const auto& id : config["defaultFilters"]) {
                defaultFilters.push_back(id.asInt());
            }
            g_userFilterManager->SaveDefaultFilters(instanceId, defaultFilters);
        }

        // 恢复用户滤镜
        if (config.isMember("userFilters") && config["userFilters"].isObject()) {
            const Json::Value& userFilters = config["userFilters"];
            for (const auto& username : userFilters.getMemberNames()) {
                const Json::Value& filterArray = userFilters[username];
                if (filterArray.isArray()) {
                    std::vector<int> filterIds;
                    for (const auto& id : filterArray) {
                        filterIds.push_back(id.asInt());
                    }
                    g_userFilterManager->UpdateUserFilters(instanceId, username, filterIds);
                }
            }
        }
    }
}

// ===== 实例导入主逻辑 =====
static bool ImportInstancesFromJson(const std::string& jsonStr, int mode, std::string& outError) {
    if (!g_database) {
        outError = "数据库未初始化";
        return false;
    }

    Json::CharReaderBuilder readerBuilder;
    Json::Value root;
    std::string parseError;
    std::istringstream stream(jsonStr);
    if (!Json::parseFromStream(readerBuilder, stream, &root, &parseError)) {
        outError = "JSON解析失败: " + parseError;
        return false;
    }

    if (!root.isMember("instances")) {
        outError = "JSON格式无效: 缺少instances字段";
        return false;
    }

    const Json::Value& instancesArray = root["instances"];
    if (!instancesArray.isArray() || instancesArray.empty()) {
        outError = "JSON中没有实例数据";
        return false;
    }

    // 覆盖模式：先删除所有现有SocksForward实例
    if (mode == 1) {
        auto existingInstances = InstanceManager::GetInstance().GetSocksForwardInstances();
        for (auto* inst : existingInstances) {
            if (inst) {
                InstanceManager::GetInstance().DeleteInstance(inst->GetId());
            }
        }
    }

    int importedCount = 0;
    for (const auto& instConfig : instancesArray) {
        std::string originalId = instConfig.get("id", "").asString();
        std::string name = instConfig.get("name", "未命名").asString();
        int port = instConfig.get("port", 1080).asInt();

        InstanceConfig config;
        config.name = name;
        config.port = port;
        config.type = InstanceType::SocksForward;

        // 创建实例（合并模式自动分配新ID，覆盖模式也使用新ID因为CreateSocksForwardInstance内部会生成）
        std::string newId = InstanceManager::GetInstance().CreateSocksForwardInstance(config);
        if (newId.empty()) continue;

        // 将完整配置写入数据库
        ApplyInstanceConfigFromJson(newId, instConfig);

        AB_LOG_INFO("[实例管理] 导入实例: " + newId + " (" + name + ")");
        importedCount++;
    }

    if (importedCount == 0) {
        outError = "没有成功导入任何实例";
        return false;
    }

    outError = "";
    return true;
}

void RenderInstanceManagerPage(bool& needsRedraw) {
    // 每帧先清理ab实例的UI上下文（由"实例配置"分支按需重新设置，供详情窗同帧渲染使用）
    g_uiActiveAbCollectorCtx = nullptr;
    g_uiActiveAbCollectorInstance = nullptr;
    g_uiActiveAbCollector = nullptr;
    g_uiActiveAbCollectorPool = nullptr;

    g_uiActiveAbHeartbeatCtx = nullptr;
    g_uiActiveAbHeartbeatInstance = nullptr;
    g_uiActiveAbHeartbeatForwarder = nullptr;
    g_uiActiveAbHeartbeatBoundPool = nullptr;

    // 如果正在显示实例配置界面
    if (g_showInstanceConfig && !g_configInstanceId.empty()) {
        ImGuiGBK::Text("实例配置");
        ImGui::SameLine();
        if (ImGuiGBK::Button("返回实例列表##backToList", ImVec2(120, 0))) {
            g_showInstanceConfig = false;
            g_configInstanceId.clear();
            needsRedraw = true;
        }
        ImGui::Separator();

        // 内置 ab 实例：复用独立运行页面，确保"逻辑/算法/UI"完全一致
        if (g_configInstanceId == InstanceManager::kAbCollectorInstanceId) {
            // 保险：RenderCollectorPage 依赖 g_collector 非空
            if (!g_collector) {
                const int port = AbStandaloneApi::GetCollectorDesiredPort();
                g_listenPort = (port > 0 && port <= 65535) ? port : 1080;
                g_collector = new PacketCollector(g_listenPort);
            }

            // 内置ab也走"ab多实例ctx"映射：确保与自建ab实例UI完全一致（每实例独立UI/配置持久化）
            auto ctx = GetOrCreateAbCollectorCtx(g_configInstanceId);
            if (ctx) {
                ctx->useGlobalPacketView = true; // 包列表仍显示全局采集链路
                ctx->listenPort = AbStandaloneApi::GetCollectorDesiredPort();
                sprintf_s(ctx->portBuffer, "%d", ctx->listenPort);
            }
            AbCollectorUiScope scope(ctx.get(), nullptr, g_collector, &g_collectedPool);

            int currentPacketCount = 0;
            {
                std::lock_guard<std::mutex> lock(g_packetsMutex);
                currentPacketCount = static_cast<int>(g_packets.size());
            }

            int cachedTotalPackets = 0;
            int cachedTotalConnections = 0;
            double cachedTotalBytes = 0.0;
            int cachedFragmentedPackets = 0;
            int cachedMultiPackets = 0;

            if (g_collector) {
                cachedTotalPackets = g_collector->GetTotalPackets();
                cachedTotalConnections = g_collector->GetTotalConnections();
                cachedTotalBytes = g_collector->GetTotalBytes() / 1024.0;
                cachedFragmentedPackets = g_collector->GetFragmentedPackets();
                cachedMultiPackets = g_collector->GetMultiPackets();
            }

            RenderCollectorPage(needsRedraw, currentPacketCount,
                cachedTotalPackets, cachedTotalConnections, cachedTotalBytes,
                cachedFragmentedPackets, cachedMultiPackets);
            return;
        }

        if (g_configInstanceId == InstanceManager::kAbHeartbeatInstanceId) {
            auto ctx = GetOrCreateAbHeartbeatCtx(g_configInstanceId);
            if (ctx) {
                ctx->useGlobalPacketView = true; // 包列表仍显示全局伪心跳链路
                ctx->heartbeatPort = AbStandaloneApi::GetHeartbeatDesiredPort();
                sprintf_s(ctx->heartbeatPortBuffer, "%d", ctx->heartbeatPort);
            }
            AbHeartbeatUiScope scope(ctx.get(), nullptr, g_heartbeatForwarder, &g_collectedPool);

            RenderHeartbeatPage(needsRedraw);
            return;
        }

        // 根据实例类型显示对应的配置界面
        auto* collectorInstance = InstanceManager::GetInstance().GetCollectorInstance(g_configInstanceId);
        if (collectorInstance) {
            RenderCollectorInstanceConfig(collectorInstance, needsRedraw);
            return;
        }

        auto* heartbeatInstance = InstanceManager::GetInstance().GetHeartbeatInstance(g_configInstanceId);
        if (heartbeatInstance) {
            RenderHeartbeatInstanceConfig(heartbeatInstance, needsRedraw);
            return;
        }

        auto* abCollectorInstance = InstanceManager::GetInstance().GetAbCollectorInstance(g_configInstanceId);
        if (abCollectorInstance) {
            RenderAbCollectorInstanceConfig(abCollectorInstance, needsRedraw);
            return;
        }

        auto* abHeartbeatInstance = InstanceManager::GetInstance().GetAbHeartbeatInstance(g_configInstanceId);
        if (abHeartbeatInstance) {
            RenderAbHeartbeatInstanceConfig(abHeartbeatInstance, needsRedraw);
            return;
        }

        auto* socksForwardInstance = InstanceManager::GetInstance().GetSocksForwardInstance(g_configInstanceId);
        if (socksForwardInstance) {
            RenderSocksForwardInstanceConfig(socksForwardInstance, needsRedraw);
            return;
        }

        ImGuiGBK::Text("实例不存在");
        return;
    }

    // 正常的实例管理界面
    ImGuiGBK::Text("实例管理");
    ImGui::Separator();

    // ===== 创建新实例区域 =====
    ImGui::BeginChild("CreateInstance", ImVec2(0, 200), true);
    ImGuiGBK::Text("创建Socks转发实例");
    ImGui::Separator();

    ImGuiGBK::Text("实例名称:");
    ImGui::SameLine();
    ImGui::InputText("##InstanceName", g_newInstanceName, sizeof(g_newInstanceName));

    ImGuiGBK::Text("监听端口:");
    ImGui::SameLine();
    ImGui::InputInt("##InstancePort", &g_newInstancePort);

    ImGui::TextDisabled("(纯转发模式，不记录数据包，支持WPE滤镜和二级代理)");

    ImGui::Spacing();

    if (ImGuiGBK::Button("创建实例")) {
        if (strlen(g_newInstanceName) == 0) {
            UiMessageBox::Show(g_mainHwnd, "请输入实例名称", "错误", MB_OK | MB_ICONERROR);
        } else {
            InstanceConfig config;
            config.name = g_newInstanceName;
            config.port = g_newInstancePort;
            config.type = InstanceType::SocksForward;

            std::string instanceId = InstanceManager::GetInstance().CreateSocksForwardInstance(config);

            AB_LOG_INFO("[实例管理] 创建实例成功: " + instanceId + " (" + config.name + ")");
            UiMessageBox::Show(g_mainHwnd, ("实例创建成功！\nID: " + instanceId).c_str(), "成功", MB_OK | MB_ICONINFORMATION);

            // 清空输入
            memset(g_newInstanceName, 0, sizeof(g_newInstanceName));
            g_newInstancePort = 1080;
            needsRedraw = true;
        }
    }

    ImGui::EndChild();

    // ===== 导入/导出按钮区域 =====
    ImGui::Spacing();
    if (ImGuiGBK::Button("导出所选##export_instances", ImVec2(100, 0))) {
        if (g_instanceExportSelectedIds.empty()) {
            g_instanceLastIoMessage = "请先勾选要导出的实例";
        } else {
            // 直接弹出文件保存对话框
            OPENFILENAMEA ofn;
            char szFile[260] = "socks_instances.absocks";
            ZeroMemory(&ofn, sizeof(ofn));
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = g_mainHwnd;
            ofn.lpstrFile = szFile;
            ofn.nMaxFile = sizeof(szFile);
            ofn.lpstrFilter = "AB2 SOCKS Instances\0*.absocks\0JSON Files\0*.json\0All Files\0*.*\0";
            ofn.nFilterIndex = 1;
            ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;

            if (GetSaveFileNameA(&ofn)) {
                // 构建导出JSON
                Json::Value root(Json::objectValue);
                root["version"] = 1;
                Json::Value instancesArray(Json::arrayValue);

                for (const auto& id : g_instanceExportSelectedIds) {
                    Json::Value instConfig = ExportInstanceConfig(id);
                    if (!instConfig.empty()) {
                        instancesArray.append(instConfig);
                    }
                }
                root["instances"] = instancesArray;

                Json::StreamWriterBuilder builder;
                builder["indentation"] = "  ";
                std::string jsonStr = Json::writeString(builder, root);

                try {
                    std::ofstream f(szFile, std::ios::binary);
                    if (f.is_open()) {
                        f.write(jsonStr.data(), jsonStr.size());
                        f.close();
                        g_instanceLastIoMessage = "导出成功: " + std::to_string(instancesArray.size()) + " 个实例";
                        AB_LOG_INFO("[实例管理] 导出 " + std::to_string(instancesArray.size()) + " 个实例到 " + std::string(szFile));
                    } else {
                        g_instanceLastIoMessage = "导出失败: 无法创建文件";
                    }
                } catch (...) {
                    g_instanceLastIoMessage = "导出失败: 写入文件异常";
                }
            }
        }
        needsRedraw = true;
    }

    ImGui::SameLine();
    if (ImGuiGBK::Button("导入##import_instances", ImVec2(100, 0))) {
        OPENFILENAMEA ofn;
        char szFile[260] = "";
        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = g_mainHwnd;
        ofn.lpstrFile = szFile;
        ofn.nMaxFile = sizeof(szFile);
        ofn.lpstrFilter = "AB2 SOCKS Instances\0*.absocks\0JSON Files\0*.json\0All Files\0*.*\0";
        ofn.nFilterIndex = 1;
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

        if (GetOpenFileNameA(&ofn)) {
            g_instancePendingImportPath = szFile;
            g_instancePendingImportJson.clear();
            g_instanceImportMode = 0;

            try {
                std::ifstream f(szFile, std::ios::binary);
                if (!f.is_open()) {
                    g_instanceLastIoMessage = "导入失败: 无法打开文件";
                } else {
                    f.seekg(0, std::ios::end);
                    const std::streamoff size = f.tellg();
                    f.seekg(0, std::ios::beg);
                    if (size <= 0) {
                        g_instanceLastIoMessage = "导入失败: 文件为空";
                    } else {
                        g_instancePendingImportJson.resize((size_t)size);
                        f.read(&g_instancePendingImportJson[0], size);
                        f.close();
                        g_showInstanceImportModal = true;
                        ImGui::OpenPopup(ImGuiText::U("导入实例配置##instance_import_modal"));
                    }
                }
            } catch (...) {
                g_instanceLastIoMessage = "导入失败: 读取文件异常";
            }
            needsRedraw = true;
        }
    }

    ImGui::SameLine();
    if (ImGuiGBK::Button("全选##inst_select_all", ImVec2(60, 0))) {
        auto allInst = InstanceManager::GetInstance().GetAllInstances();
        for (const auto& info : allInst) {
            if (info.type == InstanceType::SocksForward) {
                g_instanceExportSelectedIds.insert(info.id);
            }
        }
        needsRedraw = true;
    }
    ImGui::SameLine();
    if (ImGuiGBK::Button("全不选##inst_select_none", ImVec2(60, 0))) {
        g_instanceExportSelectedIds.clear();
        needsRedraw = true;
    }

    // 显示最近操作消息
    if (!g_instanceLastIoMessage.empty()) {
        ImGui::SameLine();
        ImGuiGBK::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f), "%s", g_instanceLastIoMessage.c_str());
    }

    // ===== 导入弹窗 =====
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(500, 250), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(ImGuiText::U("导入实例配置##instance_import_modal"), &g_showInstanceImportModal)) {
        ImGuiGBK::Text("文件: %s", g_instancePendingImportPath.c_str());
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGuiGBK::Text("导入模式:");
        ImGui::Indent(20);
        ImGui::RadioButton(ImGuiText::U("合并导入 (保留现有实例, 导入时分配新ID)"), &g_instanceImportMode, 0);
        ImGui::RadioButton(ImGuiText::U("覆盖导入 (清空所有实例, 使用原ID导入)"), &g_instanceImportMode, 1);
        ImGui::Unindent(20);
        ImGui::Spacing();

        if (g_instanceImportMode == 0) {
            ImGuiGBK::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1), "说明: 追加到现有实例列表, 自动分配新ID");
        } else {
            ImGuiGBK::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1), "说明: 清空所有Socks转发实例, 导入文件中的实例");
        }

        ImGui::Separator();
        if (ImGuiGBK::Button("开始导入##inst_import_do", ImVec2(120, 0))) {
            std::string err;
            bool ok = ImportInstancesFromJson(g_instancePendingImportJson, g_instanceImportMode, err);
            if (ok) {
                g_instanceLastIoMessage = "导入成功";
                g_instancePendingImportPath.clear();
                g_instancePendingImportJson.clear();
                g_showInstanceImportModal = false;
                ImGui::CloseCurrentPopup();
            } else {
                g_instanceLastIoMessage = "导入失败: " + err;
            }
            needsRedraw = true;
        }
        ImGui::SameLine();
        if (ImGuiGBK::Button("取消##inst_import_cancel", ImVec2(80, 0))) {
            g_showInstanceImportModal = false;
            g_instancePendingImportPath.clear();
            g_instancePendingImportJson.clear();
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    // ===== 实例列表区域 =====
    ImGui::BeginChild("InstanceList", ImVec2(0, 350), true);
    ImGuiGBK::Text("实例列表");
    ImGui::Separator();

    auto allInstances = InstanceManager::GetInstance().GetAllInstances();

    // 过滤：只显示 SocksForward 类型的实例
    std::vector<InstanceInfo> instances;
    for (const auto& info : allInstances) {
        if (info.type == InstanceType::SocksForward) {
            instances.push_back(info);
        }
    }

    if (ImGui::BeginTable("InstanceTable", 9, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn(ImGuiText::U("选择"), ImGuiTableColumnFlags_WidthFixed, 40.0f);
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn(ImGuiText::U("名称"), ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("类型", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("状态", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("端口", ImGuiTableColumnFlags_WidthFixed, 50.0f);
        ImGui::TableSetupColumn("连接数", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("操作", ImGuiTableColumnFlags_WidthFixed, 180.0f);
        ImGui::TableSetupColumn("配置", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableHeadersRow();

        for (const auto& info : instances) {
            ImGui::TableNextRow();

            // 选择列（复选框）
            ImGui::TableNextColumn();
            bool exportSelected = (g_instanceExportSelectedIds.find(info.id) != g_instanceExportSelectedIds.end());
            ImGui::PushID(("export_check_" + info.id).c_str());
            if (ImGui::Checkbox("##export_select", &exportSelected)) {
                if (exportSelected) {
                    g_instanceExportSelectedIds.insert(info.id);
                } else {
                    g_instanceExportSelectedIds.erase(info.id);
                }
                needsRedraw = true;
            }
            ImGui::PopID();

            ImGui::TableNextColumn();
            ImGui::Text("%s", info.id.c_str());

            ImGui::TableNextColumn();
            ImGui::Text("%s", info.name.c_str());

            ImGui::TableNextColumn();
            ImGui::Text("Socks转发");

            ImGui::TableNextColumn();
            const char* stateText = "";
            ImVec4 stateColor = ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
            switch (info.state) {
                case InstanceState::Running:
                    stateText = "运行中";
                    stateColor = ImVec4(0.0f, 1.0f, 0.0f, 1.0f);
                    break;
                case InstanceState::Stopped:
                    stateText = "已停止";
                    stateColor = ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
                    break;
                case InstanceState::Starting:
                    stateText = "启动中";
                    stateColor = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
                    break;
                case InstanceState::Stopping:
                    stateText = "停止中";
                    stateColor = ImVec4(1.0f, 0.5f, 0.0f, 1.0f);
                    break;
                case InstanceState::Error:
                    stateText = "错误";
                    stateColor = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
                    break;
            }
            ImGui::TextColored(stateColor, "%s", stateText);

            ImGui::TableNextColumn();
            if (info.port > 0) {
                ImGui::Text("%d", info.port);
            } else {
                ImGui::Text("-");
            }

            ImGui::TableNextColumn();
            ImGui::Text("%d", info.currentConnections);

            ImGui::TableNextColumn();
            ImGui::PushID(info.id.c_str());

            if (info.state == InstanceState::Stopped) {
                if (ImGuiGBK::Button("启动")) {
                    if (InstanceManager::GetInstance().StartInstance(info.id)) {
                        AB_LOG_INFO("[实例管理] 启动实例: " + info.id);
                        needsRedraw = true;
                    }
                }
            } else if (info.state == InstanceState::Running) {
                if (ImGuiGBK::Button("停止")) {
                    if (InstanceManager::GetInstance().StopInstance(info.id)) {
                        AB_LOG_INFO("[实例管理] 停止实例: " + info.id);
                        needsRedraw = true;
                    }
                }
            }

            ImGui::SameLine();
            if (ImGuiGBK::Button("删除")) {
                if (UiMessageBox::Show(g_mainHwnd, "确定要删除此实例吗？", "确认", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                    if (InstanceManager::GetInstance().DeleteInstance(info.id)) {
                        AB_LOG_INFO("[实例管理] 删除实例: " + info.id);
                        needsRedraw = true;
                    }
                }
            }

            ImGui::TableNextColumn();
            if (ImGuiGBK::Button("配置")) {
                g_showInstanceConfig = true;
                g_configInstanceId = info.id;
                needsRedraw = true;
            }

            ImGui::PopID();
        }

        ImGui::EndTable();
    }

    ImGui::EndChild();

    // ===== 统计信息 =====
    ImGuiGBK::Text("统计信息: Socks转发实例数: %d | 运行中: %d",
        static_cast<int>(instances.size()),
        static_cast<int>(std::count_if(instances.begin(), instances.end(),
            [](const InstanceInfo& info) { return info.state == InstanceState::Running; })));
}
