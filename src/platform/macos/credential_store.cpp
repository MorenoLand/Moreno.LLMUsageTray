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

void run_write(const std::string& command) {
    int rc = std::system(command.c_str());
    if (rc != 0) throw std::runtime_error("macOS Keychain command failed");
}

std::string service_name(const std::string& name) {
    return "LLMUsageTray." + name;
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
    std::string value = run_read("security find-generic-password -a " + quote(name) + " -s " + quote(service_name(name)) + " -w 2>/dev/null");
    if (value.empty()) return std::nullopt;
    return value;
}

void credential_save_named(const std::string& name, const std::string& json) {
    credential_delete_named(name);
    run_write("security add-generic-password -U -a " + quote(name) + " -s " + quote(service_name(name)) + " -w " + quote(json));
}

void credential_delete_named(const std::string& name) {
    std::system(("security delete-generic-password -a " + quote(name) + " -s " + quote(service_name(name)) + " >/dev/null 2>&1").c_str());
}
