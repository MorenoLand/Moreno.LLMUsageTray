#pragma once

#include <optional>
#include <string>

void open_browser(const std::string& url);
std::string wait_for_oauth_code(const std::string& expected_state);
std::string wait_for_oauth_code_on(int port, const std::string& path, const std::string& expected_state, const std::string& provider_name);
std::optional<std::string> fetch_agy_local_quota_summary();
