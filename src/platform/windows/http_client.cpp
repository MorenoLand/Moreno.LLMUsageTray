#include "http_client.h"

#include <windows.h>
#include <winhttp.h>

#include <sstream>
#include <stdexcept>
#include <vector>

static std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

static std::string url_encode(const std::string& value) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

static HttpResponse request(const std::string& method, const std::string& url, const std::map<std::string, std::string>& headers, const std::string& body) {
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256]{};
    wchar_t path[4096]{};
    wchar_t extra[4096]{};
    parts.lpszHostName = host;
    parts.dwHostNameLength = ARRAYSIZE(host);
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = ARRAYSIZE(path);
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = ARRAYSIZE(extra);

    std::wstring wurl = widen(url);
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts)) {
        throw std::runtime_error("WinHttpCrackUrl failed");
    }

    std::wstring full_path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength > 0) {
        full_path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }

    HINTERNET session = WinHttpOpen(L"LLMUsageTray/0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) throw std::runtime_error("WinHttpOpen failed");
    HINTERNET connect = WinHttpConnect(session, std::wstring(parts.lpszHostName, parts.dwHostNameLength).c_str(), parts.nPort, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        throw std::runtime_error("WinHttpConnect failed");
    }
    DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET req = WinHttpOpenRequest(connect, widen(method).c_str(), full_path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        throw std::runtime_error("WinHttpOpenRequest failed");
    }

    std::wstring all_headers;
    for (const auto& [k, v] : headers) {
        all_headers += widen(k + ": " + v + "\r\n");
    }

    BOOL ok = WinHttpSendRequest(
        req,
        all_headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : all_headers.c_str(),
        all_headers.empty() ? 0 : static_cast<DWORD>(all_headers.size()),
        body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
        static_cast<DWORD>(body.size()),
        static_cast<DWORD>(body.size()),
        0);
    if (!ok || !WinHttpReceiveResponse(req, nullptr)) {
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        throw std::runtime_error("WinHTTP request failed");
    }

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &status_size, nullptr);

    std::string response;
    DWORD available = 0;
    while (WinHttpQueryDataAvailable(req, &available) && available > 0) {
        std::vector<char> buffer(available);
        DWORD read = 0;
        if (!WinHttpReadData(req, buffer.data(), available, &read)) break;
        response.append(buffer.data(), read);
    }

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return { static_cast<int>(status), response };
}

HttpResponse http_get(const std::string& url, const std::map<std::string, std::string>& headers) {
    return request("GET", url, headers, "");
}

HttpResponse http_post_form(const std::string& url, const std::map<std::string, std::string>& fields) {
    std::string body;
    for (const auto& [k, v] : fields) {
        if (!body.empty()) body.push_back('&');
        body += url_encode(k);
        body.push_back('=');
        body += url_encode(v);
    }
    return request("POST", url, { {"Content-Type", "application/x-www-form-urlencoded"} }, body);
}

HttpResponse http_post_json(const std::string& url, const std::string& body, const std::map<std::string, std::string>& headers) {
    std::map<std::string, std::string> merged = headers;
    merged["Content-Type"] = "application/json";
    merged["Accept"] = "application/json";
    return request("POST", url, merged, body);
}
