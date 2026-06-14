#include "platform.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <stdexcept>
#include <string>

static std::string url_decode(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            char hex[3] = {s[i + 1], s[i + 2], 0};
            out.push_back(static_cast<char>(strtol(hex, nullptr, 16)));
            i += 2;
        } else if (s[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

static std::string query_value(const std::string& query, const std::string& key) {
    std::size_t pos = 0;
    while (pos < query.size()) {
        std::size_t next = query.find('&', pos);
        std::string item = query.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
        std::size_t eq = item.find('=');
        std::string k = url_decode(item.substr(0, eq));
        if (k == key) return url_decode(eq == std::string::npos ? "" : item.substr(eq + 1));
        if (next == std::string::npos) break;
        pos = next + 1;
    }
    return "";
}

static void send_response(int client, const std::string& body, int status = 200) {
    std::string status_text = status == 200 ? "OK" : "Bad Request";
    std::string response =
        "HTTP/1.1 " + std::to_string(status) + " " + status_text + "\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "Connection: close\r\n\r\n" + body;
    send(client, response.c_str(), response.size(), 0);
}

static std::string oauth_page(const std::string& provider, bool success, const std::string& details = "") {
    std::string title = success ? "Authentication complete" : "Authentication failed";
    std::string accent = success ? "#44bc7e" : "#ef4444";
    std::string close_script = success ? "<script>setTimeout(()=>window.close(),1800);</script>" : "";
    return "<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>" + title + "</title><style>html{color-scheme:dark}body{margin:0;min-height:100vh;display:grid;place-items:center;background:#090b0d;color:#f6f9f7;font-family:system-ui,sans-serif}"
        ".card{text-align:center;max-width:520px;padding:32px}.logo{width:72px;height:72px;margin:0 auto 22px;border-radius:18px;background:#24292d;display:grid;place-items:center;border:1px solid #4da174}"
        ".bars{width:38px}.bar{height:8px;margin:7px 0;border-radius:999px}.a{background:#44bc7e}.b{background:#5291e0;width:70%}h1{font-size:28px;margin:0 0 10px}p{color:#aeb8b3;line-height:1.65;margin:0}.accent{color:" + accent + "}.details{margin-top:16px;color:#8d9792;font-family:monospace;font-size:13px;word-break:break-word}</style>"
        + close_script + "</head><body><main class=\"card\"><div class=\"logo\"><div class=\"bars\"><div class=\"bar a\"></div><div class=\"bar b\"></div></div></div>"
        "<h1><span class=\"accent\">" + title + "</span></h1><p>" + provider + " is connected. You can close this tab.</p>"
        + (details.empty() ? "" : "<div class=\"details\">" + details + "</div>") + "</main></body></html>";
}

std::string wait_for_oauth_code(const std::string& expected_state) {
    return wait_for_oauth_code_on(1455, "/auth/callback", expected_state, "ChatGPT");
}

std::string wait_for_oauth_code_on(int port, const std::string& path, const std::string& expected_state, const std::string& provider_name) {
    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) throw std::runtime_error("socket failed");

    int reuse = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(server, 1) != 0) {
        close(server);
        throw std::runtime_error("Could not listen on 127.0.0.1:" + std::to_string(port));
    }

    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(server, &readfds);
    timeval timeout{};
    timeout.tv_sec = 15 * 60;
    int ready = select(server + 1, &readfds, nullptr, nullptr, &timeout);
    if (ready <= 0) {
        close(server);
        throw std::runtime_error("OAuth login timed out");
    }

    int client = accept(server, nullptr, nullptr);
    close(server);
    if (client < 0) throw std::runtime_error("accept failed");

    char buffer[8192]{};
    ssize_t received = recv(client, buffer, sizeof(buffer) - 1, 0);
    std::string req = received > 0 ? std::string(buffer, static_cast<std::size_t>(received)) : "";
    std::string first_line = req.substr(0, req.find("\r\n"));
    std::string code;
    bool ok = false;
    if (first_line.rfind("GET ", 0) == 0) {
        std::size_t path_start = 4;
        std::size_t path_end = first_line.find(' ', path_start);
        std::string target = first_line.substr(path_start, path_end - path_start);
        std::size_t q = target.find('?');
        std::string request_path = target.substr(0, q);
        std::string query = q == std::string::npos ? "" : target.substr(q + 1);
        std::string state = query_value(query, "state");
        code = query_value(query, "code");
        std::string error = query_value(query, "error");
        ok = request_path == path && state == expected_state && !code.empty() && error.empty();
    }

    send_response(client, ok ? oauth_page(provider_name, true) : oauth_page(provider_name, false, "State mismatch or missing code."), ok ? 200 : 400);
    close(client);
    if (!ok) throw std::runtime_error("OAuth callback failed validation");
    return code;
}
