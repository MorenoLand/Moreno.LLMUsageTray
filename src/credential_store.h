#pragma once

#include <optional>
#include <string>

std::optional<std::string> credential_load();
void credential_save(const std::string& json);
void credential_delete();
std::optional<std::string> credential_load_named(const std::string& name);
void credential_save_named(const std::string& name, const std::string& json);
void credential_delete_named(const std::string& name);
