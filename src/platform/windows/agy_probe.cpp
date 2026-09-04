#include "platform.h"

#include "diagnostics.h"
#include "http_client.h"

#include <winsock2.h>
#include <windows.h>
#include <tlhelp32.h>
#include <iphlpapi.h>

#include <string>
#include <vector>

static std::vector<DWORD> agy_process_ids() {
    std::vector<DWORD> ids;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return ids;
    PROCESSENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (Process32First(snapshot, &entry)) {
        do {
            if (_stricmp(entry.szExeFile, "agy.exe") == 0 || _stricmp(entry.szExeFile, "agy") == 0) ids.push_back(entry.th32ProcessID);
        } while (Process32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return ids;
}

static std::vector<unsigned short> agy_listening_ports(const std::vector<DWORD>& ids) {
    std::vector<unsigned short> ports;
    ULONG size = 0;
    if (GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) != ERROR_INSUFFICIENT_BUFFER) return ports;
    std::vector<unsigned char> buffer(size);
    auto table = reinterpret_cast<PMIB_TCPTABLE_OWNER_PID>(buffer.data());
    if (GetExtendedTcpTable(table, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) != NO_ERROR) return ports;
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto& row = table->table[i];
        if (row.dwState != MIB_TCP_STATE_LISTEN) continue;
        bool owned = false;
        for (DWORD id : ids) if (row.dwOwningPid == id) owned = true;
        if (!owned || (row.dwLocalAddr != 0 && row.dwLocalAddr != htonl(INADDR_LOOPBACK))) continue;
        ports.push_back(ntohs(static_cast<u_short>(row.dwLocalPort)));
    }
    return ports;
}

std::optional<std::string> fetch_agy_local_quota_summary() {
    std::vector<DWORD> ids = agy_process_ids();
    if (ids.empty()) return std::nullopt;
    std::vector<unsigned short> ports = agy_listening_ports(ids);
    diagnostics_log("agy local probe processes=" + std::to_string(ids.size()) + " ports=" + std::to_string(ports.size()));
    for (unsigned short port : ports) {
        for (const char* scheme : {"http", "https"}) {
            std::string url = std::string(scheme) + "://127.0.0.1:" + std::to_string(port) + "/exa.language_server_pb.LanguageServerService/RetrieveUserQuotaSummary";
            try {
                HttpResponse response = http_post_json(url, "{}", {
                    {"Connect-Protocol-Version", "1"},
                    {"User-Agent", "antigravity/cli/1.1.24 windows/amd64"},
                });
                diagnostics_log("agy local probe url=" + url + " status=" + std::to_string(response.status) + " body_length=" + std::to_string(response.body.size()));
                if (response.status >= 200 && response.status < 300 && response.body.find("\"groups\"") != std::string::npos) return response.body;
            } catch (const std::exception& error) {
                diagnostics_log("agy local probe url=" + url + " error=" + error.what());
            }
        }
    }
    return std::nullopt;
}
