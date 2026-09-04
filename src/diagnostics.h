#pragma once

#include <string>

void diagnostics_log(const std::string& message);
void diagnostics_log_raw(const std::string& label, const std::string& body);
void diagnostics_init(bool enabled);
std::string diagnostics_log_path();
