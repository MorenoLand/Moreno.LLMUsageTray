#pragma once

#include <optional>
#include <string>

std::optional<std::string> json_string(const std::string& json, const std::string& key);
std::optional<double> json_number(const std::string& json, const std::string& key);
std::string json_escape(const std::string& value);
