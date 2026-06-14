#include "credential_store.h"

#include <windows.h>
#include <wincrypt.h>

#include <fstream>
#include <stdexcept>
#include <vector>

static std::string storage_path_for(const std::string& name) {
    char appdata[MAX_PATH]{};
    DWORD len = GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        throw std::runtime_error("APPDATA is not available");
    }
    std::string dir = std::string(appdata) + "\\LLMUsageTray";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir + "\\" + name + ".dpapi";
}

static std::string storage_path() {
    return storage_path_for("openai");
}

static std::vector<unsigned char> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static void write_file(const std::string& path, const std::vector<unsigned char>& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("Could not open credential file for writing");
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

std::optional<std::string> credential_load() {
    return credential_load_named("openai");
}

void credential_save(const std::string& json) {
    credential_save_named("openai", json);
}

void credential_delete() {
    credential_delete_named("openai");
}

std::optional<std::string> credential_load_named(const std::string& name) {
    std::vector<unsigned char> encrypted = read_file(storage_path_for(name));
    if (encrypted.empty() && name == "openai") {
        encrypted = read_file(storage_path_for("auth"));
        if (encrypted.empty()) {
            char appdata[MAX_PATH]{};
            DWORD len = GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH);
            if (len > 0 && len < MAX_PATH) {
                encrypted = read_file(std::string(appdata) + "\\CodexUsageTray\\auth.dpapi");
            }
        }
    }
    if (encrypted.empty()) {
        return std::nullopt;
    }

    DATA_BLOB input{};
    input.pbData = encrypted.data();
    input.cbData = static_cast<DWORD>(encrypted.size());
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, 0, &output)) {
        return std::nullopt;
    }
    std::string value(reinterpret_cast<char*>(output.pbData), output.cbData);
    LocalFree(output.pbData);
    return value;
}

void credential_save_named(const std::string& name, const std::string& json) {
    DATA_BLOB input{};
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(json.data()));
    input.cbData = static_cast<DWORD>(json.size());
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"LLM Usage Tray OAuth", nullptr, nullptr, nullptr, 0, &output)) {
        throw std::runtime_error("CryptProtectData failed");
    }
    std::vector<unsigned char> encrypted(output.pbData, output.pbData + output.cbData);
    LocalFree(output.pbData);
    write_file(storage_path_for(name), encrypted);
}

void credential_delete_named(const std::string& name) {
    DeleteFileA(storage_path_for(name).c_str());
}
