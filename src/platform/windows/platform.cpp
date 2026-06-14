#include "platform.h"

#include <windows.h>
#include <shellapi.h>

#include <stdexcept>

static std::wstring widen(const std::string& s) {
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

void open_browser(const std::string& url) {
    auto wurl = widen(url);
    HINSTANCE result = ShellExecuteW(nullptr, L"open", wurl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<intptr_t>(result) <= 32) {
        throw std::runtime_error("ShellExecuteW failed to open browser");
    }
}
