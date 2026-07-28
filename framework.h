// header.h: 标准系统包含文件的包含文件，
// 或特定于项目的包含文件
//

#pragma once

// 禁用常见警告
#pragma warning(disable: 4819)  // 代码页字符警告
#pragma warning(disable: 4267)  // size_t 转换警告
#pragma warning(disable: 4244)  // 类型转换可能丢失数据
#pragma warning(disable: 4101)  // 未引用的局部变量
#pragma warning(disable: 4200)  // 零大小数组

#include "targetver.h"
#define WIN32_LEAN_AND_MEAN             // 从 Windows 头文件中排除极少使用的内容
// Windows 头文件
#include <windows.h>

// ========= Win32 文本编码兼容 =========
// 说明：
// - 工程使用 /utf-8，代码里大量中文字符串是 UTF-8。
// - 但如果某处仍调用 MessageBoxA（按 ACP 解释），会出现“提示窗口乱码”。
// - 这里将 MessageBoxA 统一按 UTF-8 解析并转为 UTF-16，调用 MessageBoxW。
// - 若入参并非有效 UTF-8，则回退按 ACP(GBK) 解析。
#ifndef AB3_MESSAGEBOX_UTF8_WRAPPED
#define AB3_MESSAGEBOX_UTF8_WRAPPED 1
static inline std::wstring Ab3_Utf8OrAcpToWide_(const char* s) {
    if (!s) return std::wstring();

    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, nullptr, 0);
    if (len > 0) {
        std::wstring w(static_cast<size_t>(len), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, &w[0], len);
        if (!w.empty() && w.back() == L'\0') w.pop_back();
        return w;
    }

    // 兼容：优先按 GBK(CP936) 转换（避免在非中文系统上 CP_ACP != GBK 导致乱码），再回退 CP_ACP
    len = MultiByteToWideChar(936 /*CP936*/, 0, s, -1, nullptr, 0);
    if (len > 0) {
        std::wstring w(static_cast<size_t>(len), L'\0');
        MultiByteToWideChar(936 /*CP936*/, 0, s, -1, &w[0], len);
        if (!w.empty() && w.back() == L'\0') w.pop_back();
        return w;
    }

    len = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    if (len <= 0) return std::wstring();
    std::wstring w(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_ACP, 0, s, -1, &w[0], len);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}

static inline int Ab3_MessageBoxUtf8_(HWND hWnd, const char* text, const char* caption, UINT type) {
    const std::wstring wText = Ab3_Utf8OrAcpToWide_(text);
    const std::wstring wCaption = Ab3_Utf8OrAcpToWide_(caption);
    return ::MessageBoxW(hWnd, wText.c_str(), wCaption.c_str(), type);
}

// 覆盖 MessageBoxA（仅影响显式调用 A 版的代码；MessageBox/MessageBoxW 不受影响）
#define MessageBoxA(hWnd, lpText, lpCaption, uType) Ab3_MessageBoxUtf8_((hWnd), (lpText), (lpCaption), (uType))
#endif
// C 运行时头文件
#include <stdlib.h>
#include <malloc.h>
#include <memory.h>
#include <tchar.h>
