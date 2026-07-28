#ifndef WEBVIEW2_HOST_H
#define WEBVIEW2_HOST_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    WEBVIEW2_CREATE_OK = 0,
    WEBVIEW2_CREATE_PENDING = 1,
    WEBVIEW2_CREATE_INIT_FAILED = -1,
    WEBVIEW2_CREATE_INSTALLER_LAUNCHED = -2,
    WEBVIEW2_CREATE_INSTALLER_FAILED = -3,
    WEBVIEW2_CREATE_RUNTIME_MISSING = -4
};

// Initialize WebView2 in the given parent window.
// If the runtime is missing, the host will prompt the user to install it manually.
// If port is 0, loads from embedded resources; otherwise navigates to localhost:port.
int webview2_create(HWND hParent, int port);

// Returns non-zero when WebView2 is ready to receive messages.
int webview2_is_ready(void);

// Lets the host consume custom window messages used by background runtime install.
int webview2_handle_window_message(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam, LRESULT* outResult);

// Resize WebView2 to fill parent window
void webview2_resize(HWND hParent);

// Cleanup WebView2
void webview2_destroy(void);

// Post JSON message to JavaScript frontend
int webview2_post_message(const char* json_message);

// Setup message handler to receive messages from JavaScript
// callback will be called with JSON string when message is received
int webview2_setup_message_handler(void (*callback)(const char* json));

#ifdef __cplusplus
}
#endif

#endif // WEBVIEW2_HOST_H
