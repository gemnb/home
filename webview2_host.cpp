#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <urlmon.h>
#include <atomic>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <new>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <wrl.h>
#include <wil/com.h>
#include "WebView2.h"
#include "webview2_host.h"
#include "web_resource.h"

#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "comctl32.lib")

using namespace Microsoft::WRL;

namespace {

constexpr wchar_t kBootstrapperUrl[] = L"http://disk.mumuyun.cn/down.php/d5e97bb7c19292d485e083baeb7e7691.exe";
constexpr wchar_t kOfficialDownloadUrl[] = L"https://developer.microsoft.com/en-us/microsoft-edge/webview2/";
constexpr wchar_t kMirrorDownloadUrl[] = L"http://disk.mumuyun.cn/down.php/7a859af9af33fdaf638eed723391b880.exe";
constexpr wchar_t kRuntimeMissingDialogClassName[] = L"AB3WebView2RuntimeMissingDialog";
constexpr wchar_t kWebView2RuntimeClientId[] = L"{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}";
#if defined(_WIN64)
constexpr wchar_t kRecommendedBundledInstallerName[] = L"MicrosoftEdgeWebView2RuntimeInstallerX64.exe";
#else
constexpr wchar_t kRecommendedBundledInstallerName[] = L"MicrosoftEdgeWebView2RuntimeInstallerX86.exe";
#endif
constexpr DWORD kInstallTimeoutMs = 10 * 60 * 1000;
constexpr UINT WM_WEBVIEW2_RUNTIME_STATUS = WM_APP + 0x320;
constexpr UINT WM_WEBVIEW2_RUNTIME_READY = WM_APP + 0x321;
constexpr UINT WM_WEBVIEW2_RUNTIME_FAILED = WM_APP + 0x322;
constexpr int kRuntimeMissingButtonOfficial = 1001;
constexpr int kRuntimeMissingButtonMirror = 1002;
constexpr int kPanelWidth = 520;
constexpr int kPanelHeight = 150;

wil::com_ptr<ICoreWebView2Controller> g_controller;
wil::com_ptr<ICoreWebView2> g_webview;
void (*g_message_callback)(const char* json) = nullptr;
std::mutex g_log_mutex;
std::mutex g_status_mutex;
std::atomic<bool> g_install_running(false);
std::atomic<bool> g_install_cancel(false);
HWND g_parent = nullptr;
int g_pending_port = 0;

struct UiState {
    HWND parent = nullptr;
    HWND panel = nullptr;
    HWND title = nullptr;
    HWND detail = nullptr;
    HWND progress = nullptr;
    bool marquee = false;
};

struct InstallStatus {
    bool visible = false;
    bool marquee = true;
    bool failed = false;
    int progress = 0;
    std::wstring title;
    std::wstring detail;
};

struct RuntimePresence {
    bool available = false;
    HRESULT api_hr = E_FAIL;
    std::wstring api_version;
    std::wstring registry_version;
};

struct RegistryLocation {
    HKEY root;
    const wchar_t* path;
};

struct RuntimeMissingDialogState {
    HWND parent = nullptr;
    int result = IDCANCEL;
    bool done = false;
};

UiState g_ui;
InstallStatus g_status;

const RegistryLocation kRuntimeRegistryLocations[] = {
    { HKEY_CURRENT_USER, L"Software\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}" },
    { HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}" },
    { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}" }
};

int clamp_int(int value, int min_value, int max_value) {
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

std::wstring utf8_to_wide(const std::string& value) {
    if (value.empty()) {
        return std::wstring();
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) {
        return std::wstring();
    }
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), &result[0], size);
    return result;
}

std::string wide_to_utf8(const std::wstring& value) {
    if (value.empty()) {
        return std::string();
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return std::string();
    }
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, &result[0], size, nullptr, nullptr);
    if (!result.empty() && result.back() == '\0') {
        result.pop_back();
    }
    return result;
}

std::wstring trim_copy(const std::wstring& value) {
    const size_t first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) {
        return std::wstring();
    }
    const size_t last = value.find_last_not_of(L" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string base64_encode(const unsigned char* data, size_t size) {
    static constexpr char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    if (!data || size == 0) {
        return std::string();
    }

    std::string output;
    output.reserve(((size + 2) / 3) * 4);

    for (size_t i = 0; i < size; i += 3) {
        const unsigned int byte0 = data[i];
        const unsigned int byte1 = (i + 1 < size) ? data[i + 1] : 0;
        const unsigned int byte2 = (i + 2 < size) ? data[i + 2] : 0;
        const unsigned int triple = (byte0 << 16) | (byte1 << 8) | byte2;

        output.push_back(kTable[(triple >> 18) & 0x3F]);
        output.push_back(kTable[(triple >> 12) & 0x3F]);
        output.push_back((i + 1 < size) ? kTable[(triple >> 6) & 0x3F] : '=');
        output.push_back((i + 2 < size) ? kTable[triple & 0x3F] : '=');
    }

    return output;
}

std::wstring escape_powershell_single_quoted(const std::wstring& value) {
    std::wstring escaped;
    escaped.reserve(value.size() + 8);
    for (wchar_t ch : value) {
        if (ch == L'\'') {
            escaped += L"''";
            continue;
        }
        escaped.push_back(ch);
    }
    return escaped;
}

bool is_zero_like_version(const std::wstring& value) {
    bool saw_digit = false;
    for (wchar_t ch : value) {
        if (ch >= L'0' && ch <= L'9') {
            saw_digit = true;
            if (ch != L'0') {
                return false;
            }
            continue;
        }
        if (ch != L'.') {
            return false;
        }
    }
    return saw_digit;
}

bool is_installed_runtime_version(const std::wstring& value) {
    const std::wstring trimmed = trim_copy(value);
    if (trimmed.empty()) {
        return false;
    }
    if (lstrcmpiW(trimmed.c_str(), L"null") == 0) {
        return false;
    }
    return !is_zero_like_version(trimmed);
}

bool read_registry_string(HKEY root, const wchar_t* subkey, const wchar_t* value_name, std::wstring* value) {
    if (!subkey || !value_name || !value) {
        return false;
    }

    DWORD bytes = 0;
    LONG status = RegGetValueW(root, subkey, value_name, RRF_RT_REG_SZ, nullptr, nullptr, &bytes);
    if (status != ERROR_SUCCESS || bytes < sizeof(wchar_t)) {
        return false;
    }

    std::wstring buffer(bytes / sizeof(wchar_t), L'\0');
    status = RegGetValueW(root, subkey, value_name, RRF_RT_REG_SZ, nullptr, &buffer[0], &bytes);
    if (status != ERROR_SUCCESS) {
        return false;
    }

    if (!buffer.empty() && buffer.back() == L'\0') {
        buffer.pop_back();
    }

    *value = buffer;
    return true;
}

std::wstring registry_runtime_version() {
    for (const auto& location : kRuntimeRegistryLocations) {
        std::wstring value;
        if (read_registry_string(location.root, location.path, L"pv", &value) &&
            is_installed_runtime_version(value)) {
            return trim_copy(value);
        }
    }
    return std::wstring();
}

std::filesystem::path module_dir() {
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return std::filesystem::current_path();
    }
    return std::filesystem::path(path).parent_path();
}

std::filesystem::path runtime_log_path() {
    return module_dir() / L"webview2_runtime_install.log";
}

void runtime_log(const std::string& message) {
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::ofstream file(runtime_log_path(), std::ios::app);
    if (!file.is_open()) {
        return;
    }
    file << '['
         << st.wYear << '-'
         << st.wMonth << '-'
         << st.wDay << ' '
         << st.wHour << ':'
         << st.wMinute << ':'
         << st.wSecond << '.'
         << st.wMilliseconds
         << "] "
         << message
         << '\n';
}

void runtime_log_hr(const std::string& prefix, HRESULT hr) {
    std::ostringstream oss;
    oss << prefix << " hr=0x" << std::hex << std::uppercase << static_cast<unsigned long>(hr);
    runtime_log(oss.str());
}

bool open_url(HWND parent, const wchar_t* url, const char* log_name) {
    if (!url || !*url) {
        return false;
    }

    runtime_log(std::string("Opening download URL: ") + (log_name ? log_name : "unknown") +
        " url=" + wide_to_utf8(url));
    const INT_PTR result = reinterpret_cast<INT_PTR>(
        ShellExecuteW(parent, L"open", url, nullptr, nullptr, SW_SHOWNORMAL));
    if (result > 32) {
        return true;
    }

    runtime_log("ShellExecuteW failed while opening URL. target=" +
        std::string(log_name ? log_name : "unknown") +
        " code=" + std::to_string(static_cast<long long>(result)));
    MessageBoxW(
        parent,
        L"无法打开下载链接，请手动复制以下地址后在浏览器中打开：\n\n"
        L"官方地址：\nhttps://developer.microsoft.com/en-us/microsoft-edge/webview2/\n\n"
        L"国内镜像：\nhttp://disk.mumuyun.cn/down.php/7a859af9af33fdaf638eed723391b880.exe",
        L"打开下载链接失败",
        MB_OK | MB_ICONWARNING);
    return false;
}

LRESULT CALLBACK runtime_missing_dialog_wndproc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<RuntimeMissingDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (message) {
    case WM_NCCREATE: {
        auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create ? create->lpCreateParams : nullptr));
        return TRUE;
    }
    case WM_CREATE: {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        const HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

        const struct ControlSpec {
            const wchar_t* klass;
            const wchar_t* text;
            DWORD style;
            int x;
            int y;
            int width;
            int height;
            int id;
        } controls[] = {
            { L"STATIC", L"未检测到 Microsoft Edge WebView2 Runtime。", WS_CHILD | WS_VISIBLE, 14, 14, 310, 18, 0 },
            { L"STATIC", L"请先手动下载安装，安装完成后重新启动程序。", WS_CHILD | WS_VISIBLE, 14, 34, 310, 18, 0 },
            { L"STATIC", L"可选择以下下载入口：", WS_CHILD | WS_VISIBLE, 14, 54, 310, 18, 0 },
            { L"BUTTON", L"官方下载", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 14, 84, 88, 26, kRuntimeMissingButtonOfficial },
            { L"BUTTON", L"国内镜像", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 112, 84, 88, 26, kRuntimeMissingButtonMirror },
            { L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 236, 84, 88, 26, IDCANCEL }
        };

        for (const auto& control : controls) {
            HWND child = CreateWindowExW(
                0,
                control.klass,
                control.text,
                control.style,
                control.x,
                control.y,
                control.width,
                control.height,
                hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(control.id)),
                instance,
                nullptr);
            if (child) {
                SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            }
        }
        return 0;
    }
    case WM_COMMAND: {
        const int command = LOWORD(wparam);
        if (command == kRuntimeMissingButtonOfficial ||
            command == kRuntimeMissingButtonMirror ||
            command == IDCANCEL) {
            if (state) {
                state->result = command;
            }
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        if (state) {
            state->result = IDCANCEL;
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (state) {
            state->done = true;
        }
        return 0;
    default:
        break;
    }

    return DefWindowProcW(hwnd, message, wparam, lparam);
}

void show_runtime_missing_dialog(HWND parent) {
    static const auto register_class = []() -> ATOM {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = runtime_missing_dialog_wndproc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = kRuntimeMissingDialogClassName;
        return RegisterClassExW(&wc);
    };

    static const ATOM atom = register_class();
    if (!atom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        runtime_log("Failed to register missing runtime dialog class. gle=" + std::to_string(GetLastError()));
        const int fallback = MessageBoxW(
            parent,
            L"未检测到 Microsoft Edge WebView2 Runtime。\n\n"
            L"选择“是”打开官方下载页，选择“否”打开国内镜像。\n"
            L"安装完成后请重新启动程序。",
            L"缺少 WebView2 Runtime",
            MB_ICONWARNING | MB_YESNOCANCEL);
        if (fallback == IDYES) {
            open_url(parent, kOfficialDownloadUrl, "official");
        } else if (fallback == IDNO) {
            open_url(parent, kMirrorDownloadUrl, "mirror");
        }
        return;
    }

    HWND owner = (parent && IsWindow(parent)) ? GetAncestor(parent, GA_ROOT) : nullptr;
    if (!owner || !IsWindow(owner)) {
        owner = parent;
    }

    RECT owner_rect = { 0, 0, 360, 132 };
    if (owner && IsWindow(owner)) {
        GetWindowRect(owner, &owner_rect);
    } else {
        owner_rect.right = GetSystemMetrics(SM_CXSCREEN);
        owner_rect.bottom = GetSystemMetrics(SM_CYSCREEN);
    }

    const int dialog_width = 340;
    const int dialog_height = 150;
    const int x = owner_rect.left + (((owner_rect.right - owner_rect.left) - dialog_width) / 2);
    const int y = owner_rect.top + (((owner_rect.bottom - owner_rect.top) - dialog_height) / 2);

    RuntimeMissingDialogState state = {};
    state.parent = owner;

    if (owner && IsWindow(owner)) {
        EnableWindow(owner, FALSE);
    }

    HWND dialog = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        kRuntimeMissingDialogClassName,
        L"缺少 WebView2 Runtime",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        x,
        y,
        dialog_width,
        dialog_height,
        owner,
        nullptr,
        GetModuleHandleW(nullptr),
        &state);

    if (!dialog) {
        if (owner && IsWindow(owner)) {
            EnableWindow(owner, TRUE);
        }
        runtime_log("Failed to create missing runtime dialog. gle=" + std::to_string(GetLastError()));
        const int fallback = MessageBoxW(
            parent,
            L"未检测到 Microsoft Edge WebView2 Runtime。\n\n"
            L"选择“是”打开官方下载页，选择“否”打开国内镜像。\n"
            L"安装完成后请重新启动程序。",
            L"缺少 WebView2 Runtime",
            MB_ICONWARNING | MB_YESNOCANCEL);
        if (fallback == IDYES) {
            open_url(parent, kOfficialDownloadUrl, "official");
        } else if (fallback == IDNO) {
            open_url(parent, kMirrorDownloadUrl, "mirror");
        }
        return;
    }

    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);

    MSG msg = {};
    while (!state.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dialog, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (owner && IsWindow(owner)) {
        EnableWindow(owner, TRUE);
        SetActiveWindow(owner);
        SetForegroundWindow(owner);
    }

    if (state.result == kRuntimeMissingButtonOfficial) {
        open_url(parent, kOfficialDownloadUrl, "official");
    } else if (state.result == kRuntimeMissingButtonMirror) {
        open_url(parent, kMirrorDownloadUrl, "mirror");
    }
}

std::string load_resource(int id) {
    HRSRC res = FindResource(nullptr, MAKEINTRESOURCE(id), RT_RCDATA);
    if (!res) {
        return std::string();
    }
    HGLOBAL data = LoadResource(nullptr, res);
    if (!data) {
        return std::string();
    }
    const DWORD size = SizeofResource(nullptr, res);
    const char* bytes = static_cast<const char*>(LockResource(data));
    if (!bytes || size == 0) {
        return std::string();
    }
    return std::string(bytes, bytes + size);
}

std::wstring build_full_html() {
    std::string html = load_resource(IDR_HTML_INDEX);
    const std::string css = load_resource(IDR_CSS_STYLE);
    const std::string js = load_resource(IDR_JS_APP);
    if (html.empty()) {
        return std::wstring();
    }
    const std::string link_tag = "<link rel=\"stylesheet\" href=\"style.css\">";
    const size_t link_pos = html.find(link_tag);
    if (link_pos != std::string::npos) {
        html.replace(link_pos, link_tag.length(), "<style>\n" + css + "\n</style>");
    }
    const std::string script_tag = "<script src=\"app.js\"></script>";
    const size_t script_pos = html.find(script_tag);
    if (script_pos != std::string::npos) {
        html.replace(script_pos, script_tag.length(), "<script>\n" + js + "\n</script>");
    }
    return utf8_to_wide(html);
}

RuntimePresence detect_runtime_presence() {
    RuntimePresence presence;
    LPWSTR version = nullptr;
    presence.api_hr = GetAvailableCoreWebView2BrowserVersionString(nullptr, &version);
    if (SUCCEEDED(presence.api_hr) && version && is_installed_runtime_version(version)) {
        presence.api_version = trim_copy(version);
        presence.available = true;
    }
    if (version) {
        CoTaskMemFree(version);
    }
    if (!presence.available) {
        presence.registry_version = registry_runtime_version();
        presence.available = !presence.registry_version.empty();
    }
    return presence;
}

bool has_runtime(bool log_details = false) {
    const RuntimePresence presence = detect_runtime_presence();
    if (log_details) {
        if (!presence.api_version.empty()) {
            runtime_log("WebView2 runtime detected via API. version=" + wide_to_utf8(presence.api_version));
        } else if (!presence.registry_version.empty()) {
            runtime_log_hr("GetAvailableCoreWebView2BrowserVersionString did not detect a usable runtime.", presence.api_hr);
            runtime_log("WebView2 runtime detected via registry fallback. client=" +
                wide_to_utf8(kWebView2RuntimeClientId) +
                " version=" + wide_to_utf8(presence.registry_version));
        } else {
            runtime_log_hr("WebView2 runtime not detected.", presence.api_hr);
        }
    }
    return presence.available;
}

bool install_should_stop() {
    return g_install_cancel.load(std::memory_order_acquire) || (g_parent && !IsWindow(g_parent));
}

void post_host_message(UINT message_id) {
    if (g_parent && IsWindow(g_parent)) {
        PostMessageW(g_parent, message_id, 0, 0);
    }
}

void update_status(const std::wstring& title, const std::wstring& detail, int progress, bool marquee, bool failed = false, bool visible = true) {
    {
        std::lock_guard<std::mutex> lock(g_status_mutex);
        g_status.visible = visible;
        g_status.marquee = marquee;
        g_status.failed = failed;
        g_status.progress = clamp_int(progress, 0, 100);
        g_status.title = title;
        g_status.detail = detail;
    }
    post_host_message(WM_WEBVIEW2_RUNTIME_STATUS);
}

void clear_status() {
    {
        std::lock_guard<std::mutex> lock(g_status_mutex);
        g_status = InstallStatus{};
    }
    post_host_message(WM_WEBVIEW2_RUNTIME_STATUS);
}

void set_control_font(HWND hwnd) {
    if (!hwnd) {
        return;
    }
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
}

void layout_ui() {
    if (!g_ui.parent || !g_ui.panel || !IsWindow(g_ui.parent) || !IsWindow(g_ui.panel)) {
        return;
    }

    RECT rc = {};
    GetClientRect(g_ui.parent, &rc);
    const int width = rc.right - rc.left;
    const int panel_width = (width > 80) ? ((width - 40 < kPanelWidth) ? (width - 40) : kPanelWidth) : width;
    const int x = ((width - panel_width) / 2 > 12) ? (width - panel_width) / 2 : 12;
    const int y = ((rc.bottom - kPanelHeight) / 2 > 12) ? (rc.bottom - kPanelHeight) / 2 : 12;
    const int safe_width = (panel_width > 260) ? panel_width : 260;

    MoveWindow(g_ui.panel, x, y, safe_width, kPanelHeight, TRUE);
    MoveWindow(g_ui.title, 18, 18, safe_width - 36, 24, TRUE);
    MoveWindow(g_ui.detail, 18, 48, safe_width - 36, 50, TRUE);
    MoveWindow(g_ui.progress, 18, 108, safe_width - 36, 20, TRUE);
}

bool ensure_ui(HWND parent) {
    if (!parent || !IsWindow(parent)) {
        return false;
    }

    INITCOMMONCONTROLSEX icex = {};
    icex.dwSize = sizeof(icex);
    icex.dwICC = ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&icex);

    if (g_ui.panel && IsWindow(g_ui.panel)) {
        g_ui.parent = parent;
        layout_ui();
        ShowWindow(g_ui.panel, SW_SHOW);
        return true;
    }

    g_ui = {};
    g_ui.parent = parent;
    g_ui.panel = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_ui.panel) {
        g_ui = {};
        return false;
    }

    g_ui.title = CreateWindowExW(0, L"STATIC", L"Preparing WebView2 Runtime", WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0, g_ui.panel, nullptr, GetModuleHandleW(nullptr), nullptr);
    g_ui.detail = CreateWindowExW(0, L"STATIC", L"Checking local runtime availability...", WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0, g_ui.panel, nullptr, GetModuleHandleW(nullptr), nullptr);
    g_ui.progress = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0, g_ui.panel, nullptr, GetModuleHandleW(nullptr), nullptr);

    if (!g_ui.title || !g_ui.detail || !g_ui.progress) {
        if (g_ui.progress) {
            DestroyWindow(g_ui.progress);
        }
        if (g_ui.detail) {
            DestroyWindow(g_ui.detail);
        }
        if (g_ui.title) {
            DestroyWindow(g_ui.title);
        }
        DestroyWindow(g_ui.panel);
        g_ui = {};
        return false;
    }

    set_control_font(g_ui.title);
    set_control_font(g_ui.detail);
    set_control_font(g_ui.progress);
    SendMessageW(g_ui.progress, PBM_SETRANGE32, 0, 100);
    SendMessageW(g_ui.progress, PBM_SETPOS, 0, 0);
    layout_ui();
    return true;
}

void destroy_ui() {
    if (g_ui.progress && IsWindow(g_ui.progress)) {
        DestroyWindow(g_ui.progress);
    }
    if (g_ui.detail && IsWindow(g_ui.detail)) {
        DestroyWindow(g_ui.detail);
    }
    if (g_ui.title && IsWindow(g_ui.title)) {
        DestroyWindow(g_ui.title);
    }
    if (g_ui.panel && IsWindow(g_ui.panel)) {
        DestroyWindow(g_ui.panel);
    }
    g_ui = {};
}

void set_marquee(bool enabled) {
    if (!g_ui.progress || !IsWindow(g_ui.progress) || g_ui.marquee == enabled) {
        return;
    }

    LONG_PTR style = GetWindowLongPtrW(g_ui.progress, GWL_STYLE);
    if (enabled) {
        style |= PBS_MARQUEE;
        SetWindowLongPtrW(g_ui.progress, GWL_STYLE, style);
        SetWindowPos(g_ui.progress, nullptr, 0, 0, 0, 0,
            SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
        SendMessageW(g_ui.progress, PBM_SETMARQUEE, TRUE, 25);
    } else {
        SendMessageW(g_ui.progress, PBM_SETMARQUEE, FALSE, 0);
        style &= ~static_cast<LONG_PTR>(PBS_MARQUEE);
        SetWindowLongPtrW(g_ui.progress, GWL_STYLE, style);
        SetWindowPos(g_ui.progress, nullptr, 0, 0, 0, 0,
            SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
        SendMessageW(g_ui.progress, PBM_SETRANGE32, 0, 100);
    }

    g_ui.marquee = enabled;
}

void apply_status(HWND parent) {
    InstallStatus status;
    {
        std::lock_guard<std::mutex> lock(g_status_mutex);
        status = g_status;
    }

    if (!status.visible) {
        destroy_ui();
        return;
    }

    if (!ensure_ui(parent)) {
        return;
    }

    SetWindowTextW(g_ui.title, status.title.c_str());
    SetWindowTextW(g_ui.detail, status.detail.c_str());
    if (status.marquee) {
        set_marquee(true);
    } else {
        set_marquee(false);
        SendMessageW(g_ui.progress, PBM_SETPOS, status.progress, 0);
    }
    layout_ui();
    ShowWindow(g_ui.panel, SW_SHOW);
    UpdateWindow(g_ui.panel);
}

std::filesystem::path bundled_installer_path() {
    const std::filesystem::path base = module_dir();
    const wchar_t* names[] = {
        L"MicrosoftEdgeWebView2Setup.exe",
        L"MicrosoftEdgeWebview2Setup.exe",
        L"MicrosoftEdgeWebView2RuntimeInstallerX64.exe",
        L"MicrosoftEdgeWebView2RuntimeInstallerX86.exe"
    };

    for (const auto* name : names) {
        const std::filesystem::path full = base / name;
        if (std::filesystem::exists(full)) {
            return full;
        }
    }

    return {};
}

std::filesystem::path temp_installer_path() {
    wchar_t temp[MAX_PATH] = {};
    const DWORD length = GetTempPathW(MAX_PATH, temp);
    if (length == 0 || length >= MAX_PATH) {
        return {};
    }
    return std::filesystem::path(temp) / L"MicrosoftEdgeWebView2Setup.exe";
}

std::wstring format_bytes(ULONGLONG bytes) {
    const wchar_t* units[] = { L"B", L"KB", L"MB", L"GB" };
    double value = static_cast<double>(bytes);
    size_t index = 0;
    while (value >= 1024.0 && index < (_countof(units) - 1)) {
        value /= 1024.0;
        ++index;
    }

    if (index == 0) {
        return std::to_wstring(bytes) + L" " + units[index];
    }

    wchar_t buffer[64] = {};
    swprintf(buffer, _countof(buffer), L"%.1f %ls", value, units[index]);
    return std::wstring(buffer);
}

std::filesystem::path powershell_executable_path() {
    wchar_t system_dir[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(system_dir, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return L"powershell.exe";
    }

    const std::filesystem::path path =
        std::filesystem::path(system_dir) / L"WindowsPowerShell" / L"v1.0" / L"powershell.exe";
    if (std::filesystem::exists(path)) {
        return path;
    }

    return L"powershell.exe";
}

bool run_hidden_process_and_wait(
    const std::filesystem::path& application,
    const std::wstring& arguments,
    const std::wstring& working_directory,
    DWORD timeout_ms,
    DWORD* exit_code) {
    if (exit_code) {
        *exit_code = static_cast<DWORD>(-1);
    }

    if (application.empty()) {
        runtime_log("Helper process path is empty.");
        return false;
    }

    const std::wstring application_path = application.wstring();
    std::wstring command_line = L"\"" + application_path + L"\"";
    if (!arguments.empty()) {
        command_line += L" ";
        command_line += arguments;
    }

    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};
    const BOOL created = CreateProcessW(
        application_path.c_str(),
        mutable_command.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        working_directory.empty() ? nullptr : working_directory.c_str(),
        &si,
        &pi);

    if (!created) {
        runtime_log("CreateProcessW failed for helper process. file=" +
            wide_to_utf8(application_path) +
            " gle=" + std::to_string(GetLastError()));
        return false;
    }

    CloseHandle(pi.hThread);

    bool cancelled = false;
    bool timed_out = false;
    bool wait_failed = false;
    DWORD last_error = ERROR_SUCCESS;
    const ULONGLONG start_tick = GetTickCount64();

    while (true) {
        if (install_should_stop()) {
            cancelled = true;
            TerminateProcess(pi.hProcess, ERROR_CANCELLED);
            break;
        }

        DWORD wait_ms = 250;
        if (timeout_ms != INFINITE) {
            const ULONGLONG elapsed = GetTickCount64() - start_tick;
            if (elapsed >= timeout_ms) {
                timed_out = true;
                TerminateProcess(pi.hProcess, WAIT_TIMEOUT);
                break;
            }
            const DWORD remaining = timeout_ms - static_cast<DWORD>(elapsed);
            wait_ms = (remaining < wait_ms) ? remaining : wait_ms;
        }

        const DWORD wait_result = WaitForSingleObject(pi.hProcess, wait_ms);
        if (wait_result == WAIT_OBJECT_0) {
            break;
        }
        if (wait_result == WAIT_TIMEOUT) {
            continue;
        }

        wait_failed = true;
        last_error = GetLastError();
        break;
    }

    DWORD process_exit_code = static_cast<DWORD>(-1);
    if (!GetExitCodeProcess(pi.hProcess, &process_exit_code)) {
        process_exit_code = static_cast<DWORD>(-1);
    }
    CloseHandle(pi.hProcess);

    if (exit_code) {
        *exit_code = process_exit_code;
    }

    if (cancelled) {
        runtime_log("Helper process was cancelled.");
        return false;
    }
    if (timed_out) {
        runtime_log("Helper process timed out.");
        return false;
    }
    if (wait_failed) {
        runtime_log("WaitForSingleObject failed for helper process. gle=" + std::to_string(last_error));
        return false;
    }

    return true;
}

bool download_installer_with_powershell(const std::filesystem::path& path) {
    const std::filesystem::path powershell = powershell_executable_path();
    runtime_log("Retrying installer download with PowerShell. exe=" + wide_to_utf8(powershell.wstring()));
    update_status(
        L"Retrying WebView2 Runtime Download",
        L"Primary download method failed. Retrying with PowerShell...",
        0,
        true);

    const std::wstring escaped_url = escape_powershell_single_quoted(kBootstrapperUrl);
    const std::wstring escaped_path = escape_powershell_single_quoted(path.wstring());
    const std::wstring script =
        L"$ProgressPreference='SilentlyContinue';"
        L"[Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12;"
        L"Invoke-WebRequest -UseBasicParsing -Uri '" + escaped_url + L"' -MaximumRedirection 10 -OutFile '" + escaped_path + L"';"
        L"if (!(Test-Path -LiteralPath '" + escaped_path + L"')) { exit 2 }"
        L"$item=Get-Item -LiteralPath '" + escaped_path + L"';"
        L"if ($item.Length -le 0) { exit 3 }";

    const std::string encoded_command = base64_encode(
        reinterpret_cast<const unsigned char*>(script.data()),
        script.size() * sizeof(wchar_t));
    const std::wstring arguments =
        L"-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand " +
        std::wstring(encoded_command.begin(), encoded_command.end());

    DWORD exit_code = static_cast<DWORD>(-1);
    if (!run_hidden_process_and_wait(powershell, arguments, path.parent_path().wstring(), 2 * 60 * 1000, &exit_code)) {
        runtime_log("PowerShell download helper failed before normal exit. exit_code=" + std::to_string(exit_code));
        return false;
    }

    if (exit_code != 0) {
        runtime_log("PowerShell download helper exited with code=" + std::to_string(exit_code));
        return false;
    }

    std::error_code ec;
    const std::uintmax_t size = std::filesystem::exists(path) ? std::filesystem::file_size(path, ec) : 0;
    if (ec || size == 0) {
        runtime_log("PowerShell download completed but installer file is missing or empty.");
        return false;
    }

    runtime_log("PowerShell download succeeded. bytes=" + std::to_string(static_cast<unsigned long long>(size)));
    return true;
}

class DownloadProgressCallback final : public IBindStatusCallback {
public:
    DownloadProgressCallback() : ref_count_(1), last_bucket_(-1) {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (!object) {
            return E_POINTER;
        }
        *object = nullptr;
        if (riid == IID_IUnknown || riid == IID_IBindStatusCallback) {
            *object = static_cast<IBindStatusCallback*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&ref_count_));
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG ref_count = static_cast<ULONG>(InterlockedDecrement(&ref_count_));
        if (ref_count == 0) {
            delete this;
        }
        return ref_count;
    }

    HRESULT STDMETHODCALLTYPE OnStartBinding(DWORD, IBinding*) override {
        runtime_log("Starting WebView2 installer download.");
        update_status(L"Downloading WebView2 Runtime", L"Downloading the WebView2 installer...", 0, false);
        return install_should_stop() ? E_ABORT : S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetPriority(LONG*) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE OnLowResource(DWORD) override {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnProgress(ULONG progress, ULONG progress_max, ULONG, LPCWSTR) override {
        if (install_should_stop()) {
            runtime_log("Download aborted because shutdown was requested.");
            return E_ABORT;
        }

        if (progress_max > 0) {
            const int percent = static_cast<int>((static_cast<unsigned long long>(progress) * 100ULL) / progress_max);
            update_status(
                L"Downloading WebView2 Runtime",
                L"Downloaded " + format_bytes(progress) + L" / " + format_bytes(progress_max) +
                    L" (" + std::to_wstring(clamp_int(percent, 0, 100)) + L"%)",
                percent,
                false);

            const int bucket = clamp_int(percent / 10, 0, 10);
            if (bucket != last_bucket_) {
                last_bucket_ = bucket;
                runtime_log("Download progress: " + std::to_string(bucket * 10) + "%.");
            }
        } else {
            update_status(
                L"Downloading WebView2 Runtime",
                L"Downloading the WebView2 installer. Total size is currently unavailable...",
                0,
                true);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnStopBinding(HRESULT hr_status, LPCWSTR) override {
        runtime_log_hr("Download finished.", hr_status);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetBindInfo(DWORD* flags, BINDINFO* bind_info) override {
        if (flags) {
            *flags = BINDF_GETNEWESTVERSION;
        }
        if (bind_info) {
            bind_info->cbSize = sizeof(BINDINFO);
            bind_info->dwBindVerb = BINDVERB_GET;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDataAvailable(DWORD, DWORD, FORMATETC*, STGMEDIUM*) override {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnObjectAvailable(REFIID, IUnknown*) override {
        return S_OK;
    }

private:
    LONG ref_count_;
    int last_bucket_;
};

bool download_installer(const std::filesystem::path& path) {
    if (path.empty()) {
        runtime_log("Temporary installer path is empty.");
        return false;
    }

    runtime_log("Downloading installer from: " + wide_to_utf8(kBootstrapperUrl));
    runtime_log("Downloading to: " + wide_to_utf8(path.wstring()));

    std::error_code ec;
    std::filesystem::remove(path, ec);

    auto* callback = new (std::nothrow) DownloadProgressCallback();
    if (!callback) {
        runtime_log("Failed to allocate download progress callback.");
        return false;
    }

    const HRESULT hr = URLDownloadToFileW(nullptr, kBootstrapperUrl, path.c_str(), 0, callback);
    callback->Release();

    if (SUCCEEDED(hr) && !install_should_stop() && std::filesystem::exists(path)) {
        runtime_log("Installer download succeeded.");
        return true;
    }

    runtime_log_hr("Primary installer download method failed.", hr);
    if (!install_should_stop() && download_installer_with_powershell(path)) {
        runtime_log("Installer download succeeded via fallback.");
        return true;
    }

    runtime_log_hr("Installer download failed.", hr);
    std::filesystem::remove(path, ec);
    return false;
}

bool start_installer_process(const std::filesystem::path& path, HANDLE* process) {
    if (!process) {
        return false;
    }
    *process = nullptr;

    if (path.empty() || !std::filesystem::exists(path)) {
        runtime_log("Installer file is missing: " + wide_to_utf8(path.wstring()));
        return false;
    }

    std::wstring command_line = L"\"" + path.wstring() + L"\" /silent /install";
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};
    const std::wstring workdir = path.parent_path().wstring();
    const BOOL created = CreateProcessW(
        nullptr,
        mutable_command.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        workdir.empty() ? nullptr : workdir.c_str(),
        &si,
        &pi);

    if (!created) {
        runtime_log("CreateProcessW failed for installer. gle=" + std::to_string(GetLastError()));
        return false;
    }

    CloseHandle(pi.hThread);
    *process = pi.hProcess;
    runtime_log("Installer process started. pid=" + std::to_string(pi.dwProcessId));
    return true;
}

bool wait_for_runtime(HANDLE process) {
    const ULONGLONG start_tick = GetTickCount64();
    ULONGLONG last_log_tick = 0;
    bool exited = false;
    DWORD exit_code = STILL_ACTIVE;
    ULONGLONG fail_deadline = 0;

    update_status(
        L"Installing WebView2 Runtime",
        L"Installing the runtime silently. This can take a few minutes...",
        0,
        true);

    while (!install_should_stop()) {
        if (has_runtime()) {
            runtime_log("Runtime is now available.");
            return true;
        }

        const ULONGLONG now_tick = GetTickCount64();
        if (now_tick - start_tick >= kInstallTimeoutMs) {
            runtime_log("Timed out while waiting for runtime installation.");
            break;
        }

        if (!exited && process) {
            const DWORD wait_result = WaitForSingleObject(process, 250);
            if (wait_result == WAIT_OBJECT_0) {
                exited = true;
                GetExitCodeProcess(process, &exit_code);
                runtime_log("Installer process exited. code=" + std::to_string(exit_code));
                if (exit_code != 0) {
                    fail_deadline = now_tick + 5000;
                }
            } else if (wait_result == WAIT_FAILED) {
                runtime_log("WaitForSingleObject failed. gle=" + std::to_string(GetLastError()));
                break;
            }
        } else {
            Sleep(250);
        }

        if (fail_deadline != 0 && now_tick >= fail_deadline) {
            runtime_log("Installer exited with failure and runtime did not become available.");
            break;
        }

        if (now_tick - last_log_tick >= 5000) {
            const DWORD elapsed = static_cast<DWORD>((now_tick - start_tick) / 1000);
            runtime_log("Still waiting for runtime. elapsed=" + std::to_string(elapsed) + "s");
            update_status(
                L"Installing WebView2 Runtime",
                L"Installing the runtime silently. This can take a few minutes...\nElapsed: " +
                    std::to_wstring(elapsed) + L" s",
                0,
                true);
            last_log_tick = now_tick;
        }
    }

    return false;
}

bool install_from(const std::filesystem::path& path, const wchar_t* source) {
    runtime_log("Trying installer source=" + wide_to_utf8(source ? source : L"unknown") +
        " path=" + wide_to_utf8(path.wstring()));
    update_status(
        L"Installing WebView2 Runtime",
        L"Using " + std::wstring(source ? source : L"installer") + L" package to install WebView2 silently...",
        0,
        true);

    HANDLE process = nullptr;
    if (!start_installer_process(path, &process)) {
        return false;
    }

    const bool ok = wait_for_runtime(process);
    CloseHandle(process);
    runtime_log(std::string("Install result from source=") +
        wide_to_utf8(source ? source : L"unknown") +
        (ok ? " success" : " failure"));
    return ok;
}

void setup_message_handler_if_possible() {
    if (!g_webview || !g_message_callback) {
        return;
    }

    g_webview->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                wil::unique_cotaskmem_string message;
                args->TryGetWebMessageAsString(&message);
                if (message && g_message_callback) {
                    const int utf8_size = WideCharToMultiByte(CP_UTF8, 0, message.get(), -1, nullptr, 0, nullptr, nullptr);
                    if (utf8_size > 0) {
                        char* utf8 = static_cast<char*>(malloc(static_cast<size_t>(utf8_size)));
                        if (utf8) {
                            WideCharToMultiByte(CP_UTF8, 0, message.get(), -1, utf8, utf8_size, nullptr, nullptr);
                            g_message_callback(utf8);
                            free(utf8);
                        }
                    }
                }
                return S_OK;
            }).Get(),
        nullptr);
}

int create_webview_now(HWND parent, int port) {
    if (!parent || !IsWindow(parent)) {
        runtime_log("create_webview_now aborted because parent window is invalid.");
        return WEBVIEW2_CREATE_INIT_FAILED;
    }

    if (g_webview && g_controller) {
        return WEBVIEW2_CREATE_OK;
    }

    std::wstring* embedded_html = nullptr;
    if (port == 0) {
        embedded_html = new std::wstring(build_full_html());
    }

    const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr,
        nullptr,
        nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [parent, port, embedded_html](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result) || !env) {
                    runtime_log_hr("CreateCoreWebView2Environment callback failed.", result);
                    delete embedded_html;
                    return result;
                }

                env->CreateCoreWebView2Controller(
                    parent,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [parent, port, embedded_html](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(result) || !controller) {
                                runtime_log_hr("CreateCoreWebView2Controller callback failed.", result);
                                delete embedded_html;
                                return result;
                            }

                            g_controller = controller;
                            controller->get_CoreWebView2(&g_webview);

                            wil::com_ptr<ICoreWebView2Controller2> controller2;
                            if (SUCCEEDED(controller->QueryInterface(IID_PPV_ARGS(&controller2))) && controller2) {
                                const COREWEBVIEW2_COLOR background = { 0xFF, 0x0A, 0x0E, 0x1A };
                                const HRESULT bg_hr = controller2->put_DefaultBackgroundColor(background);
                                if (FAILED(bg_hr)) {
                                    runtime_log_hr("put_DefaultBackgroundColor failed.", bg_hr);
                                }
                            }

                            ICoreWebView2Settings* settings = nullptr;
                            if (SUCCEEDED(g_webview->get_Settings(&settings)) && settings) {
                                settings->put_AreDevToolsEnabled(TRUE);
                                settings->put_AreDefaultContextMenusEnabled(TRUE);
                                settings->put_IsStatusBarEnabled(FALSE);

                                wil::com_ptr<ICoreWebView2Settings3> settings3;
                                if (SUCCEEDED(settings->QueryInterface(IID_PPV_ARGS(&settings3))) && settings3) {
                                    settings3->put_AreBrowserAcceleratorKeysEnabled(TRUE);
                                }
                                settings->Release();
                            }

                            RECT rc = {};
                            GetClientRect(parent, &rc);
                            controller->put_Bounds(rc);
                            setup_message_handler_if_possible();

                            if (port == 0 && embedded_html && !embedded_html->empty()) {
                                g_webview->NavigateToString(embedded_html->c_str());
                            } else {
                                wchar_t url[128] = {};
                                swprintf(url, _countof(url), L"http://127.0.0.1:%d/", port);
                                g_webview->Navigate(url);
                            }

                            delete embedded_html;
                            clear_status();
                            destroy_ui();
                            runtime_log("WebView2 controller created successfully.");
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());

    if (FAILED(hr)) {
        delete embedded_html;
        runtime_log_hr("CreateCoreWebView2EnvironmentWithOptions failed.", hr);
        return WEBVIEW2_CREATE_INIT_FAILED;
    }

    runtime_log("WebView2 environment creation started.");
    return WEBVIEW2_CREATE_OK;
}

void install_worker() {
    const HRESULT co_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool co_initialized = SUCCEEDED(co_hr);
    runtime_log("Background install worker started.");

    bool ok = has_runtime(true);

    if (!ok && !install_should_stop()) {
        const std::filesystem::path bundled = bundled_installer_path();
        if (!bundled.empty()) {
            ok = install_from(bundled, L"bundled");
        } else {
            runtime_log("No bundled installer found.");
            runtime_log("Recommended bundled installer name: " + wide_to_utf8(kRecommendedBundledInstallerName));
        }
    }

    if (!ok && !install_should_stop()) {
        const std::filesystem::path cached = temp_installer_path();
        if (!cached.empty() && std::filesystem::exists(cached)) {
            ok = install_from(cached, L"cached");
        } else {
            runtime_log("No cached installer found.");
        }
    }

    if (!ok && !install_should_stop()) {
        const std::filesystem::path downloaded = temp_installer_path();
        if (!downloaded.empty() && download_installer(downloaded)) {
            ok = install_from(downloaded, L"downloaded");
        } else {
            runtime_log("Download step failed.");
        }
    }

    if (install_should_stop()) {
        runtime_log("Background install worker stopped because shutdown was requested.");
        clear_status();
        g_install_running.store(false, std::memory_order_release);
        if (co_initialized) {
            CoUninitialize();
        }
        return;
    }

    if (ok) {
        update_status(
            L"Finalizing WebView2 Startup",
            L"Runtime is ready. Initializing the embedded browser...",
            100,
            false);
        runtime_log("Background install worker succeeded.");
        post_host_message(WM_WEBVIEW2_RUNTIME_READY);
    } else {
        update_status(
            L"WebView2 Runtime Install Failed",
            L"Failed to install WebView2 Runtime.\nIf online download is blocked, place " +
                std::wstring(kRecommendedBundledInstallerName) +
                L" next to the application and relaunch.\nSee webview2_runtime_install.log in the application folder.",
            0,
            false,
            true);
        runtime_log("Background install worker failed.");
        post_host_message(WM_WEBVIEW2_RUNTIME_FAILED);
    }

    g_install_running.store(false, std::memory_order_release);
    if (co_initialized) {
        CoUninitialize();
    }
}

int start_install_async(HWND parent, int port) {
    g_parent = parent;
    g_pending_port = port;
    g_install_cancel.store(false, std::memory_order_release);

    update_status(
        L"Preparing WebView2 Runtime",
        L"WebView2 Runtime is missing. Download and install will continue in the background...\n"
            L"If your network blocks Microsoft downloads, bundle " +
            std::wstring(kRecommendedBundledInstallerName) +
            L" next to the application.",
        0,
        true);

    bool expected = false;
    if (!g_install_running.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        runtime_log("Install request ignored because another install is already running.");
        return WEBVIEW2_CREATE_PENDING;
    }

    try {
        std::thread(install_worker).detach();
        runtime_log("Background install thread launched.");
        return WEBVIEW2_CREATE_PENDING;
    } catch (const std::exception& ex) {
        g_install_running.store(false, std::memory_order_release);
        runtime_log(std::string("Failed to launch background install thread: ") + ex.what());
        update_status(
            L"WebView2 Runtime Install Failed",
            L"Failed to launch background install thread.\nSee webview2_runtime_install.log in the application folder.",
            0,
            false,
            true);
        return WEBVIEW2_CREATE_INSTALLER_FAILED;
    } catch (...) {
        g_install_running.store(false, std::memory_order_release);
        runtime_log("Failed to launch background install thread: unknown exception.");
        update_status(
            L"WebView2 Runtime Install Failed",
            L"Failed to launch background install thread.\nSee webview2_runtime_install.log in the application folder.",
            0,
            false,
            true);
        return WEBVIEW2_CREATE_INSTALLER_FAILED;
    }
}

} // namespace

extern "C" int webview2_create(HWND hParent, int port) {
    g_parent = hParent;
    g_pending_port = port;

    if (has_runtime(true)) {
        return create_webview_now(hParent, port);
    }

    runtime_log("WebView2 runtime is missing. Automatic download/install is disabled.");
    show_runtime_missing_dialog(hParent);
    return WEBVIEW2_CREATE_RUNTIME_MISSING;
}

extern "C" int webview2_is_ready(void) {
    return g_webview ? 1 : 0;
}

extern "C" int webview2_handle_window_message(HWND hWnd, UINT msg, WPARAM, LPARAM, LRESULT* out_result) {
    if (!out_result) {
        return 0;
    }

    switch (msg) {
    case WM_WEBVIEW2_RUNTIME_STATUS:
        apply_status(hWnd);
        *out_result = 0;
        return 1;
    case WM_WEBVIEW2_RUNTIME_READY:
        runtime_log("UI thread received runtime READY message.");
        if (create_webview_now(hWnd, g_pending_port) != WEBVIEW2_CREATE_OK) {
            update_status(
                L"WebView2 Initialization Failed",
                L"Runtime is installed, but the embedded browser failed to initialize.\nSee webview2_runtime_install.log in the application folder.",
                0,
                false,
                true);
            apply_status(hWnd);
        }
        *out_result = 0;
        return 1;
    case WM_WEBVIEW2_RUNTIME_FAILED:
        runtime_log("UI thread received runtime FAILED message.");
        apply_status(hWnd);
        *out_result = 0;
        return 1;
    default:
        break;
    }

    return 0;
}

extern "C" void webview2_resize(HWND hParent) {
    if (g_ui.parent == hParent) {
        layout_ui();
    }

    if (g_controller) {
        RECT rc = {};
        GetClientRect(hParent, &rc);
        g_controller->put_Bounds(rc);
    }
}

extern "C" void webview2_destroy(void) {
    g_install_cancel.store(true, std::memory_order_release);
    g_parent = nullptr;
    g_pending_port = 0;
    clear_status();
    destroy_ui();

    if (g_controller) {
        g_controller->Close();
        g_controller = nullptr;
    }
    g_webview = nullptr;
    g_message_callback = nullptr;
}

extern "C" int webview2_post_message(const char* json_message) {
    if (!g_webview || !json_message) {
        return -1;
    }

    const int wide_size = MultiByteToWideChar(CP_UTF8, 0, json_message, -1, nullptr, 0);
    if (wide_size <= 0) {
        return -1;
    }

    std::vector<wchar_t> buffer(static_cast<size_t>(wide_size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, json_message, -1, buffer.data(), wide_size);
    return SUCCEEDED(g_webview->PostWebMessageAsJson(buffer.data())) ? 0 : -1;
}

extern "C" int webview2_setup_message_handler(void (*callback)(const char* json)) {
    if (!callback) {
        return -1;
    }

    g_message_callback = callback;
    if (g_webview) {
        setup_message_handler_if_possible();
    }
    return 0;
}
