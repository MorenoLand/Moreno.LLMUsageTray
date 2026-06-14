#include "credential_store.h"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace {

std::string quote(const std::string& value) {
    std::string out = "'";
    for (char c : value) {
        if (c == '\'') out += "'\\''";
        else out.push_back(c);
    }
    out += "'";
    return out;
}

std::string run_read(const std::string& command) {
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return "";
    std::string out;
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe)) out += buffer;
    int rc = pclose(pipe);
    if (rc != 0) return "";
    if (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

void run_secret_store(const std::string& name, const std::string& json) {
    std::string command = "secret-tool store --label='LLM Usage Tray " + name + "' application LLMUsageTray provider " + quote(name);
    FILE* pipe = popen(command.c_str(), "w");
    if (!pipe) throw std::runtime_error("secret-tool is required for Linux credential storage");
    fwrite(json.data(), 1, json.size(), pipe);
    int rc = pclose(pipe);
    if (rc != 0) throw std::runtime_error("secret-tool failed to store credential");
}

} // namespace

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
    std::string value = run_read("secret-tool lookup application LLMUsageTray provider " + quote(name) + " 2>/dev/null");
    if (value.empty()) return std::nullopt;
    return value;
}

void credential_save_named(const std::string& name, const std::string& json) {
    run_secret_store(name, json);
}

void credential_delete_named(const std::string& name) {
    std::system(("secret-tool clear application LLMUsageTray provider " + quote(name) + " >/dev/null 2>&1").c_str());
}
